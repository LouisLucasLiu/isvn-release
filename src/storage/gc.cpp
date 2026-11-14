#include "storage/gc.h"

#include "common/fs.h"
#include "common/system.h"

#include <sqlite3.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace isvn::storage {
namespace {

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {
        char* error = nullptr;
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("Unable to begin SQLite transaction: " + message);
        }
    }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    void commit() {
        char* error = nullptr;
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("Unable to commit SQLite transaction: " + message);
        }
        committed_ = true;
    }

private:
    sqlite3* db_;
    bool committed_ = false;
};

void secureOpenFile(int descriptor, const struct stat& status, gid_t group,
                    mode_t mode, const std::string& label) {
    if (status.st_gid != group &&
        ::fchown(descriptor, static_cast<uid_t>(-1), group) != 0) {
        throw std::runtime_error("Unable to secure " + label + ": " +
                                 std::string(std::strerror(errno)));
    }
    if ((status.st_mode & 07777) != mode && ::fchmod(descriptor, mode) != 0) {
        throw std::runtime_error("Unable to secure " + label + ": " +
                                 std::string(std::strerror(errno)));
    }
}

class HashLock {
public:
    HashLock(const std::filesystem::path& path, gid_t group) {
        for (int attempt = 0; attempt != 50; ++attempt) {
            descriptor_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0660);
            if (descriptor_ >= 0) break;
            if (errno != EACCES) {
                throw std::runtime_error("Unable to open hash lock: " +
                                         std::string(std::strerror(errno)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (descriptor_ < 0) throw std::runtime_error("Timed out opening hash lock");
        struct stat lock_status {};
        if (::fstat(descriptor_, &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("Hash lock path is not a regular file");
        }
        try {
            secureOpenFile(descriptor_, lock_status, group, 0660, "hash lock");
        } catch (const std::exception& error) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw;
        }
        while (::flock(descriptor_, LOCK_EX) != 0) {
            if (errno != EINTR) {
                const std::string message = std::strerror(errno);
                ::close(descriptor_);
                descriptor_ = -1;
                throw std::runtime_error("Unable to acquire hash lock: " + message);
            }
        }
    }
    ~HashLock() {
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
    }

private:
    int descriptor_ = -1;
};

void execute(sqlite3* db, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error != nullptr ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SQLite execution failed: " + message);
    }
}

void bindText(sqlite3* db, sqlite3_stmt* statement, int index,
              const std::string& value) {
    if (sqlite3_bind_text(statement, index, value.c_str(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(std::string("SQLite bind failed: ") + sqlite3_errmsg(db));
    }
}

void expectDone(sqlite3* db, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(db));
    }
}

void validateHash(const std::string& hash) {
    if (hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::invalid_argument("Invalid BLAKE3 hash: " + hash);
    }
}

// What this process can observe at a ref's link path.  Only kAbsent and
// kReplaced are observations of a link that is no longer there; the others mean
// the link could not be inspected from this account or filesystem view.
// kUnverifiedPath: the path to the link passes through a symlink or a different
// filesystem, so what is seen there says nothing about the recorded link.
enum class LinkState {
    kLive,
    kAbsent,
    kReplaced,
    kPermissionDenied,
    kUnreadable,
    kUnverifiedPath
};

class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() {
        if (fd_ >= 0) ::close(fd_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }

private:
    int fd_ = -1;
};

// An open scope workspace root and the filesystem it lives on.
struct AnchoredRoot {
    Descriptor directory;
    dev_t device{};
};

bool sameLinkTarget(const std::filesystem::path& link_path, const char* target_text,
                    const std::filesystem::path& object_path, LinkState& state) {
    std::filesystem::path target(target_text);
    if (!target.is_absolute()) target = link_path.parent_path() / target;
    std::error_code error;
    const auto canonical_target = std::filesystem::weakly_canonical(target, error);
    if (error) {
        state = LinkState::kUnreadable;
        return false;
    }
    const auto canonical_object = std::filesystem::weakly_canonical(object_path, error);
    if (error) {
        state = LinkState::kUnreadable;
        return false;
    }
    state = canonical_target == canonical_object ? LinkState::kLive : LinkState::kReplaced;
    return true;
}

LinkState inspectLink(const std::filesystem::path& link_path,
                      const std::filesystem::path& object_path) {
    struct stat status {};
    if (::lstat(link_path.c_str(), &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) return LinkState::kAbsent;
        if (errno == EACCES || errno == EPERM) return LinkState::kPermissionDenied;
        return LinkState::kUnreadable;
    }
    if (!S_ISLNK(status.st_mode)) return LinkState::kReplaced;
    std::vector<char> buffer(4096);
    const ssize_t size = ::readlink(link_path.c_str(), buffer.data(), buffer.size() - 1);
    if (size < 0) return LinkState::kUnreadable;
    buffer[static_cast<std::size_t>(size)] = '\0';
    LinkState state = LinkState::kUnreadable;
    sameLinkTarget(link_path, buffer.data(), object_path, state);
    return state;
}

LinkState stateForErrno(int error) {
    if (error == ENOENT) return LinkState::kAbsent;
    if (error == EACCES || error == EPERM) return LinkState::kPermissionDenied;
    return LinkState::kUnreadable;
}

// Inspects a link inside the scope workspace by walking from the open root one
// path component at a time, never following a symlink and never leaving the
// root's filesystem.  Absence is reported only when a component is missing from
// a real directory reached this way; a directory symlink, a mount point, or a
// component that cannot be opened makes the result kUnverifiedPath (or the
// matching permission/IO state), because the recorded link may still exist
// where that path leads on another host or after a remount.
LinkState inspectLinkWithin(const AnchoredRoot& root, const std::filesystem::path& relative,
                            const std::filesystem::path& link_path,
                            const std::filesystem::path& object_path) {
    std::vector<std::string> parts;
    for (const auto& part : relative) parts.push_back(part.string());
    if (parts.empty()) return LinkState::kUnverifiedPath;
    for (const auto& part : parts) {
        if (part.empty() || part == "." || part == ".." ||
            part.find('/') != std::string::npos) {
            return LinkState::kUnverifiedPath;
        }
    }
    Descriptor current(::fcntl(root.directory.get(), F_DUPFD_CLOEXEC, 0));
    if (current.get() < 0) return LinkState::kUnreadable;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        struct stat status {};
        if (::fstatat(current.get(), parts[index].c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            return stateForErrno(errno);
        }
        if (S_ISLNK(status.st_mode)) return LinkState::kUnverifiedPath;
        // Something other than a directory holds this place, so nothing can be
        // linked below it in this workspace.
        if (!S_ISDIR(status.st_mode)) return LinkState::kReplaced;
        if (status.st_dev != root.device) return LinkState::kUnverifiedPath;
        Descriptor next(::openat(current.get(), parts[index].c_str(),
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (next.get() < 0) {
            const int error = errno;
            if (error == ELOOP || error == ENOTDIR) return LinkState::kUnverifiedPath;
            return stateForErrno(error);
        }
        struct stat opened {};
        if (::fstat(next.get(), &opened) != 0) return LinkState::kUnreadable;
        if (!S_ISDIR(opened.st_mode) || opened.st_dev != root.device) {
            return LinkState::kUnverifiedPath;
        }
        current = std::move(next);
    }
    const std::string& name = parts.back();
    struct stat status {};
    if (::fstatat(current.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
        return stateForErrno(errno);
    }
    if (!S_ISLNK(status.st_mode)) return LinkState::kReplaced;
    std::vector<char> buffer(4096);
    const ssize_t size =
        ::readlinkat(current.get(), name.c_str(), buffer.data(), buffer.size() - 1);
    if (size < 0) return LinkState::kUnreadable;
    buffer[static_cast<std::size_t>(size)] = '\0';
    LinkState state = LinkState::kUnreadable;
    sameLinkTarget(link_path, buffer.data(), object_path, state);
    return state;
}

const char* linkStateName(LinkState state) {
    switch (state) {
        case LinkState::kLive: return "live";
        case LinkState::kAbsent: return "not_found";
        case LinkState::kReplaced: return "replaced";
        case LinkState::kPermissionDenied: return "permission_denied";
        case LinkState::kUnreadable: return "io_error";
        case LinkState::kUnverifiedPath: return "unverified_path";
    }
    return "io_error";
}

std::filesystem::path normalizedDirectory(const std::filesystem::path& path) {
    auto normal = std::filesystem::absolute(path).lexically_normal();
    if (!normal.has_filename() && normal.has_parent_path()) normal = normal.parent_path();
    return normal;
}

bool isInside(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const auto inner = std::filesystem::path(path).lexically_normal();
    const auto [dir_it, path_it] =
        std::mismatch(directory.begin(), directory.end(), inner.begin(), inner.end());
    return dir_it == directory.end() && path_it != inner.end();
}

// The scope root must be an iSVN workspace owned by the calling user, seen as a
// real directory in this filesystem view.  It is opened once; every release
// decision below is made relative to this descriptor.
std::optional<AnchoredRoot> openOwnWorkspaceRoot(const std::filesystem::path& workspace) {
    Descriptor directory(
        ::open(workspace.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) return std::nullopt;
    struct stat root {};
    struct stat local {};
    if (::fstat(directory.get(), &root) != 0 || !S_ISDIR(root.st_mode) ||
        root.st_uid != ::geteuid()) {
        return std::nullopt;
    }
    if (::fstatat(directory.get(), ".isvn_local", &local, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(local.st_mode) || local.st_uid != ::geteuid()) {
        return std::nullopt;
    }
    AnchoredRoot anchored;
    anchored.directory = std::move(directory);
    anchored.device = root.st_dev;
    return anchored;
}

double filesystemUseRatio(const std::filesystem::path& path) {
    const auto info = std::filesystem::space(path);
    if (info.capacity == 0) return 0.0;
    return static_cast<double>(info.capacity - info.available) /
           static_cast<double>(info.capacity);
}

void validateWatermarks(double high, double low) {
    if (high < 0.0 || high > 1.0 || low < 0.0 || low > 1.0 || low > high) {
        throw std::invalid_argument("GC watermarks must satisfy 0 <= low <= high <= 1");
    }
}

struct RefRow {
    std::int64_t ref_id;
    std::string hash;
    std::string link_path;
    std::string owner;
};

enum class RefDecision { kKeep, kRelease, kRetain };

struct RefVerdict {
    RefDecision decision;
    RetainedRef retained;
};

struct ObjectRow {
    std::string hash;
    std::int64_t size;
};

struct ForceRow {
    std::string hash;
    std::int64_t size;
    std::string last_access;
    timespec atime{};
};

bool timespecLess(const timespec& lhs, const timespec& rhs) {
    return lhs.tv_sec < rhs.tv_sec ||
           (lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec < rhs.tv_nsec);
}

bool envFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return fallback;
    return std::string(value) == "1" || std::string(value) == "true";
}

}  // namespace

class GarbageCollector::Impl {
public:
    Impl(const std::filesystem::path& pool_root, std::optional<ReconcileScope> scope)
        : root_(std::filesystem::absolute(pool_root)),
          objects_(root_ / "objects"),
          locks_(root_ / "locks"),
          database_(root_ / "meta.db") {
        if (scope && !scope->owner.empty() && !scope->workspace.empty()) {
            scope_owner_ = scope->owner;
            scope_workspace_ = normalizedDirectory(scope->workspace);
        }
        group_ = common::path_group(root_);
        common::ensure_directory(locks_, 02770, group_);
        const int rc = sqlite3_open_v2(database_.c_str(), &db_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Unable to open object metadata database: " + message);
        }
        if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
            throw std::runtime_error("Unable to set SQLite busy timeout");
        }
        execute(db_, "PRAGMA foreign_keys=ON;");
        execute(db_, "PRAGMA journal_mode=WAL;");
        execute(db_, R"sql(
            CREATE TABLE IF NOT EXISTS evict_event (
                event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                hash TEXT NOT NULL,
                evicted_at TEXT NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_evict_time ON evict_event(evicted_at);
        )sql");
        atime_usable_ = envFlag("ISVN_GC_ATIME_USABLE", detectAtimeUsable());
        secureDatabaseFiles();
    }

    ~Impl() { sqlite3_close(db_); }

    GcReport reconcile() { return reconcileReleasing(nullptr); }

    // Verifies every ref and releases only those the scope proves gone.  Hashes
    // whose refs were released are appended to `released` when it is given.
    GcReport reconcileReleasing(std::vector<std::string>* released) {
        GcReport report;
        const auto scope = openScope();
        const AnchoredRoot* scoped = scope ? &*scope : nullptr;
        for (const auto& ref : refsSnapshot()) {
            validateHash(ref.hash);
            HashLock lock(lockPath(ref.hash), group_);
            Transaction transaction(db_);
            const auto current = currentRef(ref.ref_id);
            if (!current) {
                transaction.commit();
                continue;
            }
            const RefVerdict verdict = judge(*current, scoped);
            if (verdict.decision == RefDecision::kKeep) {
                Statement touch(db_, "UPDATE ref SET last_verified = ? WHERE ref_id = ?;");
                bindText(db_, touch.get(), 1, common::utc_now());
                sqlite3_bind_int64(touch.get(), 2, ref.ref_id);
                expectDone(db_, touch.get());
            } else if (verdict.decision == RefDecision::kRelease) {
                Statement remove(db_, "DELETE FROM ref WHERE ref_id = ?;");
                sqlite3_bind_int64(remove.get(), 1, ref.ref_id);
                expectDone(db_, remove.get());
                ++report.refs_removed;
                if (released != nullptr) released->push_back(current->hash);
            } else {
                // Not verified: leave the row and its last_verified time untouched
                // so it is not mistaken for a freshly confirmed reference.
                report.retained_refs.push_back(verdict.retained);
            }
            recalculateRefcount(current->hash);
            secureDatabaseFiles();
            transaction.commit();
        }
        return report;
    }

    GcReport reconcileDryRun() {
        GcReport report;
        const auto scope = openScope();
        const AnchoredRoot* scoped = scope ? &*scope : nullptr;
        for (const auto& ref : refsSnapshot()) {
            validateHash(ref.hash);
            HashLock lock(lockPath(ref.hash), group_);
            const auto current = currentRef(ref.ref_id);
            if (!current) continue;
            const RefVerdict verdict = judge(*current, scoped);
            if (verdict.decision == RefDecision::kRelease) {
                report.phantom_refs.push_back(PhantomRef{current->hash, current->link_path});
            } else if (verdict.decision == RefDecision::kRetain) {
                report.retained_refs.push_back(verdict.retained);
            }
        }
        return report;
    }

    GcReport reclaimReleased(const std::vector<std::string>& released_hashes) {
        std::vector<std::string> released_here;
        GcReport report = reconcileReleasing(&released_here);
        std::set<std::string> candidates(released_hashes.begin(), released_hashes.end());
        candidates.insert(released_here.begin(), released_here.end());
        for (const auto& hash : candidates) {
            try {
                validateHash(hash);
                HashLock lock(lockPath(hash), group_);
                Transaction transaction(db_);
                Statement query(db_, "SELECT size, refcount FROM object WHERE hash = ?;");
                bindText(db_, query.get(), 1, hash);
                const int rc = sqlite3_step(query.get());
                if (rc == SQLITE_DONE) {
                    transaction.commit();
                    continue;
                }
                if (rc != SQLITE_ROW) {
                    throw std::runtime_error(std::string("SQLite read failed: ") +
                                             sqlite3_errmsg(db_));
                }
                const std::int64_t size = sqlite3_column_int64(query.get(), 0);
                const std::int64_t refcount = sqlite3_column_int64(query.get(), 1);
                if (refcount != 0) {
                    transaction.commit();
                    continue;
                }
                const auto path = objectPath(hash);
                Statement remove_object(db_, "DELETE FROM object WHERE hash = ?;");
                bindText(db_, remove_object.get(), 1, hash);
                expectDone(db_, remove_object.get());
                if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
                    // Typically the sticky objects directory: only the account that
                    // stored the object (or owns its bucket) may remove it.  Roll back
                    // so the row stays.
                    report.reclaim_failures.push_back(
                        ReclaimFailure{hash, std::string("unable to remove object: ") +
                                                 std::strerror(errno)});
                    continue;
                }
                try {
                    common::fsync_directory(path.parent_path());
                } catch (...) {
                }
                ++report.objects_reclaimed;
                report.bytes_freed += size;
                secureDatabaseFiles();
                transaction.commit();
            } catch (const std::exception& error) {
                // A busy lock or database error on one object rolls back only
                // that object; the others are still processed.
                report.reclaim_failures.push_back(ReclaimFailure{hash, error.what()});
            }
        }
        report.low_watermark_reached = true;
        return report;
    }

    GcReport reclaim(double high, double low) {
        validateWatermarks(high, low);
        GcReport report = reconcile();
        if (filesystemUseRatio(root_) <= high) {
            report.low_watermark_reached = filesystemUseRatio(root_) <= low;
            return report;
        }

        for (const auto& object : reclaimCandidates()) {
            HashLock lock(lockPath(object.hash), group_);
            Transaction transaction(db_);
            Statement query(db_, "SELECT size, refcount FROM object WHERE hash = ?;");
            bindText(db_, query.get(), 1, object.hash);
            const int rc = sqlite3_step(query.get());
            if (rc == SQLITE_DONE) {
                transaction.commit();
                continue;
            }
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(std::string("SQLite read failed: ") +
                                         sqlite3_errmsg(db_));
            }
            const std::int64_t size = sqlite3_column_int64(query.get(), 0);
            const std::int64_t refcount = sqlite3_column_int64(query.get(), 1);
            if (refcount == 0) {
                const auto path = objectPath(object.hash);
                Statement remove_refs(db_, "DELETE FROM ref WHERE hash = ?;");
                bindText(db_, remove_refs.get(), 1, object.hash);
                expectDone(db_, remove_refs.get());
                Statement remove_object(db_, "DELETE FROM object WHERE hash = ?;");
                bindText(db_, remove_object.get(), 1, object.hash);
                expectDone(db_, remove_object.get());
                if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
                    throw std::runtime_error("Unable to unlink object " + object.hash +
                                             ": " + std::strerror(errno));
                }
                try {
                    common::fsync_directory(path.parent_path());
                } catch (...) {
                }
                ++report.objects_reclaimed;
                report.bytes_freed += size;
            }
            secureDatabaseFiles();
            transaction.commit();
            if (filesystemUseRatio(root_) <= low) break;
        }
        while (filesystemUseRatio(root_) > low) {
            const auto victim = forceCandidate();
            if (!victim) break;
            HashLock lock(lockPath(victim->hash), group_);
            Transaction transaction(db_);
            const auto current = forceCandidate();
            if (!current || current->hash != victim->hash) {
                transaction.commit();
                continue;
            }
            Statement query(db_, "SELECT size, refcount FROM object WHERE hash = ?;");
            bindText(db_, query.get(), 1, victim->hash);
            const int rc = sqlite3_step(query.get());
            if (rc == SQLITE_DONE) {
                transaction.commit();
                continue;
            }
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(std::string("SQLite read failed: ") +
                                         sqlite3_errmsg(db_));
            }
            const std::int64_t size = sqlite3_column_int64(query.get(), 0);
            const std::int64_t refcount = sqlite3_column_int64(query.get(), 1);
            if (refcount <= 0) {
                transaction.commit();
                continue;
            }
            const auto path = objectPath(victim->hash);
            if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
                throw std::runtime_error("Unable to unlink object " + victim->hash +
                                         ": " + std::strerror(errno));
            }
            try {
                common::fsync_directory(path.parent_path());
            } catch (...) {
            }
            appendEvictEventBestEffort(victim->hash);
            ++report.objects_force_evicted;
            report.bytes_freed += size;
            secureDatabaseFiles();
            transaction.commit();
        }
        report.low_watermark_reached = filesystemUseRatio(root_) <= low;
        return report;
    }

    GcReport run(double high, double low) {
        validateWatermarks(high, low);
        GcReport report = reconcile();
        if (filesystemUseRatio(root_) > high) {
            const GcReport reclaimed = reclaim(high, low);
            report.refs_removed += reclaimed.refs_removed;
            report.objects_reclaimed += reclaimed.objects_reclaimed;
            report.objects_force_evicted += reclaimed.objects_force_evicted;
            report.bytes_freed += reclaimed.bytes_freed;
            report.low_watermark_reached = reclaimed.low_watermark_reached;
        } else {
            report.low_watermark_reached = filesystemUseRatio(root_) <= low;
        }
        return report;
    }

    // Reconcile, then delete only objects whose refcount has dropped to 0. Unlike
    // reclaim(), this is not gated on a fill watermark and never force-evicts a
    // referenced (refcount > 0) object, so it is safe to run after a logical delete
    // to immediately free space that nothing else points at.  An object that
    // cannot be removed (another account's object in the sticky objects
    // directory, a busy lock) is rolled back, recorded in reclaim_failures, and
    // does not stop the remaining candidates.
    GcReport reclaimOrphans() {
        GcReport report = reconcile();
        for (const auto& object : reclaimCandidates()) {
            try {
                HashLock lock(lockPath(object.hash), group_);
                Transaction transaction(db_);
                Statement query(db_, "SELECT size, refcount FROM object WHERE hash = ?;");
                bindText(db_, query.get(), 1, object.hash);
                const int rc = sqlite3_step(query.get());
                if (rc == SQLITE_DONE) {
                    transaction.commit();
                    continue;
                }
                if (rc != SQLITE_ROW) {
                    throw std::runtime_error(std::string("SQLite read failed: ") +
                                             sqlite3_errmsg(db_));
                }
                const std::int64_t size = sqlite3_column_int64(query.get(), 0);
                const std::int64_t refcount = sqlite3_column_int64(query.get(), 1);
                if (refcount == 0) {
                    const auto path = objectPath(object.hash);
                    Statement remove_refs(db_, "DELETE FROM ref WHERE hash = ?;");
                    bindText(db_, remove_refs.get(), 1, object.hash);
                    expectDone(db_, remove_refs.get());
                    Statement remove_object(db_, "DELETE FROM object WHERE hash = ?;");
                    bindText(db_, remove_object.get(), 1, object.hash);
                    expectDone(db_, remove_object.get());
                    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
                        report.reclaim_failures.push_back(ReclaimFailure{
                            object.hash,
                            std::string("unable to remove object: ") + std::strerror(errno)});
                        continue;
                    }
                    try {
                        common::fsync_directory(path.parent_path());
                    } catch (...) {
                    }
                    ++report.objects_reclaimed;
                    report.bytes_freed += size;
                }
                secureDatabaseFiles();
                transaction.commit();
            } catch (const std::exception& error) {
                report.reclaim_failures.push_back(ReclaimFailure{object.hash, error.what()});
            }
        }
        report.low_watermark_reached = true;
        return report;
    }

private:
    std::filesystem::path objectPath(const std::string& hash) const {
        return objects_ / hash.substr(0, 2) / hash;
    }
    std::filesystem::path lockPath(const std::string& hash) const {
        return locks_ / (hash + ".lock");
    }

    std::vector<RefRow> refsSnapshot() const {
        Statement statement(db_,
                            "SELECT ref_id, hash, link_path, owner FROM ref ORDER BY ref_id;");
        std::vector<RefRow> refs;
        while (true) {
            const int rc = sqlite3_step(statement.get());
            if (rc == SQLITE_DONE) return refs;
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(std::string("SQLite read failed: ") +
                                         sqlite3_errmsg(db_));
            }
            refs.push_back(RefRow{
                sqlite3_column_int64(statement.get(), 0),
                reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1)),
                reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2)),
                reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 3))});
        }
    }

    std::optional<RefRow> currentRef(std::int64_t ref_id) const {
        Statement current(db_, "SELECT hash, link_path, owner FROM ref WHERE ref_id = ?;");
        sqlite3_bind_int64(current.get(), 1, ref_id);
        const int rc = sqlite3_step(current.get());
        if (rc == SQLITE_DONE) return std::nullopt;
        if (rc != SQLITE_ROW) {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db_));
        }
        return RefRow{ref_id,
                      reinterpret_cast<const char*>(sqlite3_column_text(current.get(), 0)),
                      reinterpret_cast<const char*>(sqlite3_column_text(current.get(), 1)),
                      reinterpret_cast<const char*>(sqlite3_column_text(current.get(), 2))};
    }

    std::optional<AnchoredRoot> openScope() const {
        if (scope_owner_.empty() || scope_owner_ != common::system_user()) return std::nullopt;
        return openOwnWorkspaceRoot(scope_workspace_);
    }

    // A link that cannot be inspected is not evidence of removal: another
    // member's private directory, or a workspace on another server's local disk,
    // looks the same as a deleted link from here.  Release requires that the ref
    // belongs to the scope owner, lies inside the scope workspace, and that a walk
    // from the open workspace root, which follows no symlink and stays on the
    // root's filesystem, finds the link absent or replaced.
    RefVerdict judge(const RefRow& ref, const AnchoredRoot* scope) const {
        LinkState state = inspectLink(ref.link_path, objectPath(ref.hash));
        if (state == LinkState::kLive) return {RefDecision::kKeep, {}};
        std::string scope_reason;
        if (scope == nullptr) {
            scope_reason = "no_scope";
        } else if (ref.owner != scope_owner_) {
            scope_reason = "other_owner";
        } else if (!isInside(ref.link_path, scope_workspace_)) {
            scope_reason = "outside_workspace";
        } else {
            const auto relative = std::filesystem::path(ref.link_path)
                                      .lexically_normal()
                                      .lexically_relative(scope_workspace_);
            state = inspectLinkWithin(*scope, relative, ref.link_path, objectPath(ref.hash));
            if (state == LinkState::kLive) return {RefDecision::kKeep, {}};
            if (state == LinkState::kAbsent || state == LinkState::kReplaced) {
                return {RefDecision::kRelease, {}};
            }
            scope_reason = "in_scope";
        }
        return {RefDecision::kRetain,
                RetainedRef{ref.hash, ref.link_path, ref.owner, linkStateName(state),
                            scope_reason}};
    }

    std::vector<ObjectRow> reclaimCandidates() const {
        Statement statement(db_, R"sql(
            SELECT hash, size FROM object
            WHERE refcount = 0
            ORDER BY last_access ASC, hash ASC;
        )sql");
        std::vector<ObjectRow> objects;
        while (true) {
            const int rc = sqlite3_step(statement.get());
            if (rc == SQLITE_DONE) return objects;
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(std::string("SQLite read failed: ") +
                                         sqlite3_errmsg(db_));
            }
            objects.push_back(ObjectRow{
                reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)),
                sqlite3_column_int64(statement.get(), 1)});
        }
    }

    std::optional<ForceRow> forceCandidate() const {
        Statement statement(db_, R"sql(
            SELECT hash, size, last_access FROM object
            WHERE refcount > 0
            ORDER BY last_access ASC, hash ASC;
        )sql");
        std::vector<ForceRow> objects;
        while (true) {
            const int rc = sqlite3_step(statement.get());
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(std::string("SQLite read failed: ") +
                                         sqlite3_errmsg(db_));
            }
            ForceRow row;
            row.hash = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
            row.size = sqlite3_column_int64(statement.get(), 1);
            row.last_access =
                reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
            struct stat status {};
            if (::stat(objectPath(row.hash).c_str(), &status) != 0) continue;
            row.atime = status.st_atim;
            objects.push_back(row);
        }
        if (objects.empty()) return std::nullopt;
        if (atime_usable_) {
            return *std::min_element(objects.begin(), objects.end(),
                                     [](const ForceRow& lhs, const ForceRow& rhs) {
                                         if (timespecLess(lhs.atime, rhs.atime)) return true;
                                         if (timespecLess(rhs.atime, lhs.atime)) return false;
                                         return lhs.hash < rhs.hash;
                                     });
        }
        return objects.front();
    }

    void recalculateRefcount(const std::string& hash) {
        Statement count(db_, "SELECT COUNT(*) FROM ref WHERE hash = ?;");
        bindText(db_, count.get(), 1, hash);
        if (sqlite3_step(count.get()) != SQLITE_ROW) {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db_));
        }
        const std::int64_t refcount = sqlite3_column_int64(count.get(), 0);
        Statement update(db_, "UPDATE object SET refcount = ? WHERE hash = ?;");
        sqlite3_bind_int64(update.get(), 1, refcount);
        bindText(db_, update.get(), 2, hash);
        expectDone(db_, update.get());
    }

    void secureDatabaseFiles() const {
        common::ensure_file_mode_group(database_, 0660, group_);
        for (const char* suffix : {"-wal", "-shm"}) {
            const std::filesystem::path auxiliary = database_.string() + suffix;
            std::error_code error;
            if (std::filesystem::exists(auxiliary, error)) {
                common::ensure_file_mode_group(auxiliary, 0660, group_);
            }
        }
    }

    bool detectAtimeUsable() const {
        const auto test_path = root_ / (".isvn-gc-atime." + std::to_string(::getpid()));
        int descriptor = ::open(test_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        if (descriptor < 0) return false;
        const char data = 'x';
        bool usable = false;
        if (::write(descriptor, &data, 1) == 1 && ::fsync(descriptor) == 0) {
            struct stat before {};
            if (::fstat(descriptor, &before) == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                char sink = 0;
                if (::lseek(descriptor, 0, SEEK_SET) >= 0 &&
                    ::read(descriptor, &sink, 1) == 1) {
                    struct stat after {};
                    usable = ::fstat(descriptor, &after) == 0 &&
                             timespecLess(before.st_atim, after.st_atim);
                }
            }
        }
        ::close(descriptor);
        ::unlink(test_path.c_str());
        return usable;
    }

    void appendEvictEventBestEffort(const std::string& hash) {
        try {
            Statement insert(db_, "INSERT INTO evict_event(hash, evicted_at) VALUES(?, ?);");
            bindText(db_, insert.get(), 1, hash);
            bindText(db_, insert.get(), 2, common::utc_now());
            expectDone(db_, insert.get());
        } catch (...) {
        }
    }

    std::filesystem::path root_;
    std::filesystem::path objects_;
    std::filesystem::path locks_;
    std::filesystem::path database_;
    std::string scope_owner_;
    std::filesystem::path scope_workspace_;
    gid_t group_{};
    sqlite3* db_ = nullptr;
    bool atime_usable_ = false;
};

GarbageCollector::GarbageCollector(const std::filesystem::path& pool_root)
    : impl_(std::make_unique<Impl>(pool_root, std::nullopt)) {}
GarbageCollector::GarbageCollector(const std::filesystem::path& pool_root, ReconcileScope scope)
    : impl_(std::make_unique<Impl>(pool_root, std::move(scope))) {}
GarbageCollector::~GarbageCollector() = default;
GarbageCollector::GarbageCollector(GarbageCollector&&) noexcept = default;
GarbageCollector& GarbageCollector::operator=(GarbageCollector&&) noexcept = default;

GcReport GarbageCollector::reconcile() { return impl_->reconcile(); }
GcReport GarbageCollector::reconcileDryRun() { return impl_->reconcileDryRun(); }
GcReport GarbageCollector::reclaim(double high_watermark, double low_watermark) {
    return impl_->reclaim(high_watermark, low_watermark);
}
GcReport GarbageCollector::run(double high_watermark, double low_watermark) {
    return impl_->run(high_watermark, low_watermark);
}
GcReport GarbageCollector::reclaimOrphans() { return impl_->reclaimOrphans(); }
GcReport GarbageCollector::reclaimReleased(const std::vector<std::string>& released_hashes) {
    return impl_->reclaimReleased(released_hashes);
}

}  // namespace isvn::storage
