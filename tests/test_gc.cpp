#include "common/hash.h"
#include "common/system.h"
#include "storage/gc.h"
#include "storage/object_store.h"

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using isvn::storage::GarbageCollector;
using isvn::storage::GcReport;
using isvn::storage::ObjectStore;
using isvn::storage::PhantomRef;
using isvn::storage::ReconcileScope;

using ReconcileSignature = GcReport (GarbageCollector::*)();
using ReclaimSignature = GcReport (GarbageCollector::*)(double, double);
static_assert(std::is_same_v<decltype(&GarbageCollector::reconcile), ReconcileSignature>);
static_assert(std::is_same_v<decltype(&GarbageCollector::reconcileDryRun), ReconcileSignature>);
static_assert(std::is_same_v<decltype(&GarbageCollector::reclaim), ReclaimSignature>);
static_assert(std::is_same_v<decltype(&PhantomRef::hash), std::string PhantomRef::*>);
static_assert(std::is_same_v<decltype(&PhantomRef::link_path), std::string PhantomRef::*>);

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/isvn-gc-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("test could not open meta.db");
        }
    }
    ~Database() { sqlite3_close(db_); }
    std::int64_t integer(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL failed");
        }
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }
    std::string text(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL text failed");
        }
        const auto* value = sqlite3_column_text(statement, 0);
        const std::string result = value == nullptr ? "" : reinterpret_cast<const char*>(value);
        sqlite3_finalize(statement);
        return result;
    }
    void execute(const std::string& sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("test SQL failed: " + message);
        }
    }
    std::vector<std::string> columnNames(const std::string& table) const {
        sqlite3_stmt* statement = nullptr;
        const std::string sql = "PRAGMA table_info(" + table + ");";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("test PRAGMA failed");
        }
        std::vector<std::string> names;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* value = sqlite3_column_text(statement, 1);
            names.emplace_back(reinterpret_cast<const char*>(value));
        }
        sqlite3_finalize(statement);
        return names;
    }

private:
    sqlite3* db_ = nullptr;
};

class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old != nullptr) old_ = old;
        ::setenv(name, value, 1);
    }
    ~ScopedEnv() {
        if (old_.empty()) {
            ::unsetenv(name_.c_str());
        } else {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        }
    }

private:
    std::string name_;
    std::string old_;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("unable to write test file");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

struct Stored {
    std::string hash;
    std::filesystem::path source;
};

Stored put(ObjectStore& store, const std::filesystem::path& root,
           const std::string& name, const std::string& content) {
    const auto source = root / (name + ".src");
    writeFile(source, content);
    return Stored{store.put_object(source).hash, source};
}

std::filesystem::path liveLink(ObjectStore& store, const Stored& stored,
                               const std::filesystem::path& root,
                               const std::string& name) {
    const auto link = root / "workspace" / name;
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(store.object_path(stored.hash), link);
    store.register_ref(stored.hash, std::filesystem::absolute(link).string());
    return link;
}

// The workspace that liveLink() writes into, marked as an iSVN workspace owned
// by the current user so a scoped reconcile may release refs inside it.
ReconcileScope ownWorkspaceScope(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "workspace" / ".isvn_local");
    return ReconcileScope{isvn::common::system_user(), root / "workspace"};
}

void testGhostRefRemoved() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "ghost", "ghost-content");
    const auto link = liveLink(store, stored, temporary.path(), "ghost.link");
    std::filesystem::remove(link);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcile();
    Database db(temporary.path() / "pool" / "meta.db");
    require(report.refs_removed == 1, "reconcile must remove missing symlink ref");
    require(db.integer("SELECT COUNT(*) FROM ref;") == 0, "ghost ref row must be gone");
    require(store.refcount(stored.hash) == 0, "refcount must be recalculated to zero");
    require(store.has_object(stored.hash), "reconcile must not delete object content");
}

void testChangedSymlinkRemoved() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto first = put(store, temporary.path(), "first", "first-content");
    const auto second = put(store, temporary.path(), "second", "second-content");
    const auto link = liveLink(store, first, temporary.path(), "design.link");
    std::filesystem::remove(link);
    std::filesystem::create_symlink(store.object_path(second.hash), link);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcile();
    Database db(temporary.path() / "pool" / "meta.db");
    require(report.refs_removed == 1, "reconcile must remove ref that points elsewhere");
    require(db.integer("SELECT COUNT(*) FROM ref;") == 0, "changed symlink ref must be gone");
    require(store.refcount(first.hash) == 0, "old hash refcount must be zero");
}

void testReconcileDryRunIsReadOnly() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto live = put(store, temporary.path(), "live", "live-dry-run-content");
    const auto ghost = put(store, temporary.path(), "ghost", "ghost-dry-run-content");
    liveLink(store, live, temporary.path(), "live.link");
    const auto ghost_link = liveLink(store, ghost, temporary.path(), "ghost.link");
    std::filesystem::remove(ghost_link);
    Database db(temporary.path() / "pool" / "meta.db");
    const auto refs_before = db.integer("SELECT COUNT(*) FROM ref;");
    const auto live_refcount_before = store.refcount(live.hash);
    const auto ghost_refcount_before = store.refcount(ghost.hash);
    const bool live_exists_before = store.has_object(live.hash);
    const bool ghost_exists_before = store.has_object(ghost.hash);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcileDryRun();
    require(report.refs_removed == 0, "dry-run must not remove refs");
    require(report.phantom_refs.size() == 1, "dry-run must report one phantom ref");
    require(report.phantom_refs.front().hash == ghost.hash,
            "dry-run must report the ghost hash");
    require(report.phantom_refs.front().link_path == std::filesystem::absolute(ghost_link).string(),
            "dry-run must report the ghost link path");
    require(db.integer("SELECT COUNT(*) FROM ref;") == refs_before,
            "dry-run must not delete ref rows");
    require(store.refcount(live.hash) == live_refcount_before,
            "dry-run must not alter live refcount");
    require(store.refcount(ghost.hash) == ghost_refcount_before,
            "dry-run must not alter ghost refcount");
    require(store.has_object(live.hash) == live_exists_before &&
                store.has_object(ghost.hash) == ghost_exists_before,
            "dry-run must not unlink object content");
}

void testReconcileDryRunMatchesRealReconcile() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto first = put(store, temporary.path(), "first-ghost", "first-ghost");
    const auto second = put(store, temporary.path(), "second-ghost", "second-ghost");
    const auto first_link = liveLink(store, first, temporary.path(), "first.link");
    const auto second_link = liveLink(store, second, temporary.path(), "second.link");
    std::filesystem::remove(first_link);
    std::filesystem::remove(second_link);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport preview = gc.reconcileDryRun();
    const GcReport applied = gc.reconcile();
    require(preview.phantom_refs.size() == 2, "dry-run must list both phantoms");
    require(applied.refs_removed == static_cast<int>(preview.phantom_refs.size()),
            "real reconcile removals must match dry-run phantom count");
}

void testUnscopedReconcileKeepsUnverifiedRef() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "unscoped", "unscoped-content");
    const auto link = liveLink(store, stored, temporary.path(), "unscoped.link");
    std::filesystem::remove(link);
    Database db(temporary.path() / "pool" / "meta.db");
    const auto verified_before = db.text("SELECT last_verified FROM ref;");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reconcile();
    require(report.refs_removed == 0, "reconcile without a scope must not release refs");
    require(db.integer("SELECT COUNT(*) FROM ref;") == 1, "unverified ref row must stay");
    require(store.refcount(stored.hash) == 1, "unverified ref must still be counted");
    require(report.retained_refs.size() == 1 && report.retained_refs.front().reason == "not_found" &&
                report.retained_refs.front().scope == "no_scope",
            "unverified ref must be reported as retained");
    require(db.text("SELECT last_verified FROM ref;") == verified_before,
            "a retained ref must not look freshly verified");
}

void testScopedReconcileKeepsOtherOwnersRef() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "other", "other-owner-content");
    const auto link = liveLink(store, stored, temporary.path(), "other.link");
    std::filesystem::remove(link);
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE ref SET owner = 'another-account';");

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcile();
    require(report.refs_removed == 0, "reconcile must not release another account's ref");
    require(store.refcount(stored.hash) == 1, "another account's ref must stay counted");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().scope == "other_owner",
            "another account's ref must be reported as retained");
}

void testScopedReconcileKeepsRefOutsideWorkspace() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "outside", "outside-content");
    const auto link = temporary.path() / "elsewhere" / "outside.link";
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(store.object_path(stored.hash), link);
    store.register_ref(stored.hash, std::filesystem::absolute(link).string());
    std::filesystem::remove_all(link.parent_path());

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcile();
    require(report.refs_removed == 0, "reconcile must not release refs outside its workspace");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().scope == "outside_workspace",
            "ref outside the workspace must be reported as retained");
}

void testUnreadableLinkIsRetained() {
    if (::geteuid() == 0) return;
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "sealed", "sealed-content");
    const auto link = liveLink(store, stored, temporary.path(), "sealed/dir/sealed.link");
    const auto sealed = temporary.path() / "workspace" / "sealed";
    std::filesystem::permissions(sealed, std::filesystem::perms::none);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reconcile();
    std::filesystem::permissions(sealed, std::filesystem::perms::owner_all);
    require(report.refs_removed == 0, "a link that cannot be inspected must not be released");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().reason == "permission_denied",
            "an uninspectable link must be reported as permission_denied");
    require(std::filesystem::is_symlink(link), "the link itself must be untouched");
}

void testScopeRequiresWorkspaceMarker() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "unmarked", "unmarked-content");
    const auto link = liveLink(store, stored, temporary.path(), "unmarked.link");
    std::filesystem::remove(link);

    GarbageCollector gc(temporary.path() / "pool",
                        ReconcileScope{isvn::common::system_user(), temporary.path() / "workspace"});
    const GcReport report = gc.reconcile();
    require(report.refs_removed == 0,
            "a scope whose root is not an iSVN workspace must not release refs");
}

void testReclaimReleasedOnlyTouchesReleasedObjects() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto earlier = put(store, temporary.path(), "earlier", "already-unreferenced");
    const auto released = put(store, temporary.path(), "released", "released-content");
    const auto link = liveLink(store, released, temporary.path(), "released.link");
    store.unregister_ref(std::filesystem::absolute(link).string());
    std::filesystem::remove(link);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reclaimReleased({released.hash});
    require(report.objects_reclaimed == 1, "the released object must be reclaimed");
    require(!store.has_object(released.hash), "released object content must be removed");
    require(store.has_object(earlier.hash),
            "an object that was already unreferenced must be left for explicit maintenance");
}

void testReclaimReleasedReportsFailureAndContinues() {
    if (::geteuid() == 0) return;
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto blocked = put(store, temporary.path(), "blocked", "blocked-content");
    std::string other_text = "free-content";
    Stored freed = put(store, temporary.path(), "freed", other_text);
    for (int attempt = 0; freed.hash.substr(0, 2) == blocked.hash.substr(0, 2); ++attempt) {
        other_text = "free-content-" + std::to_string(attempt);
        freed = put(store, temporary.path(), "freed", other_text);
    }
    const auto bucket = store.object_path(blocked.hash).parent_path();
    std::filesystem::permissions(bucket, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);

    GarbageCollector gc(temporary.path() / "pool", ownWorkspaceScope(temporary.path()));
    const GcReport report = gc.reclaimReleased({blocked.hash, freed.hash});
    std::filesystem::permissions(bucket, std::filesystem::perms::owner_all);
    require(report.reclaim_failures.size() == 1 && report.reclaim_failures.front().hash == blocked.hash,
            "an object that cannot be removed must be reported");
    require(report.objects_reclaimed == 1 && !store.has_object(freed.hash),
            "one failure must not stop the other objects");
    Database db(temporary.path() / "pool" / "meta.db");
    require(db.integer("SELECT COUNT(*) FROM object WHERE hash = '" + blocked.hash + "';") == 1,
            "a failed removal must keep the object row");
}

void testOrphanObjectReclaimedByRefcountOnly() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto orphan = put(store, temporary.path(), "orphan", "orphan-content");
    require(store.refcount(orphan.hash) == 0, "orphan starts with refcount zero");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    Database db(temporary.path() / "pool" / "meta.db");
    require(report.objects_reclaimed == 1, "reclaim must delete refcount-zero orphan");
    require(report.bytes_freed > 0, "reclaim must report bytes freed");
    require(db.integer("SELECT COUNT(*) FROM object;") == 0,
            "orphan metadata must be removed");
    require(!std::filesystem::exists(store.object_path(orphan.hash)),
            "orphan object file must be removed");
}

void testLiveRefNotReclaimed() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "live", "live-content");
    liveLink(store, stored, temporary.path(), "live.link");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(1.0, 1.0);
    require(report.objects_reclaimed == 0, "refcount-positive object must not be reclaimed");
    require(report.objects_force_evicted == 0,
            "refcount-positive object must not be evicted without pressure");
    require(store.has_object(stored.hash), "live object must remain");
    require(store.refcount(stored.hash) == 1, "live refcount must remain one");
}

void testSpaceSufficientDoesNotReclaim() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "cache", "cache-content");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(1.0, 1.0);
    require(report.objects_reclaimed == 0, "high watermark 1.0 must not reclaim");
    require(store.has_object(stored.hash), "cache object must remain when space is sufficient");
}

void testLruDeletesOldestFirstWhenForced() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto old_object = put(store, temporary.path(), "old", "old-content");
    const auto new_object = put(store, temporary.path(), "new", "new-content");
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               old_object.hash + "';");
    db.execute("UPDATE object SET last_access='2099-01-01T00:00:00Z' WHERE hash='" +
               new_object.hash + "';");
    db.execute("CREATE TRIGGER reject_new_before_old BEFORE DELETE ON object "
               "WHEN OLD.hash='" +
               new_object.hash + "' AND EXISTS(SELECT 1 FROM object WHERE hash='" +
               old_object.hash + "') BEGIN SELECT RAISE(ABORT, 'new deleted before old'); END;");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    require(report.objects_reclaimed == 2, "forced reclaim must remove both orphans");
    require(!std::filesystem::exists(store.object_path(old_object.hash)),
            "old object must be removed");
    require(!std::filesystem::exists(store.object_path(new_object.hash)),
            "new object must be removed");
}

void testDeadCornerForceEvictsLiveRefAndKeepsRef() {
    ScopedEnv atime("ISVN_GC_ATIME_USABLE", "false");
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "live", "live-force-content");
    liveLink(store, stored, temporary.path(), "live.link");
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               stored.hash + "';");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    require(report.objects_reclaimed == 0, "B-1 must not reclaim refcount-positive object");
    require(report.objects_force_evicted == 1, "B-2 must force-evict dead-corner object");
    require(!std::filesystem::exists(store.object_path(stored.hash)),
            "B-2 must unlink object content");
    require(db.integer("SELECT COUNT(*) FROM ref WHERE hash='" + stored.hash + "';") == 1,
            "B-2 must preserve ref rows for swap-in");
    require(db.integer("SELECT refcount FROM object WHERE hash='" + stored.hash + "';") == 1,
            "B-2 must preserve object metadata and refcount");
    require(db.integer("SELECT COUNT(*) FROM evict_event WHERE hash='" + stored.hash + "';") == 1,
            "B-2 must record a hash-only eviction event");
}

void testForceEvictUsesLastAccessWhenAtimeDisabled() {
    ScopedEnv atime("ISVN_GC_ATIME_USABLE", "false");
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto old_object = put(store, temporary.path(), "old-live", "old-live-content");
    const auto new_object = put(store, temporary.path(), "new-live", "new-live-content");
    liveLink(store, old_object, temporary.path(), "old.link");
    liveLink(store, new_object, temporary.path(), "new.link");
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               old_object.hash + "';");
    db.execute("UPDATE object SET last_access='2099-01-01T00:00:00Z' WHERE hash='" +
               new_object.hash + "';");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    require(report.objects_force_evicted == 2,
            "forced reclaim should evict all live candidates under forced low watermark");
    require(!std::filesystem::exists(store.object_path(old_object.hash)),
            "old last_access candidate must be evicted");
    require(db.text("SELECT hash FROM evict_event ORDER BY event_id LIMIT 1;") ==
                old_object.hash,
            "last_access-disabled B-2 must evict oldest last_access first");
}

void testForceEvictUsesAtimeWhenEnabled() {
    ScopedEnv atime("ISVN_GC_ATIME_USABLE", "true");
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto old_atime = put(store, temporary.path(), "old-atime", "old-atime-content");
    const auto new_atime = put(store, temporary.path(), "new-atime", "new-atime-content");
    liveLink(store, old_atime, temporary.path(), "old-atime.link");
    liveLink(store, new_atime, temporary.path(), "new-atime.link");
    const timespec old_times[2]{{1000, 0}, {2000, 0}};
    const timespec new_times[2]{{3000, 0}, {2000, 0}};
    require(::utimensat(AT_FDCWD, store.object_path(old_atime.hash).c_str(), old_times, 0) == 0,
            "test must set old atime");
    require(::utimensat(AT_FDCWD, store.object_path(new_atime.hash).c_str(), new_times, 0) == 0,
            "test must set new atime");
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE object SET last_access='2099-01-01T00:00:00Z' WHERE hash='" +
               old_atime.hash + "';");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               new_atime.hash + "';");

    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    require(report.objects_force_evicted == 2,
            "forced reclaim should evict live candidates under forced low watermark");
    require(!std::filesystem::exists(store.object_path(old_atime.hash)),
            "old atime candidate must be evicted first");
    require(db.text("SELECT hash FROM evict_event ORDER BY event_id LIMIT 1;") ==
                old_atime.hash,
            "atime-enabled B-2 must evict oldest atime first");
}

void testSwapInAfterForceEvict() {
    ScopedEnv atime("ISVN_GC_ATIME_USABLE", "false");
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "swap", "swap-content");
    const auto link = liveLink(store, stored, temporary.path(), "swap.link");
    GarbageCollector gc(temporary.path() / "pool");
    const GcReport report = gc.reclaim(0.0, 0.0);
    require(report.objects_force_evicted == 1, "test must force evict the live object");
    require(store.is_dangling_link(std::filesystem::absolute(link).string()),
            "force-evicted object must leave a dangling symlink");
    const auto source = temporary.path() / "swap.src";
    const auto result = store.put_object(source);
    require(result.hash == stored.hash, "swap-in put must restore the same hash");
    require(store.has_object(stored.hash), "swap-in must recreate object content");
    require(store.refcount(stored.hash) == 1, "swap-in must keep existing refcount");
}

void testEvictEventSchemaHasNoVersionSemantics() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    GarbageCollector gc(temporary.path() / "pool");
    static_cast<void>(gc.reconcile());
    Database db(temporary.path() / "pool" / "meta.db");
    const auto columns = db.columnNames("evict_event");
    const std::vector<std::string> expected{"event_id", "hash", "evicted_at"};
    require(columns == expected, "evict_event must contain only event_id/hash/evicted_at");
}

void testGuardrailsNoVersionSemantics() {
    const std::string header =
        readFile(std::filesystem::path(ISVN_SOURCE_DIR) / "src/storage/gc.h");
    const std::string source =
        readFile(std::filesystem::path(ISVN_SOURCE_DIR) / "src/storage/gc.cpp");
    const std::string combined = header + "\n" + source;
    for (const std::string forbidden :
         {"version", "branch", "stage", "ancestor", "parentOf", "Lineage"}) {
        require(combined.find(forbidden) == std::string::npos,
                "GC must not contain version or lineage semantics");
    }
    require(header.find("struct PhantomRef") != std::string::npos,
            "GC must expose PhantomRef for dry-run previews");
    require(header.find("link_path") != std::string::npos &&
                header.find("hash") != std::string::npos,
            "PhantomRef must contain only storage ref facts");
    require(source.find("reconcileDryRun") != std::string::npos,
            "GC must implement dry-run reconcile");
    require(source.find("DELETE FROM ref") != std::string::npos,
            "real reconcile must still remove refs");
}


// A working copy reached through a directory symlink whose target is not
// available here (host-local storage that is not mounted) proves nothing about
// its links, so they must be kept and their objects must survive.
void testRefBehindUnavailableDirectorySymlinkIsRetained() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "local", "host-local-content");
    const auto scope = ownWorkspaceScope(temporary.path());
    const auto local = temporary.path() / "local-disk" / "wc";
    std::filesystem::create_directories(local);
    std::filesystem::create_symlink(local, temporary.path() / "workspace" / "wc");
    const auto link = temporary.path() / "workspace" / "wc" / "f.link";
    std::filesystem::create_symlink(store.object_path(stored.hash), link);
    store.register_ref(stored.hash, std::filesystem::absolute(link).string());
    std::filesystem::rename(temporary.path() / "local-disk",
                            temporary.path() / "local-disk.away");

    GarbageCollector gc(temporary.path() / "pool", scope);
    const GcReport report = gc.reclaimReleased({});
    std::filesystem::rename(temporary.path() / "local-disk.away",
                            temporary.path() / "local-disk");
    require(report.refs_removed == 0,
            "a link behind an unavailable directory symlink must not be released");
    require(report.objects_reclaimed == 0 && store.has_object(stored.hash),
            "the object behind an unavailable directory symlink must stay");
    require(store.refcount(stored.hash) == 1, "the unverified ref must stay counted");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().reason == "unverified_path" &&
                report.retained_refs.front().scope == "in_scope",
            "the ref must be reported as an unverified path");
    require(std::filesystem::exists(link), "the link must resolve once its storage is back");
}

// Conservative path checks must not disable ordinary cleanup: a removed link and
// a removed working-copy directory inside the caller's workspace are released
// and their objects reclaimed.
void testRemovedOwnLinksAreStillReleasedAndReclaimed() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto single = put(store, temporary.path(), "single", "single-removed-link");
    const auto nested = put(store, temporary.path(), "nested", "nested-removed-directory");
    const auto scope = ownWorkspaceScope(temporary.path());
    const auto single_link = liveLink(store, single, temporary.path(), "wc1/a.link");
    liveLink(store, nested, temporary.path(), "wc2/deep/b.link");
    std::filesystem::remove(single_link);
    std::filesystem::remove_all(temporary.path() / "workspace" / "wc2");

    GarbageCollector gc(temporary.path() / "pool", scope);
    const GcReport report = gc.reclaimReleased({});
    require(report.refs_removed == 2, "removed own links must still be released");
    require(report.objects_reclaimed == 2 && !store.has_object(single.hash) &&
                !store.has_object(nested.hash),
            "objects of removed own links must still be reclaimed");
}

void testRetainedRefOfAnotherAccountKeepsReleasedObject() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto shared = put(store, temporary.path(), "shared", "shared-between-accounts");
    const auto scope = ownWorkspaceScope(temporary.path());
    const auto mine = liveLink(store, shared, temporary.path(), "mine.link");
    const auto theirs = temporary.path() / "other-member" / "theirs.link";
    std::filesystem::create_directories(theirs.parent_path());
    std::filesystem::create_symlink(store.object_path(shared.hash), theirs);
    const auto theirs_path = std::filesystem::absolute(theirs).string();
    store.register_ref(shared.hash, theirs_path);
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE ref SET owner = 'another-account' WHERE link_path = '" + theirs_path +
               "';");
    std::filesystem::remove(mine);
    std::filesystem::remove_all(theirs.parent_path());

    GarbageCollector gc(temporary.path() / "pool", scope);
    const GcReport report = gc.reclaimReleased({shared.hash});
    require(report.refs_removed == 1, "the caller's own removed link must be released");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().scope == "other_owner",
            "another account's ref must be kept");
    require(report.objects_reclaimed == 0 && store.has_object(shared.hash),
            "an object another account's kept ref still counts must not be reclaimed");
    require(store.refcount(shared.hash) == 1, "the kept ref must stay counted");
}

void testSiblingWorkspaceWithSharedPrefixIsOutsideScope() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto stored = put(store, temporary.path(), "sibling", "sibling-workspace-content");
    const auto scope = ownWorkspaceScope(temporary.path());
    const auto link = temporary.path() / "workspace2" / "f.link";
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(store.object_path(stored.hash), link);
    store.register_ref(stored.hash, std::filesystem::absolute(link).string());
    std::filesystem::remove(link);

    GarbageCollector gc(temporary.path() / "pool", scope);
    const GcReport report = gc.reclaimReleased({});
    require(report.refs_removed == 0 && store.has_object(stored.hash),
            "a sibling directory sharing the workspace name prefix is not the workspace");
    require(report.retained_refs.size() == 1 &&
                report.retained_refs.front().scope == "outside_workspace",
            "a ref in the sibling directory must be reported as outside the workspace");
}

void testOrphanReclaimContinuesPastUndeletableObject() {
    if (::geteuid() == 0) return;
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const auto blocked = put(store, temporary.path(), "blocked", "blocked-orphan");
    // Exactly two orphans, in different buckets.
    std::string other_text = "free-orphan";
    for (int attempt = 0;
         isvn::common::blake3_hex(other_text).substr(0, 2) == blocked.hash.substr(0, 2);
         ++attempt) {
        other_text = "free-orphan-" + std::to_string(attempt);
    }
    const auto freed = put(store, temporary.path(), "freed", other_text);
    require(freed.hash.substr(0, 2) != blocked.hash.substr(0, 2),
            "test orphans must be in different buckets");
    Database db(temporary.path() / "pool" / "meta.db");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               blocked.hash + "';");
    db.execute("UPDATE object SET last_access='2099-01-01T00:00:00Z' WHERE hash='" +
               freed.hash + "';");
    const auto bucket = store.object_path(blocked.hash).parent_path();
    std::filesystem::permissions(bucket, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_exec);
    GcReport report;
    try {
        GarbageCollector gc(temporary.path() / "pool");
        report = gc.reclaimOrphans();
    } catch (...) {
        std::filesystem::permissions(bucket, std::filesystem::perms::owner_all);
        throw;
    }
    std::filesystem::permissions(bucket, std::filesystem::perms::owner_all);
    require(report.reclaim_failures.size() == 1 &&
                report.reclaim_failures.front().hash == blocked.hash,
            "the orphan that cannot be removed must be reported");
    require(report.objects_reclaimed == 1 && !store.has_object(freed.hash),
            "a later reclaimable orphan must still be processed");
    require(store.has_object(blocked.hash) &&
                db.integer("SELECT COUNT(*) FROM object WHERE hash='" + blocked.hash + "';") == 1,
            "the orphan that could not be removed must keep its file and row");
}

}  // namespace

int main() {
    try {
        testGhostRefRemoved();
        testChangedSymlinkRemoved();
        testReconcileDryRunIsReadOnly();
        testReconcileDryRunMatchesRealReconcile();
        testUnscopedReconcileKeepsUnverifiedRef();
        testScopedReconcileKeepsOtherOwnersRef();
        testScopedReconcileKeepsRefOutsideWorkspace();
        testUnreadableLinkIsRetained();
        testScopeRequiresWorkspaceMarker();
        testReclaimReleasedOnlyTouchesReleasedObjects();
        testReclaimReleasedReportsFailureAndContinues();
        testRefBehindUnavailableDirectorySymlinkIsRetained();
        testRemovedOwnLinksAreStillReleasedAndReclaimed();
        testRetainedRefOfAnotherAccountKeepsReleasedObject();
        testSiblingWorkspaceWithSharedPrefixIsOutsideScope();
        testOrphanReclaimContinuesPastUndeletableObject();
        testOrphanObjectReclaimedByRefcountOnly();
        testLiveRefNotReclaimed();
        testSpaceSufficientDoesNotReclaim();
        testLruDeletesOldestFirstWhenForced();
        testDeadCornerForceEvictsLiveRefAndKeepsRef();
        testForceEvictUsesLastAccessWhenAtimeDisabled();
        testForceEvictUsesAtimeWhenEnabled();
        testSwapInAfterForceEvict();
        testEvictEventSchemaHasNoVersionSemantics();
        testGuardrailsNoVersionSemantics();
    } catch (const std::exception& error) {
        std::cerr << "test_gc failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
