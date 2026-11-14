#include "version/lineage.h"

#include "common/hash.h"
#include "common/system.h"

#include <sqlite3.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace isvn::version {
namespace {

constexpr const char* kGenesisHash =
    "isvn-lineage-ledger-v1-genesis";

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

void bindOptionalText(sqlite3* db, sqlite3_stmt* statement, int index,
                      const std::optional<std::string>& value) {
    if (value.has_value()) {
        bindText(db, statement, index, *value);
    } else if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        throw std::runtime_error(std::string("SQLite bind failed: ") + sqlite3_errmsg(db));
    }
}

void bindOptionalInt64(sqlite3* db, sqlite3_stmt* statement, int index,
                       const std::optional<std::int64_t>& value) {
    const int rc = value.has_value() ? sqlite3_bind_int64(statement, index, *value)
                                     : sqlite3_bind_null(statement, index);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("SQLite bind failed: ") + sqlite3_errmsg(db));
    }
}

void expectDone(sqlite3* db, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(db));
    }
}

std::optional<std::string> optionalColumn(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = sqlite3_column_text(statement, column);
    return std::string(reinterpret_cast<const char*>(text));
}

std::optional<std::int64_t> optionalInt64Column(sqlite3_stmt* statement, int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(statement, column);
}

std::string requiredColumn(sqlite3_stmt* statement, int column) {
    const auto value = optionalColumn(statement, column);
    if (!value.has_value()) {
        throw std::runtime_error("Lineage database contains an unexpected NULL value");
    }
    return *value;
}

void validateVersionId(const std::string& value) {
    if (value == "ROOT") return;
    if (value.empty() || value.find("..") != std::string::npos ||
        value.find('\\') != std::string::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        throw std::invalid_argument("Invalid version identifier: " + value);
    }

    std::size_t segment_start = 0;
    int segment_count = 0;
    while (true) {
        const std::size_t separator = value.find('/', segment_start);
        const std::size_t segment_end =
            separator == std::string::npos ? value.size() : separator;
        if (segment_end == segment_start) {
            throw std::invalid_argument("Invalid version identifier: " + value);
        }
        ++segment_count;
        if (separator == std::string::npos) {
            break;
        }
        segment_start = separator + 1;
    }
    if (segment_count != 4) {
        throw std::invalid_argument("Version identifier must contain exactly four segments: " +
                                    value);
    }
}

void validateRequiredText(const std::string& value, const char* field) {
    if (value.empty() ||
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        throw std::invalid_argument(std::string(field) + " must not be empty");
    }
}

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE;"); }
    ~Transaction() {
        if (!committed_) {
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    void commit() {
        execute(db_, "COMMIT;");
        committed_ = true;
    }

private:
    sqlite3* db_;
    bool committed_ = false;
};

struct LedgerEvent {
    std::int64_t seq = 0;
    std::string event_type;
    std::optional<std::int64_t> edge_id;
    std::optional<std::string> child;
    std::optional<std::string> parent;
    std::optional<std::string> actor;
    std::optional<std::string> ts;
    std::optional<std::string> origin;
    std::optional<std::string> note;
    std::optional<std::int64_t> superseded_by;
    std::optional<std::int64_t> removed;
    std::optional<std::int64_t> audit_seq;
    std::optional<std::string> action;
    std::optional<std::string> old_parent;
    std::optional<std::string> new_parent;
    std::optional<std::string> reason;
    std::string prev_hash;
    std::string entry_hash;
};

void hashBytes(std::string& input, const std::string& value) {
    input.append(value);
}

void hashField(std::string& input, const std::optional<std::string>& value) {
    if (!value.has_value()) {
        hashBytes(input, "N;");
        return;
    }
    hashBytes(input, "S" + std::to_string(value->size()) + ":");
    hashBytes(input, *value);
    hashBytes(input, ";");
}

void hashField(std::string& input, const std::optional<std::int64_t>& value) {
    hashField(input, value.has_value()
                          ? std::optional<std::string>(std::to_string(*value))
                          : std::nullopt);
}

std::string ledgerHash(const LedgerEvent& event) {
    std::string input;
    hashBytes(input, "isvn-lineage-ledger-v1;");
    hashField(input, std::optional<std::int64_t>(event.seq));
    hashField(input, std::optional<std::string>(event.event_type));
    hashField(input, event.edge_id);
    hashField(input, event.child);
    hashField(input, event.parent);
    hashField(input, event.actor);
    hashField(input, event.ts);
    hashField(input, event.origin);
    hashField(input, event.note);
    hashField(input, event.superseded_by);
    hashField(input, event.removed);
    hashField(input, event.audit_seq);
    hashField(input, event.action);
    hashField(input, event.old_parent);
    hashField(input, event.new_parent);
    hashField(input, event.reason);
    hashField(input, std::optional<std::string>(event.prev_hash));
    return common::blake3_hex(input);
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void appendJsonString(std::ostringstream& output,
                      const std::optional<std::string>& value) {
    if (value.has_value()) {
        output << '"' << jsonEscape(*value) << '"';
    } else {
        output << "null";
    }
}

bool sameOptional(const std::optional<std::string>& left,
                  const std::optional<std::string>& right) {
    return left == right;
}

}  // namespace

class Lineage::Impl {
public:
    explicit Impl(const std::filesystem::path& project_root)
        : isvn_directory_(project_root / ".isvn"),
          database_path_(isvn_directory_ / "lineage.db") {
        struct stat project_status {};
        if (::stat(project_root.c_str(), &project_status) != 0) {
            throw std::runtime_error("Unable to read project root permissions");
        }
        std::error_code error;
        const bool created = std::filesystem::create_directories(isvn_directory_, error);
        if (error) {
            throw std::runtime_error("Unable to create .isvn directory: " + error.message());
        }
        if (created && ::chown(isvn_directory_.c_str(), static_cast<uid_t>(-1),
                               project_status.st_gid) != 0) {
            throw std::runtime_error("Unable to inherit the project group for .isvn");
        }
        ensureMode(isvn_directory_, S_IRWXU | S_IRWXG | S_ISGID,
                   "shared .isvn directory");

        const int rc = sqlite3_open_v2(database_path_.c_str(), &db_,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                           SQLITE_OPEN_FULLMUTEX,
                                       nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("Unable to open lineage database: " + message);
        }

        try {
            secureDatabaseFiles();
            if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
                throw std::runtime_error(std::string("Unable to set SQLite busy timeout: ") +
                                         sqlite3_errmsg(db_));
            }
            execute(db_, "PRAGMA foreign_keys=ON;");
            execute(db_, "PRAGMA journal_mode=WAL;");
            execute(db_, R"sql(
                CREATE TABLE IF NOT EXISTS lineage_edge (
                    edge_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    child TEXT NOT NULL,
                    parent TEXT,
                    actor TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    origin TEXT NOT NULL,
                    note TEXT,
                    superseded_by INTEGER,
                    removed INTEGER NOT NULL DEFAULT 0 CHECK (removed IN (0, 1)),
                    FOREIGN KEY (superseded_by) REFERENCES lineage_edge(edge_id)
                        DEFERRABLE INITIALLY DEFERRED
                );
                CREATE INDEX IF NOT EXISTS idx_edge_child ON lineage_edge(child);
                CREATE INDEX IF NOT EXISTS idx_edge_parent ON lineage_edge(parent);
                CREATE UNIQUE INDEX IF NOT EXISTS idx_edge_active_child
                    ON lineage_edge(child)
                    WHERE superseded_by IS NULL AND removed = 0;

                CREATE TABLE IF NOT EXISTS lineage_audit (
                    seq INTEGER PRIMARY KEY AUTOINCREMENT,
                    ts TEXT NOT NULL,
                    actor TEXT NOT NULL,
                    action TEXT NOT NULL,
                    child TEXT NOT NULL,
                    old_parent TEXT,
                    new_parent TEXT,
                    reason TEXT
                );

                CREATE TABLE IF NOT EXISTS lineage_ledger (
                    seq INTEGER PRIMARY KEY,
                    event_type TEXT NOT NULL,
                    edge_id INTEGER,
                    child TEXT,
                    parent TEXT,
                    actor TEXT,
                    ts TEXT,
                    origin TEXT,
                    note TEXT,
                    superseded_by INTEGER,
                    removed INTEGER,
                    audit_seq INTEGER,
                    action TEXT,
                    old_parent TEXT,
                    new_parent TEXT,
                    reason TEXT,
                    prev_hash TEXT NOT NULL,
                    entry_hash TEXT NOT NULL
                );

                CREATE TRIGGER IF NOT EXISTS lineage_edge_facts_immutable
                BEFORE UPDATE OF child, parent, actor, created_at, origin, note ON lineage_edge
                BEGIN
                    SELECT RAISE(ABORT, 'lineage edge facts are immutable');
                END;
                CREATE TRIGGER IF NOT EXISTS lineage_edge_no_delete
                BEFORE DELETE ON lineage_edge
                BEGIN
                    SELECT RAISE(ABORT, 'lineage edges are append-only');
                END;
                CREATE TRIGGER IF NOT EXISTS lineage_audit_no_update
                BEFORE UPDATE ON lineage_audit
                BEGIN
                    SELECT RAISE(ABORT, 'lineage audit is append-only');
                END;
                CREATE TRIGGER IF NOT EXISTS lineage_audit_no_delete
                BEFORE DELETE ON lineage_audit
                BEGIN
                    SELECT RAISE(ABORT, 'lineage audit is append-only');
                END;
                CREATE TRIGGER IF NOT EXISTS lineage_ledger_no_update
                BEFORE UPDATE ON lineage_ledger
                BEGIN
                    SELECT RAISE(ABORT, 'lineage ledger is append-only');
                END;
                CREATE TRIGGER IF NOT EXISTS lineage_ledger_no_delete
                BEFORE DELETE ON lineage_ledger
                BEGIN
                    SELECT RAISE(ABORT, 'lineage ledger is append-only');
                END;
            )sql");
            rejectLegacyData();
            secureDatabaseFiles();
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    ~Impl() { sqlite3_close(db_); }

    void secureDatabaseFiles() const {
        constexpr mode_t shared_file_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
        struct stat directory_status {};
        if (::stat(isvn_directory_.c_str(), &directory_status) != 0) {
            throw std::runtime_error("Unable to read .isvn group");
        }
        ensureGroup(database_path_, directory_status.st_gid, "lineage.db");
        ensureMode(database_path_, shared_file_mode, "lineage.db");
        for (const char* suffix : {"-wal", "-shm"}) {
            const std::filesystem::path auxiliary = database_path_.string() + suffix;
            std::error_code error;
            if (std::filesystem::exists(auxiliary, error)) {
                ensureGroup(auxiliary, directory_status.st_gid, "SQLite auxiliary file");
                ensureMode(auxiliary, shared_file_mode, "SQLite auxiliary file");
            }
        }
    }

    sqlite3* db() const { return db_; }

    std::int64_t nextEdgeId() const {
        Statement statement(db_, "SELECT COALESCE(MAX(edge_id), 0) + 1 FROM lineage_edge;");
        if (sqlite3_step(statement.get()) != SQLITE_ROW) {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db_));
        }
        return sqlite3_column_int64(statement.get(), 0);
    }

    void appendLedger(LedgerEvent event) {
        Statement tail(db_,
                       "SELECT seq, entry_hash FROM lineage_ledger ORDER BY seq DESC LIMIT 1;");
        const int rc = sqlite3_step(tail.get());
        if (rc == SQLITE_ROW) {
            event.seq = sqlite3_column_int64(tail.get(), 0) + 1;
            event.prev_hash = requiredColumn(tail.get(), 1);
        } else if (rc == SQLITE_DONE) {
            event.seq = 1;
            event.prev_hash = kGenesisHash;
        } else {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db_));
        }
        event.entry_hash = ledgerHash(event);

        Statement insert(db_, R"sql(
            INSERT INTO lineage_ledger(
                seq, event_type, edge_id, child, parent, actor, ts, origin, note,
                superseded_by, removed, audit_seq, action, old_parent, new_parent,
                reason, prev_hash, entry_hash)
            VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )sql");
        sqlite3_bind_int64(insert.get(), 1, event.seq);
        bindText(db_, insert.get(), 2, event.event_type);
        bindOptionalInt64(db_, insert.get(), 3, event.edge_id);
        bindOptionalText(db_, insert.get(), 4, event.child);
        bindOptionalText(db_, insert.get(), 5, event.parent);
        bindOptionalText(db_, insert.get(), 6, event.actor);
        bindOptionalText(db_, insert.get(), 7, event.ts);
        bindOptionalText(db_, insert.get(), 8, event.origin);
        bindOptionalText(db_, insert.get(), 9, event.note);
        bindOptionalInt64(db_, insert.get(), 10, event.superseded_by);
        bindOptionalInt64(db_, insert.get(), 11, event.removed);
        bindOptionalInt64(db_, insert.get(), 12, event.audit_seq);
        bindOptionalText(db_, insert.get(), 13, event.action);
        bindOptionalText(db_, insert.get(), 14, event.old_parent);
        bindOptionalText(db_, insert.get(), 15, event.new_parent);
        bindOptionalText(db_, insert.get(), 16, event.reason);
        bindText(db_, insert.get(), 17, event.prev_hash);
        bindText(db_, insert.get(), 18, event.entry_hash);
        expectDone(db_, insert.get());
    }

private:
    static void ensureMode(const std::filesystem::path& path, mode_t mode,
                           const char* description) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0) {
            throw std::runtime_error(std::string("Unable to read ") + description +
                                     " permissions");
        }
        if ((status.st_mode & 07777) != mode && ::chmod(path.c_str(), mode) != 0) {
            throw std::runtime_error(std::string("Unable to set ") + description +
                                     " permissions");
        }
    }

    static void ensureGroup(const std::filesystem::path& path, gid_t group,
                            const char* description) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0) {
            throw std::runtime_error(std::string("Unable to read ") + description + " group");
        }
        if (status.st_gid != group &&
            ::chown(path.c_str(), static_cast<uid_t>(-1), group) != 0) {
            throw std::runtime_error(std::string("Unable to set ") + description + " group");
        }
    }

    void rejectLegacyData() const {
        Statement table(db_,
                        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='lineage';");
        if (sqlite3_step(table.get()) != SQLITE_ROW) {
            return;
        }
        Statement count(db_, "SELECT COUNT(*) FROM lineage;");
        if (sqlite3_step(count.get()) == SQLITE_ROW && sqlite3_column_int64(count.get(), 0) != 0) {
            throw std::runtime_error(
                "Legacy mutable lineage data requires an explicit reviewed migration");
        }
    }

    std::filesystem::path isvn_directory_;
    std::filesystem::path database_path_;
    sqlite3* db_ = nullptr;
};

namespace {

std::optional<LineageEntry> activeEntry(sqlite3* db, const std::string& child) {
    Statement statement(db, R"sql(
        SELECT child, parent, actor, created_at, origin, note
        FROM lineage_edge
        WHERE child = ? AND superseded_by IS NULL AND removed = 0;
    )sql");
    bindText(db, statement.get(), 1, child);
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_DONE) {
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
    }
    return LineageEntry{requiredColumn(statement.get(), 0),
                        optionalColumn(statement.get(), 1),
                        requiredColumn(statement.get(), 2),
                        requiredColumn(statement.get(), 3),
                        requiredColumn(statement.get(), 4),
                        optionalColumn(statement.get(), 5)};
}

std::int64_t insertAudit(sqlite3* db, const std::string& timestamp,
                         const std::string& actor, const std::string& action,
                         const std::string& child,
                         const std::optional<std::string>& old_parent,
                         const std::optional<std::string>& new_parent,
                         const std::string& reason) {
    Statement statement(db, R"sql(
        INSERT INTO lineage_audit(ts, actor, action, child, old_parent, new_parent, reason)
        VALUES(?, ?, ?, ?, ?, ?, ?);
    )sql");
    bindText(db, statement.get(), 1, timestamp);
    bindText(db, statement.get(), 2, actor);
    bindText(db, statement.get(), 3, action);
    bindText(db, statement.get(), 4, child);
    bindOptionalText(db, statement.get(), 5, old_parent);
    bindOptionalText(db, statement.get(), 6, new_parent);
    bindText(db, statement.get(), 7, reason);
    expectDone(db, statement.get());
    return sqlite3_last_insert_rowid(db);
}

LedgerEvent auditEvent(std::int64_t audit_seq, const std::string& timestamp,
                       const std::string& actor, const std::string& action,
                       const std::string& child,
                       const std::optional<std::string>& old_parent,
                       const std::optional<std::string>& new_parent,
                       const std::string& reason) {
    LedgerEvent event;
    event.event_type = "audit";
    event.audit_seq = audit_seq;
    event.ts = timestamp;
    event.actor = actor;
    event.action = action;
    event.child = child;
    event.old_parent = old_parent;
    event.new_parent = new_parent;
    event.reason = reason;
    return event;
}

}  // namespace

Lineage::Lineage(const std::filesystem::path& project_root)
    : impl_(std::make_unique<Impl>(project_root)) {}
Lineage::~Lineage() = default;
Lineage::Lineage(Lineage&&) noexcept = default;
Lineage& Lineage::operator=(Lineage&&) noexcept = default;

void Lineage::recordParent(const std::string& child,
                           const std::optional<std::string>& parent,
                           const std::string& reason,
                           const std::optional<std::string>& note) {
    validateVersionId(child);
    if (parent.has_value()) {
        validateVersionId(*parent);
        if (*parent == child) {
            throw std::invalid_argument("A version cannot be its own parent");
        }
    }
    validateRequiredText(reason, "Lineage write reason");

    const std::string actor = common::system_user();
    const std::string timestamp = common::utc_now();
    sqlite3* db = impl_->db();
    Transaction transaction(db);
    const std::int64_t edge_id = impl_->nextEdgeId();

    Statement insert(db, R"sql(
        INSERT INTO lineage_edge(
            edge_id, child, parent, actor, created_at, origin, note,
            superseded_by, removed)
        VALUES(?, ?, ?, ?, ?, 'create', ?, NULL, 0);
    )sql");
    sqlite3_bind_int64(insert.get(), 1, edge_id);
    bindText(db, insert.get(), 2, child);
    bindOptionalText(db, insert.get(), 3, parent);
    bindText(db, insert.get(), 4, actor);
    bindText(db, insert.get(), 5, timestamp);
    bindOptionalText(db, insert.get(), 6, note);
    expectDone(db, insert.get());

    LedgerEvent edge_event;
    edge_event.event_type = "add_edge";
    edge_event.edge_id = edge_id;
    edge_event.child = child;
    edge_event.parent = parent;
    edge_event.actor = actor;
    edge_event.ts = timestamp;
    edge_event.origin = "create";
    edge_event.note = note;
    edge_event.removed = 0;
    impl_->appendLedger(edge_event);

    const std::int64_t audit_seq =
        insertAudit(db, timestamp, actor, "create", child, std::nullopt, parent, reason);
    impl_->appendLedger(
        auditEvent(audit_seq, timestamp, actor, "create", child, std::nullopt, parent, reason));

    impl_->secureDatabaseFiles();
    transaction.commit();
}

void Lineage::resolveParent(const std::string& child,
                            const std::optional<std::string>& new_parent,
                            const std::string& reason,
                            const std::optional<std::string>& note) {
    validateVersionId(child);
    if (new_parent.has_value()) {
        validateVersionId(*new_parent);
        if (*new_parent == child) {
            throw std::invalid_argument("A version cannot be its own parent");
        }
    }
    validateRequiredText(reason, "Lineage write reason");
    if (!note.has_value()) {
        throw std::invalid_argument("resolveParent requires a user decision note");
    }
    validateRequiredText(*note, "resolveParent note");

    sqlite3* db = impl_->db();
    Transaction transaction(db);
    Statement select(db, R"sql(
        SELECT edge_id, parent FROM lineage_edge
        WHERE child = ? AND superseded_by IS NULL AND removed = 0;
    )sql");
    bindText(db, select.get(), 1, child);
    const int select_rc = sqlite3_step(select.get());
    if (select_rc == SQLITE_DONE) {
        throw std::runtime_error("Cannot resolve parent for an unknown version: " + child);
    }
    if (select_rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
    }
    const std::int64_t old_edge_id = sqlite3_column_int64(select.get(), 0);
    const std::optional<std::string> old_parent = optionalColumn(select.get(), 1);
    const std::int64_t new_edge_id = impl_->nextEdgeId();
    const std::string actor = common::system_user();
    const std::string timestamp = common::utc_now();

    // The partial unique index requires retiring the old active edge before the
    // replacement insert. Both changes are hidden inside one IMMEDIATE transaction.
    Statement supersede(db,
                        "UPDATE lineage_edge SET superseded_by = ? WHERE edge_id = ?;");
    sqlite3_bind_int64(supersede.get(), 1, new_edge_id);
    sqlite3_bind_int64(supersede.get(), 2, old_edge_id);
    expectDone(db, supersede.get());
    LedgerEvent supersede_event;
    supersede_event.event_type = "set_superseded";
    supersede_event.edge_id = old_edge_id;
    supersede_event.superseded_by = new_edge_id;
    impl_->appendLedger(supersede_event);

    Statement insert(db, R"sql(
        INSERT INTO lineage_edge(
            edge_id, child, parent, actor, created_at, origin, note,
            superseded_by, removed)
        VALUES(?, ?, ?, ?, ?, 'reconcile', ?, NULL, 0);
    )sql");
    sqlite3_bind_int64(insert.get(), 1, new_edge_id);
    bindText(db, insert.get(), 2, child);
    bindOptionalText(db, insert.get(), 3, new_parent);
    bindText(db, insert.get(), 4, actor);
    bindText(db, insert.get(), 5, timestamp);
    bindOptionalText(db, insert.get(), 6, note);
    expectDone(db, insert.get());
    LedgerEvent edge_event;
    edge_event.event_type = "add_edge";
    edge_event.edge_id = new_edge_id;
    edge_event.child = child;
    edge_event.parent = new_parent;
    edge_event.actor = actor;
    edge_event.ts = timestamp;
    edge_event.origin = "reconcile";
    edge_event.note = note;
    edge_event.removed = 0;
    impl_->appendLedger(edge_event);

    const std::int64_t audit_seq = insertAudit(db, timestamp, actor,
                                               "reconcile_resolve", child,
                                               old_parent, new_parent, reason);
    impl_->appendLedger(auditEvent(audit_seq, timestamp, actor,
                                   "reconcile_resolve", child,
                                   old_parent, new_parent, reason));
    impl_->secureDatabaseFiles();
    transaction.commit();
}

std::optional<std::string> Lineage::parentOf(const std::string& child) const {
    validateVersionId(child);
    const auto entry = activeEntry(impl_->db(), child);
    return entry.has_value() ? entry->parent : std::nullopt;
}

std::vector<std::string> Lineage::ancestry(const std::string& child) const {
    validateVersionId(child);
    std::vector<std::string> result;
    std::unordered_set<std::string> visited{child};
    std::string current = child;
    while (true) {
        const auto entry = activeEntry(impl_->db(), current);
        if (!entry.has_value() || !entry->parent.has_value()) {
            return result;
        }
        if (!visited.insert(*entry->parent).second) {
            throw std::runtime_error("Cycle detected in Lineage ancestry for: " + child);
        }
        result.push_back(*entry->parent);
        current = *entry->parent;
    }
}

std::string Lineage::ancestryJson(const std::string& child) const {
    validateVersionId(child);
    std::vector<LineageEntry> entries;
    std::unordered_set<std::string> visited;
    std::string current = child;
    while (true) {
        if (!visited.insert(current).second) {
            throw std::runtime_error("Cycle detected in Lineage ancestry for: " + child);
        }
        const auto entry = activeEntry(impl_->db(), current);
        if (!entry.has_value()) {
            throw std::runtime_error("Lineage ancestry references an unknown version: " + current);
        }
        entries.push_back(*entry);
        if (!entry->parent.has_value()) {
            break;
        }
        current = *entry->parent;
    }

    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& entry = entries[index];
        output << "{\"version\":\"" << jsonEscape(entry.child) << "\",\"parent\":";
        appendJsonString(output, entry.parent);
        output << ",\"actor\":\"" << jsonEscape(entry.actor)
               << "\",\"created_at\":\"" << jsonEscape(entry.created_at)
               << "\",\"origin\":\"" << jsonEscape(entry.origin) << "\",\"note\":";
        appendJsonString(output, entry.note);
        output << '}';
    }
    output << ']';
    return output.str();
}

bool Lineage::hasDescendants(const std::string& target) const {
    validateVersionId(target);
    sqlite3* db = impl_->db();
    Statement statement(db, R"sql(
        SELECT 1 FROM lineage_edge
        WHERE parent = ? AND superseded_by IS NULL AND removed = 0 LIMIT 1;
    )sql");
    bindText(db, statement.get(), 1, target);
    const int rc = sqlite3_step(statement.get());
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
}

std::vector<std::string> Lineage::directChildrenOf(const std::string& target) const {
    validateVersionId(target);
    sqlite3* db = impl_->db();
    Statement statement(db, R"sql(
        SELECT child FROM lineage_edge
        WHERE parent = ? AND superseded_by IS NULL AND removed = 0
        ORDER BY child;
    )sql");
    bindText(db, statement.get(), 1, target);
    std::vector<std::string> children;
    while (true) {
        const int rc = sqlite3_step(statement.get());
        if (rc == SQLITE_DONE) return children;
        if (rc != SQLITE_ROW) {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
        }
        children.push_back(requiredColumn(statement.get(), 0));
    }
}

void Lineage::removeEntry(const std::string& child, const std::string& reason) {
    validateVersionId(child);
    validateRequiredText(reason, "Lineage write reason");
    sqlite3* db = impl_->db();
    Transaction transaction(db);

    Statement descendants(db, R"sql(
        SELECT 1 FROM lineage_edge
        WHERE parent = ? AND superseded_by IS NULL AND removed = 0 LIMIT 1;
    )sql");
    bindText(db, descendants.get(), 1, child);
    if (sqlite3_step(descendants.get()) == SQLITE_ROW) {
        throw std::runtime_error("Cannot remove a Lineage entry that has descendants: " + child);
    }

    Statement select(db, R"sql(
        SELECT edge_id, parent FROM lineage_edge
        WHERE child = ? AND superseded_by IS NULL AND removed = 0;
    )sql");
    bindText(db, select.get(), 1, child);
    const int rc = sqlite3_step(select.get());
    if (rc == SQLITE_DONE) {
        throw std::runtime_error("Cannot remove an unknown Lineage entry: " + child);
    }
    if (rc != SQLITE_ROW) {
        throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
    }
    const std::int64_t edge_id = sqlite3_column_int64(select.get(), 0);
    const std::optional<std::string> old_parent = optionalColumn(select.get(), 1);

    Statement mark_removed(db, "UPDATE lineage_edge SET removed = 1 WHERE edge_id = ?;");
    sqlite3_bind_int64(mark_removed.get(), 1, edge_id);
    expectDone(db, mark_removed.get());
    LedgerEvent removed_event;
    removed_event.event_type = "mark_removed";
    removed_event.edge_id = edge_id;
    removed_event.removed = 1;
    impl_->appendLedger(removed_event);

    const std::string actor = common::system_user();
    const std::string timestamp = common::utc_now();
    const std::int64_t audit_seq =
        insertAudit(db, timestamp, actor, "remove", child, old_parent, std::nullopt, reason);
    impl_->appendLedger(auditEvent(audit_seq, timestamp, actor, "remove", child,
                                   old_parent, std::nullopt, reason));
    impl_->secureDatabaseFiles();
    transaction.commit();
}

std::vector<AuditRecord> Lineage::auditTrail(
    const std::optional<std::string>& child) const {
    if (child.has_value()) validateVersionId(*child);
    sqlite3* db = impl_->db();
    Statement statement(db, child.has_value()
                                ? "SELECT seq, ts, actor, action, child, old_parent, new_parent, "
                                  "reason FROM lineage_audit WHERE child = ? ORDER BY seq;"
                                : "SELECT seq, ts, actor, action, child, old_parent, new_parent, "
                                  "reason FROM lineage_audit ORDER BY seq;");
    if (child.has_value()) bindText(db, statement.get(), 1, *child);
    std::vector<AuditRecord> entries;
    while (true) {
        const int rc = sqlite3_step(statement.get());
        if (rc == SQLITE_DONE) return entries;
        if (rc != SQLITE_ROW) {
            throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(db));
        }
        entries.push_back(AuditRecord{
            sqlite3_column_int64(statement.get(), 0), requiredColumn(statement.get(), 1),
            requiredColumn(statement.get(), 2), requiredColumn(statement.get(), 3),
            requiredColumn(statement.get(), 4), optionalColumn(statement.get(), 5),
            optionalColumn(statement.get(), 6), optionalColumn(statement.get(), 7)});
    }
}

IntegrityResult Lineage::verifyIntegrity() const {
    struct ExpectedEdge {
        std::int64_t edge_id;
        std::string child;
        std::optional<std::string> parent;
        std::string actor;
        std::string created_at;
        std::string origin;
        std::optional<std::string> note;
        std::optional<std::int64_t> superseded_by;
        std::int64_t removed;
        std::int64_t fact_seq;
        std::int64_t supersede_seq;
        std::int64_t removed_seq;
    };
    struct ExpectedAudit {
        LineageAuditEntry entry;
        std::int64_t ledger_seq;
    };

    sqlite3* db = impl_->db();
    Statement ledger(db, R"sql(
        SELECT seq, event_type, edge_id, child, parent, actor, ts, origin, note,
               superseded_by, removed, audit_seq, action, old_parent, new_parent,
               reason, prev_hash, entry_hash
        FROM lineage_ledger ORDER BY seq;
    )sql");
    std::map<std::int64_t, ExpectedEdge> expected_edges;
    std::map<std::int64_t, ExpectedAudit> expected_audits;
    std::int64_t expected_seq = 1;
    std::string expected_prev = kGenesisHash;
    while (true) {
        const int rc = sqlite3_step(ledger.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {false, std::nullopt, "Unable to read lineage ledger"};
        }
        LedgerEvent event;
        event.seq = sqlite3_column_int64(ledger.get(), 0);
        event.event_type = requiredColumn(ledger.get(), 1);
        event.edge_id = optionalInt64Column(ledger.get(), 2);
        event.child = optionalColumn(ledger.get(), 3);
        event.parent = optionalColumn(ledger.get(), 4);
        event.actor = optionalColumn(ledger.get(), 5);
        event.ts = optionalColumn(ledger.get(), 6);
        event.origin = optionalColumn(ledger.get(), 7);
        event.note = optionalColumn(ledger.get(), 8);
        event.superseded_by = optionalInt64Column(ledger.get(), 9);
        event.removed = optionalInt64Column(ledger.get(), 10);
        event.audit_seq = optionalInt64Column(ledger.get(), 11);
        event.action = optionalColumn(ledger.get(), 12);
        event.old_parent = optionalColumn(ledger.get(), 13);
        event.new_parent = optionalColumn(ledger.get(), 14);
        event.reason = optionalColumn(ledger.get(), 15);
        event.prev_hash = requiredColumn(ledger.get(), 16);
        event.entry_hash = requiredColumn(ledger.get(), 17);

        if (event.seq != expected_seq) {
            return {false, expected_seq, "Lineage ledger sequence is not contiguous"};
        }
        if (event.prev_hash != expected_prev) {
            return {false, event.seq, "Lineage ledger previous hash does not match"};
        }
        if (ledgerHash(event) != event.entry_hash) {
            return {false, event.seq, "Lineage ledger entry hash does not match"};
        }

        if (event.event_type == "add_edge") {
            if (!event.edge_id || !event.child || !event.actor || !event.ts ||
                !event.origin || !event.removed || *event.removed != 0 ||
                expected_edges.count(*event.edge_id) != 0) {
                return {false, event.seq, "Invalid add_edge ledger event"};
            }
            expected_edges.emplace(*event.edge_id,
                                   ExpectedEdge{*event.edge_id, *event.child, event.parent,
                                                *event.actor, *event.ts, *event.origin,
                                                event.note, std::nullopt, 0,
                                                event.seq, event.seq, event.seq});
        } else if (event.event_type == "set_superseded") {
            if (!event.edge_id || !event.superseded_by ||
                expected_edges.count(*event.edge_id) == 0) {
                return {false, event.seq, "Invalid set_superseded ledger event"};
            }
            auto& edge = expected_edges.at(*event.edge_id);
            edge.superseded_by = event.superseded_by;
            edge.supersede_seq = event.seq;
        } else if (event.event_type == "mark_removed") {
            if (!event.edge_id || !event.removed || *event.removed != 1 ||
                expected_edges.count(*event.edge_id) == 0) {
                return {false, event.seq, "Invalid mark_removed ledger event"};
            }
            auto& edge = expected_edges.at(*event.edge_id);
            edge.removed = 1;
            edge.removed_seq = event.seq;
        } else if (event.event_type == "audit") {
            if (!event.audit_seq || !event.ts || !event.actor || !event.action ||
                !event.child || !event.reason ||
                expected_audits.count(*event.audit_seq) != 0) {
                return {false, event.seq, "Invalid audit ledger event"};
            }
            expected_audits.emplace(
                *event.audit_seq,
                ExpectedAudit{LineageAuditEntry{*event.audit_seq, *event.ts, *event.actor,
                                                *event.action, *event.child, event.old_parent,
                                                event.new_parent, event.reason},
                              event.seq});
        } else {
            return {false, event.seq, "Unknown lineage ledger event type"};
        }
        expected_prev = event.entry_hash;
        ++expected_seq;
    }

    Statement edges(db, R"sql(
        SELECT edge_id, child, parent, actor, created_at, origin, note,
               superseded_by, removed
        FROM lineage_edge ORDER BY edge_id;
    )sql");
    std::size_t edge_count = 0;
    while (true) {
        const int rc = sqlite3_step(edges.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {false, std::nullopt, "Unable to read lineage edges"};
        }
        ++edge_count;
        const std::int64_t id = sqlite3_column_int64(edges.get(), 0);
        const auto found = expected_edges.find(id);
        if (found == expected_edges.end()) {
            return {false, std::nullopt, "Lineage edge has no ledger event"};
        }
        const ExpectedEdge& expected = found->second;
        if (requiredColumn(edges.get(), 1) != expected.child ||
            !sameOptional(optionalColumn(edges.get(), 2), expected.parent) ||
            requiredColumn(edges.get(), 3) != expected.actor ||
            requiredColumn(edges.get(), 4) != expected.created_at ||
            requiredColumn(edges.get(), 5) != expected.origin ||
            !sameOptional(optionalColumn(edges.get(), 6), expected.note)) {
            return {false, expected.fact_seq, "Lineage edge fact differs from ledger"};
        }
        if (optionalInt64Column(edges.get(), 7) != expected.superseded_by) {
            return {false, expected.supersede_seq,
                    "Lineage supersede pointer differs from ledger"};
        }
        if (sqlite3_column_int64(edges.get(), 8) != expected.removed) {
            return {false, expected.removed_seq, "Lineage removed marker differs from ledger"};
        }
    }
    if (edge_count != expected_edges.size()) {
        for (const auto& [id, edge] : expected_edges) {
            (void)id;
            Statement exists(db, "SELECT 1 FROM lineage_edge WHERE edge_id = ?;");
            sqlite3_bind_int64(exists.get(), 1, edge.edge_id);
            if (sqlite3_step(exists.get()) != SQLITE_ROW) {
                return {false, edge.fact_seq, "Lineage edge recorded by ledger is missing"};
            }
        }
    }

    Statement audits(db, R"sql(
        SELECT seq, ts, actor, action, child, old_parent, new_parent, reason
        FROM lineage_audit ORDER BY seq;
    )sql");
    std::size_t audit_count = 0;
    while (true) {
        const int rc = sqlite3_step(audits.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {false, std::nullopt, "Unable to read lineage audit"};
        }
        ++audit_count;
        const std::int64_t seq = sqlite3_column_int64(audits.get(), 0);
        const auto found = expected_audits.find(seq);
        if (found == expected_audits.end()) {
            return {false, std::nullopt, "Lineage audit has no ledger event"};
        }
        const auto& expected = found->second.entry;
        if (requiredColumn(audits.get(), 1) != expected.ts ||
            requiredColumn(audits.get(), 2) != expected.actor ||
            requiredColumn(audits.get(), 3) != expected.action ||
            requiredColumn(audits.get(), 4) != expected.child ||
            optionalColumn(audits.get(), 5) != expected.old_parent ||
            optionalColumn(audits.get(), 6) != expected.new_parent ||
            optionalColumn(audits.get(), 7) != expected.reason) {
            return {false, found->second.ledger_seq,
                    "Lineage audit differs from ledger"};
        }
    }
    if (audit_count != expected_audits.size()) {
        for (const auto& [seq, expected] : expected_audits) {
            Statement exists(db, "SELECT 1 FROM lineage_audit WHERE seq = ?;");
            sqlite3_bind_int64(exists.get(), 1, seq);
            if (sqlite3_step(exists.get()) != SQLITE_ROW) {
                return {false, expected.ledger_seq,
                        "Lineage audit recorded by ledger is missing"};
            }
        }
    }
    return {true, std::nullopt, "Lineage ledger and projections are consistent"};
}

}  // namespace isvn::version
