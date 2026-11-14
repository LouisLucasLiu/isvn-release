#include "version/lineage.h"

#include <sqlite3.h>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using isvn::version::AuditRecord;
using isvn::version::IntegrityResult;
using isvn::version::Lineage;

constexpr const char* kRoot = "IMPL/cpu/FLOORPLAN/v1";
constexpr const char* kChild = "IMPL/cpu/FLOORPLAN/v2";
constexpr const char* kGrandchild = "IMPL/cpu/PLACE/v3";
constexpr const char* kGreatGrandchild = "IMPL/cpu/CTS/v4";

// Interface guard: the public write contract accepts only explicit
// version IDs, user reasons, and notes. It has no SVN/copy-from/inference input.
using RecordParentSignature = void (Lineage::*)(
    const std::string&, const std::optional<std::string>&, const std::string&,
    const std::optional<std::string>&);
using ResolveParentSignature = void (Lineage::*)(
    const std::string&, const std::optional<std::string>&, const std::string&,
    const std::optional<std::string>&);
using AncestryJsonSignature = std::string (Lineage::*)(const std::string&) const;
using AuditTrailSignature =
    std::vector<AuditRecord> (Lineage::*)(const std::optional<std::string>&) const;
static_assert(std::is_same_v<decltype(&Lineage::recordParent), RecordParentSignature>);
static_assert(std::is_same_v<decltype(&Lineage::resolveParent), ResolveParentSignature>);
static_assert(std::is_same_v<decltype(&Lineage::ancestryJson), AncestryJsonSignature>);
static_assert(std::is_same_v<decltype(&Lineage::auditTrail), AuditTrailSignature>);

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/isvn-lineage-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path database() const { return path_ / ".isvn" / "lineage.db"; }

private:
    std::filesystem::path path_;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("test could not open lineage.db");
        }
        sqlite3_busy_timeout(db_, 5000);
    }
    ~Database() { sqlite3_close(db_); }
    sqlite3* get() const { return db_; }
    void execute(const std::string& sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("test SQL failed: " + message);
        }
    }
    std::int64_t integer(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("test SQL prepare failed");
        }
        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL did not return a row");
        }
        const auto result = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return result;
    }
    std::string text(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error("test SQL prepare failed");
        }
        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_ROW || sqlite3_column_type(statement, 0) == SQLITE_NULL) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL did not return text");
        }
        const auto* value = sqlite3_column_text(statement, 0);
        const std::string result(reinterpret_cast<const char*>(value));
        sqlite3_finalize(statement);
        return result;
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

void testRecordAndRootParent() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent("ROOT", std::nullopt, "init");
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    require(!lineage.parentOf("ROOT").has_value(), "ROOT sentinel must have no parent");
    require(!lineage.parentOf(kRoot).has_value(), "root must have no parent");
    require(lineage.parentOf(kChild) == std::optional<std::string>(kRoot),
            "child parent must match");
    require(lineage.verifyIntegrity().ok, "normal writes must pass integrity verification");
}

void testDuplicateRecordFails() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    requireThrows([&] { lineage.recordParent(kRoot, std::nullopt, "user_create"); },
                  "duplicate recordParent must fail");
    require(lineage.auditTrail().size() == 1, "failed duplicate must not add audit");
}

void testAncestryOrder() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    lineage.recordParent(kGrandchild, std::string(kChild), "user_create");
    lineage.recordParent(kGreatGrandchild, std::string(kGrandchild), "user_create");
    require(lineage.ancestry(kGreatGrandchild) ==
                std::vector<std::string>({kGrandchild, kChild, kRoot}),
            "ancestry must run from direct parent to root");
}

void testAncestryCycleFails() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    Database db(project.database());
    db.execute("DROP TRIGGER lineage_edge_facts_immutable;");
    db.execute("UPDATE lineage_edge SET parent = 'IMPL/cpu/FLOORPLAN/v2' "
               "WHERE child = 'IMPL/cpu/FLOORPLAN/v1';");
    requireThrows([&] { static_cast<void>(lineage.ancestry(kChild)); },
                  "ancestry must detect a corrupt cycle");
}

void testDescendantQueries() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    lineage.recordParent(kGrandchild, std::string(kRoot), "user_create");
    require(lineage.hasDescendants(kRoot), "root must report descendants");
    require(!lineage.hasDescendants(kChild), "leaf must not report descendants");
    require(lineage.directChildrenOf(kRoot) ==
                std::vector<std::string>({kChild, kGrandchild}),
            "direct children must be deterministic");
}

void testSupersedePreservesFacts() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create",
                         std::string("original"));
    lineage.recordParent(kGrandchild, std::nullopt, "user_create");
    Database db(project.database());
    const std::string old_actor = db.text(
        "SELECT actor FROM lineage_edge WHERE child='IMPL/cpu/FLOORPLAN/v2';");
    const std::string old_time = db.text(
        "SELECT created_at FROM lineage_edge WHERE child='IMPL/cpu/FLOORPLAN/v2';");

    lineage.resolveParent(kChild, std::string(kGrandchild), "user_reconcile",
                          std::string("member-approved correction"));
    require(lineage.parentOf(kChild) == std::optional<std::string>(kGrandchild),
            "resolved parent must be active");
    require(db.integer("SELECT COUNT(*) FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2';") == 2,
            "old and new edges must both remain");
    require(db.integer("SELECT COUNT(*) FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2' AND superseded_by IS NULL "
                       "AND removed=0;") == 1,
            "child must have exactly one active edge");
    require(db.text("SELECT parent FROM lineage_edge WHERE child="
                    "'IMPL/cpu/FLOORPLAN/v2' ORDER BY edge_id LIMIT 1;") == kRoot,
            "old parent fact must remain unchanged");
    require(db.text("SELECT actor FROM lineage_edge WHERE child="
                    "'IMPL/cpu/FLOORPLAN/v2' ORDER BY edge_id LIMIT 1;") == old_actor,
            "old actor fact must remain unchanged");
    require(db.text("SELECT created_at FROM lineage_edge WHERE child="
                    "'IMPL/cpu/FLOORPLAN/v2' ORDER BY edge_id LIMIT 1;") == old_time,
            "old timestamp fact must remain unchanged");
    require(db.integer("SELECT superseded_by FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2' ORDER BY edge_id LIMIT 1;") ==
                db.integer("SELECT edge_id FROM lineage_edge WHERE child="
                           "'IMPL/cpu/FLOORPLAN/v2' ORDER BY edge_id DESC LIMIT 1;"),
            "old edge must point to replacement");
    require(lineage.verifyIntegrity().ok, "supersede ledger must verify");
}

void testMultipleResolveKeepsHistory() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kGrandchild, std::nullopt, "user_create");
    lineage.recordParent(kGreatGrandchild, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    lineage.resolveParent(kChild, std::string(kGrandchild), "user_reconcile",
                          std::string("first decision"));
    lineage.resolveParent(kChild, std::string(kGreatGrandchild), "user_reconcile",
                          std::string("second decision"));
    Database db(project.database());
    require(db.integer("SELECT COUNT(*) FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2';") == 3,
            "multiple resolves must retain every edge");
    require(db.integer("SELECT COUNT(*) FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2' AND superseded_by IS NULL "
                       "AND removed=0;") == 1,
            "multiple resolves must leave one active edge");
    require(lineage.parentOf(kChild) == std::optional<std::string>(kGreatGrandchild),
            "latest explicit decision must be active");
    require(lineage.auditTrail(std::string(kChild)).size() == 3,
            "each decision must have an audit entry");
}

void testAncestryJsonIsDeterministicAndReadOnly() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create",
                         std::string("root \"note\""));
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    const std::string first = lineage.ancestryJson(kChild);
    const std::string second = lineage.ancestryJson(kChild);
    require(first == second, "ancestry JSON must be deterministic");
    require(first.find("[{\"version\":\"IMPL/cpu/FLOORPLAN/v2\"") == 0,
            "JSON must start with the requested child");
    require(first.find("\"parent\":null") != std::string::npos,
            "root parent must be JSON null");
    require(first.find("root \\\"note\\\"") != std::string::npos,
            "JSON strings must be escaped");
    for (const std::string forbidden : {"svn", "copy-from", "inferred", "hash"}) {
        require(first.find(forbidden) == std::string::npos,
                "ancestry JSON must not expose forbidden inferred fields");
    }
}

void testRemoveMarksHistoryAndAudits() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    requireThrows([&] { lineage.removeEntry(kRoot, "user_delete"); },
                  "remove must reject entries with descendants");
    lineage.removeEntry(kChild, "user_delete");
    require(!lineage.parentOf(kChild).has_value(), "removed edge must not be active");
    Database db(project.database());
    require(db.integer("SELECT COUNT(*) FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2';") == 1,
            "remove must not delete edge history");
    require(db.integer("SELECT removed FROM lineage_edge WHERE child="
                       "'IMPL/cpu/FLOORPLAN/v2';") == 1,
            "remove must set the marker");
    const auto audit = lineage.auditTrail();
    require(audit.size() == 3 && audit.back().action == "remove",
            "remove must append an audit");
    require(lineage.verifyIntegrity().ok, "remove marker ledger must verify");
}

void testAuditTrailRecordsAndReadOnlyFilter() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    lineage.recordParent(kGrandchild, std::nullopt, "user_create");
    lineage.resolveParent(kChild, std::string(kGrandchild), "user_reconcile",
                          std::string("member-approved correction"));
    lineage.removeEntry(kChild, "user_delete");

    Database db(project.database());
    const std::int64_t edge_count_before =
        db.integer("SELECT COUNT(*) FROM lineage_edge;");
    const std::int64_t audit_count_before =
        db.integer("SELECT COUNT(*) FROM lineage_audit;");
    const std::int64_t ledger_count_before =
        db.integer("SELECT COUNT(*) FROM lineage_ledger;");

    const std::vector<AuditRecord> all = lineage.auditTrail();
    const std::vector<AuditRecord> child_only = lineage.auditTrail(std::string(kChild));

    require(db.integer("SELECT COUNT(*) FROM lineage_edge;") == edge_count_before,
            "auditTrail must not modify lineage_edge");
    require(db.integer("SELECT COUNT(*) FROM lineage_audit;") == audit_count_before,
            "auditTrail must not modify lineage_audit");
    require(db.integer("SELECT COUNT(*) FROM lineage_ledger;") == ledger_count_before,
            "auditTrail must not modify lineage_ledger");

    require(all.size() == 5, "auditTrail must list all audit rows");
    require(child_only.size() == 3, "auditTrail child filter must return only that child");
    for (const AuditRecord& record : child_only) {
        require(record.child == kChild, "filtered audit record must match requested child");
    }

    require(child_only[0].seq < child_only[1].seq &&
                child_only[1].seq < child_only[2].seq,
            "auditTrail must be ordered by seq");
    require(child_only[0].action == "create", "first child audit must be create");
    require(!child_only[0].old_parent.has_value(),
            "create old_parent must be null");
    require(child_only[0].new_parent == std::optional<std::string>(kRoot),
            "create new_parent must be the explicit parent");
    require(child_only[0].reason == std::optional<std::string>("user_create"),
            "create reason must come from lineage_audit");

    require(child_only[1].action == "reconcile_resolve",
            "second child audit must be reconcile_resolve");
    require(child_only[1].old_parent == std::optional<std::string>(kRoot),
            "reconcile old_parent must be preserved");
    require(child_only[1].new_parent == std::optional<std::string>(kGrandchild),
            "reconcile new_parent must be preserved");
    require(child_only[1].reason == std::optional<std::string>("user_reconcile"),
            "reconcile reason must come from lineage_audit");

    require(child_only[2].action == "remove", "third child audit must be remove");
    require(child_only[2].old_parent == std::optional<std::string>(kGrandchild),
            "remove old_parent must be the active parent before remove");
    require(!child_only[2].new_parent.has_value(),
            "remove new_parent must be null");
    require(child_only[2].reason == std::optional<std::string>("user_delete"),
            "remove reason must come from lineage_audit");

    for (const AuditRecord& record : all) {
        require(!record.ts.empty(), "audit timestamp must be populated");
        require(!record.actor.empty(), "audit actor must be populated");
    }
}

void testActorAndInvalidInputs() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    const passwd* entry = getpwuid(geteuid());
    require(entry != nullptr && entry->pw_name != nullptr, "test needs system identity");
    require(lineage.auditTrail().front().actor == entry->pw_name,
            "actor must be the effective system user");
    requireThrows<std::invalid_argument>(
        [&] { lineage.recordParent("IMPL/cpu/v1", std::nullopt, "user_create"); },
        "three segments must fail");
    requireThrows<std::invalid_argument>(
        [&] { lineage.recordParent("IMPL/cpu core/STAGE/v1", std::nullopt,
                                   "user_create"); },
        "whitespace must fail");
    requireThrows<std::invalid_argument>(
        [&] { lineage.recordParent("IMPL/../STAGE/v1", std::nullopt, "user_create"); },
        "path traversal must fail");
    requireThrows<std::invalid_argument>(
        [&] { lineage.resolveParent(kRoot, std::nullopt, "user_reconcile", std::nullopt); },
        "reconcile must require a user decision note");
}

void testSharedPermissions() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    struct stat directory_status {};
    struct stat database_status {};
    require(::stat((project.path() / ".isvn").c_str(), &directory_status) == 0,
            ".isvn must exist");
    require(::stat(project.database().c_str(), &database_status) == 0,
            "lineage.db must exist");
    require((directory_status.st_mode & 07777) == 02770,
            ".isvn must be setgid and group writable");
    require((database_status.st_mode & 0777) == 0660,
            "lineage.db must be group readable and writable");
    for (const char* suffix : {"-wal", "-shm"}) {
        const std::filesystem::path auxiliary = project.database().string() + suffix;
        if (std::filesystem::exists(auxiliary)) {
            struct stat status {};
            require(::stat(auxiliary.c_str(), &status) == 0, "auxiliary file must be statable");
            require((status.st_mode & 0777) == 0660,
                    "SQLite auxiliary file must be group writable");
        }
    }
}

void testIntegrityDetectsEdgeTampering() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    Database db(project.database());
    const std::int64_t child_add_seq = db.integer(
        "SELECT seq FROM lineage_ledger WHERE event_type='add_edge' AND child="
        "'IMPL/cpu/FLOORPLAN/v2';");
    db.execute("DROP TRIGGER lineage_edge_facts_immutable;");
    db.execute("UPDATE lineage_edge SET parent='IMPL/cpu/PLACE/v3' WHERE child="
               "'IMPL/cpu/FLOORPLAN/v2';");
    const IntegrityResult result = lineage.verifyIntegrity();
    require(!result.ok, "direct edge tampering must fail verification");
    require(result.first_bad_seq == std::optional<std::int64_t>(child_add_seq),
            "tampered edge must identify its originating ledger event");
}

void testIntegrityDetectsLedgerDeletion() {
    TemporaryDirectory project;
    Lineage lineage(project.path());
    lineage.recordParent(kRoot, std::nullopt, "user_create");
    lineage.recordParent(kChild, std::string(kRoot), "user_create");
    Database db(project.database());
    db.execute("DROP TRIGGER lineage_ledger_no_delete;");
    db.execute("DELETE FROM lineage_ledger WHERE seq=2;");
    const IntegrityResult result = lineage.verifyIntegrity();
    require(!result.ok, "deleted ledger event must fail verification");
    require(result.first_bad_seq == std::optional<std::int64_t>(2),
            "ledger gap must report the missing sequence");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"record and root", testRecordAndRootParent},
        {"duplicate record", testDuplicateRecordFails},
        {"ancestry order", testAncestryOrder},
        {"ancestry cycle", testAncestryCycleFails},
        {"descendant queries", testDescendantQueries},
        {"supersede immutability", testSupersedePreservesFacts},
        {"multiple resolve", testMultipleResolveKeepsHistory},
        {"ancestry JSON", testAncestryJsonIsDeterministicAndReadOnly},
        {"remove marker", testRemoveMarksHistoryAndAudits},
        {"audit trail", testAuditTrailRecordsAndReadOnlyFilter},
        {"actor and inputs", testActorAndInvalidInputs},
        {"shared permissions", testSharedPermissions},
        {"edge tampering", testIntegrityDetectsEdgeTampering},
        {"ledger deletion", testIntegrityDetectsLedgerDeletion},
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
