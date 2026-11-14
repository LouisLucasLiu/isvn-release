#include "storage/object_store.h"

#include "common/fs.h"
#include "common/hash.h"
#include "common/system.h"

#include <sqlite3.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <thread>
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
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return descriptor_; }

private:
    int descriptor_;
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
    HashLock(const HashLock&) = delete;
    HashLock& operator=(const HashLock&) = delete;

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

mode_t objectMode(const std::filesystem::path& source) {
    struct stat status {};
    if (::stat(source.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw std::invalid_argument("Object source must be a regular file");
    }
    return (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 ? 0550 : 0440;
}

std::int64_t fileSize(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw std::runtime_error("Unable to read object file size");
    }
    return status.st_size;
}

bool isRegularFileWithoutFollowingSymlink(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error));
}

void copyFileToDescriptor(const std::filesystem::path& source, int destination) {
    FileDescriptor input(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
    if (input.get() < 0) {
        throw std::runtime_error("Unable to open source file: " +
                                 std::string(std::strerror(errno)));
    }
    std::array<char, 1024 * 1024> buffer{};
    while (true) {
        const ssize_t read_count = ::read(input.get(), buffer.data(), buffer.size());
        if (read_count == 0) break;
        if (read_count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Unable to read source file: " +
                                     std::string(std::strerror(errno)));
        }
        ssize_t offset = 0;
        while (offset < read_count) {
            const ssize_t written =
                ::write(destination, buffer.data() + offset,
                        static_cast<std::size_t>(read_count - offset));
            if (written < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Unable to write object file: " +
                                         std::string(std::strerror(errno)));
            }
            offset += written;
        }
    }
}

std::filesystem::path createTempFile(const std::filesystem::path& directory,
                                     const std::string& prefix, gid_t group,
                                     int& descriptor) {
    std::string pattern = (directory / (prefix + ".XXXXXX")).string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    descriptor = ::mkstemp(writable.data());
    if (descriptor < 0) {
        throw std::runtime_error("Unable to create temporary file: " +
                                 std::string(std::strerror(errno)));
    }
    if (::fchown(descriptor, static_cast<uid_t>(-1), group) != 0) {
        const int saved_errno = errno;
        ::close(descriptor);
        descriptor = -1;
        ::unlink(writable.data());
        throw std::runtime_error("Unable to set temporary file group: " +
                                 std::string(std::strerror(saved_errno)));
    }
    return writable.data();
}

bool timespecGreater(const timespec& lhs, const timespec& rhs) {
    return lhs.tv_sec > rhs.tv_sec ||
           (lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec > rhs.tv_nsec);
}

bool atimeSelfTest(const std::filesystem::path& directory, gid_t group) {
    int descriptor = -1;
    std::filesystem::path path;
    try {
        path = createTempFile(directory, ".isvn-atime", group, descriptor);
        FileDescriptor file(descriptor);
        const char data = 'x';
        if (::write(file.get(), &data, 1) != 1) {
            throw std::runtime_error("Unable to write atime self-test file");
        }
        if (::fsync(file.get()) != 0) {
            throw std::runtime_error("Unable to fsync atime self-test file");
        }
        struct stat before {};
        if (::fstat(file.get(), &before) != 0) {
            throw std::runtime_error("Unable to stat atime self-test file");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (::lseek(file.get(), 0, SEEK_SET) < 0) {
            throw std::runtime_error("Unable to rewind atime self-test file");
        }
        char sink = 0;
        if (::read(file.get(), &sink, 1) != 1) {
            throw std::runtime_error("Unable to read atime self-test file");
        }
        struct stat after {};
        if (::fstat(file.get(), &after) != 0) {
            throw std::runtime_error("Unable to restat atime self-test file");
        }
        ::unlink(path.c_str());
        return timespecGreater(after.st_atim, before.st_atim);
    } catch (...) {
        if (!path.empty()) ::unlink(path.c_str());
        throw;
    }
}

std::filesystem::path symlinkTarget(const std::filesystem::path& link_path) {
    std::vector<char> buffer(4096);
    const ssize_t size = ::readlink(link_path.c_str(), buffer.data(), buffer.size() - 1);
    if (size < 0) {
        throw std::runtime_error("Unable to read symlink target: " +
                                 std::string(std::strerror(errno)));
    }
    buffer[static_cast<std::size_t>(size)] = '\0';
    std::filesystem::path target(buffer.data());
    if (!target.is_absolute()) target = link_path.parent_path() / target;
    return target;
}

}  // namespace

class ObjectStore::Impl {
public:
    explicit Impl(const std::filesystem::path& pool_root)
        : root_(std::filesystem::absolute(pool_root)),
          objects_(root_ / "objects"), locks_(root_ / "locks"),
          temporary_(root_ / "tmp"), backup_(root_ / "backup"),
          database_(root_ / "meta.db") {
        std::error_code error;
        if (!std::filesystem::exists(root_, error)) {
            const auto parent = root_.parent_path();
            group_ = common::path_group(parent);
            common::ensure_directory(root_, 02770, group_);
        } else {
            group_ = common::path_group(root_);
            common::ensure_directory(root_, 02770, group_);
        }
        common::ensure_directory(objects_, 03770, group_);
        common::ensure_directory(locks_, 02770, group_);
        common::ensure_directory(temporary_, 02770, group_);
        common::ensure_directory(backup_, 02770, group_);
        atime_usable_ = atimeSelfTest(root_, group_);
        prepareDatabaseFile();

        const int rc = sqlite3_open_v2(database_.c_str(), &db_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                           SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Unable to open object metadata database: " + message);
        }
        try {
            if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
                throw std::runtime_error("Unable to set SQLite busy timeout");
            }
            execute(db_, "PRAGMA foreign_keys=ON;");
            execute(db_, "PRAGMA journal_mode=WAL;");
            execute(db_, R"sql(
                CREATE TABLE IF NOT EXISTS object (
                    hash TEXT PRIMARY KEY,
                    size INTEGER NOT NULL,
                    mode INTEGER NOT NULL,
                    created_at TEXT NOT NULL,
                    last_access TEXT NOT NULL,
                    refcount INTEGER NOT NULL DEFAULT 0 CHECK (refcount >= 0)
                );
                CREATE TABLE IF NOT EXISTS ref (
                    ref_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    hash TEXT NOT NULL REFERENCES object(hash),
                    owner TEXT NOT NULL,
                    link_path TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    last_verified TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_ref_hash ON ref(hash);
                CREATE UNIQUE INDEX IF NOT EXISTS idx_ref_link ON ref(link_path);
                CREATE INDEX IF NOT EXISTS idx_object_lru ON object(refcount, last_access);
                CREATE TABLE IF NOT EXISTS evict_event (
                    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    hash TEXT NOT NULL,
                    evicted_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_evict_time ON evict_event(evicted_at);
                CREATE TABLE IF NOT EXISTS renew_event (
                    event_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    hash TEXT NOT NULL,
                    actor TEXT NOT NULL,
                    renewed_at TEXT NOT NULL
                );
                CREATE INDEX IF NOT EXISTS idx_renew_time ON renew_event(renewed_at);
            )sql");
            secureDatabaseFiles();
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    ~Impl() { sqlite3_close(db_); }

    std::filesystem::path objectPath(const std::string& hash) const {
        return objects_ / hash.substr(0, 2) / hash;
    }
    std::filesystem::path lockPath(const std::string& hash) const {
        return locks_ / (hash + ".lock");
    }

    void ensureBucket(const std::string& hash) const {
        common::ensure_directory(objects_ / hash.substr(0, 2), 03770, group_);
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

    sqlite3* db() const { return db_; }
    gid_t group() const { return group_; }
    bool atimeUsable() const { return atime_usable_; }
    const std::filesystem::path& temporary() const { return temporary_; }
    const std::filesystem::path& objects() const { return objects_; }
    std::mutex& mutex() const { return database_mutex_; }

private:
    void prepareDatabaseFile() {
        int descriptor = -1;
        for (int attempt = 0; attempt != 50; ++attempt) {
            descriptor =
                ::open(database_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0660);
            if (descriptor >= 0) break;
            if (errno != EACCES) {
                throw std::runtime_error("Unable to create meta.db: " +
                                         std::string(std::strerror(errno)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (descriptor < 0) throw std::runtime_error("Timed out opening meta.db");
        FileDescriptor file(descriptor);
        struct stat database_status {};
        if (::fstat(file.get(), &database_status) != 0 ||
            !S_ISREG(database_status.st_mode)) {
            throw std::runtime_error("meta.db path is not a regular file");
        }
        secureOpenFile(file.get(), database_status, group_, 0660, "meta.db");
    }

    std::filesystem::path root_;
    std::filesystem::path objects_;
    std::filesystem::path locks_;
    std::filesystem::path temporary_;
    std::filesystem::path backup_;
    std::filesystem::path database_;
    gid_t group_{};
    sqlite3* db_ = nullptr;
    mutable std::mutex database_mutex_;
    bool atime_usable_ = false;
};

ObjectStore::ObjectStore(const std::filesystem::path& pool_root)
    : impl_(std::make_unique<Impl>(pool_root)) {}
ObjectStore::~ObjectStore() = default;
ObjectStore::ObjectStore(ObjectStore&&) noexcept = default;
ObjectStore& ObjectStore::operator=(ObjectStore&&) noexcept = default;

PutResult ObjectStore::put_object(const std::filesystem::path& source_file) {
    const mode_t mode = objectMode(source_file);
    const std::string hash = common::blake3_file_hex(source_file);
    HashLock hash_lock(impl_->lockPath(hash), impl_->group());
    std::lock_guard<std::mutex> database_lock(impl_->mutex());
    sqlite3* db = impl_->db();
    Transaction transaction(db);
    const std::string now = common::utc_now();

    Statement existing(db, "SELECT 1 FROM object WHERE hash = ?;");
    bindText(db, existing.get(), 1, hash);
    const int existing_rc = sqlite3_step(existing.get());
    if (existing_rc == SQLITE_ROW &&
        isRegularFileWithoutFollowingSymlink(impl_->objectPath(hash))) {
        Statement touch(db, "UPDATE object SET last_access = ? WHERE hash = ?;");
        bindText(db, touch.get(), 1, now);
        bindText(db, touch.get(), 2, hash);
        expectDone(db, touch.get());
        impl_->secureDatabaseFiles();
        transaction.commit();
        return {hash, true};
    }
    if (existing_rc != SQLITE_ROW && existing_rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
    }
    const bool reinstall_evicted = existing_rc == SQLITE_ROW;

    impl_->ensureBucket(hash);
    const std::filesystem::path final_path = impl_->objectPath(hash);
    bool installed = false;
    bool reused_orphan = false;
    std::filesystem::path temp_path;
    try {
        if (std::filesystem::symlink_status(final_path).type() !=
            std::filesystem::file_type::not_found) {
            if (!isRegularFileWithoutFollowingSymlink(final_path)) {
                throw std::runtime_error("Physical object path is not a regular file: " + hash);
            }
            if (common::blake3_file_hex(final_path) != hash) {
                throw std::runtime_error("Existing physical object has invalid content: " + hash);
            }
            reused_orphan = true;
        } else {
            int temp_descriptor = -1;
            temp_path = createTempFile(impl_->temporary(), hash, impl_->group(), temp_descriptor);
            FileDescriptor temporary_file(temp_descriptor);
            copyFileToDescriptor(source_file, temporary_file.get());
            if (::fsync(temporary_file.get()) != 0) {
                throw std::runtime_error("Unable to fsync temporary object");
            }
            if (::fchmod(temporary_file.get(), mode) != 0) {
                throw std::runtime_error("Unable to set object read-only mode");
            }
            if (common::blake3_file_hex(temp_path) != hash) {
                throw std::runtime_error("Source file changed while being stored");
            }
            if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
                throw std::runtime_error("Unable to atomically install object: " +
                                         std::string(std::strerror(errno)));
            }
            temp_path.clear();
            installed = true;
            common::fsync_directory(final_path.parent_path());
        }

        if (reinstall_evicted) {
            Statement update(db, R"sql(
                UPDATE object SET size = ?, mode = ?, last_access = ? WHERE hash = ?;
            )sql");
            sqlite3_bind_int64(update.get(), 1, fileSize(final_path));
            sqlite3_bind_int(update.get(), 2, mode);
            bindText(db, update.get(), 3, now);
            bindText(db, update.get(), 4, hash);
            expectDone(db, update.get());
        } else {
            Statement insert(db, R"sql(
                INSERT INTO object(hash, size, mode, created_at, last_access, refcount)
                VALUES(?, ?, ?, ?, ?, 0);
            )sql");
            bindText(db, insert.get(), 1, hash);
            sqlite3_bind_int64(insert.get(), 2, fileSize(final_path));
            sqlite3_bind_int(insert.get(), 3, mode);
            bindText(db, insert.get(), 4, now);
            bindText(db, insert.get(), 5, now);
            expectDone(db, insert.get());
        }
        impl_->secureDatabaseFiles();
        transaction.commit();
    } catch (...) {
        if (!temp_path.empty()) ::unlink(temp_path.c_str());
        if (installed) {
            ::unlink(final_path.c_str());
            try {
                common::fsync_directory(final_path.parent_path());
            } catch (...) {
            }
        }
        throw;
    }
    return {hash, reused_orphan};
}

void ObjectStore::get_object(const std::string& hash,
                             const std::filesystem::path& dest) const {
    validateHash(hash);
    const auto source = impl_->objectPath(hash);
    mode_t mode = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex());
        sqlite3* db = impl_->db();
        Statement query(db, "SELECT mode FROM object WHERE hash = ?;");
        bindText(db, query.get(), 1, hash);
        if (sqlite3_step(query.get()) != SQLITE_ROW) {
            throw std::runtime_error("Unknown object hash: " + hash);
        }
        mode = static_cast<mode_t>(sqlite3_column_int(query.get(), 0));
    }
    std::error_code error;
    if (!dest.parent_path().empty()) std::filesystem::create_directories(dest.parent_path(), error);
    if (error) throw std::runtime_error("Unable to create destination directory");
    int descriptor = -1;
    const auto temp_directory = dest.parent_path().empty() ? std::filesystem::path(".")
                                                           : dest.parent_path();
    const auto temp = createTempFile(temp_directory, ".isvn-get",
                                     common::path_group(temp_directory), descriptor);
    try {
        FileDescriptor output(descriptor);
        copyFileToDescriptor(source, output.get());
        if (::fsync(output.get()) != 0 || ::fchmod(output.get(), mode) != 0) {
            throw std::runtime_error("Unable to finalize retrieved object");
        }
        if (::rename(temp.c_str(), dest.c_str()) != 0) {
            throw std::runtime_error("Unable to install retrieved object");
        }
        common::fsync_directory(temp_directory);
    } catch (...) {
        ::unlink(temp.c_str());
        throw;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Transaction transaction(impl_->db());
    Statement touch(impl_->db(), "UPDATE object SET last_access = ? WHERE hash = ?;");
    bindText(impl_->db(), touch.get(), 1, common::utc_now());
    bindText(impl_->db(), touch.get(), 2, hash);
    expectDone(impl_->db(), touch.get());
    impl_->secureDatabaseFiles();
    transaction.commit();
}

std::filesystem::path ObjectStore::object_path(const std::string& hash) const {
    validateHash(hash);
    return impl_->objectPath(hash);
}

bool ObjectStore::has_object(const std::string& hash) const {
    validateHash(hash);
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Statement query(impl_->db(), "SELECT 1 FROM object WHERE hash = ?;");
    bindText(impl_->db(), query.get(), 1, hash);
    const int rc = sqlite3_step(query.get());
    if (rc == SQLITE_DONE) return false;
    if (rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("SQLite read failed: ") +
                                 sqlite3_errmsg(impl_->db()));
    }
    return isRegularFileWithoutFollowingSymlink(impl_->objectPath(hash));
}

void ObjectStore::register_ref(const std::string& hash, const std::string& link_path) {
    validateHash(hash);
    if (!std::filesystem::path(link_path).is_absolute()) {
        throw std::invalid_argument("Reference link path must be absolute");
    }
    if (!isRegularFileWithoutFollowingSymlink(impl_->objectPath(hash))) {
        throw std::runtime_error("Cannot reference missing object content: " + hash);
    }
    std::lock_guard<std::mutex> lock(impl_->mutex());
    sqlite3* db = impl_->db();
    Transaction transaction(db);
    Statement exists(db, "SELECT 1 FROM object WHERE hash = ?;");
    bindText(db, exists.get(), 1, hash);
    if (sqlite3_step(exists.get()) != SQLITE_ROW) {
        throw std::runtime_error("Cannot reference an unknown object: " + hash);
    }
    const std::string now = common::utc_now();
    Statement insert(db, R"sql(
        INSERT INTO ref(hash, owner, link_path, created_at, last_verified)
        VALUES(?, ?, ?, ?, ?);
    )sql");
    bindText(db, insert.get(), 1, hash);
    bindText(db, insert.get(), 2, common::system_user());
    bindText(db, insert.get(), 3, link_path);
    bindText(db, insert.get(), 4, now);
    bindText(db, insert.get(), 5, now);
    expectDone(db, insert.get());
    Statement increment(db, R"sql(
        UPDATE object SET refcount = refcount + 1, last_access = ? WHERE hash = ?;
    )sql");
    bindText(db, increment.get(), 1, now);
    bindText(db, increment.get(), 2, hash);
    expectDone(db, increment.get());
    impl_->secureDatabaseFiles();
    transaction.commit();
}

void ObjectStore::unregister_ref(const std::string& link_path) {
    if (!std::filesystem::path(link_path).is_absolute()) {
        throw std::invalid_argument("Reference link path must be absolute");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex());
    sqlite3* db = impl_->db();
    Transaction transaction(db);
    Statement query(db, "SELECT hash FROM ref WHERE link_path = ?;");
    bindText(db, query.get(), 1, link_path);
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        throw std::runtime_error("Cannot unregister an unknown reference: " + link_path);
    }
    const auto* hash_text = sqlite3_column_text(query.get(), 0);
    const std::string hash(reinterpret_cast<const char*>(hash_text));
    Statement remove(db, "DELETE FROM ref WHERE link_path = ?;");
    bindText(db, remove.get(), 1, link_path);
    expectDone(db, remove.get());
    Statement decrement(db, "UPDATE object SET refcount = refcount - 1 WHERE hash = ?;");
    bindText(db, decrement.get(), 1, hash);
    expectDone(db, decrement.get());
    impl_->secureDatabaseFiles();
    transaction.commit();
}

bool ObjectStore::has_ref(const std::string& link_path) const {
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Statement query(impl_->db(), "SELECT 1 FROM ref WHERE link_path = ?;");
    bindText(impl_->db(), query.get(), 1, link_path);
    const int rc = sqlite3_step(query.get());
    if (rc == SQLITE_ROW) return true;
    if (rc != SQLITE_DONE) {
        throw std::runtime_error(std::string("SQLite read failed: ") +
                                 sqlite3_errmsg(impl_->db()));
    }
    return false;
}

std::optional<std::string> ObjectStore::ref_hash(const std::string& link_path) const {
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Statement query(impl_->db(), "SELECT hash FROM ref WHERE link_path = ?;");
    bindText(impl_->db(), query.get(), 1, link_path);
    const int rc = sqlite3_step(query.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("SQLite read failed: ") +
                                 sqlite3_errmsg(impl_->db()));
    }
    const auto* text = sqlite3_column_text(query.get(), 0);
    if (text == nullptr) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(text));
}

int ObjectStore::refcount(const std::string& hash) const {
    validateHash(hash);
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Statement query(impl_->db(), "SELECT refcount FROM object WHERE hash = ?;");
    bindText(impl_->db(), query.get(), 1, hash);
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        throw std::runtime_error("Unknown object hash: " + hash);
    }
    return sqlite3_column_int(query.get(), 0);
}

bool ObjectStore::verify_object(const std::string& hash) const {
    validateHash(hash);
    if (!has_object(hash)) return false;
    return common::blake3_file_hex(impl_->objectPath(hash)) == hash;
}

void ObjectStore::touch_last_access(const std::string& hash) {
    validateHash(hash);
    std::lock_guard<std::mutex> lock(impl_->mutex());
    sqlite3* db = impl_->db();
    Transaction transaction(db);
    Statement touch(db, "UPDATE object SET last_access = ? WHERE hash = ?;");
    bindText(db, touch.get(), 1, common::utc_now());
    bindText(db, touch.get(), 2, hash);
    expectDone(db, touch.get());
    if (sqlite3_changes(db) != 1) {
        throw std::runtime_error("Unknown object hash: " + hash);
    }
    impl_->secureDatabaseFiles();
    transaction.commit();
}

std::string ObjectStore::last_access_of(const std::string& hash) const {
    validateHash(hash);
    std::lock_guard<std::mutex> lock(impl_->mutex());
    Statement query(impl_->db(), "SELECT last_access FROM object WHERE hash = ?;");
    bindText(impl_->db(), query.get(), 1, hash);
    if (sqlite3_step(query.get()) != SQLITE_ROW) {
        throw std::runtime_error("Unknown object hash: " + hash);
    }
    const auto* value = sqlite3_column_text(query.get(), 0);
    if (value == nullptr) throw std::runtime_error("Object last_access is NULL: " + hash);
    return reinterpret_cast<const char*>(value);
}

void ObjectStore::record_renew_event(const std::string& hash) noexcept {
    try {
        validateHash(hash);
        std::lock_guard<std::mutex> lock(impl_->mutex());
        Statement insert(impl_->db(), R"sql(
            INSERT INTO renew_event(hash, actor, renewed_at) VALUES(?, ?, ?);
        )sql");
        bindText(impl_->db(), insert.get(), 1, hash);
        bindText(impl_->db(), insert.get(), 2, common::system_user());
        bindText(impl_->db(), insert.get(), 3, common::utc_now());
        expectDone(impl_->db(), insert.get());
        impl_->secureDatabaseFiles();
    } catch (...) {
    }
}

bool ObjectStore::atime_usable() const { return impl_->atimeUsable(); }

bool ObjectStore::is_dangling_link(const std::string& link_path) const {
    const std::filesystem::path path(link_path);
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) return false;
        throw std::runtime_error("Unable to lstat link path: " +
                                 std::string(std::strerror(errno)));
    }
    if (!S_ISLNK(status.st_mode)) return false;
    const std::filesystem::path target = symlinkTarget(path);
    std::error_code error;
    return !std::filesystem::exists(target, error);
}

}  // namespace isvn::storage
