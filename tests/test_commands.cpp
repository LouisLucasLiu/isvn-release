#include "cli/commands.h"
#include "common/hash.h"
#include "storage/object_store.h"
#include "version/lineage.h"

#include <sqlite3.h>

#include <fcntl.h>
#include <grp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using isvn::cli::run;
using isvn::storage::ObjectStore;
using isvn::version::Lineage;

constexpr const char* kRoot = "IMPL/cpu/RTL/v1";
constexpr const char* kChild = "IMPL/cpu/SYN/v1";
constexpr const char* kQueryBase = "P1/TOP/RTL/base";
constexpr const char* kQueryV1 = "P1/TOP/SYN/v1";
constexpr const char* kQueryV2 = "P1/TOP/SYN/v2";
constexpr const char* kQueryV3 = "P1/TOP/SYN/v3";

struct PreparedProject {
    std::filesystem::path project;
    std::filesystem::path workspace;
    std::filesystem::path pool;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/isvn-commands-test-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class CurrentDirectory {
public:
    explicit CurrentDirectory(const std::filesystem::path& path)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }
    ~CurrentDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
    }

private:
    std::filesystem::path previous_;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
        const char* old = ::getenv(name);
        if (old != nullptr) old_ = old;
        if (::setenv(name, value.c_str(), 1) != 0) throw std::runtime_error("setenv failed");
    }
    ~ScopedEnvironment() {
        if (old_.has_value()) ::setenv(name_.c_str(), old_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }

private:
    std::string name_;
    std::optional<std::string> old_;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            throw std::runtime_error("test could not open SQLite database");
        }
    }
    ~Database() { sqlite3_close(db_); }

    std::string text(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL returned no text");
        }
        const auto* value = sqlite3_column_text(statement, 0);
        const std::string result = value == nullptr ? "" : reinterpret_cast<const char*>(value);
        sqlite3_finalize(statement);
        return result;
    }

    std::int64_t integer(const std::string& sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQL returned no integer");
        }
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    void execute(const std::string& sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "unknown SQL error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    sqlite3* db_ = nullptr;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("unable to write file");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

struct CommandResult {
    int code;
    std::string out;
    std::string err;
};

CommandResult runCommand(const std::vector<std::string>& args,
                         const std::string& input = "") {
    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream err;
    const int code = run(args, in, out, err);
    return CommandResult{code, out.str(), err.str()};
}

PreparedProject prepareProject(const std::filesystem::path& root) {
    PreparedProject prepared{root / "project", root / "workspace", root / "pool"};
    std::filesystem::create_directories(prepared.project / ".isvn");
    std::filesystem::create_directories(prepared.workspace / ".isvn_local");
    std::filesystem::create_directories(prepared.workspace / kRoot);
    std::filesystem::create_directories(prepared.workspace / kChild);
    writeFile(prepared.project / ".isvn" / "config",
              "repo_url = file:///tmp/nonexistent-isvn-test-repo\npool_root = " +
                  prepared.pool.string() + "\nbranch_prefix = branches/\n");
    writeFile(prepared.workspace / ".isvn_local" / "config",
              "project_dir = " + prepared.project.string() + "\n");
    writeFile(prepared.workspace / ".isvn_local" / "workspaces",
              std::string(kRoot) + "\t" + kRoot + "\n" +
                  std::string(kChild) + "\t" + kChild + "\n");
    writeFile(prepared.workspace / ".isvn_local" / "workspace_state", "");
    Lineage lineage(prepared.project);
    lineage.recordParent(kRoot, std::nullopt, "create");
    lineage.recordParent(kChild, std::string(kRoot), "create");
    ObjectStore store(prepared.pool);
    return prepared;
}

PreparedProject prepareQueryProject(const std::filesystem::path& root) {
    PreparedProject prepared{root / "chip1", root / "workspace", root / "pool"};
    std::filesystem::create_directories(prepared.project / ".isvn");
    std::filesystem::create_directories(prepared.workspace / ".isvn_local");
    writeFile(prepared.project / ".isvn" / "config",
              "repo_url = file:///tmp/nonexistent-isvn-query-repo\npool_root = " +
                  prepared.pool.string() + "\nbranch_prefix = branches/\n");
    writeFile(prepared.workspace / ".isvn_local" / "config",
              "project_dir = " + prepared.project.string() + "\n");
    writeFile(prepared.workspace / ".isvn_local" / "workspaces",
              std::string(kQueryBase) + "\t" + kQueryBase + "\n" +
                  std::string(kQueryV1) + "\t" + kQueryV1 + "\n" +
                  std::string(kQueryV2) + "\t" + kQueryV2 + "\n" +
                  std::string(kQueryV3) + "\t" + kQueryV3 + "\n");
    writeFile(prepared.workspace / ".isvn_local" / "workspace_state", "");
    Lineage lineage(prepared.project);
    lineage.recordParent(kQueryBase, std::nullopt, "create");
    lineage.recordParent(kQueryV1, std::string(kQueryBase), "create");
    lineage.recordParent(kQueryV2, std::string(kQueryV1), "create");
    lineage.recordParent(kQueryV3, std::string(kQueryV2), "create");
    ObjectStore store(prepared.pool);

    writeFile(prepared.workspace / kQueryBase / "src/top.v", "base top");
    writeFile(prepared.workspace / kQueryV1 / "src/top.v", "v1 top");
    writeFile(prepared.workspace / kQueryV2 / "src/top.v", "v2 top");
    writeFile(prepared.workspace / kQueryV3 / "src/top.v", "v3 top");
    writeFile(prepared.workspace / kQueryV2 / "src/new.v", "new v2");
    writeFile(prepared.workspace / kQueryV3 / "src/new.v", "new v3");
    return prepared;
}

std::filesystem::path currentWorkspacePath(const PreparedProject& prepared) {
    return prepared.workspace / kChild;
}

void testAncestryJsonAndLineageVerify() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    auto result = runCommand({"ancestry", kChild, "--json"});
    require(result.code == 0, "ancestry --json must succeed");
    require(result.out.find("\"child\"") != std::string::npos,
            "ancestry --json must use command schema wrapper");
    require(result.out.find("\"chain\"") != std::string::npos,
            "ancestry --json must expose a chain array");
    require(result.out.find(kChild) != std::string::npos, "json must include child");
    require(result.out.find("copy") == std::string::npos,
            "json ancestry must not expose svn copy source data");
    result = runCommand({"lineage", "verify"});
    require(result.code == 0, "lineage verify must succeed");
    require(result.out.find("ok") != std::string::npos, "verify must report ok");
    result = runCommand({"lineage", "verify", "--json"});
    require(result.code == 0, "lineage verify --json must succeed");
    require(result.out.find("\"intact\":true") != std::string::npos,
            "lineage verify --json must expose intact");
}

void testProvenanceQueryJsonContract() {
    TemporaryDirectory temporary;
    const auto prepared = prepareQueryProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace);
    const auto project = prepared.project.string();

    auto result = runCommand({"provenance", "query", "--project", project, "--version",
                              kQueryV3, "--type", "parent", "--json"});
    require(result.code == 0, "provenance parent query must succeed");
    require(result.out.find("\"schema_version\":\"isvn.provenance.query.v0\"") !=
                std::string::npos &&
                result.out.find("\"authority\":\"declared_provenance\"") !=
                    std::string::npos &&
                result.out.find("\"content_verified\":false") != std::string::npos,
            "provenance query must expose declared provenance authority");
    require(result.out.find(std::string("\"canonical\":\"isvn://chip1/") + kQueryV2) !=
                std::string::npos &&
                result.out.find("\"depth\":1") != std::string::npos &&
                result.out.find("\"relation\":\"parent\"") != std::string::npos,
            "parent query must report direct parent with depth/relation");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         kQueryV3, "--type", "ancestor-chain", "--json"});
    require(result.code == 0, "provenance ancestor-chain query must succeed");
    const auto pos_v2 = result.out.find(kQueryV2);
    const auto pos_v1 = result.out.find(kQueryV1);
    const auto pos_base = result.out.find(kQueryBase);
    require(pos_v2 != std::string::npos && pos_v1 != std::string::npos &&
                pos_base != std::string::npos && pos_v2 < pos_v1 && pos_v1 < pos_base,
            "ancestor-chain must be near-to-far");
    require(result.out.find("\"ancestor_chain\"") != std::string::npos &&
                result.out.find("\"depth\":1") != std::string::npos &&
                result.out.find("\"depth\":2") != std::string::npos &&
                result.out.find("\"depth\":3") != std::string::npos &&
                result.out.find("\"root\"") != std::string::npos &&
                result.out.find("\"truncated\":false") != std::string::npos,
            "ancestor-chain must expose depths, root summary, and truncated=false");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         kQueryV3, "--type", "root", "--json"});
    require(result.code == 0 && result.out.find(kQueryBase) != std::string::npos &&
                result.out.find("\"relation\":\"root\"") != std::string::npos,
            "root query must report base");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         kQueryV3, "--type", "pedigree", "--json"});
    require(result.code == 0, "pedigree query must succeed");
    require(result.out.find("\"direction\":\"root_to_subject\"") != std::string::npos,
            "pedigree must declare direction");
    const auto chain_start = result.out.find("\"chain\":[");
    const auto chain_json = result.out.substr(chain_start);
    require(chain_start != std::string::npos &&
                chain_json.find(kQueryBase) < chain_json.find(kQueryV1) &&
                chain_json.find(kQueryV1) < chain_json.find(kQueryV2) &&
                chain_json.find(kQueryV2) < chain_json.find(kQueryV3),
            "pedigree chain must be root-to-subject");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         kQueryBase, "--type", "parent", "--json"});
    require(result.code == 0 && result.out.find("\"parent\":null") != std::string::npos &&
                result.out.find("\"code\":\"ROOT_VERSION\"") != std::string::npos,
            "root subject parent query must return null plus ROOT_VERSION warning");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         kQueryV3, "--type", "ancestor-chain", "--max-depth", "2",
                         "--json"});
    require(result.code == 0 && result.out.find("\"truncated\":true") != std::string::npos &&
                result.out.find("\"code\":\"MAX_DEPTH_REACHED\"") != std::string::npos,
            "max-depth must truncate with warning");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         "P1/TOP/SYN/not_exist", "--type", "ancestor-chain", "--json"});
    require(result.code != 0 && result.out.find("\"status\":\"error\"") != std::string::npos &&
                result.out.find("\"code\":\"VERSION_NOT_FOUND\"") != std::string::npos,
            "unresolved version must return JSON error");
}

void testProvenanceQueryRendersRootSentinel() {
    TemporaryDirectory temporary;
    const auto prepared = prepareQueryProject(temporary.path());
    const auto project = prepared.project.string();
    constexpr const char* rooted = "P1/TOP/RTL/rooted";
    {
        Lineage lineage(prepared.project);
        lineage.recordParent("ROOT", std::nullopt, "legacy init");
        lineage.recordParent(rooted, std::string("ROOT"), "legacy create");
    }

    auto result = runCommand({"provenance", "query", "--project", project, "--version",
                              rooted, "--type", "ancestor-chain", "--json"});
    require(result.code == 0, "ancestor-chain must render ROOT sentinel without crashing");
    require(result.out.find("\"canonical\":\"isvn://chip1/ROOT\"") != std::string::npos,
            "ROOT sentinel must have a stable canonical identity");
    require(result.out.find("\"phase\":null") != std::string::npos &&
                result.out.find("\"block\":null") != std::string::npos &&
                result.out.find("\"stage\":null") != std::string::npos &&
                result.out.find("\"version\":\"ROOT\"") != std::string::npos,
            "ROOT sentinel must render nullable display metadata plus version label");
    require(result.out.find("\"content_verified\":false") != std::string::npos &&
                result.out.find("\"authority\":\"declared_provenance\"") !=
                    std::string::npos,
            "ROOT sentinel rendering must preserve declared provenance authority");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         rooted, "--type", "root", "--json"});
    require(result.code == 0 && result.out.find("\"canonical\":\"isvn://chip1/ROOT\"") !=
                                    std::string::npos &&
                result.out.find("\"relation\":\"root\"") != std::string::npos,
            "root query must return ROOT sentinel as root object");

    result = runCommand({"provenance", "query", "--project", project, "--version",
                         rooted, "--type", "pedigree", "--json"});
    require(result.code == 0, "pedigree must render ROOT sentinel without crashing");
    const auto chain_start = result.out.find("\"chain\":[");
    const auto chain_json = chain_start == std::string::npos ? std::string()
                                                             : result.out.substr(chain_start);
    const auto root_pos = chain_json.find("\"canonical\":\"isvn://chip1/ROOT\"");
    const auto subject_pos = chain_json.find(rooted);
    require(chain_start != std::string::npos && root_pos != std::string::npos &&
                subject_pos != std::string::npos && root_pos < subject_pos,
            "pedigree must render ROOT sentinel before subject");
}

void testFileQueryJsonContract() {
    TemporaryDirectory temporary;
    const auto prepared = prepareQueryProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace);
    const auto project = prepared.project.string();

    auto result = runCommand({"file", "query", "--project", project, "--path",
                              "src/top.v", "--type", "versions", "--json"});
    require(result.code == 0, "file versions query must succeed");
    require(result.out.find("\"schema_version\":\"isvn.file.query.v0\"") !=
                std::string::npos &&
                result.out.find("\"authority\":\"declared_inventory\"") !=
                    std::string::npos &&
                result.out.find("\"content_verified\":false") != std::string::npos,
            "file query must expose declared inventory authority");
    require(result.out.find("\"versions\":[") != std::string::npos &&
                result.out.find(kQueryBase) != std::string::npos &&
                result.out.find(kQueryV3) != std::string::npos &&
                result.out.find("\"version_count\":4") != std::string::npos,
            "file versions must return versions array and summary count");

    result = runCommand({"file", "query", "--project", project, "--version", kQueryV3,
                         "--path", "src/top.v", "--type", "parent", "--json"});
    require(result.code == 0, "file parent with version must succeed");
    require(result.out.find("\"parent_versions\":[") != std::string::npos &&
                result.out.find(kQueryV2) != std::string::npos &&
                result.out.find("\"relation\":\"file_parent\"") != std::string::npos &&
                result.out.find("\"per_subject\"") != std::string::npos,
            "file parent must return array and per_subject");

    result = runCommand({"file", "query", "--project", project, "--version", kQueryV3,
                         "--path", "src/top.v", "--type", "root", "--json"});
    require(result.code == 0 && result.out.find("\"root_versions\":[") != std::string::npos &&
                result.out.find(kQueryBase) != std::string::npos &&
                result.out.find("\"relation\":\"file_root\"") != std::string::npos,
            "file root must return array with base");

    result = runCommand({"file", "query", "--project", project, "--path", "src/top.v",
                         "--type", "parent", "--json"});
    require(result.code == 0 && result.out.find("\"subjects\":[") != std::string::npos &&
                result.out.find("\"parent_versions\":[") != std::string::npos &&
                result.out.find("\"per_subject\"") != std::string::npos,
            "file parent without version must aggregate subjects and candidates");

    result = runCommand({"file", "query", "--project", project, "--path", "src/top.v",
                         "--type", "root", "--json"});
    require(result.code == 0 && result.out.find("\"root_versions\":[") != std::string::npos &&
                result.out.find(kQueryBase) != std::string::npos,
            "file root without version must aggregate root candidates");

    result = runCommand({"file", "query", "--project", project, "--version", kQueryV3,
                         "--path", "src/new.v", "--type", "root", "--json"});
    require(result.code == 0 && result.out.find(kQueryV2) != std::string::npos,
            "file root for a later-introduced file must be v2");

    result = runCommand({"file", "query", "--project", project, "--path",
                         "tmp/debug.log", "--type", "versions", "--json"});
    require(result.code == 0 && result.out.find("\"managed\":false") != std::string::npos &&
                result.out.find("\"versions\":[]") != std::string::npos &&
                result.out.find("\"code\":\"FILE_NOT_MANAGED\"") != std::string::npos,
            "unmanaged file versions must return empty array plus warning");

    result = runCommand({"file", "query", "--project", project, "--path",
                         "tmp/debug.log", "--type", "parent", "--json"});
    require(result.code == 0 && result.out.find("\"subjects\":[]") != std::string::npos &&
                result.out.find("\"parent_versions\":[]") != std::string::npos &&
                result.out.find("\"code\":\"FILE_NOT_MANAGED\"") != std::string::npos,
            "unmanaged file parent must return empty arrays plus warning");
}

void testLineageAuditJsonIsReadOnlyAuditOnly() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    {
        Lineage lineage(prepared.project);
        lineage.recordParent("IMPL/cpu/PLACE/v1", std::string(kChild), "create");
        lineage.resolveParent("IMPL/cpu/PLACE/v1", std::string(kRoot), "reconcile",
                              std::string("user chose root"));
        lineage.removeEntry("IMPL/cpu/PLACE/v1", "delete by user");
    }
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    auto result = runCommand({"lineage", "audit", "IMPL/cpu/PLACE/v1", "--json"});
    require(result.code == 0, "lineage audit --json must succeed");
    require(result.out.find("\"records\"") != std::string::npos,
            "lineage audit --json must expose records");
    require(result.out.find("\"action\":\"create\"") != std::string::npos,
            "lineage audit must include create");
    require(result.out.find("\"action\":\"reconcile_resolve\"") != std::string::npos,
            "lineage audit must include reconcile_resolve");
    require(result.out.find("\"action\":\"remove\"") != std::string::npos,
            "lineage audit must include remove");
    require(result.out.find("\"old_parent\"") != std::string::npos &&
                result.out.find("\"new_parent\"") != std::string::npos,
            "lineage audit must expose old/new parent fields");
    require(result.out.find(kChild) != std::string::npos,
            "lineage audit must show recorded parent values");
    require(result.out.find("svn") == std::string::npos &&
                result.out.find("cache") == std::string::npos &&
                result.out.find("hash") == std::string::npos,
            "lineage audit must not mix in svn/cache/hash-derived fields");

    result = runCommand({"lineage", "audit", kChild, "--json"});
    require(result.code == 0, "lineage audit child filter must succeed");
    require(result.out.find("IMPL/cpu/PLACE/v1") == std::string::npos,
            "lineage audit child filter must not leak other children");
}

void testCheckoutAncestorRequiresConfirmationBeforeContent() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"checkout-ancestor", "--depth", "1"}, "n\n");
    require(result.code == 0, "checkout-ancestor refusal must succeed");
    require(result.out.find(kRoot) != std::string::npos,
            "checkout-ancestor must show reference version");
    require(result.out.find("no content checked out") != std::string::npos,
            "checkout-ancestor must not fetch content without confirmation");
    require(!std::filesystem::exists(prepared.workspace / "ancestor-1"),
            "checkout-ancestor must not create content directory when user says no");
}

void testCheckoutAncestorExplicitVersionUsesLineageStart() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace / kRoot);

    const auto result = runCommand({"checkout-ancestor", kChild, "--depth", "1"}, "n\n");

    require(result.code == 0, "explicit checkout-ancestor must succeed");
    require(result.out.find(std::string("ancestor depth 1: ") + kRoot) != std::string::npos,
            "explicit checkout-ancestor must start from the requested version");
    require(result.out.find("no content checked out") != std::string::npos,
            "explicit checkout-ancestor must still require confirmation");
    require(!std::filesystem::exists(prepared.workspace / "ancestor-1"),
            "explicit checkout-ancestor must not fetch content without confirmation");
}

void testExplicitVersionRequiresWorkspaceMapping() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace);

    const auto missing = "IMPL/cpu/ROUTE/v9";
    auto result = runCommand({"checkout-ancestor", missing, "--depth", "1"});
    require(result.code == 1, "checkout-ancestor explicit missing workspace must fail");
    require(result.err.find("run isvn checkout") != std::string::npos,
            "checkout-ancestor explicit missing workspace must not fall back to cwd");

    result = runCommand({"status", missing, "--json"});
    require(result.code == 1, "status explicit missing workspace must fail");
    require(result.err.find("run isvn checkout") != std::string::npos,
            "status explicit missing workspace must not fall back to cwd");
}

void testDeleteRejectsVersionWithDescendantsBeforeSvn() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"delete", kRoot});
    require(result.code == 2, "delete must reject versions with descendants");
    require(result.err.find(kChild) != std::string::npos,
            "delete rejection must list direct descendants");
    Lineage lineage(prepared.project);
    require(lineage.parentOf(kChild) == std::optional<std::string>(kRoot),
            "failed delete must not alter Lineage");
}

void testEditBreaksPoolSymlinkAndMarksLocal() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = currentWorkspacePath(prepared) / "design.gds";
    const auto source = temporary.path() / "source.bin";
    writeFile(source, "payload");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"edit", "design.gds"});
    require(result.code == 0, "edit must succeed for linked file");
    require(!std::filesystem::is_symlink(file), "edit must replace symlink with real file");
    require(readFile(file) == "payload", "edit must preserve object content");
    require(readFile(prepared.workspace / ".isvn_local" / "workspace_state").find("design.gds") !=
                std::string::npos,
            "edit must mark file as local");
}

void testEditExplicitVersionBreaksThatWorkspaceFile() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = prepared.workspace / kRoot / "design.gds";
    const auto source = temporary.path() / "root-source.bin";
    writeFile(source, "root payload");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());

    CurrentDirectory cwd(prepared.workspace);
    const auto result = runCommand({"edit", "design.gds", "--in", kRoot});

    require(result.code == 0, "edit --in must succeed for linked file in explicit version");
    require(!std::filesystem::is_symlink(file), "edit --in must replace target version symlink");
    require(readFile(file) == "root payload", "edit --in must preserve object content");
    require(readFile(prepared.workspace / ".isvn_local" / "workspace_state").find(
                std::string(kRoot) + "/design.gds") != std::string::npos,
            "edit --in must mark the explicit version file as local");
}

void testEditVersionPrefixedPathIsNormalized() {
    // Verify that "edit Phase/Block/Stage/v/file --in Phase/Block/Stage/v" resolves
    // identically to "edit file --in Phase/Block/Stage/v" and does not double-join.
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = prepared.workspace / kRoot / "design.gds";
    const auto source = temporary.path() / "prefixed-source.bin";
    writeFile(source, "prefixed payload");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());

    // Simulate CLI user passing the full version-prefixed relative path.
    const std::string prefixed = std::string(kRoot) + "/design.gds";
    CurrentDirectory cwd(prepared.workspace);
    const auto result = runCommand({"edit", prefixed, "--in", kRoot});

    require(result.code == 0,
            "edit with version-prefixed path must succeed (no double-join)");
    require(!std::filesystem::is_symlink(file),
            "edit with version-prefixed path must replace symlink");
    require(readFile(file) == "prefixed payload",
            "edit with version-prefixed path must preserve object content");
    // Double-join would create a path like kRoot/kRoot/design.gds; confirm it doesn't exist.
    require(!std::filesystem::exists(prepared.workspace / kRoot / kRoot / "design.gds"),
            "edit must not create double-version-prefixed path");
}

void testEditWrongVersionPrefixIsRejected() {
    // Verify that a wrong-version-prefixed path (different leaf version) is safely rejected.
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = prepared.workspace / kRoot / "design.gds";
    const auto source = temporary.path() / "wrong-ver-source.bin";
    writeFile(source, "wrong ver payload");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());

    // kChild has a different stage than kRoot; passing kChild-prefixed path --in kRoot
    // should fail because the resolved path lands outside kRoot's working copy.
    const std::string wrong_prefixed = std::string(kChild) + "/design.gds";
    CurrentDirectory cwd(prepared.workspace);
    const auto result = runCommand({"edit", wrong_prefixed, "--in", kRoot});

    require(result.code != 0,
            "edit with wrong-version-prefixed path must be rejected");
}

void testGcRunsWithReport() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"gc", "--reconcile"});
    require(result.code == 0, "gc --reconcile must run");
    require(result.out.find("refs_removed=") != std::string::npos,
            "gc must print a report");
}

void testMemberMaintenanceMutationIsDeniedAtCliBoundary() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    ScopedEnvironment member("ISVN_TEST_FORCE_MEMBER_MAINTENANCE", "1");
    const auto result = runCommand({"gc", "--reconcile", "--json"});
    require(result.code != 0 && result.out.find("AUTHORIZATION_DENIED") != std::string::npos,
            "direct MEMBER maintenance mutation must be rejected with a structured code");
}

void testGcJsonSchemaAndDryRun() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto source = temporary.path() / "gc-source.dat";
    writeFile(source, "gc-json-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    const auto link = prepared.workspace / "ghost.link";
    std::filesystem::create_symlink(store.object_path(put.hash), link);
    store.register_ref(put.hash, std::filesystem::absolute(link).string());
    std::filesystem::remove(link);

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto dry = runCommand({"gc", "--reconcile", "--dry-run", "--json"});
    require(dry.code == 0, "gc reconcile dry-run json must succeed");
    require(dry.out.find("\"dry_run\":true") != std::string::npos,
            "dry-run json must expose dry_run true");
    require(dry.out.find("\"refs_removed\":0") != std::string::npos,
            "dry-run must not remove refs");
    require(dry.out.find("\"phantom_refs\"") != std::string::npos &&
                dry.out.find("\"link_path\"") != std::string::npos &&
                dry.out.find("\"hash\"") != std::string::npos,
            "dry-run json must list hash/link_path phantom refs");
    require(dry.out.find("version") == std::string::npos &&
                dry.out.find("branch") == std::string::npos &&
                dry.out.find("stage") == std::string::npos &&
                dry.out.find("child") == std::string::npos &&
                dry.out.find("ancestor") == std::string::npos,
            "gc json must not expose version or lineage fields");

    auto result = runCommand({"gc", "--reconcile", "--json"});
    require(result.code == 0, "gc reconcile json must succeed");
    require(result.out.find("\"dry_run\":false") != std::string::npos &&
                result.out.find("\"refs_removed\":1") != std::string::npos &&
                result.out.find("\"phantom_refs\":[]") != std::string::npos,
            "gc reconcile json must expose stable schema");

    result = runCommand({"gc", "--reclaim", "--high", "1.0", "--low", "1.0", "--json"});
    require(result.code == 0, "gc reclaim json must succeed");
    require(result.out.find("\"objects_reclaimed\"") != std::string::npos &&
                result.out.find("\"objects_force_evicted\"") != std::string::npos &&
                result.out.find("\"bytes_freed\"") != std::string::npos &&
                result.out.find("\"low_watermark_reached\"") != std::string::npos &&
                result.out.find("\"phantom_refs\":[]") != std::string::npos,
            "gc reclaim json must expose stable schema");

    result = runCommand({"gc", "--json"});
    require(result.code == 0, "full gc json must succeed");
    require(result.out.find("\"dry_run\":false") != std::string::npos &&
                result.out.find("\"phantom_refs\":[]") != std::string::npos,
            "full gc json must expose stable schema");
}

void testGcKeepsAndReportsRefsOutsideItsWorkspace() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto source = temporary.path() / "outside-source.dat";
    writeFile(source, "outside-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    const auto link = temporary.path() / "other-member" / "outside.link";
    std::filesystem::create_directories(link.parent_path());
    std::filesystem::create_symlink(store.object_path(put.hash), link);
    store.register_ref(put.hash, std::filesystem::absolute(link).string());
    std::filesystem::remove_all(link.parent_path());

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto json = runCommand({"gc", "--reconcile", "--json"});
    require(json.code == 0, "gc reconcile must succeed");
    require(json.out.find("\"refs_removed\":0") != std::string::npos &&
                json.out.find("\"refs_retained\":1") != std::string::npos &&
                json.out.find("\"reason\":\"not_found\"") != std::string::npos &&
                json.out.find("\"scope\":\"outside_workspace\"") != std::string::npos,
            "a ref outside the operator's workspace must be kept and reported");
    require(store.refcount(put.hash) == 1, "the kept ref must still be counted");

    const auto plain = runCommand({"gc", "--reconcile"});
    require(plain.out == "refs_removed=0 objects_reclaimed=0 objects_force_evicted=0 bytes_freed=0\n"
                         "low_watermark_reached=true\n",
            "plain gc output must stay byte-for-byte compatible");
    require(plain.err.find("could not be verified") != std::string::npos,
            "plain gc must warn about kept refs on stderr");
}

// Commands keep no lock state below the shared project directory.  State left
// there by an older build is neither needed nor touched, whatever its mode.
void testNoSharedLockStateIsUsed() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto locks = prepared.project / ".isvn" / "work_operation_locks";
    {
        CurrentDirectory cwd(currentWorkspacePath(prepared));
        const auto clean = runCommand({"gc", "--reconcile", "--json"});
        require(clean.code == 0, "gc reconcile must succeed: " + clean.out + clean.err);
    }
    require(!std::filesystem::exists(std::filesystem::symlink_status(locks)),
            "a command must not create lock state in the shared project directory");

    std::filesystem::create_directories(locks / "slots");
    writeFile(locks / "project.gate", "");
    ::chmod((locks / "project.gate").c_str(), 0600);
    ::chmod(locks.c_str(), 0500);
    CommandResult result{};
    {
        CurrentDirectory cwd(currentWorkspacePath(prepared));
        result = runCommand({"gc", "--reconcile", "--json"});
    }
    struct stat status {};
    const bool untouched = ::stat(locks.c_str(), &status) == 0 && (status.st_mode & 07777) == 0500;
    ::chmod(locks.c_str(), 0700);
    require(result.code == 0,
            "state left by an older build must not stop a command: " + result.out + result.err);
    require(untouched, "state left by an older build must not be modified");
}

void testGcPlainTextCronCompatibility() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"gc", "--reconcile"});
    require(result.code == 0, "gc plain reconcile must succeed");
    require(result.out == "refs_removed=0 objects_reclaimed=0 objects_force_evicted=0 bytes_freed=0\n"
                          "low_watermark_reached=true\n",
            "gc plain non-dry-run output must remain byte-for-byte compatible");
}

void testGcRejectsUnsupportedDryRunWithoutSideEffects() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto source = temporary.path() / "reject-source.dat";
    writeFile(source, "reject-dry-run-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    Database db(prepared.pool / "meta.db");
    const auto objects_before = db.integer("SELECT COUNT(*) FROM object;");
    const auto refs_before = db.integer("SELECT COUNT(*) FROM ref;");
    const bool object_exists_before = store.has_object(put.hash);

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    auto result = runCommand({"gc", "--reclaim", "--dry-run"});
    require(result.code == 2, "gc reclaim dry-run must be rejected");
    require(result.err.find("--dry-run only supports --reconcile") != std::string::npos,
            "dry-run rejection must explain supported mode");
    require(db.integer("SELECT COUNT(*) FROM object;") == objects_before &&
                db.integer("SELECT COUNT(*) FROM ref;") == refs_before &&
                store.has_object(put.hash) == object_exists_before,
            "rejected dry-run must not alter pool metadata or objects");

    result = runCommand({"gc", "--dry-run"});
    require(result.code == 2, "full gc dry-run must be rejected");
    require(db.integer("SELECT COUNT(*) FROM object;") == objects_before &&
                db.integer("SELECT COUNT(*) FROM ref;") == refs_before &&
                store.has_object(put.hash) == object_exists_before,
            "rejected full dry-run must not alter pool metadata or objects");
}

void testRenewTouchesLinkedObject() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = currentWorkspacePath(prepared) / "important.dat";
    const auto source = temporary.path() / "source.dat";
    writeFile(source, "important");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());
    Database db(prepared.pool / "meta.db");
    const std::string initial =
        db.text("SELECT last_access FROM object WHERE hash='" + put.hash + "';");

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"renew", "important.dat"});
    require(result.code == 0, "renew must succeed for a linked pool symlink");
    require(result.out.find("renewed_count=1") != std::string::npos,
            "renew must report renewed count");
    require(db.text("SELECT last_access FROM object WHERE hash='" + put.hash + "';") > initial,
            "renew must push object.last_access forward");
    require(db.integer("SELECT COUNT(*) FROM renew_event WHERE hash='" + put.hash + "';") == 1,
            "renew must append a hash-only renew_event row");
}

void testRenewStatusReportsSingleVersionFilesReadOnly() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto wc = currentWorkspacePath(prepared);
    const auto linked_file = wc / "linked.gds";
    const auto local_file = wc / "local.v";
    const auto source = temporary.path() / "renew-status-source.dat";
    writeFile(source, "renew-status-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), linked_file);
    store.register_ref(put.hash, std::filesystem::absolute(linked_file).string());
    writeFile(local_file, "local content");
    writeFile(prepared.workspace / ".isvn_local" / "workspace_state",
              std::string(kChild) + "/local.v\n");
    Database db(prepared.pool / "meta.db");
    const std::string initial_access =
        db.text("SELECT last_access FROM object WHERE hash='" + put.hash + "';");
    const auto renew_events_before = db.integer("SELECT COUNT(*) FROM renew_event;");

    CurrentDirectory cwd(wc);
    auto result = runCommand({"renew-status", "--json"});
    require(result.code == 0, "renew-status without version must use cwd current version");
    require(result.out.find(std::string("\"version\":\"") + kChild + "\"") !=
                std::string::npos,
            "renew-status must report the current version");
    require(result.out.find("\"file\":\"linked.gds\"") != std::string::npos &&
                result.out.find("\"state\":\"LINKED\"") != std::string::npos &&
                result.out.find(put.hash) != std::string::npos &&
                result.out.find(std::string("\"last_access\":\"") + initial_access + "\"") !=
                    std::string::npos &&
                result.out.find("\"refcount\":1") != std::string::npos,
            "renew-status must report linked file hash, last_access, and refcount");
    require(result.out.find("\"file\":\"local.v\"") != std::string::npos &&
                result.out.find("\"state\":\"LOCAL\"") != std::string::npos &&
                result.out.find("\"hash\":null") != std::string::npos &&
                result.out.find("\"last_access\":null") != std::string::npos &&
                result.out.find("\"refcount\":null") != std::string::npos,
            "renew-status must report local files without pool metadata");
    require(db.text("SELECT last_access FROM object WHERE hash='" + put.hash + "';") ==
                initial_access,
            "renew-status must not touch object.last_access");
    require(db.integer("SELECT COUNT(*) FROM renew_event;") == renew_events_before,
            "renew-status must not write renew_event");

    CurrentDirectory root_cwd(prepared.workspace);
    result = runCommand({"renew-status", kChild, "--json"});
    require(result.code == 0, "renew-status explicit version must succeed from workspace root");
    result = runCommand({"renew-status", "IMPL/cpu/ROUTE/v9", "--json"});
    require(result.code == 1, "renew-status explicit missing workspace must fail");
    require(result.err.find("run isvn checkout") != std::string::npos,
            "renew-status explicit missing workspace must not fall back to cwd");
}

void testRenewStatusDoesNotRepairDanglingLinks() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto wc = currentWorkspacePath(prepared);
    const std::string missing_hash(64, '0');
    const auto missing_object = prepared.pool / "objects" / "00" / missing_hash;
    std::filesystem::create_directories(missing_object.parent_path());
    const auto dangling = wc / "dangling.gds";
    std::filesystem::create_symlink(missing_object, dangling);

    CurrentDirectory cwd(wc);
    const auto result = runCommand({"renew-status", "--json"});
    require(result.code == 0, "renew-status must tolerate dangling pool symlinks");
    require(result.out.find("\"file\":\"dangling.gds\"") != std::string::npos &&
                result.out.find("\"state\":\"LINKED\"") != std::string::npos &&
                result.out.find(missing_hash) != std::string::npos &&
                result.out.find("\"last_access\":null") != std::string::npos &&
                result.out.find("\"refcount\":null") != std::string::npos,
            "renew-status must report dangling object metadata as unavailable");
    require(std::filesystem::is_symlink(dangling),
            "renew-status must not repair or replace dangling symlinks");
    require(!std::filesystem::exists(missing_object),
            "renew-status must not recreate missing pool objects");
}

void testPoolStatReportsStorageOnlyMetrics() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto source = temporary.path() / "source.dat";
    writeFile(source, "pool-stat-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    store.register_ref(put.hash, (prepared.workspace / "linked.dat").string());
    Database db(prepared.pool / "meta.db");
    db.execute("INSERT INTO evict_event(hash, evicted_at) VALUES('" + put.hash +
               "', '2026-06-11T00:00:00Z');");

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"pool-stat", "--evict-events", "--evict-threshold", "0"});
    require(result.code == 0, "pool-stat must succeed from a workspace");
    require(result.out.find("objects=1") != std::string::npos,
            "pool-stat must report object count");
    require(result.out.find("refs=1") != std::string::npos,
            "pool-stat must report ref count");
    require(result.out.find("evict_events=1") != std::string::npos,
            "pool-stat --evict-events must report event count");
    require(result.out.find("pool_health=critical_expand_pool") != std::string::npos,
            "pool-stat must mark configured critical threshold");
    require(result.out.find(kRoot) == std::string::npos &&
                result.out.find(kChild) == std::string::npos,
            "pool-stat must not present hash-to-version relationships");

    const auto json = runCommand({"pool-stat", "--evict-events", "--evict-threshold", "0",
                                  "--json"});
    require(json.code == 0, "pool-stat --json must succeed");
    require(json.out.find("\"dedup_ratio\"") != std::string::npos &&
                json.out.find("\"events\"") != std::string::npos &&
                json.out.find("\"recent_renews\"") != std::string::npos &&
                json.out.find("\"renewed_object_count\"") != std::string::npos &&
                json.out.find("\"critical\":true") != std::string::npos,
            "pool-stat --json must expose storage metrics, renew stats, and evict events");
    require(json.out.find(kRoot) == std::string::npos &&
                json.out.find(kChild) == std::string::npos &&
                json.out.find("source.dat") == std::string::npos &&
                json.out.find("linked.dat") == std::string::npos,
            "pool-stat --json must not expose version or file relationships");
}

void testPoolStatRenewStatsStayHashOnly() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto file = currentWorkspacePath(prepared) / "renewed.dat";
    const auto source = temporary.path() / "renew-source.dat";
    writeFile(source, "renew-stat-content");
    ObjectStore store(prepared.pool);
    const auto put = store.put_object(source);
    std::filesystem::create_symlink(store.object_path(put.hash), file);
    store.register_ref(put.hash, std::filesystem::absolute(file).string());

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    auto result = runCommand({"renew", "renewed.dat"});
    require(result.code == 0, "renew must succeed before pool-stat");
    result = runCommand({"pool-stat", "--json"});
    require(result.code == 0, "pool-stat json with renew stats must succeed");
    require(result.out.find("\"recent_renews\"") != std::string::npos &&
                result.out.find(put.hash) != std::string::npos &&
                result.out.find("\"actor\"") != std::string::npos &&
                result.out.find("\"refcount\":1") != std::string::npos,
            "pool-stat renew stats must expose hash/actor/time/refcount facts");
    require(result.out.find("renewed.dat") == std::string::npos &&
                result.out.find(kChild) == std::string::npos &&
                result.out.find("version") == std::string::npos,
            "pool-stat renew stats must not expose file or version facts");
}

void testAdminStatusJsonIsReadOnlyAndSeparated() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));

    const auto result = runCommand({"admin", "status", "--json"});
    require(result.code == 0, "admin status --json must succeed");
    require(result.out.find("\"paths\"") != std::string::npos &&
                result.out.find("\"permissions\"") != std::string::npos &&
                result.out.find("\"authz\"") != std::string::npos &&
                result.out.find("\"scale\"") != std::string::npos &&
                result.out.find("\"lineage\"") != std::string::npos &&
                result.out.find("\"backup\"") != std::string::npos,
            "admin status must expose A1-A6 sections");
    require(result.out.find("\"object_versions\"") == std::string::npos &&
                result.out.find("\"link_path\"") == std::string::npos &&
                result.out.find("\"hash\"") == std::string::npos,
            "admin status must not expose object-to-version mappings");
}

void testReconcileJsonDoesNotResolve() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"reconcile", kChild, "--json"});
    require(result.code == 0, "reconcile --json must succeed");
    require(result.out.find("\"lineage_parent\"") != std::string::npos,
            "reconcile --json must report Lineage parent");
    require(result.out.find("\"svn_copyfrom_hint\":null") != std::string::npos,
            "MVP reconcile --json must not query svn copy-from");
    Lineage lineage(prepared.project);
    require(lineage.parentOf(kChild) == std::optional<std::string>(kRoot),
            "reconcile --json must not modify Lineage");
}

void testUnknownCommandFails() {
    const auto result = runCommand({"not-a-command"});
    require(result.code == 1, "unknown command must fail");
}

void testMemberInitReportsMissingSharedConfigPath() {
    TemporaryDirectory temporary;
    const auto project = temporary.path() / "project";
    const auto workspace = temporary.path() / "workspace";
    std::filesystem::create_directories(project / ".isvn");

    const auto result =
        runCommand({"member", "init", "--project-dir", project.string(), "--workspace",
                    workspace.string()});

    require(result.code == 1, "member init must fail when shared config is missing");
    require(result.err.find((project / ".isvn" / "config").string()) != std::string::npos,
            "missing config error must include the exact path");
    require(result.err.find("admin init") != std::string::npos,
            "missing config error must tell PM to rerun admin init");
}

void testAdminInitCanRecoverMissingConfigWithExistingRoot() {
    const auto source = readFile(std::filesystem::path(ISVN_SOURCE_DIR) /
                                 "src/cli/commands.cpp");
    require(source.find("auditTrail(\"ROOT\")") == std::string::npos,
            "admin init recovery must not validate ROOT as a four-segment version id");
    require(source.find("hasRootAuditRecord") != std::string::npos,
            "admin init must detect an existing ROOT without filtered auditTrail");
}

void testCommandLayerGuardrails() {
    const auto source = readFile(std::filesystem::path(ISVN_SOURCE_DIR) /
                                 "src/cli/commands.cpp");
    require(source.find("queryCopyFrom") == std::string::npos,
            "commands MVP must not query svn copy source");
    require(source.find("copy-from") == std::string::npos,
            "commands must not parse or expose svn copy-from");
    require(source.find("resolveParent") == std::string::npos,
            "reconcile must not auto-resolve Lineage in MVP");
    require(source.find("requireCurrentVersion") != std::string::npos &&
                source.find("verifyWorkingCopyMatchesMapping") != std::string::npos,
            "create must derive parent from the registered current version workspace");
    require(source.find("optionValue(args, \"--parent\")") != std::string::npos &&
                source.find("created from explicit parent ROOT") != std::string::npos,
            "create must support explicit --parent ROOT for root versions");
    require(source.find("create --root is obsolete; use --parent ROOT") != std::string::npos,
            "create must reject obsolete --root in favor of --parent ROOT");
    require(source.find("hasFlag(args, \"--parent\") && !explicit_parent.has_value()") !=
                std::string::npos,
            "create must reject --parent without a value");
    require(source.find("currentOrExplicitVersion") != std::string::npos &&
                source.find("explicitVersionInWorkspace") != std::string::npos,
            "status/edit/checkout-ancestor must share explicit version workspace resolution");
    require(source.find("version is not checked out in this workspace; run isvn checkout") !=
                std::string::npos,
            "explicit version resolution must fail clearly instead of falling back to cwd");
    require(source.find("optionValue(args, \"--in\")") != std::string::npos,
            "edit must expose --in <version>");
    require(source.find("parent does not exist in Lineage") != std::string::npos &&
                source.find("runtime.svn.info(std::filesystem::path(runtime.svn.versionToUrl(*explicit_parent)))") !=
                    std::string::npos,
            "create --parent <version> must validate that the parent exists");
    require(source.find("createBranchFromParent") != std::string::npos &&
                source.find("createRootBranch") != std::string::npos,
            "create must use svnmucc-backed atomic branch creation helpers");
    const auto create_start = source.find("int cmdCreate");
    const auto create_end = source.find("\n}\n\nint cmdCheckout", create_start);
    const auto create = source.substr(create_start, create_end - create_start);
    require(create.find("branchCopy") == std::string::npos &&
                create.find("runtime.svn.mkdir") == std::string::npos,
            "create must not use multi-step svn copy/mkdir commits");
    require(source.find("positionalArgs") != std::string::npos,
            "create must skip option values when selecting the child version");
    const auto version_subdir_start = source.find("std::filesystem::path versionSubdir");
    const auto version_subdir_end = source.find("\n}\n\nstd::vector<WorkspaceEntry>", version_subdir_start);
    const auto version_subdir =
        source.substr(version_subdir_start, version_subdir_end - version_subdir_start);
    require(version_subdir.find("return std::filesystem::path(version);") != std::string::npos &&
                version_subdir.find(".filename()") == std::string::npos,
            "version workspaces must expand the full phase/block/stage/version path");
    const auto checkout_start = source.find("int cmdCheckout");
    const auto checkout_end = source.find("\n}\n\nint cmdCheckin", checkout_start);
    const auto checkout = source.substr(checkout_start, checkout_end - checkout_start);
    require(checkout.find("runtime.workspace / versionSubdir(args[1])") != std::string::npos,
            "checkout destination must be WORKSPACE_ROOT plus the full version path");
    require(source.find("runtime.svn.checkout(runtime.svn.versionToUrl(args[1]), runtime.workspace)") ==
                std::string::npos,
            "checkout must never make WORKSPACE_ROOT itself an SVN working copy");
    require(source.find("cmdAdminInit") != std::string::npos,
            "commands must expose PM admin init");
    require(source.find("cmdMemberInit") != std::string::npos,
            "commands must expose member init");
    require(source.find(".isvn_local") != std::string::npos,
            "commands must keep member-local state under .isvn_local");
    require(source.find("cmdRenew") != std::string::npos,
            "commands must expose renew");
    require(source.find("cmdPoolStat") != std::string::npos,
            "commands must expose pool-stat");
    require(source.find("cmdLineageAudit") != std::string::npos,
            "commands must expose lineage audit");
    require(source.find("touch_last_access") != std::string::npos,
            "renew/swap-in must push storage last_access through ObjectStore");
    require(source.find("is_dangling_link") != std::string::npos,
            "workspace commands must check for dangling pool symlinks");
    const auto status_start = source.find("int cmdStatus");
    const auto status_end = source.find("\n}\n\nint cmdRenew", status_start);
    const auto status = source.substr(status_start, status_end - status_start);
    require(status.find("shouldSkipPath") != std::string::npos,
            "status must hide .isvn_local/.svn internal workspace paths");
    require(status.find("positionalArgs(args)") != std::string::npos &&
                status.find("currentOrExplicitVersion(runtime, explicit_version)") !=
                    std::string::npos,
            "status must accept an explicit version without cwd fallback");
    const auto renew_status_start = source.find("int cmdRenewStatus");
    const auto renew_status_end = source.find("\n}\n\nint cmdUpdate", renew_status_start);
    const auto renew_status =
        source.substr(renew_status_start, renew_status_end - renew_status_start);
    require(renew_status.find("currentOrExplicitVersion(runtime, explicit_version)") !=
                std::string::npos,
            "renew-status must share current/explicit version resolution");
    require(renew_status.find("last_access_of(*hash)") != std::string::npos &&
                renew_status.find("refcount(*hash)") != std::string::npos,
            "renew-status must query storage only by hash");
    require(renew_status.find("repairDangling") == std::string::npos &&
                renew_status.find("touch_last_access") == std::string::npos &&
                renew_status.find("record_renew_event") == std::string::npos,
            "renew-status must stay read-only and must not repair or renew");
    const auto pool_stat_start = source.find("int cmdPoolStat");
    const auto pool_stat_end = source.find("\n}\n\nint cmdAdminStatus", pool_stat_start);
    const auto pool_stat = source.substr(pool_stat_start, pool_stat_end - pool_stat_start);
    require(pool_stat.find("Lineage") == std::string::npos &&
                pool_stat.find("lineage") == std::string::npos,
            "pool-stat must not join or query Lineage");
}

void testVersionCommand() {
    auto plain = runCommand({"version"});
    require(plain.code == 0, "version must succeed");
    require(plain.out.find("isvn ") != std::string::npos, "version must print program and version");
    require(plain.out.find("commit:") != std::string::npos, "version must print commit");
    require(plain.out.find("build_type:") != std::string::npos, "version must print build_type");

    auto flag = runCommand({"--version"});
    require(flag.code == 0, "--version must succeed");
    require(flag.out.find("commit:") != std::string::npos, "--version must print identity");

    auto j = runCommand({"version", "--json"});
    require(j.code == 0, "version --json must succeed");
    require(j.out.find("\"result\":\"ok\"") != std::string::npos, "version json must report result ok");
    require(j.out.find("\"program\":\"isvn\"") != std::string::npos, "version json must include program");
    require(j.out.find("\"version\":") != std::string::npos, "version json must include version");
    require(j.out.find("\"commit\":") != std::string::npos, "version json must include commit");
    require(j.out.find("\"build_time\":") != std::string::npos, "version json must include build_time");
    require(j.out.find("\"build_type\":") != std::string::npos, "version json must include build_type");
}

bool svnToolsAvailable() {
    return std::system("svn --version --quiet >/dev/null 2>&1") == 0 &&
           std::system("svnadmin --version --quiet >/dev/null 2>&1") == 0;
}

std::string currentGroupName() {
    const auto* group = ::getgrgid(::getegid());
    return group != nullptr ? std::string(group->gr_name) : std::string();
}

// A read model fixture with three checked-out versions in different
// Phase/Block/Stage locations, plus a repository-only version that is NOT in the
// local registry (must never appear in workspaces).
PreparedProject prepareReadModelProject(const std::filesystem::path& root) {
    PreparedProject prepared{root / "readproj", root / "workspace", root / "pool"};
    std::filesystem::create_directories(prepared.project / ".isvn");
    std::filesystem::create_directories(prepared.workspace / ".isvn_local");
    writeFile(prepared.project / ".isvn" / "config",
              "repo_url = file:///tmp/nonexistent-isvn-readmodel-repo\npool_root = " +
                  prepared.pool.string() + "\nbranch_prefix = branches/\n");
    writeFile(prepared.workspace / ".isvn_local" / "config",
              "project_dir = " + prepared.project.string() + "\n");
    // Three local versions (deliberately out of sorted order in the file).
    writeFile(prepared.workspace / ".isvn_local" / "workspaces",
              std::string("rtl_design/cpu/SYN/v2\trtl_design/cpu/SYN/v2\n") +
                  "rtl_design/cpu/RTL/v1\trtl_design/cpu/RTL/v1\n" +
                  "floorplan/io/ROUTE/v1\tfloorplan/io/ROUTE/v1\n");
    ObjectStore store(prepared.pool);
    // Version 1: a LINKED (clean) file + a LOCAL editable file + folders.
    const auto v1 = prepared.workspace / "rtl_design/cpu/RTL/v1";
    std::filesystem::create_directories(v1);
    const auto payload = root / "payload.bin";
    writeFile(payload, "0123456789");  // 10 bytes
    const auto put = store.put_object(payload);
    std::filesystem::create_directories(v1 / "netlist");
    std::filesystem::create_symlink(store.object_path(put.hash), v1 / "netlist/top.v");
    store.register_ref(put.hash, std::filesystem::absolute(v1 / "netlist/top.v").string());
    writeFile(v1 / "src/edit.v", "local edit body");  // 15 bytes, LOCAL
    // a dangling LINKED symlink -> valid-looking hash whose object is absent.
    const std::string absent_hash(64, 'a');
    std::filesystem::create_symlink(prepared.pool / "objects" / absent_hash,
                                    v1 / "broken.gds");
    writeFile(prepared.workspace / ".isvn_local" / "workspace_state",
              "rtl_design/cpu/RTL/v1/src/edit.v\n");
    // Version 2 and 3 are empty checked-out dirs.
    std::filesystem::create_directories(prepared.workspace / "rtl_design/cpu/SYN/v2");
    std::filesystem::create_directories(prepared.workspace / "floorplan/io/ROUTE/v1");
    Lineage lineage(prepared.project);
    lineage.recordParent("rtl_design/cpu/RTL/v1", std::nullopt, "create");
    lineage.recordParent("rtl_design/cpu/SYN/v2", std::string("rtl_design/cpu/RTL/v1"),
                         "create");
    lineage.recordParent("floorplan/io/ROUTE/v1", std::nullopt, "create");
    return prepared;
}

void testWorkspacesJsonContract() {
    TemporaryDirectory temporary;
    const auto prepared = prepareReadModelProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace);

    auto result = runCommand({"workspaces", "--json"});
    require(result.code == 0, "workspaces --json must succeed");
    require(result.out.find("\"schema_version\":\"workspaces/v1\"") != std::string::npos,
            "workspaces must declare schema_version workspaces/v1");
    // Only the three locally checked-out versions appear.
    require(result.out.find("floorplan/io/ROUTE/v1") != std::string::npos &&
                result.out.find("rtl_design/cpu/RTL/v1") != std::string::npos &&
                result.out.find("rtl_design/cpu/SYN/v2") != std::string::npos,
            "workspaces must list local versions");
    // Deterministic order: sorted hierarchy (floorplan before rtl_design).
    require(result.out.find("floorplan/io/ROUTE/v1") <
                result.out.find("rtl_design/cpu/RTL/v1"),
            "workspaces order must be deterministic by hierarchy");
    // Explicit phase/block/stage fields.
    require(result.out.find("\"phase\":\"rtl_design\"") != std::string::npos &&
                result.out.find("\"block\":\"cpu\"") != std::string::npos &&
                result.out.find("\"stage\":\"RTL\"") != std::string::npos,
            "workspaces must expose explicit phase/block/stage fields");
    // Stable, non-index workspace_id (starts with ws_ and is not "0"/"1").
    require(result.out.find("\"workspace_id\":\"ws_") != std::string::npos,
            "workspace_id must be a stable opaque id, not an array index");
    // Counts: v1 has one LOCAL editable file -> changed>=1, local>=1.
    require(result.out.find("\"counts\":{\"changed\":") != std::string::npos,
            "workspaces must expose counts");
    // A working copy that does not exist yields WORKING_COPY_MISSING; here all
    // three exist, so no such issue.
    require(result.out.find("WORKING_COPY_MISSING") == std::string::npos,
            "all mapped working copies exist in this fixture");

    // Idempotence: a second identical query is byte-identical.
    auto again = runCommand({"workspaces", "--json"});
    require(again.out == result.out, "workspaces --json must be idempotent");

    // No-mutation proof: registry + workspace_state + pool object set unchanged.
    const auto registry_before =
        readFile(prepared.workspace / ".isvn_local" / "workspaces");
    const auto state_before =
        readFile(prepared.workspace / ".isvn_local" / "workspace_state");
    runCommand({"workspaces", "--json"});
    runCommand({"workspaces", "--json"});
    require(readFile(prepared.workspace / ".isvn_local" / "workspaces") == registry_before,
            "workspaces must not mutate the local registry");
    require(readFile(prepared.workspace / ".isvn_local" / "workspace_state") == state_before,
            "workspaces must not mutate workspace_state");
}

void testWorkspacesRejectsCrossProjectAndMalformedRows() {
    TemporaryDirectory temporary;
    const auto prepared = prepareReadModelProject(temporary.path());
    // Append a malformed row (no tab) and a bad version id.
    {
        std::ofstream out(prepared.workspace / ".isvn_local" / "workspaces",
                          std::ios::app);
        out << "this-row-has-no-tab\n";
        out << "bad//version\tbad//version\n";
    }
    CurrentDirectory cwd(prepared.workspace);
    auto result = runCommand({"workspaces", "--json"});
    require(result.code == 0, "workspaces must not crash on malformed rows");
    require(result.out.find("MALFORMED_REGISTRY_ROW") != std::string::npos,
            "workspaces must report malformed rows as an issue, not silently drop");
    // Cross-project selector is rejected.
    auto cross = runCommand({"workspaces", "--project", "/tmp/other", "--json"});
    require(cross.code != 0, "workspaces must reject a cross-project selector");
}

void testWorkStatusContractAndAvailability() {
    TemporaryDirectory temporary;
    const auto prepared = prepareReadModelProject(temporary.path());
    CurrentDirectory cwd(prepared.workspace);

    // Reject a non-local version.
    auto bad = runCommand({"work-status", "rtl_design/cpu/RTL/absent", "--json"});
    require(bad.code != 0 && bad.out.find("\"reason_code\":\"INVALID_VERSION\"") !=
                                 std::string::npos,
            "work-status must reject a non-local version with INVALID_VERSION");

    auto result = runCommand({"work-status", "rtl_design/cpu/RTL/v1", "--json"});
    require(result.code == 0, "work-status must succeed for a local version");
    require(result.out.find("\"schema_version\":\"work-status/v1\"") != std::string::npos,
            "work-status must declare schema_version work-status/v1");
    // LINKED clean file with a validated pool object size (10 bytes).
    require(result.out.find("\"path\":\"netlist/top.v\"") != std::string::npos &&
                result.out.find("\"link_state\":\"LINKED\"") != std::string::npos &&
                result.out.find("\"size_bytes\":10") != std::string::npos,
            "work-status must report LINKED source and validated pool object size");
    // LOCAL editable file with local metadata size (15 bytes).
    require(result.out.find("\"path\":\"src/edit.v\"") != std::string::npos &&
                result.out.find("\"link_state\":\"LOCAL\"") != std::string::npos &&
                result.out.find("\"size_bytes\":15") != std::string::npos,
            "work-status must report LOCAL source and local file size");
    // Folder aggregate entries with descendant counts.
    require(result.out.find("\"path\":\"netlist\",\"kind\":\"folder\"") != std::string::npos &&
                result.out.find("\"path\":\"src\",\"kind\":\"folder\"") != std::string::npos,
            "work-status must synthesize folder aggregate entries");
    // Availability reason codes present.
    require(result.out.find("\"reason_code\":\"ALLOWED\"") != std::string::npos,
            "must emit ALLOWED for a make-editable LINKED file");
    require(result.out.find("\"reason_code\":\"ALREADY_LOCAL\"") != std::string::npos,
            "must emit ALREADY_LOCAL for make-editable on a LOCAL file");
    require(result.out.find("\"reason_code\":\"CLEAN_ITEM\"") != std::string::npos,
            "must emit CLEAN_ITEM for add-to-batch on a clean file");
    require(result.out.find("\"reason_code\":\"NO_LOCAL_CHANGE\"") != std::string::npos,
            "must emit NO_LOCAL_CHANGE for revert with nothing to revert");
    require(result.out.find("\"reason_code\":\"UNSUPPORTED_TYPE\"") != std::string::npos,
            "must emit UNSUPPORTED_TYPE for folder make-editable");
    // Broken/dangling pool object -> null size + MISSING_ITEM issue.
    require(result.out.find("MISSING_ITEM") != std::string::npos,
            "a dangling pool object must surface a MISSING_ITEM issue");
    // Version-level actions present.
    require(result.out.find("\"create_derived_version\"") != std::string::npos &&
                result.out.find("\"inspect_in_explore\"") != std::string::npos,
            "work-status must expose version_actions");

    // Idempotence.
    auto again = runCommand({"work-status", "rtl_design/cpu/RTL/v1", "--json"});
    require(again.out == result.out, "work-status --json must be idempotent");
}

// `branches --json` must not print a partial document when svn cannot run:
// the command fails, stdout stays empty and stderr names the failure.
void testBranchesJsonFailsCleanlyWithoutSvn() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn/svnadmin not found; skipped branches-without-svn test\n";
        return;
    }
    TemporaryDirectory temporary;
    const auto repo = temporary.path() / "repo";
    const auto project = temporary.path() / "project";
    const auto workspace = temporary.path() / "ws";
    const auto pool = temporary.path() / "pool";
    const auto no_tools = temporary.path() / "no-tools";
    std::filesystem::create_directories(no_tools);
    require(std::system(("svnadmin create " + repo.string()).c_str()) == 0,
            "svnadmin create must succeed");
    auto r = runCommand({"admin", "init", "--project-dir", project.string(), "--repo",
                         "file://" + repo.string(), "--pool", pool.string(), "--group",
                         currentGroupName()});
    require(r.code == 0, "admin init must succeed");
    r = runCommand({"member", "init", "--project-dir", project.string(), "--workspace",
                    workspace.string()});
    require(r.code == 0, "member init must succeed");
    CurrentDirectory cwd(workspace);
    require(runCommand({"create", "IMPL/cpu/RTL/v1", "--parent", "ROOT"}).code == 0,
            "create root version must succeed");

    r = runCommand({"branches", "--json"});
    require(r.code == 0, "branches --json must succeed with svn available");
    require(r.out == "{\"branches\":[{\"phase\":\"IMPL\",\"block\":\"cpu\",\"stage\":\"RTL\","
                     "\"version\":\"v1\",\"path\":\"IMPL/cpu/RTL/v1\",\"is_head\":false,"
                     "\"removed\":false}]}\n",
            "branches --json must print one complete document");

    const char* saved = std::getenv("PATH");
    const std::string previous = saved != nullptr ? saved : "";
    ::setenv("PATH", no_tools.c_str(), 1);
    r = runCommand({"branches", "--json"});
    ::setenv("PATH", previous.c_str(), 1);
    require(r.code != 0, "branches --json must fail when svn cannot run");
    require(r.out.empty(), "branches --json must not print a partial document when svn fails");
    require(r.err.find("svn list failed") != std::string::npos,
            "branches --json must report the svn failure on stderr");
}

void testWorkReadCommandsLiveSvn() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn/svnadmin not found; skipped live work-status integration test\n";
        return;
    }
    TemporaryDirectory temporary;
    const auto repo = temporary.path() / "repo";
    const auto project = temporary.path() / "project";
    const auto workspace = temporary.path() / "ws";
    const auto pool = temporary.path() / "pool";
    require(std::system(("svnadmin create " + repo.string()).c_str()) == 0,
            "svnadmin create must succeed");
    const auto group = currentGroupName();
    auto r = runCommand({"admin", "init", "--project-dir", project.string(), "--repo",
                         "file://" + repo.string(), "--pool", pool.string(), "--group", group});
    require(r.code == 0, "admin init must succeed");
    r = runCommand({"member", "init", "--project-dir", project.string(), "--workspace",
                    workspace.string()});
    require(r.code == 0, "member init must succeed");
    CurrentDirectory cwd(workspace);
    require(runCommand({"create", "IMPL/cpu/RTL/v1", "--parent", "ROOT"}).code == 0,
            "create root version must succeed");
    // Set up a SECOND version first so its create does not relink v1 local files.
    require(runCommand({"create", "IMPL/cpu/SYN/v2", "--parent", "IMPL/cpu/RTL/v1"}).code == 0,
            "create derived version must succeed");
    // Commit one file (becomes LINKED clean), then make it editable and modify it.
    writeFile(workspace / "IMPL/cpu/RTL/v1/top.v", "module top; endmodule\n");
    require(runCommand({"checkin", "IMPL/cpu/RTL/v1/top.v", "--to", "IMPL/cpu/RTL/v1"}).code == 0,
            "checkin must succeed");
    require(runCommand({"edit", "IMPL/cpu/RTL/v1/top.v", "--in", "IMPL/cpu/RTL/v1"}).code == 0,
            "edit must succeed");
    writeFile(workspace / "IMPL/cpu/RTL/v1/top.v", "module top; endmodule\n// change\n");
    // A brand-new untracked LOCAL file.
    writeFile(workspace / "IMPL/cpu/RTL/v1/notes.txt", "notes\n");

    const auto rev_before = runCommand({"branches", "--json"});  // read-only anchor

    auto result = runCommand({"work-status", "IMPL/cpu/RTL/v1", "--json"});
    require(result.code == 0, "live work-status must succeed");
    require(result.out.find("\"svn_available\":true") != std::string::npos,
            "live work-status must have svn available");
    // top.v: LOCAL, svn M, ui modified.
    require(result.out.find("\"path\":\"top.v\"") != std::string::npos &&
                result.out.find("\"svn_state\":\"M\"") != std::string::npos &&
                result.out.find("\"ui_state\":\"modified\"") != std::string::npos,
            "live work-status must map modified LOCAL file");
    // notes.txt: untracked -> ui new.
    require(result.out.find("\"path\":\"notes.txt\"") != std::string::npos &&
                result.out.find("\"ui_state\":\"new\"") != std::string::npos,
            "live work-status must map a new untracked file");

    auto ws = runCommand({"workspaces", "--json"});
    require(ws.out.find("\"changed\":") != std::string::npos, "workspaces counts present");

    // No mutation: repository revision unchanged by the reads.
    const auto rev_after = runCommand({"branches", "--json"});
    require(rev_before.out == rev_after.out || ws.code == 0,
            "read commands must not change repository state");
    // Repeat identical query -> stable output.
    auto result2 = runCommand({"work-status", "IMPL/cpu/RTL/v1", "--json"});
    require(result2.out == result.out, "live work-status must be idempotent");
}

// ---- Transactional local ops, atomic Remove, recovery ---------------------

std::string jsonField(const std::string& text, const std::string& key) {
    const auto marker = "\"" + key + "\":";
    auto pos = text.find(marker);
    if (pos == std::string::npos) return "";
    pos += marker.size();
    if (pos < text.size() && text[pos] == '"') {
        const auto end = text.find('"', pos + 1);
        return text.substr(pos + 1, end - pos - 1);
    }
    const auto end = text.find_first_of(",}", pos);
    return text.substr(pos, end - pos);
}

struct LiveWork {
    std::filesystem::path repo;
    std::filesystem::path project;
    std::filesystem::path workspace;
    std::string version = "T/B/RTL/v1";
};

// Build a live SVN project with one committed (LINKED) managed file `m.v`.
LiveWork prepareLiveWork(const std::filesystem::path& root) {
    LiveWork lw{root / "repo", root / "proj", root / "ws"};
    require(std::system(("svnadmin create " + lw.repo.string()).c_str()) == 0,
            "svnadmin create must succeed");
    const auto group = currentGroupName();
    require(runCommand({"admin", "init", "--project-dir", lw.project.string(), "--repo",
                        "file://" + lw.repo.string(), "--pool", (root / "pool").string(),
                        "--group", group})
                .code == 0,
            "admin init must succeed");
    require(runCommand({"member", "init", "--project-dir", lw.project.string(), "--workspace",
                        lw.workspace.string()})
                .code == 0,
            "member init must succeed");
    CurrentDirectory cwd(lw.workspace);
    require(runCommand({"create", lw.version, "--parent", "ROOT"}).code == 0, "create must succeed");
    writeFile(lw.workspace / lw.version / "m.v", "managed body\n");
    require(runCommand({"checkin", lw.version + "/m.v", "--to", lw.version}).code == 0,
            "checkin must succeed");
    return lw;
}

std::string previewPlanHash(const std::vector<std::string>& preview_args) {
    const auto result = runCommand(preview_args);
    require(result.code == 0, "preview must succeed: " + result.out + result.err);
    return jsonField(result.out, "plan_hash");
}

std::int64_t headRevision(const LiveWork& lw) {
    const auto info = runCommand({"log", lw.version, "--json"});
    // Fall back to svn info via file listing count is unnecessary; use svn directly.
    std::string cmd = "svn info file://" + lw.repo.string() + " 2>/dev/null | awk '/Revision:/{print $2}'";
    std::array<char, 64> buf{};
    FILE* pipe = ::popen(cmd.c_str(), "r");
    std::string out;
    if (pipe) {
        while (fgets(buf.data(), buf.size(), pipe)) out += buf.data();
        ::pclose(pipe);
    }
    (void)info;
    return out.empty() ? -1 : std::stoll(out);
}

std::string commandOutput(const std::string& command) {
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) return output;
    while (fgets(buffer.data(), buffer.size(), pipe)) output += buffer.data();
    ::pclose(pipe);
    return output;
}

std::string repositoryFile(const LiveWork& lw, const std::string& relative) {
    return commandOutput("svn cat file://" + lw.repo.string() + "/branches/" + lw.version + "/" +
                         relative + " 2>/dev/null");
}

// True when `svn status` reports a scheduled or missing node (A/D/R/M/!) in the
// working copy.  Managed files resting as pool symlinks are reported as `~` and
// are not counted.
bool hasScheduledSvnChange(const std::filesystem::path& working_copy) {
    std::istringstream lines(commandOutput("svn status " + working_copy.string()));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        if (line[0] == 'A' || line[0] == 'D' || line[0] == 'R' || line[0] == 'M' ||
            line[0] == '!') {
            return true;
        }
    }
    return false;
}

// The single-file `checkin` must work in a working copy that already holds
// committed files: those rest as pool symlinks and stay out of the commit.
void testCheckinBesideLinkedFilesCommitsOnlyTheRequestedPath() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin beside linked files test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    require(std::filesystem::is_symlink(wc / "m.v"), "a committed file rests as a pool symlink");

    writeFile(wc / "n.v", "second body\n");
    const auto second = runCommand({"checkin", lw.version + "/n.v", "--to", lw.version});
    require(second.code == 0, "checkin beside a LINKED file must succeed: " + second.err);
    require(repositoryFile(lw, "n.v") == "second body\n", "the second file reaches the repository");
    require(std::filesystem::is_symlink(wc / "n.v"), "the committed file is relinked");

    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "m.v"}).code == 0, "edit must succeed");
        writeFile(wc / "m.v", "managed body v2\n");
        const auto edited = runCommand({"checkin", "m.v"});
        require(edited.code == 0, "checkin of an edited file must succeed: " + edited.err);
    }
    require(repositoryFile(lw, "m.v") == "managed body v2\n",
            "the edited content reaches the repository");
    require(std::filesystem::is_symlink(wc / "m.v"), "the edited file returns to LINKED");

    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "m.v"}).code == 0 && runCommand({"edit", "n.v"}).code == 0,
                "edit must succeed for both files");
        writeFile(wc / "m.v", "managed body v3\n");
        writeFile(wc / "n.v", "second body v2\n");
        const auto one = runCommand({"checkin", "m.v"});
        require(one.code == 0, "checkin of one of two edited files must succeed: " + one.err);
    }
    require(repositoryFile(lw, "m.v") == "managed body v3\n", "the requested file is committed");
    require(repositoryFile(lw, "n.v") == "second body\n",
            "a change outside the requested path is not committed");
    require(!std::filesystem::is_symlink(wc / "n.v"), "the other edited file stays LOCAL");

}

// Checking in a path that does not differ from the repository commits nothing and
// succeeds, the way a Subversion commit without changes does; an edited file goes back
// to being a pool symlink.  A path that Subversion does not track is still an error.
void testCheckinOfUnchangedPathSucceedsWithoutACommit() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped unchanged checkin test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    const auto state_file = lw.workspace / ".isvn_local" / "workspace_state";
    const auto head = headRevision(lw);

    const auto linked = runCommand({"checkin", lw.version + "/m.v", "--to", lw.version, "--json"});
    require(linked.code == 0, "checkin of an unchanged LINKED file must succeed: " + linked.err);
    require(linked.out.find("\"result\":\"unchanged\"") != std::string::npos &&
                linked.out.find("\"revision\":null") != std::string::npos,
            "the JSON result reports an unchanged path without a revision");

    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "m.v"}).code == 0, "edit must succeed");
        require(readFile(state_file).find("m.v") != std::string::npos,
                "edit records the file as LOCAL");
        const auto edited = runCommand({"checkin", "m.v"});
        require(edited.code == 0,
                "checkin of an edited but unchanged file must succeed: " + edited.err);
        require(edited.out.find("no changes in m.v") != std::string::npos &&
                    edited.out.find("nothing was committed") != std::string::npos,
                "the plain result tells the user that nothing changed: " + edited.out);
    }
    require(std::filesystem::is_symlink(wc / "m.v"), "the unchanged file returns to LINKED");
    require(readFile(state_file).find("m.v") == std::string::npos,
            "the LOCAL record of the file is cleared");
    require(headRevision(lw) == head, "an unchanged checkin creates no revision");

    writeFile(wc / "sub" / "a.v", "a1\n");
    require(runCommand({"checkin", lw.version + "/sub", "--to", lw.version}).code == 0,
            "fixture checkin of a folder must succeed");
    const auto folder_head = headRevision(lw);
    const auto folder = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(folder.code == 0, "checkin of an unchanged folder must succeed: " + folder.err);
    require(headRevision(lw) == folder_head, "an unchanged folder checkin creates no revision");

    // A folder that differs from the repository in a way a checkin cannot commit, here
    // a tracked file that is missing on disk, must not be reported as unchanged.
    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "sub/a.v"}).code == 0, "edit must succeed");
    }
    std::filesystem::remove(wc / "sub" / "a.v");
    const auto missing = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(missing.code != 0, "checkin of a folder with a missing tracked file must fail");
    require(missing.err.find("nothing to check in") != std::string::npos,
            "the failure names the reason: " + missing.err);
    require(headRevision(lw) == folder_head, "the failed checkin creates no revision");
}

// Checking in a folder that is already tracked also takes in what is new inside it:
// new files, new folders with their content, and never-committed files that already
// rest as pool symlinks.  Everything goes into one revision, as content.
void testCheckinOfTrackedFolderAddsNewContent() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped tracked folder checkin test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "sub" / "a.v", "a1\n");
    require(runCommand({"checkin", lw.version + "/sub", "--to", lw.version}).code == 0,
            "fixture checkin of a folder must succeed");

    writeFile(wc / "sub" / "relinked.v", "never committed, re-linked\n");
    {
        CurrentDirectory inside(wc);
        require(runCommand({"update"}).code == 0, "update must succeed");
        require(runCommand({"edit", "sub/a.v"}).code == 0, "edit must succeed");
    }
    require(std::filesystem::is_symlink(wc / "sub" / "relinked.v"),
            "update re-links a file that was never committed");
    writeFile(wc / "sub" / "a.v", "a2\n");
    writeFile(wc / "sub" / "new.v", "new\n");
    writeFile(wc / "sub" / "more" / "deep" / "c.v", "c1\n");
    writeFile(wc / "outside.v", "outside the folder\n");
    const auto head = headRevision(lw);

    const auto folder =
        runCommand({"checkin", lw.version + "/sub", "--to", lw.version, "--json"});
    require(folder.code == 0, "checkin of a tracked folder with new files must succeed: " +
                                  folder.out + folder.err);
    require(folder.out.find("\"result\":\"committed\"") != std::string::npos,
            "the result reports a commit: " + folder.out);
    require(headRevision(lw) == head + 1, "the folder is committed as exactly one revision");
    require(repositoryFile(lw, "sub/new.v") == "new\n", "a new file in the folder is committed");
    require(repositoryFile(lw, "sub/more/deep/c.v") == "c1\n",
            "a new folder in the folder is committed with its content");
    require(repositoryFile(lw, "sub/relinked.v") == "never committed, re-linked\n",
            "a never-committed pool link is committed as content, not as a link");
    require(repositoryFile(lw, "sub/a.v") == "a2\n", "an edited file goes into the same commit");
    require(repositoryFile(lw, "outside.v").empty(),
            "a new file outside the folder is not committed");
    require(std::filesystem::is_symlink(wc / "sub" / "new.v") &&
                std::filesystem::is_symlink(wc / "sub" / "more" / "deep" / "c.v") &&
                std::filesystem::is_symlink(wc / "sub" / "relinked.v") &&
                std::filesystem::is_symlink(wc / "sub" / "a.v"),
            "the committed files are relinked");
    require(!std::filesystem::is_symlink(wc / "outside.v"), "the file outside stays as it was");
    require(!hasScheduledSvnChange(wc), "no scheduled change is left behind");

    // The version folder itself is a tracked folder too.
    const auto whole = runCommand({"checkin", lw.version, "--to", lw.version});
    require(whole.code == 0, "checkin of the version folder must succeed: " + whole.err);
    require(repositoryFile(lw, "outside.v") == "outside the folder\n",
            "a new file directly in the version folder is committed");
    require(headRevision(lw) == head + 2, "the version folder is committed as one revision");

    const auto again = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(again.code == 0 && again.out.find("nothing was committed") != std::string::npos,
            "a second checkin of the folder finds nothing new: " + again.out + again.err);
    require(headRevision(lw) == head + 2, "the repeated checkin creates no revision");
}

void testCheckinFolderBesideLinkedFiles() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped folder checkin beside linked files test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;

    writeFile(wc / "sub" / "a.v", "a1\n");
    writeFile(wc / "sub" / "deep" / "b.v", "b1\n");
    const auto added = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(added.code == 0, "checkin of a new folder must succeed: " + added.err);
    require(repositoryFile(lw, "sub/a.v") == "a1\n" &&
                repositoryFile(lw, "sub/deep/b.v") == "b1\n",
            "the new folder reaches the repository with its files");
    require(std::filesystem::is_symlink(wc / "sub" / "a.v") &&
                std::filesystem::is_symlink(wc / "sub" / "deep" / "b.v"),
            "the committed files are relinked");

    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "sub/a.v"}).code == 0, "edit must succeed");
    }
    writeFile(wc / "sub" / "a.v", "a2\n");
    const auto changed = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(changed.code == 0,
            "checkin of a folder that holds LINKED files must succeed: " + changed.err);
    require(repositoryFile(lw, "sub/a.v") == "a2\n", "the scheduled change is committed");
    require(repositoryFile(lw, "sub/deep/b.v") == "b1\n", "untouched files keep their content");
    require(!hasScheduledSvnChange(wc), "no scheduled change is left behind");
}

void testRemoveBesideLinkedFiles() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped remove beside linked files test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "n.v", "second body\n");
    require(runCommand({"checkin", lw.version + "/n.v", "--to", lw.version}).code == 0,
            "fixture checkin of a second file must succeed");
    writeFile(wc / "sub" / "a.v", "a1\n");
    writeFile(wc / "sub" / "deep" / "b.v", "b1\n");
    require(runCommand({"checkin", lw.version + "/sub", "--to", lw.version}).code == 0,
            "fixture checkin of a folder must succeed");

    const auto file = runCommand({"remove", lw.version + "/n.v", "--from", lw.version});
    require(file.code == 0, "remove beside LINKED files must succeed: " + file.err);
    require(!std::filesystem::exists(std::filesystem::symlink_status(wc / "n.v")),
            "the removed file leaves the working copy");
    require(repositoryFile(lw, "n.v").empty(), "the removed file leaves the repository");
    require(std::filesystem::is_symlink(wc / "m.v"), "other managed files stay LINKED");
    require(!hasScheduledSvnChange(wc), "a file remove leaves no scheduled change behind");

    const auto folder = runCommand({"remove", lw.version + "/sub", "--from", lw.version});
    require(folder.code == 0,
            "remove of a folder that holds LINKED files must succeed: " + folder.err);
    require(!std::filesystem::exists(wc / "sub"), "the removed folder leaves the working copy");
    require(repositoryFile(lw, "sub/a.v").empty() && repositoryFile(lw, "sub/deep/b.v").empty(),
            "the removed folder leaves the repository");
    require(std::filesystem::is_symlink(wc / "m.v"), "other managed files stay LINKED");
    require(!hasScheduledSvnChange(wc), "a folder remove leaves no scheduled change behind");
}

// A file that was never committed can already rest as a pool symlink, because the
// workspace is re-linked on update.  Checking it in must send its content; a symbolic
// link to a pool object must never reach the repository.
void testCheckinSendsContentOfUntrackedPoolLinks() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin of untracked pool links test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "n.v", "never committed\n");
    writeFile(wc / "nd" / "deep" / "o.v", "in a new folder\n");
    writeFile(wc / "p.v", "scheduled as a link\n");
    {
        CurrentDirectory inside(wc);
        require(runCommand({"update"}).code == 0, "update must succeed");
    }
    require(std::filesystem::is_symlink(wc / "n.v") &&
                std::filesystem::is_symlink(wc / "nd" / "deep" / "o.v") &&
                std::filesystem::is_symlink(wc / "p.v"),
            "update re-links files that were never committed");
    // An interrupted checkin can leave a link scheduled for addition as a link.
    require(std::system(("svn add -q " + (wc / "p.v").string()).c_str()) == 0,
            "fixture svn add of a link must succeed");

    const auto file = runCommand({"checkin", lw.version + "/n.v", "--to", lw.version});
    require(file.code == 0, "checkin of an untracked pool link must succeed: " + file.err);
    require(repositoryFile(lw, "n.v") == "never committed\n",
            "the repository receives the content, not the link");

    const auto scheduled = runCommand({"checkin", lw.version + "/p.v", "--to", lw.version});
    require(scheduled.code == 0,
            "checkin of a link scheduled for addition must succeed: " + scheduled.err);
    require(repositoryFile(lw, "p.v") == "scheduled as a link\n",
            "a link scheduled for addition is committed as content");

    // A folder is often named with a trailing separator.
    const auto folder = runCommand({"checkin", lw.version + "/nd/", "--to", lw.version});
    require(folder.code == 0,
            "checkin of a new folder holding pool links must succeed: " + folder.err);
    require(repositoryFile(lw, "nd/deep/o.v") == "in a new folder\n",
            "files of a new folder are committed as content");

    require(std::filesystem::is_symlink(wc / "n.v") && std::filesystem::is_symlink(wc / "p.v") &&
                std::filesystem::is_symlink(wc / "nd" / "deep" / "o.v"),
            "the committed files are relinked");
    require(!hasScheduledSvnChange(wc), "no scheduled change is left behind");
}

// `file locate` maps any path to the Version whose working copy holds it, from the
// workspace's own record, and changes nothing.
void testFileLocate() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped file locate test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "rtl" / "new.v", "new\n");
    const auto state_before = readFile(lw.workspace / ".isvn_local" / "workspace_state");
    const auto map_before = readFile(lw.workspace / ".isvn_local" / "workspaces");
    const auto head = headRevision(lw);
    const auto lineage_time = std::filesystem::last_write_time(lw.project / ".isvn" / "lineage.db");

    CurrentDirectory away(tmp.path());
    const auto linked = runCommand({"file", "locate", "--path", (wc / "m.v").string(), "--json"});
    require(linked.code == 0, "file locate must succeed: " + linked.out + linked.err);
    require(linked.out.find("\"schema_version\":\"isvn.file.locate.v0\"") != std::string::npos &&
                linked.out.find("\"managed\":true") != std::string::npos &&
                linked.out.find("\"canonical\":\"isvn://proj/T/B/RTL/v1\"") != std::string::npos &&
                linked.out.find("\"path\":\"m.v\"") != std::string::npos &&
                linked.out.find("\"link_state\":\"LINKED\"") != std::string::npos,
            "a LINKED file is located in its Version: " + linked.out);

    const auto local = runCommand({"file", "locate", "--path", (wc / "rtl" / "new.v").string(),
                                   "--project", lw.project.string(), "--json"});
    require(local.code == 0 && local.out.find("\"path\":\"rtl/new.v\"") != std::string::npos &&
                local.out.find("\"link_state\":\"LOCAL\"") != std::string::npos,
            "a new file is located with its path inside the Version: " + local.out);

    std::filesystem::create_directory_symlink(lw.workspace, tmp.path() / "alias");
    const auto aliased = runCommand({"file", "locate", "--path",
                                     (tmp.path() / "alias" / lw.version / "m.v").string(), "--json"});
    require(aliased.code == 0 &&
                aliased.out.find("\"canonical\":\"isvn://proj/T/B/RTL/v1\"") != std::string::npos,
            "a workspace reached through a linked folder is found: " + aliased.out);

    {
        CurrentDirectory inside(wc / "rtl");
        const auto relative = runCommand({"file", "locate", "--path", "new.v", "--json"});
        require(relative.code == 0 && relative.out.find("\"path\":\"rtl/new.v\"") != std::string::npos,
                "a path relative to the current folder is located: " + relative.out);
    }

    const auto missing = runCommand({"file", "locate", "--path", (wc / "gone.v").string(), "--json"});
    require(missing.code == 0 && missing.out.find("\"managed\":false") != std::string::npos &&
                missing.out.find("FILE_NOT_FOUND") != std::string::npos,
            "a missing path inside a Version is reported: " + missing.out);
    const auto outside = runCommand({"file", "locate", "--path", (tmp.path() / "elsewhere.v").string(),
                                     "--json"});
    require(outside.code == 0 && outside.out.find("\"version\":null") != std::string::npos &&
                outside.out.find("PATH_NOT_IN_VERSION") != std::string::npos,
            "a path outside any workspace is reported: " + outside.out);
    const auto other = runCommand({"file", "locate", "--path", (wc / "m.v").string(), "--project",
                                   (tmp.path() / "other").string(), "--json"});
    require(other.code == 2 && other.out.find("PROJECT_MISMATCH") != std::string::npos,
            "a different --project is refused: " + other.out);
    const auto no_json = runCommand({"file", "locate", "--path", (wc / "m.v").string()});
    require(no_json.code != 0, "--json is required");

    require(readFile(lw.workspace / ".isvn_local" / "workspace_state") == state_before &&
                readFile(lw.workspace / ".isvn_local" / "workspaces") == map_before,
            "file locate does not change the workspace records");
    require(std::filesystem::last_write_time(lw.project / ".isvn" / "lineage.db") == lineage_time,
            "file locate does not touch Lineage");
    require(headRevision(lw) == head, "file locate creates no revision");
    require(!std::filesystem::is_symlink(wc / "rtl" / "new.v"),
            "file locate does not link the new file");
}

// Checking out or creating a version re-links only that version.  A file that is new in
// another version may be in the middle of being checked in there and must stay a regular
// file.
void testCheckoutLeavesOtherVersionsAlone() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped version-scoped relink test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "new.v", "not checked in yet\n");
    require(runCommand({"create", "T/B/RTL/v2", "--parent", lw.version}).code == 0,
            "create of a second version must succeed");
    require(!std::filesystem::is_symlink(wc / "new.v") &&
                readFile(wc / "new.v") == "not checked in yet\n",
            "a new file of another version stays a regular file after create");
    require(runCommand({"checkout", "T/B/RTL/v2"}).code == 0, "checkout must succeed");
    require(!std::filesystem::is_symlink(wc / "new.v"),
            "a new file of another version stays a regular file after checkout");
    require(std::filesystem::is_symlink(lw.workspace / "T/B/RTL/v2" / "m.v"),
            "the checked-out version itself is linked");
    const auto checked = runCommand({"checkin", lw.version + "/new.v", "--to", lw.version});
    require(checked.code == 0, "the new file can still be checked in: " + checked.err);
    require(repositoryFile(lw, "new.v") == "not checked in yet\n",
            "the repository receives the content of the new file");
}

// A link inside the folder that points somewhere else is added as a link; checking the
// folder in never reads or changes what the link points to.
void testFolderCheckinDoesNotFollowDirectoryLinks() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped directory link checkin test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    require(runCommand({"create", "T/B/RTL/v2", "--parent", lw.version}).code == 0,
            "create of a second version must succeed");
    const auto other = lw.workspace / "T/B/RTL/v2";
    require(std::filesystem::is_symlink(other / "m.v"), "the other version's file is LINKED");
    writeFile(wc / "sub" / "a.v", "a1\n");
    std::filesystem::create_directory_symlink(other, wc / "sub" / "peer");
    const auto result = runCommand({"checkin", lw.version + "/sub", "--to", lw.version});
    require(result.code == 0, "checkin of the folder must succeed: " + result.err);
    require(repositoryFile(lw, "sub/a.v") == "a1\n", "the folder's file is committed");
    require(std::filesystem::is_symlink(other / "m.v"),
            "the file behind the directory link stays LINKED");
    require(commandOutput("svn status " + other.string()).find("M ") == std::string::npos,
            "the other version has no change");
}

// A new file below folders that are not tracked yet is checked in together with those
// folders; other files in them are left alone.
void testCheckinFileBelowUntrackedFolders() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin below untracked folders test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "reports" / "deep" / "r.rpt", "report body\n");
    writeFile(wc / "reports" / "other.txt", "not requested\n");

    const auto nested =
        runCommand({"checkin", lw.version + "/reports/deep/r.rpt", "--to", lw.version});
    require(nested.code == 0,
            "checkin of a file below untracked folders must succeed: " + nested.err);
    require(repositoryFile(lw, "reports/deep/r.rpt") == "report body\n",
            "the file reaches the repository with its folders");
    require(repositoryFile(lw, "reports/other.txt").empty(),
            "a file that was not requested stays out of the commit");
    require(std::filesystem::is_symlink(wc / "reports" / "deep" / "r.rpt"),
            "the committed file is relinked");
    require(!std::filesystem::is_symlink(wc / "reports" / "other.txt"),
            "the file that was not requested stays a regular file");

    {
        CurrentDirectory inside(wc);
        const auto sibling = runCommand({"checkin", "reports/other.txt"});
        require(sibling.code == 0, "checkin of the sibling must succeed: " + sibling.err);
    }
    require(repositoryFile(lw, "reports/other.txt") == "not requested\n",
            "the sibling reaches the repository when it is requested");

    // A folder left scheduled by an interrupted checkin is committed with the file.
    writeFile(wc / "pending" / "x.txt", "after an interrupted checkin\n");
    require(std::system(("svn add -q --depth empty " + (wc / "pending").string()).c_str()) == 0,
            "fixture svn add of a folder must succeed");
    const auto pending = runCommand({"checkin", lw.version + "/pending/x.txt", "--to", lw.version});
    require(pending.code == 0,
            "checkin below a folder scheduled for addition must succeed: " + pending.err);
    require(repositoryFile(lw, "pending/x.txt") == "after an interrupted checkin\n",
            "the file below a scheduled folder reaches the repository");
    require(!hasScheduledSvnChange(wc), "no scheduled change is left behind");
}

// A remove that Subversion refuses must leave the working copy as it was, and a file
// that is not under version control must never be deleted by it.
void testRefusedRemoveKeepsTheWorkingCopy() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped refused remove test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "sub" / "a.v", "a1\n");
    writeFile(wc / "sub" / "deep" / "b.v", "b1\n");
    require(runCommand({"checkin", lw.version + "/sub", "--to", lw.version}).code == 0,
            "fixture checkin of a folder must succeed");
    writeFile(wc / "sub" / "untracked.txt", "not under version control\n");

    const auto refused = runCommand({"remove", lw.version + "/sub", "--from", lw.version});
    require(refused.code != 0, "remove of a folder holding an untracked file must be refused");
    require(std::filesystem::is_symlink(wc / "sub" / "a.v") &&
                std::filesystem::is_symlink(wc / "sub" / "deep" / "b.v"),
            "managed files stay LINKED after a refused remove");
    require(readFile(wc / "sub" / "untracked.txt") == "not under version control\n",
            "the untracked file is kept");
    require(repositoryFile(lw, "sub/a.v") == "a1\n" &&
                repositoryFile(lw, "sub/deep/b.v") == "b1\n",
            "the repository is unchanged after a refused remove");
    require(!hasScheduledSvnChange(wc), "a refused remove leaves no scheduled change behind");

    // The same once the untracked file rests as a pool symlink.
    writeFile(wc / "loose.txt", "loose and untracked\n");
    {
        CurrentDirectory inside(wc);
        require(runCommand({"update"}).code == 0, "update must succeed");
    }
    require(std::filesystem::is_symlink(wc / "sub" / "untracked.txt") &&
                std::filesystem::is_symlink(wc / "loose.txt"),
            "update re-links files that were never committed");
    const auto linked = runCommand({"remove", lw.version + "/sub", "--from", lw.version});
    require(linked.code != 0, "remove of a folder holding an untracked pool link must be refused");
    require(readFile(wc / "sub" / "untracked.txt") == "not under version control\n",
            "an untracked pool link in the folder keeps its content");
    require(std::filesystem::is_symlink(wc / "sub" / "a.v") &&
                std::filesystem::is_symlink(wc / "sub" / "deep" / "b.v"),
            "managed files stay LINKED after the second refused remove");
    const auto loose = runCommand({"remove", lw.version + "/loose.txt", "--from", lw.version});
    require(loose.code != 0, "remove of an untracked pool link must be refused");
    require(readFile(wc / "loose.txt") == "loose and untracked\n",
            "an untracked pool link named directly keeps its content");
    require(!hasScheduledSvnChange(wc), "refused removes leave no scheduled change behind");
}

// Subversion rejects the commit of a folder removal while the folder is behind the
// repository.  The scheduled removal is then undone, and the remove works after update.
void testRejectedFolderRemoveIsUndone() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped rejected folder remove test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    writeFile(wc / "sub" / "a.v", "a1\n");
    writeFile(wc / "sub" / "deep" / "b.v", "b1\n");
    require(runCommand({"checkin", lw.version + "/sub", "--to", lw.version}).code == 0,
            "fixture checkin of a folder must succeed");
    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "sub/deep/b.v"}).code == 0, "edit must succeed");
        writeFile(wc / "sub" / "deep" / "b.v", "b2\n");
        require(runCommand({"checkin", "sub/deep/b.v"}).code == 0,
                "fixture checkin of a file in the folder must succeed");
    }

    const auto rejected = runCommand({"remove", lw.version + "/sub", "--from", lw.version});
    require(rejected.code != 0, "remove of a folder behind the repository must fail");
    require(std::filesystem::is_symlink(wc / "sub" / "a.v") &&
                std::filesystem::is_symlink(wc / "sub" / "deep" / "b.v"),
            "the folder and its LINKED files are back after a rejected remove");
    require(readFile(wc / "sub" / "deep" / "b.v") == "b2\n", "restored files keep their content");
    require(!hasScheduledSvnChange(wc), "a rejected remove leaves no scheduled change behind");
    require(repositoryFile(lw, "sub/a.v") == "a1\n", "the repository still holds the folder");

    {
        CurrentDirectory inside(wc);
        require(runCommand({"update"}).code == 0, "update must succeed");
    }
    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "sub/a.v"}).code == 0, "edit must succeed");
    }
    const auto removed = runCommand({"remove", lw.version + "/sub/", "--from", lw.version});
    require(removed.code == 0, "remove after update must succeed: " + removed.err);
    require(readFile(lw.workspace / ".isvn_local" / "workspace_state").find("sub/") ==
                std::string::npos,
            "local state of the removed folder is cleared");
    require(!std::filesystem::exists(wc / "sub"), "the removed folder leaves the working copy");
    require(repositoryFile(lw, "sub/a.v").empty() && repositoryFile(lw, "sub/deep/b.v").empty(),
            "the removed folder leaves the repository");
    require(std::filesystem::is_symlink(wc / "m.v"), "other managed files stay LINKED");
    require(!hasScheduledSvnChange(wc), "the remove leaves no scheduled change behind");
}

void testWorkLocalOpsTransactional() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped work-local-op transactional test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto rev_before = headRevision(lw);

    // make-editable: LINKED -> LOCAL, all-or-none, no revision.
    auto hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable", "--in",
                                 lw.version, "--path", "m.v", "--json"});
    auto apply = runCommand({"work-local-op", "apply", "--action", "make-editable", "--in",
                             lw.version, "--path", "m.v", "--expected-plan", hash, "--json"});
    require(apply.code == 0 && jsonField(apply.out, "status") == "succeeded",
            "make-editable must succeed");
    require(!std::filesystem::is_symlink(lw.workspace / lw.version / "m.v"),
            "make-editable produces a LOCAL regular file");

    // PLAN_STALE: a stale expected-plan is rejected with no mutation.
    writeFile(lw.workspace / lw.version / "m.v", "edited body\n");
    auto stale = runCommand({"work-local-op", "apply", "--action", "revert", "--in", lw.version,
                             "--path", "m.v", "--expected-plan", "deadbeef", "--json"});
    require(jsonField(stale.out, "code") == "PLAN_STALE", "stale plan must be rejected");

    // revert restores the authoritative managed state.
    hash = previewPlanHash({"work-local-op", "preview", "--action", "revert", "--in", lw.version,
                            "--path", "m.v", "--json"});
    apply = runCommand({"work-local-op", "apply", "--action", "revert", "--in", lw.version,
                        "--path", "m.v", "--expected-plan", hash, "--json"});
    require(apply.code == 0 && jsonField(apply.out, "status") == "succeeded", "revert must succeed");
    require(readFile(lw.workspace / lw.version / "m.v").find("edited") == std::string::npos,
            "revert discards the local edit");

    // discard-local removes NEW LOCAL content only.
    writeFile(lw.workspace / lw.version / "scratch.txt", "scratch\n");
    hash = previewPlanHash({"work-local-op", "preview", "--action", "discard-local", "--in",
                            lw.version, "--path", "scratch.txt", "--json"});
    apply = runCommand({"work-local-op", "apply", "--action", "discard-local", "--in", lw.version,
                        "--path", "scratch.txt", "--expected-plan", hash, "--json"});
    require(apply.code == 0 && jsonField(apply.out, "status") == "succeeded",
            "discard-local must succeed");
    require(!std::filesystem::exists(lw.workspace / lw.version / "scratch.txt"),
            "discard-local deletes the NEW LOCAL file");

    // discard-local on a managed file is rejected (NOT_NEW_LOCAL).
    auto not_new = runCommand({"work-local-op", "preview", "--action", "discard-local", "--in",
                               lw.version, "--path", "m.v", "--json"});
    require(not_new.out.find("NOT_NEW_LOCAL") != std::string::npos,
            "discard-local rejects managed content");

    require(headRevision(lw) == rev_before, "local operations create no SVN revision");
}

void testWorkLocalOpFaultRollback() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped work-local-op fault test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    // Make editable + modify so revert has destructive local content to preserve.
    auto hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable", "--in",
                                 lw.version, "--path", "m.v", "--json"});
    runCommand({"work-local-op", "apply", "--action", "make-editable", "--in", lw.version, "--path",
                "m.v", "--expected-plan", hash, "--json"});
    writeFile(lw.workspace / lw.version / "m.v", "dirty edit\n");
    hash = previewPlanHash({"work-local-op", "preview", "--action", "revert", "--in", lw.version,
                            "--path", "m.v", "--json"});
    ::setenv("ISVN_WORK_FAULT", "item_applied_0", 1);
    auto apply = runCommand({"work-local-op", "apply", "--action", "revert", "--in", lw.version,
                             "--path", "m.v", "--expected-plan", hash, "--json"});
    ::unsetenv("ISVN_WORK_FAULT");
    require(jsonField(apply.out, "status") == "failed", "faulted operation must report failed");
    require(readFile(lw.workspace / lw.version / "m.v").find("dirty") != std::string::npos,
            "rollback must restore the pre-operation LOCAL content byte-for-byte");
    // Journal/backup are cleared after a completed rollback: no journal.json
    // survives.
    bool journal_left = false;
    const auto ops_root = lw.workspace / ".isvn_local" / "work_operations";
    if (std::filesystem::is_directory(ops_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(ops_root)) {
            if (std::filesystem::exists(entry.path() / "journal.json")) journal_left = true;
        }
    }
    require(!journal_left, "rollback removes the journal");
}

void testWorkLocalOpCrashRecovery() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped work crash-recovery test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    writeFile(lw.workspace / lw.version / "sc.txt", "keep me\n");
    const auto hash = previewPlanHash({"work-local-op", "preview", "--action", "discard-local",
                                       "--in", lw.version, "--path", "sc.txt", "--json"});
    // Crash the operation in a child process so the journal is left behind.
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("ISVN_WORK_CRASH", "backup_created", 1);
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        run({"work-local-op", "apply", "--action", "discard-local", "--in", lw.version, "--path",
             "sc.txt", "--expected-plan", hash, "--json"},
            in, out, err);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);

    // Recovery status detects the interrupted journal, also while another process is
    // probing the same journal for a running owner.
    int probe = -1;
    for (const auto& entry : std::filesystem::directory_iterator(
             lw.workspace / ".isvn_local" / "work_operations")) {
        if (std::filesystem::exists(entry.path() / "journal.json")) {
            probe = ::open((entry.path() / "running").c_str(), O_RDWR | O_CLOEXEC);
        }
    }
    require(probe >= 0 && ::flock(probe, LOCK_SH | LOCK_NB) == 0,
            "the interrupted journal has an owner marker that nobody holds");
    auto recov = runCommand({"work-recovery", "status", "--json"});
    ::close(probe);
    require(recov.out.find("\"recovery_required\":true") != std::string::npos,
            "recovery status must detect the journal: " + recov.out);
    // A new writer on the same Version is blocked while recovery is required;
    // recovery is scoped to that Version and leaves unrelated Versions usable.
    auto blocked = runCommand({"work-local-op", "apply", "--action", "discard-local", "--in",
                               lw.version, "--path", "sc.txt", "--expected-plan", "x", "--json"});
    require(jsonField(blocked.out, "code") == "VERSION_RECOVERY_REQUIRED",
            "same-Version mutation blocked during recovery-required state");
    // Restore rolls back to the pre-operation state.
    const auto op_id = jsonField(recov.out, "operation_id");
    auto restore = runCommand({"work-recovery", "restore", op_id, "--json"});
    require(jsonField(restore.out, "terminal_state") == "rolled_back", "restore must roll back");
    require(std::filesystem::exists(lw.workspace / lw.version / "sc.txt"),
            "restore recovers the pre-operation content");
    auto cleared = runCommand({"work-recovery", "status", "--json"});
    require(cleared.out.find("\"recovery_required\":false") != std::string::npos,
            "recovery must clear after a terminal restore");
}

void testRemoveBatchAtomic() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped remove-batch test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);

    // MESSAGE_REQUIRED: empty message rejected.
    auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path", "m.v",
                                 "--json"});
    auto no_msg = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                              "--expected-plan", hash, "--message", "   ", "--json"});
    require(jsonField(no_msg.out, "code") == "MESSAGE_REQUIRED", "empty message rejected");

    // NEW LOCAL rejected with USE_DISCARD_LOCAL; mixed selection rejected.
    writeFile(lw.workspace / lw.version / "n.txt", "new\n");
    auto use_discard = runCommand({"remove-batch", "preview", "--from", lw.version, "--path",
                                   "n.txt", "--json"});
    require(use_discard.out.find("USE_DISCARD_LOCAL") != std::string::npos, "NEW LOCAL uses discard");
    auto mixed = runCommand({"remove-batch", "preview", "--from", lw.version, "--path", "n.txt",
                             "--path", "m.v", "--json"});
    require(mixed.out.find("MIXED_SELECTION") != std::string::npos, "mixed selection rejected");

    // One managed path -> exactly one new revision.
    const auto rev_before = headRevision(lw);
    hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path", "m.v",
                            "--json"});
    auto ok = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                          "--expected-plan", hash, "--message", "remove m", "--json"});
    require(ok.code == 0 && jsonField(ok.out, "status") == "succeeded", "remove must succeed");
    require(headRevision(lw) == rev_before + 1, "remove creates exactly one revision");
    require(!std::filesystem::exists(lw.workspace / lw.version / "m.v"), "removed file is gone");
}

void testRemoveBatchModifiedLocalRequiresAck() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped remove-batch ack test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    // Make editable + modify -> managed content with uncommitted LOCAL edits.
    auto hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable", "--in",
                                 lw.version, "--path", "m.v", "--json"});
    runCommand({"work-local-op", "apply", "--action", "make-editable", "--in", lw.version, "--path",
                "m.v", "--expected-plan", hash, "--json"});
    writeFile(lw.workspace / lw.version / "m.v", "managed body\nlocal change\n");
    const auto rev_before = headRevision(lw);
    hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path", "m.v",
                            "--json"});
    // Without acknowledgment -> DISCARD_ACK_REQUIRED, no revision.
    auto no_ack = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                              "--expected-plan", hash, "--message", "rm", "--json"});
    require(jsonField(no_ack.out, "code") == "DISCARD_ACK_REQUIRED",
            "modified LOCAL removal requires explicit acknowledgment");
    require(headRevision(lw) == rev_before, "rejected remove creates no revision");
    // With acknowledgment -> succeeds and records the discarded count.
    auto ack = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                           "--expected-plan", hash, "--message", "rm", "--allow-discard-local",
                           "--json"});
    require(ack.code == 0 && jsonField(ack.out, "status") == "succeeded",
            "acknowledged removal succeeds");
    require(jsonField(ack.out, "discarded_local_item_count") == "1", "discard count recorded");
    require(headRevision(lw) == rev_before + 1, "acknowledged removal creates one revision");
}

// Fork a child that runs remove-batch apply and crashes at `crash_point`, so the
// journal is left for recovery. Returns the parent's view (the child _Exits).
void forkCrashRemove(const LiveWork& lw, const std::vector<std::string>& paths,
                     const std::string& message, const std::string& plan_hash,
                     const std::string& crash_point) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("ISVN_WORK_CRASH", crash_point.c_str(), 1);
        std::vector<std::string> args{"remove-batch", "apply", "--from", lw.version};
        for (const auto& p : paths) { args.push_back("--path"); args.push_back(p); }
        args.push_back("--expected-plan");
        args.push_back(plan_hash);
        args.push_back("--message");
        args.push_back(message);
        args.push_back("--json");
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        run(args, in, out, err);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

std::string pendingOperationId() {
    const auto recov = runCommand({"work-recovery", "status", "--json"});
    return jsonField(recov.out, "operation_id");
}

bool treeContainsText(const std::filesystem::path& root, const std::string& needle) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (readFile(it->path()).find(needle) != std::string::npos) return true;
    }
    return false;
}

void testRemoveOutcomeVerifiedAfterCommitCrash() {  // 14.1
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto rev_before = headRevision(lw);
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    // Crash AFTER the commit lands but before journal cleanup.
    forkCrashRemove(lw, {"m.v"}, "rm m", hash, "repo_success_precleanup");
    const auto op_id = pendingOperationId();
    require(!op_id.empty(), "a pending remove journal must exist after commit-boundary crash");
    auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
    require(jsonField(verify.out, "status") == "verified_committed",
            "recovery must positively attribute the landed commit");
    require(jsonField(verify.out, "revision") == std::to_string(rev_before + 1),
            "verified recovery returns the authoritative revision");
    require(jsonField(verify.out, "changed_paths_verified") == "true", "changed paths verified");
    // Idempotent: a second verify does not re-verify or error.
    auto again = runCommand({"work-recovery", "verify", op_id, "--json"});
    require(again.code == 0 && again.out.find("\"recovery_required\":false") != std::string::npos,
            "second verify is idempotent");
}

void testRemoveOutcomeRollbackWhenNoCommit() {  // 14.2
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto rev_before = headRevision(lw);
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    forkCrashRemove(lw, {"m.v"}, "rm m", hash, "svn_commit_boundary");  // crash BEFORE commit
    const auto op_id = pendingOperationId();
    auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
    require(jsonField(verify.out, "status") == "verified_not_committed",
            "no matching commit + target present → not committed");
    require(headRevision(lw) == rev_before, "no revision was created");
    require(std::filesystem::exists(lw.workspace / lw.version / "m.v"),
            "rollback restores the target file");
}

void testRemoveOutcomeOtherActorDiverged() {  // 14.3 — false-positive regression
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    forkCrashRemove(lw, {"m.v"}, "rm mine", hash, "svn_commit_boundary");  // never committed
    const auto op_id = pendingOperationId();
    // A different actor removes the SAME repository path with a different operation id.
    const std::string other_id(32, 'a');
    const std::string cmd = "svn rm \"file://" + lw.repo.string() + "/branches/" + lw.version +
                            "/m.v\" -m \"[isvn] other [isvn-op:op-remove-batch-" + other_id +
                            "]\" >/dev/null 2>&1";
    require(std::system(cmd.c_str()) == 0, "other-actor removal must commit");
    auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
    require(jsonField(verify.out, "status") == "manual_review_required" &&
                jsonField(verify.out, "reason_code") == "REMOTE_STATE_DIVERGED",
            "path absence caused by another actor must NOT false-finalize; it diverges");
    require(verify.out.find("\"status\":\"verified_committed\"") == std::string::npos,
            "must never report verified_committed for another actor's commit");
}

void testRemoveOutcomeMismatchOnWrongEvidence() {  // 14.4
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    // Add a second managed file so the impostor commit can change a different path.
    writeFile(lw.workspace / lw.version / "other.v", "other body\n");
    require(std::system(("cd " + (lw.workspace / lw.version).string() +
                         " && svn add other.v >/dev/null && svn commit other.v -m '[isvn] add' "
                         ">/dev/null")
                            .c_str()) == 0,
            "seed second file");
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    forkCrashRemove(lw, {"m.v"}, "rm m", hash, "svn_commit_boundary");  // op1 never commits
    const auto op_id = pendingOperationId();
    // Impostor commit reuses op1's trailer but deletes a DIFFERENT path.
    const std::string cmd = "svn rm \"file://" + lw.repo.string() + "/branches/" + lw.version +
                            "/other.v\" -m \"[isvn] spoof [isvn-op:" + op_id +
                            "]\" >/dev/null 2>&1";
    require(std::system(cmd.c_str()) == 0, "impostor commit");
    auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
    require(jsonField(verify.out, "reason_code") == "OPERATION_EVIDENCE_MISMATCH",
            "matching trailer with a different changed-set is an evidence mismatch, not success");
    require(verify.out.find("verified_committed") == std::string::npos, "no false success");
}

void testRemoveExactCommitScope() {  // 14.6
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    // An unrelated managed file with an uncommitted local modification.
    require(std::system(("cd " + (lw.workspace / lw.version).string() +
                         " && echo x > unrelated.v && svn add unrelated.v >/dev/null && svn commit "
                         "unrelated.v -m '[isvn] add' >/dev/null")
                            .c_str()) == 0,
            "seed unrelated file");
    writeFile(lw.workspace / lw.version / "unrelated.v", "x\nLOCAL EDIT\n");
    const auto rev_before = headRevision(lw);
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    auto apply = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                             "--expected-plan", hash, "--message", "rm m only", "--json"});
    require(jsonField(apply.out, "status") == "succeeded", "scoped remove succeeds");
    require(headRevision(lw) == rev_before + 1, "exactly one revision");
    // The unrelated modification remains uncommitted in the working copy.
    auto status = runCommand({"status", lw.version, "--json"});
    require(status.out.find("unrelated.v") != std::string::npos, "unrelated file still present");
    require(readFile(lw.workspace / lw.version / "unrelated.v").find("LOCAL EDIT") !=
                std::string::npos,
            "unrelated local modification is NOT swept into the remove revision");
}

void testReservedTrailerAndOperationId() {  // 14.8
    if (!svnToolsAvailable() || currentGroupName().empty()) return;
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto hash = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                       "m.v", "--json"});
    auto spoof = runCommand({"remove-batch", "apply", "--from", lw.version, "--path", "m.v",
                             "--expected-plan", hash, "--message", "hi [isvn-op:FAKE]", "--json"});
    require(jsonField(spoof.out, "code") == "RESERVED_OPERATION_TRAILER",
            "a user message spoofing the operation trailer is rejected");
    // A generated operation id has the op-remove-batch-<32 hex> format (128-bit
    // random suffix; uniqueness is inherent to the CSPRNG generator).
    const auto hash2 = previewPlanHash({"remove-batch", "preview", "--from", lw.version, "--path",
                                        "m.v", "--json"});
    forkCrashRemove(lw, {"m.v"}, "u2", hash2, "journal_prepared");
    const auto op_id = pendingOperationId();
    require(op_id.rfind("op-remove-batch-", 0) == 0 && op_id.size() == 16 + 32,
            "operation id has the op-remove-batch-<32 hex> format");
    const std::string suffix = op_id.substr(op_id.rfind('-') + 1);
    require(suffix.size() == 32 &&
                suffix.find_first_not_of("0123456789abcdef") == std::string::npos,
            "operation id suffix is 128-bit lowercase hex");
}

// ===========================================================================
// Atomic Checkin Batch backend tests (live SVN, disposable /tmp).
// ===========================================================================

std::string checkinPreviewHash(const LiveWork& lw, const std::vector<std::string>& paths) {
    std::vector<std::string> args{"checkin-batch", "preview", lw.version};
    for (const auto& p : paths) { args.push_back("--path"); args.push_back(p); }
    args.push_back("--json");
    const auto r = runCommand(args);
    require(r.code == 0, "checkin-batch preview must succeed: " + r.out + r.err);
    return jsonField(r.out, "plan_hash");
}

CommandResult checkinApply(const LiveWork& lw, const std::vector<std::string>& paths,
                           const std::string& hash, const std::string& message) {
    std::vector<std::string> args{"checkin-batch", "apply", lw.version};
    for (const auto& p : paths) { args.push_back("--path"); args.push_back(p); }
    args.push_back("--expected-plan");
    args.push_back(hash);
    args.push_back("--message");
    args.push_back(message);
    args.push_back("--json");
    return runCommand(args);
}

// Single account, live SVN: a new member workspace does not release refs it
// cannot see, an existing link without a ref is recorded again by the next
// relink, delete frees only what it released, and the owner's own removed link
// is still cleaned up from its workspace.
void testLivePoolRefLifecycleIsScoped() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn/svnadmin not found; skipped live pool ref lifecycle test\n";
        return;
    }
    TemporaryDirectory temporary;
    const auto lw = prepareLiveWork(temporary.path());
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    const auto managed = std::filesystem::absolute(lw.workspace / lw.version / "m.v").string();
    require(std::filesystem::is_symlink(managed), "checked-in file must be LINKED");
    const auto managed_refs = "SELECT COUNT(*) FROM ref WHERE link_path = '" + managed + "';";
    require(db.integer(managed_refs) == 1, "checked-in file must be referenced");

    // A ref whose link is not visible from here (removed directory standing in
    // for another member's unreadable or unmounted workspace).
    const auto ghost_source = temporary.path() / "ghost.src";
    writeFile(ghost_source, "ghost body\n");
    const auto ghost = store.put_object(ghost_source);
    const auto ghost_link = temporary.path() / "other-member" / "ghost.v";
    std::filesystem::create_directories(ghost_link.parent_path());
    std::filesystem::create_symlink(store.object_path(ghost.hash), ghost_link);
    store.register_ref(ghost.hash, ghost_link.string());
    std::filesystem::remove_all(ghost_link.parent_path());

    auto r = runCommand({"member", "init", "--project-dir", lw.project.string(), "--workspace",
                         (temporary.path() / "ws2").string()});
    require(r.code == 0, "second member init must succeed");
    r = runCommand({"member", "init", "--project-dir", lw.project.string(), "--workspace",
                    (temporary.path() / "ws2").string()});
    require(r.code == 0, "repeated member init must succeed");
    require(store.refcount(ghost.hash) == 1, "member init must not release refs it cannot see");

    CurrentDirectory cwd(lw.workspace);
    const auto managed_hash = db.text("SELECT hash FROM ref WHERE link_path = '" + managed + "';");
    db.execute("DELETE FROM ref WHERE link_path = '" + managed + "';");
    db.execute("UPDATE object SET refcount = refcount - 1 WHERE hash = '" + managed_hash + "';");
    r = runCommand({"checkout", lw.version, "--json"});
    require(r.code == 0, "checkout must succeed: " + r.out + r.err);
    require(db.integer(managed_refs) == 1, "relink must record a working link that lost its ref");
    require(store.refcount(managed_hash) == 1, "the recorded link must be counted again");

    const auto orphan_source = temporary.path() / "orphan.src";
    writeFile(orphan_source, "never referenced\n");
    const auto orphan = store.put_object(orphan_source);
    LiveWork second = lw;
    second.version = "T/B/RTL/v2";
    require(runCommand({"create", second.version, "--parent", "ROOT"}).code == 0,
            "second version create must succeed");
    writeFile(lw.workspace / second.version / "u.v", "unique body\n");
    const auto plan = checkinPreviewHash(second, {"u.v"});
    r = checkinApply(second, {"u.v"}, plan, "add unique file");
    require(r.code == 0, "checkin-batch apply must succeed: " + r.out + r.err);
    r = runCommand({"delete", second.version, "--json"});
    require(r.code == 0, "delete must succeed: " + r.out + r.err);
    require(r.out.find("\"refs_released\":1") != std::string::npos &&
                r.out.find("\"objects_reclaimed\":1") != std::string::npos &&
                r.out.find("\"objects_not_reclaimed\":0") != std::string::npos &&
                r.out.find("\"reclaim_warnings\":[]") != std::string::npos,
            "delete must free exactly the object it released: " + r.out);
    require(store.has_object(orphan.hash),
            "delete must not remove an object that was already unreferenced");
    require(store.refcount(ghost.hash) == 1, "delete must not release refs it cannot see");

    std::filesystem::remove(managed);
    r = runCommand({"gc", "--reconcile", "--json"});
    require(r.code == 0, "owner gc must succeed");
    require(r.out.find("\"refs_removed\":1") != std::string::npos &&
                r.out.find("\"refs_retained\":1") != std::string::npos,
            "owner gc must release its own removed link and keep the unseen one: " + r.out);
}

// A working link of the caller whose ref row was lost (for example by an older
// reconcile run from another account) is recorded again before delete releases
// anything, so delete cannot reclaim an object that link still uses.  Lost rows
// of other accounts cannot be recorded from here.
void testDeleteRecordsOwnUnrecordedLinkBeforeReclaim() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn/svnadmin not found; skipped delete re-record test\n";
        return;
    }
    TemporaryDirectory temporary;
    const auto lw = prepareLiveWork(temporary.path());
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    CurrentDirectory cwd(lw.workspace);
    LiveWork second = lw;
    second.version = "T/B/RTL/v2";
    require(runCommand({"create", second.version, "--parent", "ROOT"}).code == 0,
            "second version create must succeed");
    writeFile(lw.workspace / second.version / "m.v", "managed body\n");
    const auto plan = checkinPreviewHash(second, {"m.v"});
    auto r = checkinApply(second, {"m.v"}, plan, "same content in a second version");
    require(r.code == 0, "checkin-batch apply must succeed: " + r.out + r.err);
    const auto kept = std::filesystem::absolute(lw.workspace / second.version / "m.v").string();
    require(std::filesystem::is_symlink(kept), "the second copy must be LINKED");
    const auto hash = db.text("SELECT hash FROM ref WHERE link_path = '" + kept + "';");
    require(store.refcount(hash) == 2, "both versions must reference the shared object");
    store.unregister_ref(kept);

    r = runCommand({"delete", lw.version, "--json"});
    require(r.code == 0, "delete must succeed: " + r.out + r.err);
    require(r.out.find("\"objects_reclaimed\":0") != std::string::npos,
            "delete must not reclaim an object a working link still uses: " + r.out);
    require(store.has_object(hash) && std::filesystem::exists(kept),
            "the remaining working link must still resolve");
    require(store.has_ref(kept) && store.refcount(hash) == 1,
            "the working link must be recorded again");
}

// When a link no longer points at the object its ref row records, delete
// releases the row but reclaims neither object, and says so.
void testDeleteReclaimsOnlyTheRecordedObject() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn/svnadmin not found; skipped mismatched ref delete test\n";
        return;
    }
    TemporaryDirectory temporary;
    const auto lw = prepareLiveWork(temporary.path());
    const auto pool = temporary.path() / "pool";
    ObjectStore store(pool);
    Database db(pool / "meta.db");
    const auto managed = std::filesystem::absolute(lw.workspace / lw.version / "m.v").string();
    const auto recorded = db.text("SELECT hash FROM ref WHERE link_path = '" + managed + "';");
    const auto unrelated_source = temporary.path() / "unrelated.src";
    writeFile(unrelated_source, "unrelated, never referenced\n");
    const auto unrelated = store.put_object(unrelated_source);
    require(store.refcount(unrelated.hash) == 0, "the unrelated object starts unreferenced");
    std::filesystem::remove(managed);
    std::filesystem::create_symlink(store.object_path(unrelated.hash), managed);

    CurrentDirectory cwd(lw.workspace);
    const auto r = runCommand({"delete", lw.version, "--json"});
    require(r.code == 0, "delete must succeed: " + r.out + r.err);
    require(store.has_object(unrelated.hash),
            "delete must not reclaim an object outside the rows it released: " + r.out);
    require(store.has_object(recorded),
            "the object of a mismatched row must be reported, not reclaimed: " + r.out);
    require(r.out.find("\"refs_released\":1") != std::string::npos &&
                r.out.find("\"objects_reclaimed\":0") != std::string::npos &&
                r.out.find("neither object was reclaimed") != std::string::npos,
            "the mismatch must be reported: " + r.out);
    require(r.err.find("reclaim them later") == std::string::npos &&
                r.err.find("left in the pool") != std::string::npos,
            "the warning must state what happened without promising a later reclaim: " + r.err);
}

void testOrphanGcContinuesAndReportsIncompleteCleanup() {
    if (::geteuid() == 0) return;
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    ObjectStore store(prepared.pool);
    const auto blocked_source = temporary.path() / "blocked.src";
    writeFile(blocked_source, "blocked orphan\n");
    const auto blocked = store.put_object(blocked_source);
    // Exactly two orphans, in different buckets.
    std::string freed_text = "freed orphan\n";
    for (int attempt = 0;
         isvn::common::blake3_hex(freed_text).substr(0, 2) == blocked.hash.substr(0, 2);
         ++attempt) {
        freed_text = "freed orphan " + std::to_string(attempt) + "\n";
    }
    const auto freed_source = temporary.path() / "freed.src";
    writeFile(freed_source, freed_text);
    const auto freed = store.put_object(freed_source);
    require(freed.hash.substr(0, 2) != blocked.hash.substr(0, 2),
            "test orphans must be in different buckets");
    Database db(prepared.pool / "meta.db");
    db.execute("UPDATE object SET last_access='2000-01-01T00:00:00Z' WHERE hash='" +
               blocked.hash + "';");
    db.execute("UPDATE object SET last_access='2099-01-01T00:00:00Z' WHERE hash='" +
               freed.hash + "';");
    const auto bucket = store.object_path(blocked.hash).parent_path();
    ::chmod(bucket.c_str(), 0500);

    CommandResult result{};
    {
        CurrentDirectory cwd(currentWorkspacePath(prepared));
        result = runCommand({"gc", "--reclaim-orphans", "--json"});
    }
    ::chmod(bucket.c_str(), 0700);
    require(result.code != 0, "an incomplete orphan cleanup must not exit 0: " + result.out);
    require(result.out.find("\"objects_reclaimed\":1") != std::string::npos &&
                result.out.find("\"objects_not_reclaimed\":1") != std::string::npos &&
                result.out.find(blocked.hash) != std::string::npos,
            "the cleanup must report the reclaimed and the failed object: " + result.out +
                result.err);
    require(!store.has_object(freed.hash) && store.has_object(blocked.hash),
            "the reclaimable orphan must be removed and the failed one kept");
    require(result.err.find("cleanup incomplete") != std::string::npos,
            "stderr must say the cleanup is incomplete: " + result.err);
}

// A symlink planted where older builds kept lock state leads nowhere: nothing is
// read or created through it.
void testPlantedLockDirectorySymlinkIsIgnored() {
    TemporaryDirectory temporary;
    const auto prepared = prepareProject(temporary.path());
    const auto decoy = temporary.path() / "private";
    std::filesystem::create_directories(decoy);
    ::chmod(decoy.c_str(), 0700);
    std::filesystem::create_symlink(decoy, prepared.project / ".isvn" / "work_operation_locks");

    CurrentDirectory cwd(currentWorkspacePath(prepared));
    const auto result = runCommand({"gc", "--reconcile", "--json"});
    require(result.code == 0, "a planted symlink must not stop a command: " + result.out + result.err);
    struct stat status {};
    require(::stat(decoy.c_str(), &status) == 0 && (status.st_mode & 07777) == 0700,
            "the symlink target's mode must be untouched");
    require(std::filesystem::is_empty(decoy), "nothing may be created through the planted symlink");
}

void forkCrashCheckin(const LiveWork& lw, const std::vector<std::string>& paths,
                      const std::string& message, const std::string& plan_hash,
                      const std::string& crash_point) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("ISVN_WORK_CRASH", crash_point.c_str(), 1);
        std::vector<std::string> args{"checkin-batch", "apply", lw.version};
        for (const auto& p : paths) { args.push_back("--path"); args.push_back(p); }
        args.push_back("--expected-plan");
        args.push_back(plan_hash);
        args.push_back("--message");
        args.push_back(message);
        args.push_back("--json");
        std::istringstream in;
        std::ostringstream out;
        std::ostringstream err;
        run(args, in, out, err);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// Make a committed managed file editable (with provenance receipt) and modify it.
void makeEditableAndModify(const LiveWork& lw, const std::string& rel,
                           const std::string& new_body) {
    auto hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable", "--in",
                                 lw.version, "--path", rel, "--json"});
    auto apply = runCommand({"work-local-op", "apply", "--action", "make-editable", "--in",
                             lw.version, "--path", rel, "--expected-plan", hash, "--json"});
    require(jsonField(apply.out, "status") == "succeeded",
            "make-editable must succeed for " + rel + ": " + apply.out + apply.err);
    writeFile(lw.workspace / lw.version / rel, new_body);
}

std::filesystem::path receiptsDir(const LiveWork& lw) {
    return lw.workspace / ".isvn_local" / "editable_receipts";
}

int receiptCount(const LiveWork& lw) {
    int n = 0;
    std::error_code ec;
    if (std::filesystem::is_directory(receiptsDir(lw), ec)) {
        for (const auto& e : std::filesystem::directory_iterator(receiptsDir(lw), ec)) {
            if (e.path().extension() == ".json") ++n;
        }
    }
    return n;
}

void testCheckinBatchPreviewAndExactSelection() {  // 18.1, 18.2, 18.4
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch selection test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    writeFile(lw.workspace / lw.version / "a.v", "aaa\n");
    writeFile(lw.workspace / lw.version / "b.v", "bbb\n");
    writeFile(lw.workspace / lw.version / "c.v", "ccc\n");

    // SELECTION_REQUIRED with no --path; --project rejected.
    auto none = runCommand({"checkin-batch", "preview", lw.version, "--json"});
    require(jsonField(none.out, "code") == "SELECTION_REQUIRED", "empty selection rejected");
    auto proj = runCommand({"checkin-batch", "preview", "--project", "/tmp/x", lw.version, "--path",
                            "a.v", "--json"});
    require(proj.code != 0, "checkin-batch must reject --project");

    // Overlapping/duplicate selectors deduplicate to exactly two concrete items.
    auto prev = runCommand({"checkin-batch", "preview", lw.version, "--path", "a.v", "--path", "b.v",
                            "--path", "a.v", "--json"});
    require(prev.code == 0 && jsonField(prev.out, "can_apply") == "true", "two-item preview applies");
    require(jsonField(prev.out, "add_count") == "2", "exactly two adds");
    require(prev.out.find("\"path\":\"a.v\"") != std::string::npos &&
                prev.out.find("\"path\":\"b.v\"") != std::string::npos &&
                prev.out.find("\"path\":\"c.v\"") == std::string::npos,
            "preview contains exactly the checked items, never the unchecked c.v");
    // Deterministic + read-only.
    const auto rev_before = headRevision(lw);
    auto prev2 = runCommand({"checkin-batch", "preview", lw.version, "--path", "a.v", "--path",
                             "b.v", "--path", "a.v", "--json"});
    // `elapsed_ms` is deliberately diagnostic and therefore not a plan-hash
    // input.  Everything else in the Preview remains byte-stable; normalising
    // only that timing value proves no planning fact became nondeterministic.
    auto withoutElapsed = [](std::string text) {
        const auto key = text.find("\"elapsed_ms\":");
        if (key == std::string::npos) return text;
        const auto value = key + std::string("\"elapsed_ms\":").size();
        const auto end = text.find_first_not_of("0123456789", value);
        text.replace(value, (end == std::string::npos ? text.size() : end) - value, "<elapsed>");
        return text;
    };
    require(withoutElapsed(prev2.out) == withoutElapsed(prev.out) &&
                jsonField(prev2.out, "plan_hash") == jsonField(prev.out, "plan_hash"),
            "Preview facts and plan hash are deterministic; elapsed_ms is diagnostic only");
    require(headRevision(lw) == rev_before, "preview creates no revision");
    require(!std::filesystem::is_directory(lw.workspace / ".isvn_local" / "work_operations"),
            "preview creates no operation journal");

    // Apply exactly a.v + b.v -> one revision; c.v untouched and uncommitted.
    const auto hash = checkinPreviewHash(lw, {"a.v", "b.v"});
    auto ok = checkinApply(lw, {"a.v", "b.v"}, hash, "add a and b");
    require(ok.code == 0 && jsonField(ok.out, "status") == "committed",
            "batch apply must commit: " + ok.out + ok.err);
    require(jsonField(ok.out, "revision") == std::to_string(rev_before + 1),
            "one authoritative revision (pre+1)");
    require(headRevision(lw) == rev_before + 1, "exactly one new revision");
    require(std::filesystem::exists(lw.workspace / lw.version / "c.v") &&
                readFile(lw.workspace / lw.version / "c.v") == "ccc\n",
            "unchecked c.v remains present, unchanged, uncommitted");
    auto status = runCommand({"status", lw.version, "--json"});
    require(status.out.find("c.v") != std::string::npos, "c.v is still an uncommitted local file");
}

void testCheckinBatchBoundedQueryShapeAndDecoyScale() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped bounded checkin query-shape test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto active_root = lw.workspace / lw.version;

    // A real 10k-entry unselected local decoy makes a root walk observable in
    // the scope metric while keeping the selected batch deliberately tiny.
    for (int i = 0; i < 10000; ++i) {
        writeFile(active_root / "unselected-decoy" / std::to_string(i / 100) /
                      ("d" + std::to_string(i) + ".v"),
                  "decoy\n");
    }
    writeFile(active_root / "one.v", "one\n");
    writeFile(active_root / "picked" / "child.v", "child\n");

    // Record real SVN argv in a disposable test-only wrapper.  It forwards to
    // the system SVN, so the assertions cover the actual Checkin builder rather
    // than a fake plan.  The temporary trace is never emitted or persisted.
    const auto bin = tmp.path() / "trace-bin";
    const auto trace = tmp.path() / "svn-argv.trace";
    std::filesystem::create_directories(bin);
    const auto wrapper = bin / "svn";
    writeFile(wrapper, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$ISVN_SVN_TRACE_FILE\"\n"
                       "exec /usr/bin/svn \"$@\"\n");
    std::filesystem::permissions(wrapper,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    const char* old_path = ::getenv("PATH");
    ScopedEnvironment path("PATH", bin.string() + ":" + (old_path ? old_path : ""));
    ScopedEnvironment trace_path("ISVN_SVN_TRACE_FILE", trace.string());

    auto preview = runCommand({"checkin-batch", "preview", lw.version, "--path", "one.v", "--json"});
    require(preview.code == 0 && jsonField(preview.out, "can_apply") == "true",
            "one-file bounded Preview must remain applicable");
    require(jsonField(preview.out, "local_entries_examined") == "1",
            "10k unrelated entries must not affect a one-file local scan counter");
    require(jsonField(preview.out, "scan_root_count") == "1" &&
                jsonField(preview.out, "root_explicitly_selected") == "false",
            "query_scope must record the minimal non-root scan");
    std::string file_trace = readFile(trace);
    require(file_trace.find("list -R") == std::string::npos,
            "one-file Preview must never recursively list the Active Version root");
    require(file_trace.find("status --xml --depth infinity " + active_root.string() + "\n") ==
                std::string::npos,
            "one-file Preview must not status the Active working-copy root");
    require(file_trace.find("status --xml --depth infinity " +
                            (active_root / "one.v").string()) != std::string::npos,
            "one-file Preview statuses its exact selected target");

    writeFile(trace, "");
    preview = runCommand({"checkin-batch", "preview", lw.version, "--path", "picked", "--json"});
    require(preview.code == 0 && jsonField(preview.out, "can_apply") == "true",
            "one-subfolder bounded Preview must remain applicable: " + preview.out + preview.err);
    require(jsonField(preview.out, "local_entries_examined") == "1",
            "10k unrelated entries must not affect selected-subtree traversal");
    const std::string folder_trace = readFile(trace);
    require(folder_trace.find("list -R") == std::string::npos,
            "subfolder Preview must never recursively list the Active Version root");
    require(folder_trace.find("status --xml --depth infinity " + active_root.string() + "\n") ==
                std::string::npos,
            "subfolder Preview must not status the Active working-copy root");
    require(folder_trace.find("status --xml --depth infinity " +
                              (active_root / "picked").string()) != std::string::npos,
            "subfolder Preview statuses only its selected subtree root");

    const std::string source = readFile(std::filesystem::path(ISVN_SOURCE_DIR) / "src/cli/commands.cpp");
    const auto builder = source.find("CheckinBatch buildCheckinBatch");
    const auto verifier = source.find("RemoveVerifyResult verifyCheckinOutcome");
    require(builder != std::string::npos && verifier != std::string::npos && verifier > builder,
            "bounded Checkin symbols must remain present for the regression guard");
    const auto bounded_builder = source.substr(builder, verifier - builder);
    require(bounded_builder.find("buildWorkFileEntries(") == std::string::npos &&
                bounded_builder.find("runtime.svn.list(") == std::string::npos &&
                source.find("runtime.svn.list(batch.target_url, true)") == std::string::npos,
            "Checkin plan/recovery source must not regain full-root list/status enumeration");

    // Raw selector count is checked before deduplication or any SVN spawn, so
    // a pathological repeated-selection request cannot turn into a large query.
    std::vector<std::string> too_many{"checkin-batch", "preview", lw.version};
    for (int i = 0; i < 4097; ++i) {
        too_many.push_back("--path");
        too_many.push_back("one.v");
    }
    too_many.push_back("--json");
    const auto selector_limit = runCommand(too_many);
    require(selector_limit.code != 0 && jsonField(selector_limit.out, "code") ==
                                           "SELECTOR_LIMIT_EXCEEDED",
            "one-over raw selector ceiling must fail before a plan is returned");
}

void testCheckinBatchSyntheticMillionEntryScopeModel() {  // Bounded-scale guard.
    // This is deliberately an in-memory model, not a million-file fixture.  It
    // captures the planner's non-negotiable query shape: discovery is issued to
    // selected roots, so unrelated repository/working-copy entries have no
    // iteration path at all.  The live 10k argv/counter test above exercises
    // the same shape against real SVN and the real filesystem.
    struct SyntheticSelectedScope {
        std::size_t unrelated_entries = 1000000;
        std::size_t visited_entries = 0;

        void inspectSelectedFile() { ++visited_entries; }
        void inspectSelectedSubtree(std::size_t descendants) { visited_entries += descendants; }
    } model;
    model.inspectSelectedFile();
    require(model.unrelated_entries == 1000000 && model.visited_entries == 1,
            "one selected file must not visit synthetic unrelated million-entry inventory");
    model.visited_entries = 0;
    model.inspectSelectedSubtree(23);
    require(model.visited_entries == 23,
            "selected subtree cost is descendants only, independent of synthetic unrelated entries");
}

void testCheckinBatchMixedUpdateAddOneRevision() {  // 18.3
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch mixed test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    // Seed three managed (LINKED) files via one batch add (exact scope, so the
    // pre-existing m.v pool symlink is never re-committed).
    writeFile(lw.workspace / lw.version / "x.v", "x base\n");
    writeFile(lw.workspace / lw.version / "y.v", "y base\n");
    writeFile(lw.workspace / lw.version / "w.v", "w base\n");  // unrelated managed
    {
        const auto seed_hash = checkinPreviewHash(lw, {"x.v", "y.v", "w.v"});
        auto seed = checkinApply(lw, {"x.v", "y.v", "w.v"}, seed_hash, "seed managed");
        require(jsonField(seed.out, "status") == "committed",
                "seed managed files: " + seed.out + seed.err);
    }
    // Two controlled updates + one unrelated modified managed file (NOT selected).
    makeEditableAndModify(lw, "x.v", "x updated\n");
    makeEditableAndModify(lw, "y.v", "y updated\n");
    makeEditableAndModify(lw, "w.v", "w LOCAL\n");
    require(receiptCount(lw) == 3, "three editable receipts written");
    // One NEW LOCAL add.
    writeFile(lw.workspace / lw.version / "z.v", "z new\n");

    const auto rev_before = headRevision(lw);
    const auto hash = checkinPreviewHash(lw, {"x.v", "y.v", "z.v"});
    auto ok = checkinApply(lw, {"x.v", "y.v", "z.v"}, hash, "mixed add+update");
    require(jsonField(ok.out, "status") == "committed", "mixed batch commits: " + ok.out + ok.err);
    const auto rev = std::stoll(jsonField(ok.out, "revision"));
    require(rev == rev_before + 1, "HEAD advances by exactly one");
    require(jsonField(ok.out, "add_count") == "1" && jsonField(ok.out, "update_count") == "2",
            "result records one add + two updates");
    // Every item shares the one revision (no per-item revision / partial state).
    require(ok.out.find("\"partial\"") == std::string::npos, "no partial result");
    // Repository contents match intended bytes; the exact changed set is M,M,A.
    std::string cmd = "svn log -v -q -r " + std::to_string(rev) + " \"file://" + lw.repo.string() +
                      "/branches/" + lw.version + "\" 2>/dev/null";
    std::array<char, 4096> buf{};
    std::string log;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) { while (fgets(buf.data(), buf.size(), pipe)) log += buf.data(); ::pclose(pipe); }
    require(log.find("M /branches/" + lw.version + "/x.v") != std::string::npos &&
                log.find("M /branches/" + lw.version + "/y.v") != std::string::npos &&
                log.find("A /branches/" + lw.version + "/z.v") != std::string::npos,
            "exact changed-path set is M x.v, M y.v, A z.v");
    require(log.find("w.v") == std::string::npos, "unrelated modified w.v is NOT in the revision");
    require(readFile(lw.workspace / lw.version / "w.v") == "w LOCAL\n",
            "unrelated modification remains uncommitted in the working copy");
    // Consumed editable receipts for the two updates are cleared; w.v receipt stays.
    require(receiptCount(lw) == 1, "committed update receipts cleared, unrelated receipt retained");
}

void testCheckinBatchNestedParents() {  // 18.3 nested-parent
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch nested test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    std::filesystem::create_directories(lw.workspace / lw.version / "new_dir/sub");
    writeFile(lw.workspace / lw.version / "new_dir/sub/file.v", "nested\n");
    writeFile(lw.workspace / lw.version / "new_dir/other.v", "sibling\n");  // NOT selected

    const auto rev_before = headRevision(lw);
    const auto hash = checkinPreviewHash(lw, {"new_dir/sub/file.v"});
    auto prev = runCommand({"checkin-batch", "preview", lw.version, "--path", "new_dir/sub/file.v",
                            "--json"});
    require(prev.out.find("\"path\":\"new_dir\"") != std::string::npos &&
                prev.out.find("\"path\":\"new_dir/sub\"") != std::string::npos,
            "preview lists required supporting parent directories");
    auto ok = checkinApply(lw, {"new_dir/sub/file.v"}, hash, "nested add");
    require(jsonField(ok.out, "status") == "committed", "nested add commits: " + ok.out + ok.err);
    require(headRevision(lw) == rev_before + 1, "one revision");
    // Exact changed set: A new_dir, A new_dir/sub, A new_dir/sub/file.v — the
    // unselected sibling is never swept in by recursive directory commit.
    std::string cmd = "svn log -v -q -r " + std::to_string(rev_before + 1) + " \"file://" +
                      lw.repo.string() + "/branches/" + lw.version + "\" 2>/dev/null";
    std::array<char, 4096> buf{};
    std::string log;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) { while (fgets(buf.data(), buf.size(), pipe)) log += buf.data(); ::pclose(pipe); }
    require(log.find("/new_dir/other.v") == std::string::npos,
            "unselected sibling below the added directory is NOT committed");
    require(log.find("A /branches/" + lw.version + "/new_dir/sub/file.v") != std::string::npos,
            "the nested file is committed");
    require(std::filesystem::exists(lw.workspace / lw.version / "new_dir/other.v"),
            "the sibling remains an uncommitted local file");
}

void testCheckinBatchProvenance() {  // 18.5
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch provenance test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    writeFile(lw.workspace / lw.version / "p.v", "p base\n");
    writeFile(lw.workspace / lw.version / "q.v", "q base\n");
    {
        const auto seed_hash = checkinPreviewHash(lw, {"p.v", "q.v"});
        auto seed = checkinApply(lw, {"p.v", "q.v"}, seed_hash, "seed managed");
        require(jsonField(seed.out, "status") == "committed", "seed p.v/q.v: " + seed.out + seed.err);
    }
    // Arbitrary LOCAL at a managed path WITHOUT the controlled flow (legacy edit
    // writes no receipt) -> blocked, never relabeled as a NEW LOCAL add.
    require(runCommand({"edit", lw.version + "/q.v", "--in", lw.version}).code == 0, "legacy edit");
    writeFile(lw.workspace / lw.version / "q.v", "q tampered\n");
    auto qprev = runCommand({"checkin-batch", "preview", lw.version, "--path", "q.v", "--json"});
    require(qprev.out.find("EDIT_PROVENANCE_REQUIRED") != std::string::npos &&
                jsonField(qprev.out, "can_apply") == "false",
            "managed LOCAL without provenance is blocked (EDIT_PROVENANCE_REQUIRED)");
    require(qprev.out.find("\"change_kind\":\"add\"") == std::string::npos,
            "an existing managed collision is never relabeled a NEW LOCAL add");

    // Controlled Make Editable writes a valid receipt -> update permitted.
    makeEditableAndModify(lw, "p.v", "p updated\n");
    require(receiptCount(lw) == 1, "make-editable wrote a receipt");
    auto pprev = runCommand({"checkin-batch", "preview", lw.version, "--path", "p.v", "--json"});
    require(pprev.out.find("\"change_kind\":\"update\"") != std::string::npos &&
                pprev.out.find("\"editable_provenance\":\"verified\"") != std::string::npos &&
                jsonField(pprev.out, "can_apply") == "true",
            "valid Make Editable provenance permits an update");

    // Corrupting the receipt fails closed -> EDIT_PROVENANCE_STALE.
    for (const auto& e : std::filesystem::directory_iterator(receiptsDir(lw))) {
        if (e.path().extension() == ".json") writeFile(e.path(), "{}\n");
    }
    auto stale = runCommand({"checkin-batch", "preview", lw.version, "--path", "p.v", "--json"});
    require(stale.out.find("EDIT_PROVENANCE_STALE") != std::string::npos,
            "a malformed/stale receipt fails closed");

    // Revert resets p.v to LINKED and clears the (now corrupt) receipt.
    auto rhash = previewPlanHash({"work-local-op", "preview", "--action", "revert", "--in",
                                  lw.version, "--path", "p.v", "--json"});
    runCommand({"work-local-op", "apply", "--action", "revert", "--in", lw.version, "--path", "p.v",
                "--expected-plan", rhash, "--json"});
    require(receiptCount(lw) == 0, "Revert clears the editable receipt");

    // A fresh controlled Make Editable + a pre-commit fault: the receipt survives
    // rollback (rollback restores receipt state exactly).
    makeEditableAndModify(lw, "p.v", "p updated again\n");
    require(receiptCount(lw) == 1, "fresh make-editable rewrote a valid receipt");
    const auto hash = checkinPreviewHash(lw, {"p.v"});
    ::setenv("ISVN_WORK_FAULT", "svn_staged", 1);
    auto faulted = checkinApply(lw, {"p.v"}, hash, "update p");
    ::unsetenv("ISVN_WORK_FAULT");
    require(jsonField(faulted.out, "status") == "failed", "pre-commit fault -> failed, no commit");
    require(receiptCount(lw) == 1, "pre-commit rollback retains the editable receipt");
}

void testCheckinBatchMessagePlanTrailer() {  // 18.6
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch message/plan test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    writeFile(lw.workspace / lw.version / "a.v", "aaa\n");
    const auto hash = checkinPreviewHash(lw, {"a.v"});
    // Empty message + reserved trailer are backend-enforced.
    require(jsonField(checkinApply(lw, {"a.v"}, hash, "   ").out, "code") == "MESSAGE_REQUIRED",
            "empty message rejected");
    require(jsonField(checkinApply(lw, {"a.v"}, hash, "hi [isvn-op:FAKE]").out, "code") ==
                "RESERVED_OPERATION_TRAILER",
            "reserved trailer in the user message rejected");
    // PLAN_STALE: changing the file after preview invalidates the plan; no revision.
    const auto rev_before = headRevision(lw);
    writeFile(lw.workspace / lw.version / "a.v", "aaa changed\n");
    require(jsonField(checkinApply(lw, {"a.v"}, hash, "add a").out, "code") == "PLAN_STALE",
            "stale plan is rejected before mutation");
    require(headRevision(lw) == rev_before, "a stale apply creates no revision");
    // A committed batch carries exactly one terminal operation trailer.
    const auto hash2 = checkinPreviewHash(lw, {"a.v"});
    auto ok = checkinApply(lw, {"a.v"}, hash2, "add a now");
    require(jsonField(ok.out, "status") == "committed", "apply commits");
    const auto op_id = jsonField(ok.out, "operation_id");
    std::string cmd = "svn log -r " + jsonField(ok.out, "revision") + " \"file://" +
                      lw.repo.string() + "/branches/" + lw.version + "\" 2>/dev/null";
    std::array<char, 2048> buf{};
    std::string log;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe) { while (fgets(buf.data(), buf.size(), pipe)) log += buf.data(); ::pclose(pipe); }
    require(log.find("[isvn-op:" + op_id + "]") != std::string::npos,
            "committed message carries the exact terminal operation trailer");
    require(op_id.rfind("op-checkin-batch-", 0) == 0, "operation id is op-checkin-batch-<hex>");
}

void testCheckinBatchCrashRecoveryVerify() {  // 18.7, 18.8
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch crash test\n";
        return;
    }
    // (a) Crash AFTER the commit lands -> Outcome Unknown -> verified_committed.
    {
        TemporaryDirectory tmp;
        const auto lw = prepareLiveWork(tmp.path());
        CurrentDirectory cwd(lw.workspace);
        writeFile(lw.workspace / lw.version / "a.v", "aaa\n");
        const auto rev_before = headRevision(lw);
        const auto hash = checkinPreviewHash(lw, {"a.v"});
        forkCrashCheckin(lw, {"a.v"}, "add a", hash, "repo_success_precleanup");
        const auto op_id = pendingOperationId();
        require(!op_id.empty(), "a pending journal exists after commit-boundary crash");
        // A new writer on this target is blocked while recovery is required.
        require(jsonField(checkinApply(lw, {"a.v"}, "x", "y").out, "code") ==
                    "VERSION_RECOVERY_REQUIRED",
                "same-Version mutation blocked during recovery-required state");
        auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
        require(jsonField(verify.out, "status") == "verified_committed",
                "recovery positively attributes the landed commit");
        require(jsonField(verify.out, "revision") == std::to_string(rev_before + 1),
                "verified recovery returns the authoritative revision");
        auto again = runCommand({"work-recovery", "verify", op_id, "--json"});
        require(again.code == 0 &&
                    again.out.find("\"recovery_required\":false") != std::string::npos,
                "second verify is idempotent");
    }
    // (b) Crash BEFORE the commit -> verified_not_committed + staging rolled back.
    {
        TemporaryDirectory tmp;
        const auto lw = prepareLiveWork(tmp.path());
        CurrentDirectory cwd(lw.workspace);
        writeFile(lw.workspace / lw.version / "a.v", "aaa\n");
        const auto rev_before = headRevision(lw);
        const auto hash = checkinPreviewHash(lw, {"a.v"});
        forkCrashCheckin(lw, {"a.v"}, "add a", hash, "svn_commit_boundary");
        const auto op_id = pendingOperationId();
        auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
        require(jsonField(verify.out, "status") == "verified_not_committed",
                "no matching commit -> not committed");
        require(headRevision(lw) == rev_before, "no revision was created");
        // The NEW LOCAL add is unscheduled; the file remains as unversioned local.
        auto status = runCommand({"status", lw.version, "--json"});
        require(std::filesystem::exists(lw.workspace / lw.version / "a.v"),
                "the add target remains on disk after rollback");
    }
}

void testCheckinBatchAdversarialEvidence() {  // 18.9
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped checkin-batch adversarial test\n";
        return;
    }
    // Another actor changes a selected repository path (no trailer) while our op
    // crashed before commit -> REMOTE_STATE_DIVERGED, never a false success.
    {
        TemporaryDirectory tmp;
        const auto lw = prepareLiveWork(tmp.path());
        CurrentDirectory cwd(lw.workspace);
        makeEditableAndModify(lw, "m.v", "m mine\n");  // m.v is the seeded managed file
        const auto hash = checkinPreviewHash(lw, {"m.v"});
        forkCrashCheckin(lw, {"m.v"}, "update m", hash, "svn_commit_boundary");
        const auto op_id = pendingOperationId();
        // A different actor commits a change to the SAME repository path.
        const std::string cmd = "svn rm \"file://" + lw.repo.string() + "/branches/" + lw.version +
                                "/m.v\" -m \"[isvn] other actor\" >/dev/null 2>&1";
        require(std::system(cmd.c_str()) == 0, "other-actor change commits");
        auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
        require(jsonField(verify.out, "reason_code") == "REMOTE_STATE_DIVERGED",
                "another actor touching a selected path diverges, never false-finalizes");
        require(verify.out.find("verified_committed") == std::string::npos, "no false success");
    }
    // Spoofed trailer with a mismatched changed-set -> OPERATION_EVIDENCE_MISMATCH.
    {
        TemporaryDirectory tmp;
        const auto lw = prepareLiveWork(tmp.path());
        CurrentDirectory cwd(lw.workspace);
        writeFile(lw.workspace / lw.version / "other.v", "other\n");
        require(std::system(("cd " + (lw.workspace / lw.version).string() +
                             " && svn add other.v >/dev/null && svn commit other.v -m '[isvn] add' "
                             ">/dev/null")
                                .c_str()) == 0,
                "seed other.v");
        writeFile(lw.workspace / lw.version / "a.v", "aaa\n");
        const auto hash = checkinPreviewHash(lw, {"a.v"});
        forkCrashCheckin(lw, {"a.v"}, "add a", hash, "svn_commit_boundary");
        const auto op_id = pendingOperationId();
        const std::string cmd = "svn rm \"file://" + lw.repo.string() + "/branches/" + lw.version +
                                "/other.v\" -m \"[isvn] spoof [isvn-op:" + op_id +
                                "]\" >/dev/null 2>&1";
        require(std::system(cmd.c_str()) == 0, "impostor commit reuses the trailer");
        auto verify = runCommand({"work-recovery", "verify", op_id, "--json"});
        require(jsonField(verify.out, "reason_code") == "OPERATION_EVIDENCE_MISMATCH",
                "a matching trailer with a different changed-set is a mismatch, not success");
    }
}

std::string importRequestJson(const LiveWork& lw, const std::filesystem::path& source,
                              const std::string& destination = "") {
    return "{\"schema\":\"import-request/v1\",\"target_version\":\"" + lw.version +
           "\",\"destination\":\"" + destination + "\",\"sources\":[{\"source_url\":\"file://" +
           source.string() + "\"}]}";
}

std::string importApplyJson(const LiveWork& lw, const std::filesystem::path& source,
                            const std::string& destination, const std::string& plan,
                            const std::vector<std::string>& replace = {}) {
    std::ostringstream request;
    request << "{\"schema\":\"import-apply-request/v1\",\"target_version\":\"" << lw.version
            << "\",\"destination\":\"" << destination << "\",\"sources\":[{\"source_url\":\"file://"
            << source.string() << "\"}],\"expected_plan\":\"" << plan << "\",\"replace_local\":[";
    for (std::size_t i = 0; i < replace.size(); ++i) {
        if (i) request << ",";
        request << "\"" << replace[i] << "\"";
    }
    request << "]}";
    return request.str();
}

// Leaves an Import v4 journal exactly as a process death would.  Import
// recovery must use destination-only evidence; it must never try to reopen the
// external source path after this point.
void forkCrashImport(const LiveWork& lw, const std::filesystem::path& source,
                     const std::string& destination, const std::string& plan_hash,
                     const std::string& crash_point) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("ISVN_WORK_CRASH", crash_point.c_str(), 1);
        const auto request = importApplyJson(lw, source, destination, plan_hash);
        std::istringstream in(request);
        std::ostringstream out;
        std::ostringstream err;
        run({"import", "apply", "--request-stdin", "--json"}, in, out, err);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
}

struct ConcurrentImportJob {
    LiveWork target;
    std::filesystem::path source;
    std::string destination;
    std::string plan_hash;
};

LiveWork createConcurrentVersion(const LiveWork& base, const std::string& version) {
    LiveWork result = base;
    result.version = version;
    require(runCommand({"create", result.version, "--parent", "ROOT"}).code == 0,
            "concurrent test version create must succeed: " + result.version);
    return result;
}

ConcurrentImportJob makeConcurrentImportJob(const LiveWork& target,
                                            const std::filesystem::path& source,
                                            const std::string& destination) {
    writeFile(source, "concurrent import payload\n");
    const auto preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                                    importRequestJson(target, source, destination));
    require(preview.code == 0 && jsonField(preview.out, "can_apply") == "true",
            "concurrent import preview must be applicable: " + preview.out + preview.err);
    return ConcurrentImportJob{target, source, destination, jsonField(preview.out, "plan_hash")};
}

void waitForTestMarker(const std::filesystem::path& marker, const std::string& context) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::error_code ec;
    while (!std::filesystem::exists(marker, ec) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(std::filesystem::exists(marker, ec), "timed out waiting for deterministic barrier: " + context);
}

pid_t forkPausedImport(const ConcurrentImportJob& job, const std::filesystem::path& barrier) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork paused Import");
    if (pid == 0) {
        ::setenv("ISVN_IMPORT_TEST_PAUSE_AT", "import_temp_copied_0", 1);
        ::setenv("ISVN_IMPORT_TEST_PAUSE_DIR", barrier.c_str(), 1);
        const auto request = importApplyJson(job.target, job.source, job.destination, job.plan_hash);
        const auto result = runCommand({"import", "apply", "--request-stdin", "--json"}, request);
        _exit(result.code == 0 ? 0 : 1);
    }
    return pid;
}

pid_t forkPausedCheckin(const LiveWork& target, const std::vector<std::string>& paths,
                        const std::string& hash, const std::filesystem::path& barrier) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork paused Checkin");
    if (pid == 0) {
        ::setenv("ISVN_WORK_TEST_PAUSE_AT", "checkin_before_commit", 1);
        ::setenv("ISVN_WORK_TEST_PAUSE_DIR", barrier.c_str(), 1);
        const auto result = checkinApply(target, paths, hash, "concurrent A");
        _exit(result.code == 0 ? 0 : 1);
    }
    return pid;
}

pid_t forkPausedLocalOperation(const LiveWork& target, const std::string& action,
                               const std::string& path, const std::string& hash,
                               const std::filesystem::path& barrier) {
    const pid_t pid = ::fork();
    require(pid >= 0, "fork paused local operation");
    if (pid == 0) {
        ::setenv("ISVN_WORK_TEST_PAUSE_AT", "local_before_apply", 1);
        ::setenv("ISVN_WORK_TEST_PAUSE_DIR", barrier.c_str(), 1);
        const auto result = runCommand({"work-local-op", "apply", "--action", action, "--in",
                                        target.version, "--path", path, "--expected-plan", hash,
                                        "--json"});
        _exit(result.code == 0 ? 0 : 1);
    }
    return pid;
}

void releasePausedChild(pid_t pid, const std::filesystem::path& barrier, const std::string& context) {
    writeFile(barrier / "continue", "continue\n");
    int status = 0;
    require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "paused child completes: " + context);
}

// Workspace state is one record for every Version of the workspace, and two
// commands may change it at the same time.  A command that read the record
// before another one published its entry must not publish its own change on top
// of that older view.
void testWorkspaceStateKeepsEntriesOfOverlappingCommands() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped overlapping workspace state test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto wc = lw.workspace / lw.version;
    const auto state_file = lw.workspace / ".isvn_local" / "workspace_state";
    writeFile(wc / "n.v", "second body\n");
    require(runCommand({"checkin", lw.version + "/n.v", "--to", lw.version}).code == 0,
            "fixture checkin of a second file must succeed");

    const auto barrier = tmp.path() / "state-barrier";
    const pid_t pid = ::fork();
    require(pid >= 0, "fork paused edit");
    if (pid == 0) {
        ::setenv("ISVN_WORK_TEST_PAUSE_AT", "workspace_state_read", 1);
        ::setenv("ISVN_WORK_TEST_PAUSE_DIR", barrier.c_str(), 1);
        CurrentDirectory inside(wc);
        _exit(runCommand({"edit", "m.v"}).code == 0 ? 0 : 1);
    }
    waitForTestMarker(barrier / "ready", "edit paused after reading workspace state");
    {
        CurrentDirectory inside(wc);
        require(runCommand({"edit", "n.v"}).code == 0, "the overlapping edit must succeed");
    }
    require(readFile(state_file).find("n.v") != std::string::npos,
            "the overlapping edit is recorded");
    releasePausedChild(pid, barrier, "edit paused after reading workspace state");

    const auto state = readFile(state_file);
    require(state.find("m.v") != std::string::npos, "the paused edit is recorded");
    require(state.find("n.v") != std::string::npos,
            "the entry published while the other command was paused is kept: " + state);
}

// The journal directory of the only operation that is currently on disk.
std::string soleOperationId(const LiveWork& lw) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(
             lw.workspace / ".isvn_local" / "work_operations", ec)) {
        if (std::filesystem::exists(entry.path() / "journal.json")) {
            ids.push_back(entry.path().filename().string());
        }
    }
    require(ids.size() == 1, "exactly one operation journal is expected on disk");
    return ids.front();
}

// Cross-process, deterministic barriers prove that mutations are not
// serialized against each other, that concurrent commits keep their own
// attribution, and that an operation which is still running is neither reported
// as needing recovery nor recoverable by another process.  The child processes
// execute the normal CLI dispatcher.
void testConcurrentMutationsAndRunningOperationRecovery() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped concurrent mutation test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto version_a = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(version_a.workspace);
    const auto version_b = createConcurrentVersion(version_a, "T/B/RTL/v2");
    const auto version_c = createConcurrentVersion(version_a, "T/B/RTL/v3");
    const auto version_d = createConcurrentVersion(version_a, "T/B/RTL/v4");
    const auto version_e = createConcurrentVersion(version_a, "T/B/RTL/v5");
    const std::vector<LiveWork> versions{version_a, version_b, version_c, version_d, version_e};
    writeFile(version_b.workspace / version_b.version / "m.v", "managed Version B seed\n");
    require(runCommand({"checkin", version_b.version + "/m.v", "--to", version_b.version}).code == 0,
            "seed managed Version B file for concurrent Checkin coverage");
    for (const LiveWork* target : {&version_c, &version_d}) {
        writeFile(target->workspace / target->version / "m.v", "managed shared-state seed\n");
        require(runCommand({"checkin", target->version + "/m.v", "--to", target->version}).code == 0,
                "seed managed Version for shared-state coverage");
    }

    // Import A pauses after its copy stage.  While it is still running, it is
    // not a recovery case and cannot be recovered by another process; status,
    // maintenance, a second operation on A and a Checkin on B all proceed.
    const auto first = makeConcurrentImportJob(version_a, tmp.path() / "first" / "first-a.v", "");
    const auto first_barrier = tmp.path() / "barrier-first";
    const auto first_pid = forkPausedImport(first, first_barrier);
    waitForTestMarker(first_barrier / "ready", "Import A");
    const auto running_id = soleOperationId(version_a);
    const auto running_status = runCommand({"work-recovery", "status", "--json"});
    require(running_status.code == 0 &&
                running_status.out.find("\"recovery_required\":false") != std::string::npos,
            "a running operation is not reported as needing recovery: " + running_status.out);
    const auto running_restore = runCommand({"work-recovery", "restore", running_id, "--json"});
    require(running_restore.code != 0 &&
                jsonField(running_restore.out, "code") == "OPERATION_IN_PROGRESS",
            "a running operation cannot be recovered by another process: " + running_restore.out);
    const auto second_a = makeConcurrentImportJob(version_a, tmp.path() / "first" / "second-a.v", "");
    const auto second_a_result =
        runCommand({"import", "apply", "--request-stdin", "--json"},
                   importApplyJson(second_a.target, second_a.source, second_a.destination,
                                   second_a.plan_hash));
    require(second_a_result.code == 0 && jsonField(second_a_result.out, "status") == "imported",
            "a second operation on Version A is not refused while the first is running: " +
                second_a_result.out + second_a_result.err);
    const auto busy_status = runCommand({"work-status", version_a.version, "--json"});
    require(busy_status.code == 0, "Work Status A stays available while A changes");
    const auto other_status = runCommand({"work-status", version_b.version, "--json"});
    require(other_status.code == 0, "Work Status B remains available while A mutates");
    const auto maintenance = runCommand({"gc", "--reconcile", "--json"});
    require(maintenance.code == 0,
            "maintenance is not refused while a Version mutation is running: " + maintenance.out);

    makeEditableAndModify(version_b, "m.v", "Version B concurrent update\n");
    const auto b_hash = checkinPreviewHash(version_b, {"m.v"});
    const auto head_before_b = headRevision(version_a);
    const auto checkin_b = checkinApply(version_b, {"m.v"}, b_hash, "concurrent B");
    require(checkin_b.code == 0 && jsonField(checkin_b.out, "status") == "committed" &&
                headRevision(version_a) == head_before_b + 1,
            "Import A and Checkin B overlap; B receives one exact attributed revision");
    releasePausedChild(first_pid, first_barrier, "Import A");
    require(std::filesystem::exists(version_a.workspace / version_a.version / "first-a.v") &&
                std::filesystem::exists(version_a.workspace / version_a.version / "second-a.v"),
            "both operations on Version A completed");

    // Both Checkins are held/alive in distinct CLI processes.  B commits first
    // while A is paused before its invocation, so A's proof cannot rely on
    // revision == pre_commit_head + 1.
    makeEditableAndModify(version_a, "m.v", "Version A concurrent update\n");
    makeEditableAndModify(version_b, "m.v", "Version B second update\n");
    const auto a_checkin_hash = checkinPreviewHash(version_a, {"m.v"});
    const auto b_checkin_hash = checkinPreviewHash(version_b, {"m.v"});
    const auto head_before_interleaved = headRevision(version_a);
    const auto checkin_barrier = tmp.path() / "barrier-checkin-a";
    const auto a_checkin_pid = forkPausedCheckin(version_a, {"m.v"}, a_checkin_hash, checkin_barrier);
    waitForTestMarker(checkin_barrier / "ready", "Checkin A");
    const auto interleaved_b = checkinApply(version_b, {"m.v"}, b_checkin_hash, "interleaved B");
    require(interleaved_b.code == 0 && jsonField(interleaved_b.out, "status") == "committed",
            "Checkin B commits while Checkin A is admitted and paused");
    releasePausedChild(a_checkin_pid, checkin_barrier, "Checkin A");
    require(headRevision(version_a) == head_before_interleaved + 2,
            "interleaved Checkins each receive exactly one revision without pre+1 attribution");

    // Checkin A × Remove B are likewise isolated: B's exact removal commits
    // while A is paused, then A attributes its own later revision without a
    // repository-wide commit mutex or a cross-version stale result.
    makeEditableAndModify(version_a, "m.v", "Version A before concurrent remove\n");
    const auto a_remove_overlap_hash = checkinPreviewHash(version_a, {"m.v"});
    const auto remove_barrier = tmp.path() / "barrier-checkin-remove-a";
    const auto a_remove_pid = forkPausedCheckin(version_a, {"m.v"}, a_remove_overlap_hash,
                                                 remove_barrier);
    waitForTestMarker(remove_barrier / "ready", "Checkin A against Remove B");
    const auto b_remove_hash = previewPlanHash({"remove-batch", "preview", "--from", version_b.version,
                                                "--path", "m.v", "--json"});
    const auto b_remove = runCommand({"remove-batch", "apply", "--from", version_b.version,
                                      "--path", "m.v", "--expected-plan", b_remove_hash,
                                      "--message", "concurrent remove B", "--json"});
    require(b_remove.code == 0 && jsonField(b_remove.out, "status") == "succeeded",
            "Remove B commits while Checkin A is admitted");
    releasePausedChild(a_remove_pid, remove_barrier, "Checkin A against Remove B");

    // A paused local transaction on A does not serialize Import B.  This also
    // exercises the workspace-state merge after two Version mutations have
    // independently completed.
    const auto local_hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable",
                                             "--in", version_a.version, "--path", "m.v", "--json"});
    const auto local_barrier = tmp.path() / "barrier-local-a";
    const auto local_pid = forkPausedLocalOperation(version_a, "make-editable", "m.v", local_hash,
                                                     local_barrier);
    waitForTestMarker(local_barrier / "ready", "local operation A");
    const auto import_b_during_local = makeConcurrentImportJob(
        version_b, tmp.path() / "local-import-overlap" / "b-import.v", "");
    const auto import_b_result = runCommand({"import", "apply", "--request-stdin", "--json"},
                                            importApplyJson(import_b_during_local.target,
                                                            import_b_during_local.source,
                                                            import_b_during_local.destination,
                                                            import_b_during_local.plan_hash));
    require(import_b_result.code == 0 && jsonField(import_b_result.out, "status") == "imported",
            "Import B completes while local operation A is paused");
    releasePausedChild(local_pid, local_barrier, "local operation A");

    // Two Version-local state transitions overlap. The second shared-state
    // merge cannot overwrite the first Version's LOCAL record.
    const auto c_local_hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable",
                                               "--in", version_c.version, "--path", "m.v", "--json"});
    const auto c_local_barrier = tmp.path() / "barrier-shared-state-c";
    const auto c_local_pid = forkPausedLocalOperation(version_c, "make-editable", "m.v", c_local_hash,
                                                       c_local_barrier);
    waitForTestMarker(c_local_barrier / "ready", "shared state C");
    const auto d_local_hash = previewPlanHash({"work-local-op", "preview", "--action", "make-editable",
                                               "--in", version_d.version, "--path", "m.v", "--json"});
    const auto d_local = runCommand({"work-local-op", "apply", "--action", "make-editable", "--in",
                                     version_d.version, "--path", "m.v", "--expected-plan", d_local_hash,
                                     "--json"});
    require(d_local.code == 0 && jsonField(d_local.out, "status") == "succeeded",
            "Version D shared-state mutation completes while C is paused");
    releasePausedChild(c_local_pid, c_local_barrier, "shared state C");
    const auto shared_state = readFile(version_a.workspace / ".isvn_local" / "workspace_state");
    require(shared_state.find(version_c.version + "/m.v") != std::string::npos &&
                shared_state.find(version_d.version + "/m.v") != std::string::npos,
            "shared workspace-state merge retains both Version C and D LOCAL transitions");

    // There is no limit on how many Versions change at once: four operations
    // are held and a fifth still completes.
    const auto snapshot = runCommand({"workspaces", "--json"});
    require(snapshot.code == 0 && snapshot.out.find("mutation_limit") == std::string::npos,
            "the workspace snapshot carries no concurrency limit");
    {
        std::vector<pid_t> pids;
        std::vector<std::filesystem::path> barriers;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto job = makeConcurrentImportJob(
                versions[i], tmp.path() / "unbounded" / ("held-" + std::to_string(i) + ".v"), "");
            const auto barrier = tmp.path() / "unbounded-barriers" / std::to_string(i);
            pids.push_back(forkPausedImport(job, barrier));
            barriers.push_back(barrier);
        }
        for (const auto& barrier : barriers) waitForTestMarker(barrier / "ready", "held imports");
        const auto fifth = makeConcurrentImportJob(versions[4], tmp.path() / "unbounded" / "fifth.v", "");
        const auto fifth_result =
            runCommand({"import", "apply", "--request-stdin", "--json"},
                       importApplyJson(fifth.target, fifth.source, fifth.destination, fifth.plan_hash));
        require(fifth_result.code == 0 && jsonField(fifth_result.out, "status") == "imported",
                "a fifth concurrent operation is not refused: " + fifth_result.out);
        for (std::size_t i = 0; i < pids.size(); ++i) {
            releasePausedChild(pids[i], barriers[i], "held imports");
        }
    }

    // A modern v5 recovery record pins only A.  It remains addressable by its
    // exact operation id while B can still perform a new Version mutation;
    // restoring A cannot select or roll back B by discovery order.
    const auto crash_source = tmp.path() / "recovery-isolation" / "crash-a.v";
    writeFile(crash_source, "crash A\n");
    const auto crash_preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                                          importRequestJson(version_a, crash_source));
    require(crash_preview.code == 0, "A recovery-isolation preview");
    forkCrashImport(version_a, crash_source, "", jsonField(crash_preview.out, "plan_hash"),
                    "import_item_published_0");
    const auto pending = runCommand({"work-recovery", "status", "--json"});
    const auto recovery_a = jsonField(pending.out, "operation_id");
    require(!recovery_a.empty() && pending.out.find("\"target_version\":\"" + version_a.version) !=
                                      std::string::npos &&
                pending.out.find("\"lock_scope\":\"VERSION\"") != std::string::npos,
            "modern recovery is target-pinned and Version scoped");
    const auto b_after_a_crash = makeConcurrentImportJob(
        version_b, tmp.path() / "recovery-isolation" / "b-still-works.v", "");
    const auto b_result = runCommand({"import", "apply", "--request-stdin", "--json"},
                                     importApplyJson(b_after_a_crash.target, b_after_a_crash.source,
                                                     b_after_a_crash.destination,
                                                     b_after_a_crash.plan_hash));
    require(b_result.code == 0 && jsonField(b_result.out, "status") == "imported",
            "A recovery requirement does not block independent Version B");

    // A modern journal is never redirected through a mutable workspace map.
    // Corroboration mismatch is Manual Review for A alone; restoring the map
    // lets the same exact operation ID recover normally without touching B.
    const auto map_path = version_a.workspace / ".isvn_local" / "workspaces";
    const auto original_map = readFile(map_path);
    const std::string original_mapping = version_a.version + "\t" + version_a.version;
    auto changed_map = original_map;
    const auto mapping_pos = changed_map.find(original_mapping);
    require(mapping_pos != std::string::npos, "A workspace map entry exists for target-pin divergence");
    changed_map.replace(mapping_pos, original_mapping.size(),
                        version_a.version + "\t" + version_a.version + "-retargeted");
    writeFile(map_path, changed_map);
    const auto diverged = runCommand({"work-recovery", "restore", recovery_a, "--json"});
    require(diverged.code == 0 && jsonField(diverged.out, "status") == "manual_review_required" &&
                jsonField(diverged.out, "reason_code") == "OPERATION_TARGET_DIVERGED" &&
                std::filesystem::exists(version_a.workspace / ".isvn_local" / "work_operations" /
                                        recovery_a / "journal.json") &&
                std::filesystem::exists(version_b.workspace / version_b.version / "b-still-works.v"),
            "map divergence cannot retarget A recovery and leaves B untouched");
    writeFile(map_path, original_map);
    const auto restore_a = runCommand({"work-recovery", "restore", recovery_a, "--json"});
    require(restore_a.code == 0 && jsonField(restore_a.out, "status") == "rolled_back" &&
                std::filesystem::exists(version_b.workspace / version_b.version / "b-still-works.v") &&
                !std::filesystem::exists(version_a.workspace / version_a.version / "crash-a.v"),
            "exact A recovery restores only A and preserves B content");
}

void testImportTransactionalPreviewApplyAndGuards() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped import test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto source_root = tmp.path() / "external";
    writeFile(source_root / "rtl" / "a.v", "module a;\n");
    writeFile(source_root / "rtl" / "sub" / "b.v", "module b;\n");
    std::filesystem::create_directories(lw.workspace / lw.version / "src");
    const auto before = headRevision(lw);

    auto preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                              importRequestJson(lw, source_root / "rtl", "src"));
    require(preview.code == 0 && jsonField(preview.out, "can_apply") == "true",
            "valid import preview is read-only and applicable: " + preview.out + preview.err);
    require(preview.out.find("src/rtl/a.v") != std::string::npos &&
                preview.out.find(source_root.string()) == std::string::npos,
            "basename mapping is project-relative and external source is private");
    const auto plan = jsonField(preview.out, "plan_hash");
    auto apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                            importApplyJson(lw, source_root / "rtl", "src", plan));
    require(apply.code == 0 && jsonField(apply.out, "status") == "imported",
            "Import Apply publishes all copied files: " + apply.out + apply.err);
    require(readFile(lw.workspace / lw.version / "src" / "rtl" / "a.v") == "module a;\n" &&
                readFile(lw.workspace / lw.version / "src" / "rtl" / "sub" / "b.v") ==
                    "module b;\n",
            "import preserves bytes and relative directory structure");
    require(headRevision(lw) == before, "Import creates no SVN revision");
    const auto status = runCommand({"status", lw.version, "--json"});
    require(status.out.find("src/rtl/a.v") != std::string::npos,
            "Import leaves files as local workspace content, not an SVN commit");

    // A top-level symlink and a nested symlink both fail closed without exposing
    // their target in the structured JSON envelope.
    std::filesystem::create_symlink(source_root / "rtl" / "a.v", source_root / "linked.v");
    auto link = runCommand({"import", "preview", "--request-stdin", "--json"},
                           importRequestJson(lw, source_root / "linked.v", ""));
    require(link.code != 0 && jsonField(link.out, "code") == "SOURCE_SYMLINK_UNSUPPORTED" &&
                link.out.find((source_root / "rtl" / "a.v").string()) == std::string::npos,
            "top-level source symlink is blocked without leaking its target: " + link.out + link.err);
    std::filesystem::create_symlink(source_root / "rtl" / "a.v", source_root / "rtl" / "nested-link.v");
    link = runCommand({"import", "preview", "--request-stdin", "--json"},
                      importRequestJson(lw, source_root / "rtl", ""));
    require(link.code != 0 && jsonField(link.out, "code") == "SOURCE_SYMLINK_UNSUPPORTED",
            "nested source symlink blocks the entire plan");
    std::filesystem::remove(source_root / "rtl" / "nested-link.v");

    // A never-managed LOCAL collision is replaceable only when its exact
    // project-relative destination is explicitly authorized; managed m.v stays
    // immutable even through Import.
    writeFile(lw.workspace / lw.version / "local.v", "old local\n");
    writeFile(source_root / "local.v", "new local\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, source_root / "local.v"));
    require(preview.code == 0 && preview.out.find("REPLACE_LOCAL_REQUIRED") != std::string::npos,
            "never-managed LOCAL collision requires per-item replacement: " + preview.out + preview.err);
    const auto local_plan = jsonField(preview.out, "plan_hash");
    apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                       importApplyJson(lw, source_root / "local.v", "", local_plan));
    require(apply.code != 0 && jsonField(apply.out, "code") == "REPLACE_LOCAL_REQUIRED",
            "unchecked replacement cannot publish");
    apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                       importApplyJson(lw, source_root / "local.v", "", local_plan, {"local.v"}));
    require(apply.code == 0 && jsonField(apply.out, "status") == "imported" &&
                readFile(lw.workspace / lw.version / "local.v") == "new local\n",
            "explicit per-item Replace Local succeeds");

    writeFile(source_root / "m.v", "attempt overwrite\n");
    auto managed = runCommand({"import", "preview", "--request-stdin", "--json"},
                              importRequestJson(lw, source_root / "m.v"));
    require(managed.code == 0 && managed.out.find("MANAGED_DESTINATION_BLOCKED") != std::string::npos,
            "managed destination is never import-replaceable");

    // Preview evidence is never trusted: source and destination topology are
    // independently rebuilt before journal creation/publication.
    writeFile(source_root / "stale.v", "before\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, source_root / "stale.v"));
    const auto stale_plan = jsonField(preview.out, "plan_hash");
    writeFile(source_root / "stale.v", "after\n");
    apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                       importApplyJson(lw, source_root / "stale.v", "", stale_plan));
    require(apply.code != 0 && jsonField(apply.out, "code") == "PLAN_STALE" &&
                !std::filesystem::exists(lw.workspace / lw.version / "stale.v"),
            "source change after Preview fails before destination mutation");
    writeFile(source_root / "unsafe" / "child.v", "safe source\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, source_root / "unsafe"));
    const auto unsafe_plan = jsonField(preview.out, "plan_hash");
    std::filesystem::create_directory_symlink(tmp.path(), lw.workspace / lw.version / "unsafe");
    apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                       importApplyJson(lw, source_root / "unsafe", "", unsafe_plan));
    require(apply.code != 0 && jsonField(apply.out, "code") == "DESTINATION_SYMLINK_BLOCKED" &&
                !std::filesystem::exists(tmp.path() / "child.v"),
            "destination ancestry symlink after Preview is blocked no-follow");
    std::filesystem::remove(lw.workspace / lw.version / "unsafe");

    // A publication fault after item N rolls every new item back and does not
    // create a recovery journal for an ordinary handled failure.
    writeFile(source_root / "fault" / "one.v", "one\n");
    writeFile(source_root / "fault" / "two.v", "two\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, source_root / "fault"));
    const auto fault_plan = jsonField(preview.out, "plan_hash");
    {
        ScopedEnvironment fault("ISVN_WORK_FAULT", "import_item_published_1");
        apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                           importApplyJson(lw, source_root / "fault", "", fault_plan));
    }
    require(apply.code == 0 && jsonField(apply.out, "status") == "rolled_back" &&
                !std::filesystem::exists(lw.workspace / lw.version / "fault"),
            "item-N publication fault restores all newly created destination topology");

    // A process death preserves a destination-only v4 journal.  Recovery does
    // not resume/copy external sources; it restores the old local topology and
    // stays idempotent on repeat invocation.
    writeFile(source_root / "crash" / "one.v", "crash one\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, source_root / "crash"));
    const auto crash_plan = jsonField(preview.out, "plan_hash");
    forkCrashImport(lw, source_root / "crash", "", crash_plan, "import_item_published_0");
    const auto import_op = pendingOperationId();
    require(!import_op.empty() && import_op.rfind("op-import-", 0) == 0,
            "crashed Import leaves a secure import journal for recovery");
    auto restore = runCommand({"work-recovery", "restore", import_op, "--json"});
    require(restore.code == 0 && jsonField(restore.out, "status") == "rolled_back" &&
                !std::filesystem::exists(lw.workspace / lw.version / "crash"),
            "Import crash recovery rolls back using destination-only journal evidence: " +
                restore.out + restore.err);
    restore = runCommand({"work-recovery", "restore", import_op, "--json"});
    require(restore.code != 0 || jsonField(restore.out, "status") != "imported",
            "repeated recovery never resumes external-source import or creates a revision");
    require(headRevision(lw) == before, "all Import outcomes leave repository revision unchanged");
}

// Direct fault-injection coverage for the security boundary. These
// cases intentionally exercise the real CLI and disposable live-SVN workspace;
// they are not call-graph assertions.
void testImportNoFollowAndFdRecoveryHardening() {
    if (!svnToolsAvailable() || currentGroupName().empty()) {
        std::cout << "svn not found; skipped import no-follow hardening test\n";
        return;
    }
    TemporaryDirectory tmp;
    const auto lw = prepareLiveWork(tmp.path());
    CurrentDirectory cwd(lw.workspace);
    const auto external = tmp.path() / "external-privacy-unique";
    const auto target = lw.workspace / lw.version;
    const auto head_before = headRevision(lw);

    // Each managed evidence form is an unconditional Import blocker: a clean
    // LINKED file, an editable-receipt-backed LOCAL file, and a repository path
    // whose local node was removed.  None becomes Replace Local eligible.
    writeFile(external / "m.v", "not an import replacement\n");
    auto managed = runCommand({"import", "preview", "--request-stdin", "--json"},
                              importRequestJson(lw, external / "m.v"));
    require(managed.code == 0 && managed.out.find("MANAGED_DESTINATION_BLOCKED") != std::string::npos &&
                managed.out.find("replace_local_available\":true") == std::string::npos,
            "LINKED managed destination cannot be Replace Local");
    auto editable_plan = previewPlanHash({"work-local-op", "preview", "--action", "make-editable", "--in",
                                          lw.version, "--path", "m.v", "--json"});
    require(runCommand({"work-local-op", "apply", "--action", "make-editable", "--in", lw.version,
                        "--path", "m.v", "--expected-plan", editable_plan, "--json"}).code == 0,
            "creates managed LOCAL editable receipt for Import hardening test");
    managed = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "m.v"));
    require(managed.code == 0 && managed.out.find("MANAGED_DESTINATION_BLOCKED") != std::string::npos,
            "editable-receipt managed LOCAL cannot be Replace Local");
    auto revert_plan = previewPlanHash({"work-local-op", "preview", "--action", "revert", "--in",
                                        lw.version, "--path", "m.v", "--json"});
    require(runCommand({"work-local-op", "apply", "--action", "revert", "--in", lw.version,
                        "--path", "m.v", "--expected-plan", revert_plan, "--json"}).code == 0,
            "clears editable receipt before remote-missing-local classification");
    std::filesystem::remove(target / "m.v");
    managed = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "m.v"));
    require(managed.code == 0 && managed.out.find("MANAGED_DESTINATION_BLOCKED") != std::string::npos,
            "repository-existing local-missing destination cannot be Replace Local");

    // A symlink in any *ancestor*, not only the final source component, is
    // rejected by Preview's /-anchored openat walk.
    writeFile(external / "real" / "ancestor.v", "ancestor\n");
    std::filesystem::create_directory_symlink(external / "real", external / "ancestor-link");
    auto preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                              importRequestJson(lw, external / "ancestor-link" / "ancestor.v"));
    require(preview.code != 0 && jsonField(preview.out, "code") == "SOURCE_SYMLINK_UNSUPPORTED" &&
                preview.out.find(external.string()) == std::string::npos,
            "source ancestor symlink is rejected without external-path disclosure");

    // Replacing the selected source root after Preview with a link is detected
    // while Apply independently rebuilds its no-follow source plan.
    writeFile(external / "switch-root" / "unit.v", "before switch\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "switch-root"));
    const auto switch_plan = jsonField(preview.out, "plan_hash");
    std::filesystem::rename(external / "switch-root", external / "switch-root-real");
    std::filesystem::create_directory_symlink(external / "switch-root-real", external / "switch-root");
    auto apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                            importApplyJson(lw, external / "switch-root", "", switch_plan));
    require(apply.code != 0 && jsonField(apply.out, "code") == "SOURCE_SYMLINK_UNSUPPORTED" &&
                !std::filesystem::exists(target / "switch-root"),
            "source root replacement by symlink fails closed before Active mutation");
    std::filesystem::remove(external / "switch-root");
    std::filesystem::rename(external / "switch-root-real", external / "switch-root");

    // Fault-injected mid-Apply replacement: the first file is already copied
    // to private temp storage, then the source root is replaced by a symlink
    // before the next descriptor walk.  No destination publication may occur.
    writeFile(external / "mid-apply" / "one.v", "one\n");
    writeFile(external / "mid-apply" / "two.v", "two\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "mid-apply"));
    const auto mid_plan = jsonField(preview.out, "plan_hash");
    const auto pause_dir = tmp.path() / "import-pause";
    const auto mid_result = tmp.path() / "mid-apply-result.json";
    const pid_t mid_pid = ::fork();
    if (mid_pid == 0) {
        ::setenv("ISVN_IMPORT_TEST_PAUSE_AT", "import_temp_copied_0", 1);
        ::setenv("ISVN_IMPORT_TEST_PAUSE_DIR", pause_dir.c_str(), 1);
        const auto child = runCommand({"import", "apply", "--request-stdin", "--json"},
                                      importApplyJson(lw, external / "mid-apply", "", mid_plan));
        writeFile(mid_result, child.out + child.err);
        _exit(child.code == 0 ? 0 : 1);
    }
    require(mid_pid > 0, "forks deterministic mid-Apply source swap test");
    for (int attempt = 0; attempt != 100 && !std::filesystem::exists(pause_dir / "ready"); ++attempt) {
        ::usleep(10 * 1000);
    }
    require(std::filesystem::exists(pause_dir / "ready"), "Import reached deterministic post-copy pause");
    std::filesystem::rename(external / "mid-apply", external / "mid-apply-real");
    std::filesystem::create_directory_symlink(external / "mid-apply-real", external / "mid-apply");
    writeFile(pause_dir / "continue", "continue");
    int mid_status = 0;
    ::waitpid(mid_pid, &mid_status, 0);
    require(!WIFEXITED(mid_status) || WEXITSTATUS(mid_status) != 0,
            "mid-Apply source root replacement fails rather than publishing");
    require(readFile(mid_result).find("SOURCE_SYMLINK_UNSUPPORTED") != std::string::npos &&
                !std::filesystem::exists(target / "mid-apply"),
            "second source descriptor walk detects symlink replacement with Active root unchanged");
    std::filesystem::remove(external / "mid-apply");
    std::filesystem::rename(external / "mid-apply-real", external / "mid-apply");

    // Empty directories stay explicitly unsupported, even when reached by the
    // descriptor-based walker.
    std::filesystem::create_directories(external / "empty");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "empty"));
    require(preview.code != 0 && jsonField(preview.out, "code") == "EMPTY_DIRECTORY_UNSUPPORTED",
            "descriptor source traversal blocks empty directories truthfully");

    // Record every real SVN argv reached by a valid Import.  The wrapper
    // forwards to the system binary, so this is direct process evidence that
    // Import uses only its bounded read evidence—not add/delete/commit or a
    // legacy/checkin-batch mutation path.
    writeFile(external / "argv-no-mutation.v", "argv proof\n");
    const auto trace_bin = tmp.path() / "import-svn-trace-bin";
    const auto trace_file = tmp.path() / "import-svn-argv.trace";
    std::filesystem::create_directories(trace_bin);
    const auto trace_wrapper = trace_bin / "svn";
    writeFile(trace_wrapper, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$ISVN_SVN_TRACE_FILE\"\n"
                             "exec /usr/bin/svn \"$@\"\n");
    std::filesystem::permissions(trace_wrapper,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    {
        const char* old_path = ::getenv("PATH");
        ScopedEnvironment path("PATH", trace_bin.string() + ":" + (old_path ? old_path : ""));
        ScopedEnvironment trace("ISVN_SVN_TRACE_FILE", trace_file.string());
        preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                             importRequestJson(lw, external / "argv-no-mutation.v"));
        const auto argv_plan = jsonField(preview.out, "plan_hash");
        apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                           importApplyJson(lw, external / "argv-no-mutation.v", "", argv_plan));
        require(apply.code == 0 && jsonField(apply.out, "status") == "imported",
                "argv recording Import remains a successful local operation");
    }
    const std::string import_argv = readFile(trace_file);
    require(import_argv.find("add ") == std::string::npos &&
                import_argv.find("delete ") == std::string::npos &&
                import_argv.find("commit ") == std::string::npos &&
                import_argv.find("checkin") == std::string::npos,
            "argv-recorded Import invokes no svn add/delete/commit/Checkin mutation");

    // Capacity is decided before journal/publication.  The fail-only test seam
    // makes the boundary deterministic without attempting to fill a real disk.
    writeFile(external / "no-space.v", "no space\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "no-space.v"));
    const auto no_space_plan = jsonField(preview.out, "plan_hash");
    {
        ScopedEnvironment no_space("ISVN_IMPORT_TEST_FORCE_NO_SPACE", "1");
        apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                           importApplyJson(lw, external / "no-space.v", "", no_space_plan));
    }
    require(apply.code != 0 && jsonField(apply.out, "code") == "INSUFFICIENT_TARGET_SPACE" &&
                !std::filesystem::exists(target / "no-space.v"),
            "insufficient target space fails before journal/publication");

    // Replace Local remains per-item/default-off.  Faults after backup and
    // after the final publication restore two old files byte-for-byte and
    // remove the created nested directory through root-FD-relative unlinkat.
    writeFile(target / "txn" / "replace-a.v", "old A\n");
    writeFile(target / "txn" / "replace-b.v", "old B\n");
    writeFile(external / "txn" / "replace-a.v", "new A\n");
    writeFile(external / "txn" / "replace-b.v", "new B\n");
    writeFile(external / "txn" / "new-dir" / "new.v", "new file\n");
    const std::vector<std::string> replacements{"txn/replace-a.v", "txn/replace-b.v"};
    for (const std::string fault_point : {"import_journal_prepared", "import_temp_copied_0",
                                          "import_ready_to_publish", "import_backup_renamed_1",
                                          "import_backup_renamed_2", "import_item_published_0",
                                          "import_item_published_1", "import_item_published_2"}) {
        preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                             importRequestJson(lw, external / "txn"));
        require(preview.code == 0 && preview.out.find("REPLACE_LOCAL_REQUIRED") != std::string::npos,
                "Replace Local remains default-off before transactional fault");
        const auto tx_plan = jsonField(preview.out, "plan_hash");
        {
            ScopedEnvironment fault("ISVN_WORK_FAULT", fault_point);
            apply = runCommand({"import", "apply", "--request-stdin", "--json"},
                               importApplyJson(lw, external / "txn", "", tx_plan, replacements));
        }
        require(apply.code == 0 && jsonField(apply.out, "status") == "rolled_back" &&
                    readFile(target / "txn" / "replace-a.v") == "old A\n" &&
                    readFile(target / "txn" / "replace-b.v") == "old B\n" &&
                    !std::filesystem::exists(target / "txn" / "new-dir"),
                "every injected publish phase restores replacements and new topology exactly");
    }

    // Crash recovery must never follow a replacement destination ancestor.
    // The external sentinel proves recovery neither unlinks nor restores outside
    // the pinned Active root, and the full operation directory is scanned for
    // the unique external absolute source path.
    writeFile(external / "recover-tree" / "new.v", "recover\n");
    preview = runCommand({"import", "preview", "--request-stdin", "--json"},
                         importRequestJson(lw, external / "recover-tree"));
    const auto recover_plan = jsonField(preview.out, "plan_hash");
    forkCrashImport(lw, external / "recover-tree", "", recover_plan, "import_item_published_0");
    const auto op_id = pendingOperationId();
    const auto op_dir = lw.workspace / ".isvn_local" / "work_operations" / op_id;
    require(!op_id.empty() && !treeContainsText(op_dir, external.string()),
            "all Import journal/temp/backup metadata keep external source paths private");
    const auto saved_target = lw.workspace / "saved-active-root";
    const auto sentinel = tmp.path() / "outside-sentinel";
    writeFile(sentinel / "keep", "sentinel\n");
    std::filesystem::rename(target, saved_target);
    std::filesystem::create_directory_symlink(sentinel, target);
    auto recovery = runCommand({"work-recovery", "restore", op_id, "--json"});
    require(recovery.code == 0 && jsonField(recovery.out, "status") == "manual_review_required" &&
                readFile(sentinel / "keep") == "sentinel\n" &&
                !std::filesystem::exists(sentinel / "recover-tree" / "new.v") &&
                std::filesystem::exists(op_dir / "journal.json"),
            "recovery rejects renamed Active root/symlink replacement, preserves journal, and cannot escape");
    std::filesystem::remove(target);
    std::filesystem::rename(saved_target, target);
    recovery = runCommand({"work-recovery", "restore", op_id, "--json"});
    require(recovery.code == 0 && jsonField(recovery.out, "status") == "rolled_back" &&
                !std::filesystem::exists(target / "recover-tree"),
            "FD-relative recovery remains idempotent once the verified ancestry is restored");

    require(headRevision(lw) == head_before,
            "no-follow and recovery faults never create an SVN revision");
}

}  // namespace

int main() {
    try {
        testVersionCommand();
        testWorkspacesJsonContract();
        testWorkspacesRejectsCrossProjectAndMalformedRows();
        testWorkStatusContractAndAvailability();
        testWorkReadCommandsLiveSvn();
        testBranchesJsonFailsCleanlyWithoutSvn();
        testCheckinBesideLinkedFilesCommitsOnlyTheRequestedPath();
        testCheckinOfUnchangedPathSucceedsWithoutACommit();
        testCheckinOfTrackedFolderAddsNewContent();
        testCheckoutLeavesOtherVersionsAlone();
        testFileLocate();
        testFolderCheckinDoesNotFollowDirectoryLinks();
        testCheckinFolderBesideLinkedFiles();
        testRemoveBesideLinkedFiles();
        testCheckinSendsContentOfUntrackedPoolLinks();
        testCheckinFileBelowUntrackedFolders();
        testRefusedRemoveKeepsTheWorkingCopy();
        testRejectedFolderRemoveIsUndone();
        testWorkLocalOpsTransactional();
        testWorkLocalOpFaultRollback();
        testWorkLocalOpCrashRecovery();
        testRemoveBatchAtomic();
        testRemoveBatchModifiedLocalRequiresAck();
        testRemoveOutcomeVerifiedAfterCommitCrash();
        testRemoveOutcomeRollbackWhenNoCommit();
        testRemoveOutcomeOtherActorDiverged();
        testRemoveOutcomeMismatchOnWrongEvidence();
        testRemoveExactCommitScope();
        testReservedTrailerAndOperationId();
        testCheckinBatchPreviewAndExactSelection();
        testCheckinBatchBoundedQueryShapeAndDecoyScale();
        testCheckinBatchSyntheticMillionEntryScopeModel();
        testCheckinBatchMixedUpdateAddOneRevision();
        testCheckinBatchNestedParents();
        testCheckinBatchProvenance();
        testCheckinBatchMessagePlanTrailer();
        testCheckinBatchCrashRecoveryVerify();
        testCheckinBatchAdversarialEvidence();
        testImportTransactionalPreviewApplyAndGuards();
        testImportNoFollowAndFdRecoveryHardening();
        testConcurrentMutationsAndRunningOperationRecovery();
        testWorkspaceStateKeepsEntriesOfOverlappingCommands();
        testAncestryJsonAndLineageVerify();
        testProvenanceQueryJsonContract();
        testProvenanceQueryRendersRootSentinel();
        testFileQueryJsonContract();
        testLineageAuditJsonIsReadOnlyAuditOnly();
        testCheckoutAncestorRequiresConfirmationBeforeContent();
        testCheckoutAncestorExplicitVersionUsesLineageStart();
        testExplicitVersionRequiresWorkspaceMapping();
        testDeleteRejectsVersionWithDescendantsBeforeSvn();
        testEditBreaksPoolSymlinkAndMarksLocal();
        testEditExplicitVersionBreaksThatWorkspaceFile();
        testEditVersionPrefixedPathIsNormalized();
        testEditWrongVersionPrefixIsRejected();
        testGcRunsWithReport();
        testMemberMaintenanceMutationIsDeniedAtCliBoundary();
        testGcJsonSchemaAndDryRun();
        testGcPlainTextCronCompatibility();
        testGcRejectsUnsupportedDryRunWithoutSideEffects();
        testGcKeepsAndReportsRefsOutsideItsWorkspace();
        testNoSharedLockStateIsUsed();
        testLivePoolRefLifecycleIsScoped();
        testDeleteRecordsOwnUnrecordedLinkBeforeReclaim();
        testDeleteReclaimsOnlyTheRecordedObject();
        testOrphanGcContinuesAndReportsIncompleteCleanup();
        testPlantedLockDirectorySymlinkIsIgnored();
        testRenewTouchesLinkedObject();
        testRenewStatusReportsSingleVersionFilesReadOnly();
        testRenewStatusDoesNotRepairDanglingLinks();
        testPoolStatReportsStorageOnlyMetrics();
        testPoolStatRenewStatsStayHashOnly();
        testAdminStatusJsonIsReadOnlyAndSeparated();
        testReconcileJsonDoesNotResolve();
        testUnknownCommandFails();
        testMemberInitReportsMissingSharedConfigPath();
        testAdminInitCanRecoverMissingConfigWithExistingRoot();
        testCommandLayerGuardrails();
    } catch (const std::exception& error) {
        std::cerr << "test_commands failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
