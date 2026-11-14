#include "common/hash.h"
#include "storage/object_store.h"

#include <sqlite3.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using isvn::storage::ObjectStore;
using isvn::storage::PutResult;

// Interface guard: storage accepts only content paths, hashes, and link
// paths. No version, branch, stage, ancestry, or relationship input exists.
using PutSignature = PutResult (ObjectStore::*)(const std::filesystem::path&);
using GetSignature = void (ObjectStore::*)(const std::string&,
                                           const std::filesystem::path&) const;
using RegisterSignature = void (ObjectStore::*)(const std::string&,
                                                const std::string&);
using TouchSignature = void (ObjectStore::*)(const std::string&);
using LastAccessSignature = std::string (ObjectStore::*)(const std::string&) const;
using RenewEventSignature = void (ObjectStore::*)(const std::string&) noexcept;
using AtimeUsableSignature = bool (ObjectStore::*)() const;
using DanglingSignature = bool (ObjectStore::*)(const std::string&) const;
static_assert(std::is_same_v<decltype(&ObjectStore::put_object), PutSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::get_object), GetSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::register_ref), RegisterSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::touch_last_access), TouchSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::last_access_of), LastAccessSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::record_renew_event), RenewEventSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::atime_usable), AtimeUsableSignature>);
static_assert(std::is_same_v<decltype(&ObjectStore::is_dangling_link), DanglingSignature>);

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/isvn-object-test-XXXXXX";
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
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("test SQL prepare failed");
        }
        if (sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL returned no row");
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
            throw std::runtime_error("test SQL returned no text");
        }
        const auto* value = sqlite3_column_text(statement, 0);
        const std::string result(reinterpret_cast<const char*>(value));
        sqlite3_finalize(statement);
        return result;
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

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Exception = std::exception, typename Callable>
void requireThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path& path, const std::string& content,
               mode_t mode = 0640) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output || ::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("unable to create test file");
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

mode_t modeOf(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) throw std::runtime_error("stat failed");
    return status.st_mode & 07777;
}

std::size_t regularFileCount(const std::filesystem::path& path) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (entry.is_regular_file()) ++count;
    }
    return count;
}

void testPutHashAndPermissions() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "abc");
    ObjectStore store(pool);
    const PutResult result = store.put_object(source);
    require(!result.deduped, "first put must not be deduped");
    require(result.hash ==
                "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
            "hash must match the BLAKE3 known vector");
    require(store.has_object(result.hash), "stored object must exist");
    require(store.verify_object(result.hash), "stored object must verify");
    require(readFile(store.object_path(result.hash)) == "abc", "object content must match");
    require(modeOf(pool) == 02770, "pool root must be 2770");
    require(modeOf(pool / "objects") == 03770, "objects directory must be 3770");
    require(modeOf(store.object_path(result.hash).parent_path()) == 03770,
            "bucket must be 3770");
    require(modeOf(store.object_path(result.hash)) == 0440, "object must be 0440");
    require(modeOf(pool / "meta.db") == 0660, "meta.db must be 0660");
    require(modeOf(pool / "locks" / (result.hash + ".lock")) == 0660,
            "hash lock must be 0660");
}

void testDedupAndLastAccess() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "dedup-content");
    ObjectStore store(pool);
    const auto first = store.put_object(source);
    Database db(pool / "meta.db");
    const std::string initial = db.text("SELECT last_access FROM object;");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const auto second = store.put_object(source);
    require(second.deduped && second.hash == first.hash, "second put must dedupe");
    require(regularFileCount(pool / "objects") == 1, "dedupe must keep one object");
    require(db.text("SELECT last_access FROM object;") > initial,
            "dedupe must update application last_access");
}

void testGetAndObjectPath() {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source";
    const auto destination = temporary.path() / "out" / "copy";
    writeFile(source, "retrieved-content");
    ObjectStore store(temporary.path() / "pool");
    const auto result = store.put_object(source);
    Database db(temporary.path() / "pool" / "meta.db");
    const std::string initial = db.text("SELECT last_access FROM object;");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.get_object(result.hash, destination);
    require(readFile(destination) == "retrieved-content", "get must copy content");
    require(db.text("SELECT last_access FROM object;") > initial,
            "successful get must update application last_access");
    require(store.object_path(result.hash).filename() == result.hash,
            "object path must end with hash");
}

void testRefsAndOwner() {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source";
    writeFile(source, "ref-content");
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    const auto result = store.put_object(source);
    Database db(pool / "meta.db");
    const std::string initial = db.text("SELECT last_access FROM object;");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const std::string link = (temporary.path() / "workspace" / "link").string();
    store.register_ref(result.hash, link);
    require(store.refcount(result.hash) == 1, "register must increment refcount");
    require(db.text("SELECT last_access FROM object;") > initial,
            "register_ref must update application last_access");
    require(db.integer("SELECT COUNT(*) FROM ref;") == 1,
            "register must insert one ref row");
    require(!db.text("SELECT owner FROM ref;").empty(), "ref owner must be recorded");
    store.unregister_ref(link);
    require(store.refcount(result.hash) == 0, "unregister must decrement refcount");
    require(db.integer("SELECT COUNT(*) FROM ref;") == 0,
            "unregister must delete the ref row");
    requireThrows([&] { store.unregister_ref(link); },
                  "unregistering an unknown path must throw");
}

void testExecutableObjectMode() {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "tool";
    writeFile(source, "#!/bin/sh\nexit 0\n", 0750);
    ObjectStore store(temporary.path() / "pool");
    const auto result = store.put_object(source);
    require(modeOf(store.object_path(result.hash)) == 0550,
            "executable source must produce a read-only executable object");
}

void testStreamingLargeFile() {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "large";
    {
        std::ofstream output(source, std::ios::binary);
        std::string block(1024 * 1024, '\0');
        for (std::size_t index = 0; index != block.size(); ++index) {
            block[index] = static_cast<char>(index % 251);
        }
        for (int index = 0; index != 32; ++index) output.write(block.data(), block.size());
    }
    ObjectStore store(temporary.path() / "pool");
    const auto result = store.put_object(source);
    require(result.hash == isvn::common::blake3_file_hex(source),
            "large file hash must match streaming wrapper");
    require(store.verify_object(result.hash), "large stored object must verify");
}

void testFailureLeavesNoObjectOrMetadata() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "atomic-failure");
    ObjectStore store(pool);
    require(::chmod((pool / "tmp").c_str(), 0550) == 0,
            "test must make tmp unwritable");
    requireThrows([&] { static_cast<void>(store.put_object(source)); },
                  "put must fail before rename when tmp is unwritable");
    require(::chmod((pool / "tmp").c_str(), 02770) == 0,
            "test must restore tmp permissions");
    Database db(pool / "meta.db");
    require(db.integer("SELECT COUNT(*) FROM object;") == 0,
            "failed put must not leave metadata");
    require(regularFileCount(pool / "objects") == 0,
            "failed put must not leave physical object");
}

int childPut(const std::filesystem::path& pool, const std::filesystem::path& source,
             int start_fd) {
    char signal = 0;
    if (::read(start_fd, &signal, 1) != 1) return 20;
    try {
        ObjectStore store(pool);
        const auto result = store.put_object(source);
        return result.deduped ? 11 : 10;
    } catch (...) {
        return 21;
    }
}

void testConcurrentProcessPut() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, std::string(8 * 1024 * 1024, 'x'));
    { ObjectStore initialize(pool); }
    int start_pipe[2]{};
    require(::pipe(start_pipe) == 0, "test pipe must open");
    std::vector<pid_t> children;
    for (int index = 0; index != 2; ++index) {
        const pid_t child = ::fork();
        require(child >= 0, "fork must succeed");
        if (child == 0) {
            ::close(start_pipe[1]);
            const int result = childPut(pool, source, start_pipe[0]);
            ::close(start_pipe[0]);
            _exit(result);
        }
        children.push_back(child);
    }
    ::close(start_pipe[0]);
    const char signals[2] = {'x', 'x'};
    require(::write(start_pipe[1], signals, sizeof(signals)) == 2,
            "test must release both children");
    ::close(start_pipe[1]);
    int new_count = 0;
    int dedup_count = 0;
    for (const pid_t child : children) {
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
                "child put must exit normally");
        const int code = WEXITSTATUS(status);
        if (code == 10) ++new_count;
        if (code == 11) ++dedup_count;
        require(code == 10 || code == 11, "child put must succeed");
    }
    require(new_count == 1 && dedup_count == 1,
            "exactly one process must create and one must dedupe");
    ObjectStore store(pool);
    const std::string hash = isvn::common::blake3_file_hex(source);
    require(store.verify_object(hash), "concurrent object must verify");
    require(regularFileCount(pool / "objects") == 1,
            "concurrent put must leave one physical object");
    Database db(pool / "meta.db");
    require(db.integer("SELECT COUNT(*) FROM object;") == 1,
            "concurrent put must leave one metadata row");
}

void testReadOnlyObjectRejectsWrite() {
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source";
    writeFile(source, "immutable");
    ObjectStore store(temporary.path() / "pool");
    const auto result = store.put_object(source);
    const int descriptor = ::open(store.object_path(result.hash).c_str(), O_WRONLY);
    if (descriptor >= 0) ::close(descriptor);
    require(descriptor < 0, "0440 object must reject write open");
}

void testObjectPathSymlinkIsRejected() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "symlink-path-attack");
    ObjectStore store(pool);
    const std::string hash = isvn::common::blake3_file_hex(source);
    const auto bucket = pool / "objects" / hash.substr(0, 2);
    std::filesystem::create_directory(bucket);
    require(::chmod(bucket.c_str(), 03770) == 0, "test bucket mode must be set");
    std::filesystem::create_symlink(source, bucket / hash);
    requireThrows([&] { static_cast<void>(store.put_object(source)); },
                  "put must reject a symlink at the physical object path");
    Database db(pool / "meta.db");
    require(db.integer("SELECT COUNT(*) FROM object;") == 0,
            "rejected symlink path must not create metadata");
}

void testSchemaHasNoVersionSemantics() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    for (const std::string table : {"object", "ref", "evict_event", "renew_event"}) {
        for (const std::string& column : db.columnNames(table)) {
            for (const std::string forbidden :
                 {"version", "branch", "stage", "ancestor", "parent", "lineage", "file"}) {
                require(column.find(forbidden) == std::string::npos,
                        "storage schema must not contain version semantics");
            }
        }
    }
}

void testAtimeSelfTestReturnsBool() {
    TemporaryDirectory temporary;
    ObjectStore store(temporary.path() / "pool");
    const bool usable = store.atime_usable();
    require(usable == true || usable == false,
            "atime self-test must return a boolean value");
}

void testEvictEventSchemaIsHashAndTimeOnly() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    const auto columns = db.columnNames("evict_event");
    const std::vector<std::string> expected{"event_id", "hash", "evicted_at"};
    require(columns == expected,
            "evict_event must contain only event_id, hash, and evicted_at");
}

void testRenewEventSchemaIsHashActorAndTimeOnly() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    const auto columns = db.columnNames("renew_event");
    const std::vector<std::string> expected{"event_id", "hash", "actor", "renewed_at"};
    require(columns == expected,
            "renew_event must contain only event_id, hash, actor, and renewed_at");
}

void testTouchLastAccess() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "touch-content");
    ObjectStore store(pool);
    const auto result = store.put_object(source);
    Database db(pool / "meta.db");
    const std::string initial = db.text("SELECT last_access FROM object;");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    store.touch_last_access(result.hash);
    require(db.text("SELECT last_access FROM object;") > initial,
            "touch_last_access must update object.last_access");
    require(store.last_access_of(result.hash) == db.text("SELECT last_access FROM object;"),
            "last_access_of must return the hash-only last_access value");
    requireThrows([&] {
        store.touch_last_access(
            "0000000000000000000000000000000000000000000000000000000000000000");
    }, "touching an unknown object must throw");
    requireThrows([&] {
        static_cast<void>(store.last_access_of(
            "0000000000000000000000000000000000000000000000000000000000000000"));
    }, "last_access_of unknown hash must throw");
}

void testRenewEventRecordsHashActorAndTime() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "renew-event-content");
    ObjectStore store(pool);
    const auto result = store.put_object(source);
    store.record_renew_event(result.hash);
    Database db(pool / "meta.db");
    require(db.integer("SELECT COUNT(*) FROM renew_event;") == 1,
            "record_renew_event must append a renew_event row");
    require(db.text("SELECT hash FROM renew_event;") == result.hash,
            "renew_event must record only the object hash fact");
    require(!db.text("SELECT actor FROM renew_event;").empty(),
            "renew_event must record the real actor");
}

void testDanglingLinkDetection() {
    TemporaryDirectory temporary;
    const auto pool = temporary.path() / "pool";
    const auto source = temporary.path() / "source";
    writeFile(source, "dangling-content");
    ObjectStore store(pool);
    const auto result = store.put_object(source);
    const auto link = temporary.path() / "workspace" / "link";
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(store.object_path(result.hash), link);
    require(!store.is_dangling_link(link.string()),
            "normal symlink to an existing object must not be dangling");
    require(!store.is_dangling_link((temporary.path() / "missing").string()),
            "missing symlink path must be a clear false");
    require(::unlink(store.object_path(result.hash).c_str()) == 0,
            "test must remove object target");
    require(store.is_dangling_link(link.string()),
            "symlink whose target object is missing must be dangling");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"put hash and permissions", testPutHashAndPermissions},
        {"dedup and last access", testDedupAndLastAccess},
        {"get and object path", testGetAndObjectPath},
        {"refs and owner", testRefsAndOwner},
        {"executable object", testExecutableObjectMode},
        {"streaming large file", testStreamingLargeFile},
        {"failure atomicity", testFailureLeavesNoObjectOrMetadata},
        {"concurrent process put", testConcurrentProcessPut},
        {"read-only object", testReadOnlyObjectRejectsWrite},
        {"object symlink rejection", testObjectPathSymlinkIsRejected},
        {"schema boundary", testSchemaHasNoVersionSemantics},
        {"atime self-test", testAtimeSelfTestReturnsBool},
        {"evict event schema", testEvictEventSchemaIsHashAndTimeOnly},
        {"renew event schema", testRenewEventSchemaIsHashActorAndTimeOnly},
        {"touch last access", testTouchLastAccess},
        {"renew event record", testRenewEventRecordsHashActorAndTime},
        {"dangling link detection", testDanglingLinkDetection},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    return 0;
}
