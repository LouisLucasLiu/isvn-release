#include "cli/commands.h"

#include "version.h"
#include "common/fs.h"
#include "common/hash.h"
#include "common/system.h"
#include "storage/gc.h"
#include "storage/object_store.h"
#include "version/lineage.h"
#include "version/svn_ops.h"

#include <sqlite3.h>

#include <dirent.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <optional>
#include <pwd.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace isvn::cli {
namespace {

using storage::ObjectStore;
using storage::GarbageCollector;
using version::Lineage;
using version::SvnOps;
using version::SvnQueryBudget;
using version::SvnQueryLimits;

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
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(character) << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void appendJsonString(std::ostream& out, const std::string& value) {
    out << '"' << jsonEscape(value) << '"';
}

// Inverse of jsonEscape for the escapes the journal writer can emit
// (\" \\ \b \f \n \r \t \uXXXX). Used when reading string fields back from the
// controlled-format journal for exact comparison.
std::string jsonUnescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const char next = value[++i];
        switch (next) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (i + 4 < value.size()) {
                    const int code = std::stoi(value.substr(i + 1, 4), nullptr, 16);
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    i += 4;
                }
                break;
            default: out.push_back(next); break;
        }
    }
    return out;
}

void appendJsonNullable(std::ostream& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        appendJsonString(out, *value);
    } else {
        out << "null";
    }
}

struct Config {
    std::string repo_url;
    std::filesystem::path pool_root;
    std::string branch_prefix = "branches/";
    std::string project_group;
};

std::string trimTrailingSlash(std::string value) {
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, delimiter)) parts.push_back(part);
    return parts;
}

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

void validateVersionId(const std::string& version) {
    if (version.empty() || version.front() == '/' ||
        version.find("//") != std::string::npos) {
        throw std::invalid_argument("version must be phase/block/stage/version");
    }
    const auto parts = split(version, '/');
    if (parts.size() != 4) {
        throw std::invalid_argument("version must contain exactly four path segments");
    }
    for (const auto& part : parts) {
        if (part.empty() || part == "." || part == ".." ||
            part.find_first_of(" \t\r\n") != std::string::npos) {
            throw std::invalid_argument("version contains an invalid segment");
        }
    }
}

std::filesystem::path findWorkspaceRoot(std::filesystem::path start) {
    start = std::filesystem::absolute(start);
    while (true) {
        if (std::filesystem::is_directory(start / ".isvn_local")) return start;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    throw std::runtime_error("not inside an isvn workspace; run isvn member init first");
}

std::optional<std::filesystem::path> maybeWorkspaceRoot(std::filesystem::path start) {
    try {
        return findWorkspaceRoot(start);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::filesystem::path sharedStatePath(const std::filesystem::path& project_dir,
                                      const std::string& name) {
    return project_dir / ".isvn" / name;
}

std::filesystem::path localStatePath(const std::filesystem::path& workspace,
                                     const std::string& name) {
    return workspace / ".isvn_local" / name;
}

struct WorkspaceEntry {
    std::string version;
    std::filesystem::path relative_subdir;
};

// How often a workspace record is computed again when another command changed it in the
// meantime, before the update gives up with an error.
constexpr int kStateUpdateAttempts = 50;

// Replaces `path` with `text`.  A distinct O_EXCL temporary plus fsync and an
// atomic rename leave either the old complete record or the new complete
// record, after a crash and when two writers finish close together.  When
// `still_current` is given it is asked once the new file is complete, right
// before the rename; if it answers false, `path` is left as it is and the
// function returns false.
bool writeTextFileIf(const std::filesystem::path& path, const std::string& text, mode_t mode,
                     const std::function<bool()>& still_current) {
    std::string temp = path.string() + ".tmp.XXXXXX";
    std::vector<char> template_bytes(temp.begin(), temp.end());
    template_bytes.push_back('\0');
    const int fd = ::mkstemp(template_bytes.data());
    if (fd < 0) throw std::runtime_error("unable to create temporary " + path.string());
    temp.assign(template_bytes.data());
    auto fail = [&]() {
        const int saved = errno;
        ::close(fd);
        ::unlink(temp.c_str());
        errno = saved;
        throw std::runtime_error("unable to write " + path.string());
    };
    if (::fchmod(fd, mode) != 0) fail();
    std::size_t written = 0;
    while (written < text.size()) {
        const auto n = ::write(fd, text.data() + written, text.size() - written);
        if (n <= 0) fail();
        written += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) fail();
    if (::close(fd) != 0) {
        ::unlink(temp.c_str());
        throw std::runtime_error("unable to close " + path.string());
    }
    try {
        if (still_current && !still_current()) {
            ::unlink(temp.c_str());
            return false;
        }
        std::filesystem::rename(temp, path);
    } catch (...) {
        ::unlink(temp.c_str());
        throw;
    }
    common::fsync_directory(path.parent_path());
    return true;
}

// Used for the small workspace-state records as well as project-local state.
void writeTextFile(const std::filesystem::path& path, const std::string& text,
                   mode_t mode = 0660) {
    writeTextFileIf(path, text, mode, nullptr);
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::filesystem::path versionSubdir(const std::string& version) {
    validateVersionId(version);
    return std::filesystem::path(version);
}

std::vector<WorkspaceEntry> readWorkspaceMap(const std::filesystem::path& workspace) {
    std::vector<WorkspaceEntry> entries;
    const auto path = localStatePath(workspace, "workspaces");
    std::ifstream input(path);
    if (!input) return entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        WorkspaceEntry entry{line.substr(0, tab), std::filesystem::path(line.substr(tab + 1))};
        validateVersionId(entry.version);
        if (entry.relative_subdir.is_absolute() ||
            entry.relative_subdir.generic_string().find("..") != std::string::npos) {
            throw std::runtime_error(".isvn_local/workspaces contains an invalid subdir");
        }
        entries.push_back(entry);
    }
    return entries;
}

std::string serializeWorkspaceMap(std::vector<WorkspaceEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.version < rhs.version;
    });
    std::ostringstream output;
    for (const auto& entry : entries) {
        output << entry.version << '\t' << entry.relative_subdir.generic_string() << '\n';
    }
    return output.str();
}

// Applies `mutation` to the map of checked-out Versions.  Commands are not
// serialized against each other, so the change is computed from a fresh read
// and published only if the map still reads the same once the replacement is
// ready; otherwise it is computed again from the newer map.
void mutateWorkspaceMap(const std::filesystem::path& workspace,
                        const std::function<void(std::vector<WorkspaceEntry>&)>& mutation) {
    for (int attempt = 1; attempt <= kStateUpdateAttempts; ++attempt) {
        auto entries = readWorkspaceMap(workspace);
        const auto before = serializeWorkspaceMap(entries);
        mutation(entries);
        const bool published = writeTextFileIf(
            localStatePath(workspace, "workspaces"), serializeWorkspaceMap(entries), 0600,
            [&] { return serializeWorkspaceMap(readWorkspaceMap(workspace)) == before; });
        if (published) return;
        ::usleep(static_cast<useconds_t>(1000 * attempt));
    }
    throw std::runtime_error(
        "the list of checked-out versions kept changing while it was being updated; "
        "run the command again");
}

void upsertWorkspaceEntry(const std::filesystem::path& workspace,
                          const std::string& version) {
    const auto subdir = versionSubdir(version);
    mutateWorkspaceMap(workspace, [&](std::vector<WorkspaceEntry>& entries) {
        for (auto& entry : entries) {
            if (entry.version == version) {
                entry.relative_subdir = subdir;
                return;
            }
        }
        entries.push_back(WorkspaceEntry{version, subdir});
    });
}

Config readConfig(const std::filesystem::path& project_root) {
    const auto path = sharedStatePath(project_root, "config");
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("missing project config: " + path.string() +
                                 "; ask the PM to rerun isvn admin init for " +
                                 project_root.string());
    }
    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        values[trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    Config config;
    config.repo_url = trimTrailingSlash(values["repo_url"]);
    config.pool_root = values["pool_root"];
    if (values.count("branch_prefix")) config.branch_prefix = values["branch_prefix"];
    if (values.count("branch_layout_prefix")) config.branch_prefix = values["branch_layout_prefix"];
    if (values.count("project_group")) config.project_group = values["project_group"];
    if (config.repo_url.empty() || config.pool_root.empty()) {
        throw std::runtime_error(".isvn/config must contain repo_url and pool_root");
    }
    return config;
}

std::filesystem::path readProjectDirFromWorkspace(const std::filesystem::path& workspace) {
    const auto path = localStatePath(workspace, "config");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("missing .isvn_local/config; run isvn member init first");
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (trim(line.substr(0, equals)) == "project_dir") {
            return std::filesystem::absolute(trim(line.substr(equals + 1)));
        }
    }
    throw std::runtime_error(".isvn_local/config must contain project_dir");
}

std::filesystem::path findProjectDirForSharedCommand() {
    if (const char* env = std::getenv("ISVN_PROJECT_DIR"); env != nullptr && env[0] != '\0') {
        return std::filesystem::absolute(env);
    }
    if (const auto workspace = maybeWorkspaceRoot(std::filesystem::current_path())) {
        return readProjectDirFromWorkspace(*workspace);
    }
    std::filesystem::path start = std::filesystem::absolute(std::filesystem::current_path());
    while (true) {
        if (std::filesystem::is_directory(start / ".isvn")) return start;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    throw std::runtime_error("cannot locate project dir; set ISVN_PROJECT_DIR or run from a workspace");
}

struct Runtime {
    std::filesystem::path project_dir;
    std::filesystem::path workspace;
    Config config;
    Lineage lineage;
    ObjectStore store;
    SvnOps svn;

    Runtime(const std::filesystem::path& shared_project_dir,
            const std::filesystem::path& member_workspace)
        : project_dir(shared_project_dir),
          workspace(member_workspace),
          config(readConfig(project_dir)),
          lineage(project_dir),
          store(config.pool_root),
          svn(config.repo_url) {}
};

Runtime workspaceRuntime() {
    const auto workspace = findWorkspaceRoot(std::filesystem::current_path());
    return Runtime(readProjectDirFromWorkspace(workspace), workspace);
}

Runtime sharedRuntime() {
    const auto workspace = maybeWorkspaceRoot(std::filesystem::current_path()).value_or(
        std::filesystem::current_path());
    return Runtime(findProjectDirForSharedCommand(), workspace);
}

std::set<std::string> readWorkspaceState(const std::filesystem::path& root) {
    std::set<std::string> paths;
    const auto path = localStatePath(root, "workspace_state");
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) paths.insert(line);
    }
    return paths;
}

void workTestPause(const std::string& point);

// Applies `mutation` to the workspace state.  The record covers every Version of
// the workspace and operations are not serialized against each other, so the
// change is made optimistically: it is computed from a fresh read and published
// only if the record still reads the same once the replacement is ready;
// otherwise it is computed again from the newer record.  An entry written by an
// operation that finishes at the same time is thereby kept instead of being
// overwritten with an older view.
void mutateWorkspaceState(const std::filesystem::path& root,
                          const std::function<void(std::set<std::string>&)>& mutation) {
    for (int attempt = 1; attempt <= kStateUpdateAttempts; ++attempt) {
        const auto before = readWorkspaceState(root);
        if (attempt == 1) workTestPause("workspace_state_read");
        auto state = before;
        mutation(state);
        std::ostringstream output;
        for (const auto& path : state) output << path << '\n';
        const bool published = writeTextFileIf(
            localStatePath(root, "workspace_state"), output.str(), 0600,
            [&] { return readWorkspaceState(root) == before; });
        if (published) return;
        ::usleep(static_cast<useconds_t>(1000 * attempt));
    }
    throw std::runtime_error(
        "the workspace state kept changing while it was being updated; run the command again");
}

std::string relativeString(const std::filesystem::path& root,
                           const std::filesystem::path& path) {
    return std::filesystem::relative(std::filesystem::absolute(path), root).generic_string();
}

std::string lexicalRelativeString(const std::filesystem::path& root,
                                  const std::filesystem::path& path) {
    const auto absolute_root = std::filesystem::absolute(root).lexically_normal();
    const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
    return absolute_path.lexically_relative(absolute_root).generic_string();
}

bool isUnder(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto absolute_child = std::filesystem::weakly_canonical(child);
    const auto absolute_parent = std::filesystem::weakly_canonical(parent);
    auto child_it = absolute_child.begin();
    for (auto parent_it = absolute_parent.begin(); parent_it != absolute_parent.end();
         ++parent_it, ++child_it) {
        if (child_it == absolute_child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

bool isSameOrUnder(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto absolute_child = std::filesystem::weakly_canonical(child);
    const auto absolute_parent = std::filesystem::weakly_canonical(parent);
    return absolute_child == absolute_parent || isUnder(absolute_child, absolute_parent);
}

bool isSameOrUnderLexical(const std::filesystem::path& child,
                          const std::filesystem::path& parent) {
    const auto absolute_child = std::filesystem::absolute(child).lexically_normal();
    const auto absolute_parent = std::filesystem::absolute(parent).lexically_normal();
    auto child_it = absolute_child.begin();
    for (auto parent_it = absolute_parent.begin(); parent_it != absolute_parent.end();
         ++parent_it, ++child_it) {
        if (child_it == absolute_child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

// Containment check that resolves directory symlinks on BOTH sides (so two valid
// spellings of the same working copy — e.g. an automount/home symlink vs its real
// path — still match) but never follows the leaf, which for a committed file is a
// pool symlink pointing outside the working copy. Used by checkin/edit/remove.
bool isSameOrUnderResolvingParents(const std::filesystem::path& child,
                                   const std::filesystem::path& parent) {
    std::error_code ec;
    auto resolved_parent = std::filesystem::weakly_canonical(parent, ec);
    if (ec) resolved_parent = std::filesystem::absolute(parent).lexically_normal();
    const auto child_abs = std::filesystem::absolute(child);
    auto resolved_child_dir = std::filesystem::weakly_canonical(child_abs.parent_path(), ec);
    if (ec) resolved_child_dir = child_abs.parent_path().lexically_normal();
    const auto resolved_child = resolved_child_dir / child_abs.filename();
    if (resolved_child == resolved_parent) return true;
    auto child_it = resolved_child.begin();
    for (auto parent_it = resolved_parent.begin(); parent_it != resolved_parent.end();
         ++parent_it, ++child_it) {
        if (child_it == resolved_child.end() || *child_it != *parent_it) return false;
    }
    return true;
}

struct CurrentVersion {
    std::string version;
    std::filesystem::path working_copy;
};

std::optional<CurrentVersion> currentVersionFromCwd(const Runtime& runtime) {
    const auto cwd = std::filesystem::current_path();
    std::optional<CurrentVersion> result;
    std::size_t best_depth = 0;
    for (const auto& entry : readWorkspaceMap(runtime.workspace)) {
        const auto working_copy = runtime.workspace / entry.relative_subdir;
        std::error_code error;
        if (!std::filesystem::exists(working_copy, error)) continue;
        if (!isSameOrUnder(cwd, working_copy)) continue;
        const auto depth =
            static_cast<std::size_t>(std::distance(entry.relative_subdir.begin(),
                                                   entry.relative_subdir.end()));
        if (!result.has_value() || depth > best_depth) {
            result = CurrentVersion{entry.version, working_copy};
            best_depth = depth;
        }
    }
    return result;
}

CurrentVersion requireCurrentVersion(const Runtime& runtime) {
    const auto current = currentVersionFromCwd(runtime);
    if (!current.has_value()) {
        throw std::runtime_error(
            "current directory is not a registered isvn version workspace; cd into "
            "WORKSPACE_ROOT/<phase>/<block>/<stage>/<version> or pass --parent <version|ROOT>");
    }
    return *current;
}

std::filesystem::path mappedWorkingCopy(const Runtime& runtime,
                                        const std::string& version) {
    validateVersionId(version);
    for (const auto& entry : readWorkspaceMap(runtime.workspace)) {
        if (entry.version == version) {
            const auto working_copy = runtime.workspace / entry.relative_subdir;
            if (!std::filesystem::is_directory(working_copy)) {
                throw std::runtime_error("version workspace is missing; run isvn checkout " +
                                         version + " first: " + working_copy.string());
            }
            return working_copy;
        }
    }
    throw std::runtime_error("version is not checked out in this workspace; run isvn checkout " +
                             version + " first");
}

CurrentVersion explicitVersionInWorkspace(const Runtime& runtime,
                                          const std::string& version) {
    return CurrentVersion{version, mappedWorkingCopy(runtime, version)};
}

CurrentVersion currentOrExplicitVersion(const Runtime& runtime,
                                        const std::optional<std::string>& version) {
    if (version.has_value()) return explicitVersionInWorkspace(runtime, *version);
    return requireCurrentVersion(runtime);
}

void verifyWorkingCopyMatchesMapping(const Runtime& runtime,
                                     const CurrentVersion& current) {
    const auto info = runtime.svn.info(current.working_copy);
    const auto actual = runtime.svn.urlToVersion(info.url);
    if (!actual.has_value() || *actual != current.version) {
        std::ostringstream message;
        message << "workspace mapping mismatch for " << current.working_copy.string()
                << ": isvn records " << current.version << " but svn working copy is ";
        if (actual.has_value()) {
            message << *actual;
        } else {
            message << info.url;
        }
        message << ". Stop and repair the local workspace mapping or working copy before create.";
        throw std::runtime_error(message.str());
    }
}

bool hasLineageRecord(Lineage& lineage, const std::string& version) {
    return !lineage.auditTrail(version).empty();
}

bool isPoolSymlink(const std::filesystem::path& path, const std::filesystem::path& pool) {
    std::error_code error;
    if (!std::filesystem::is_symlink(path, error)) return false;
    const auto target = std::filesystem::read_symlink(path, error);
    if (error) return false;
    const auto resolved =
        target.is_absolute() ? target : std::filesystem::absolute(path.parent_path() / target);
    return isUnder(resolved, pool / "objects");
}

std::string poolHashFromSymlink(const std::filesystem::path& path,
                                const std::filesystem::path& pool) {
    std::error_code error;
    if (!std::filesystem::is_symlink(path, error)) {
        throw std::runtime_error("path is not a [LINKED] pool symlink: " + path.string());
    }
    const auto target = std::filesystem::read_symlink(path, error);
    if (error) throw std::runtime_error("unable to read symlink: " + path.string());
    const auto resolved =
        target.is_absolute() ? target : std::filesystem::absolute(path.parent_path() / target);
    if (!isUnder(resolved, pool / "objects")) {
        throw std::runtime_error("symlink does not point into the object pool: " + path.string());
    }
    const std::string hash = resolved.filename().string();
    if (hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
        throw std::runtime_error("pool symlink target does not end in a BLAKE3 hash");
    }
    return hash;
}

std::optional<std::string> poolHashFromSymlinkIfAny(const std::filesystem::path& path,
                                                    const std::filesystem::path& pool) {
    std::error_code error;
    if (!std::filesystem::is_symlink(path, error)) return std::nullopt;
    const auto target = std::filesystem::read_symlink(path, error);
    if (error) return std::nullopt;
    const auto resolved =
        target.is_absolute() ? target : std::filesystem::absolute(path.parent_path() / target);
    if (!isUnder(resolved, pool / "objects")) return std::nullopt;
    const std::string hash = resolved.filename().string();
    if (hash.size() != 64 ||
        hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
        return std::nullopt;
    }
    return hash;
}

bool shouldSkipPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = std::filesystem::relative(path, root);
    for (const auto& part : relative) {
        const auto text = part.string();
        if (text == ".isvn" || text == ".isvn_local" || text == ".svn") return true;
    }
    return false;
}

std::vector<std::filesystem::path> regularWorkspaceFiles(
    const std::filesystem::path& root, const std::set<std::string>& local_paths) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (shouldSkipPath(root, entry.path())) continue;
        std::error_code error;
        if (!entry.is_regular_file(error) || std::filesystem::is_symlink(entry.path(), error)) {
            continue;
        }
        const auto relative = relativeString(root, entry.path());
        if (local_paths.count(relative) != 0) continue;
        files.push_back(entry.path());
    }
    return files;
}

struct StatusDisplayRow {
    std::string path;
    std::string workspace_path;
    std::string link_state;
    std::string svn_state;
};

std::optional<std::string> optionValue(const std::vector<std::string>& args,
                                       const std::string& name);

struct VersionParts {
    std::optional<std::string> phase;
    std::optional<std::string> block;
    std::optional<std::string> stage;
    std::string version;
};

VersionParts parseVersionParts(const std::string& version) {
    if (version == "ROOT") {
        return VersionParts{std::nullopt, std::nullopt, std::nullopt, "ROOT"};
    }
    validateVersionId(version);
    const auto parts = split(version, '/');
    return VersionParts{parts[0], parts[1], parts[2], parts[3]};
}

std::string projectIdentity(const std::filesystem::path& project) {
    const auto name = project.filename().string();
    return name.empty() ? "project" : name;
}

std::string canonicalVersion(const std::filesystem::path& project,
                             const std::string& version) {
    return "isvn://" + projectIdentity(project) + "/" + version;
}

bool hasActiveLineageRecord(Lineage& lineage, const std::string& version) {
    if (!hasLineageRecord(lineage, version)) return false;
    if (lineage.parentOf(version).has_value()) return true;
    const auto records = lineage.auditTrail(version);
    return std::any_of(records.begin(), records.end(),
                       [](const auto& record) {
                           return record.action == "create" &&
                                  !record.new_parent.has_value();
                       });
}

void appendWarningJson(std::ostream& out, const std::string& code,
                       const std::string& message) {
    out << "{\"code\":";
    appendJsonString(out, code);
    out << ",\"message\":";
    appendJsonString(out, message);
    out << "}";
}

void appendWarningsJson(std::ostream& out,
                        const std::vector<std::pair<std::string, std::string>>& warnings) {
    out << "[";
    for (std::size_t index = 0; index < warnings.size(); ++index) {
        if (index != 0) out << ",";
        appendWarningJson(out, warnings[index].first, warnings[index].second);
    }
    out << "]";
}

void appendVersionObjectJson(std::ostream& out, const std::filesystem::path& project,
                             const std::string& version,
                             const std::optional<int>& depth = std::nullopt,
                             const std::optional<std::string>& relation = std::nullopt,
                             const std::optional<std::string>& file_state = std::nullopt,
                             const std::optional<std::string>& role = std::nullopt,
                             const char* depth_name = "depth") {
    const auto parts = parseVersionParts(version);
    out << "{\"canonical\":";
    appendJsonString(out, canonicalVersion(project, version));
    out << ",\"phase\":";
    appendJsonNullable(out, parts.phase);
    out << ",\"block\":";
    appendJsonNullable(out, parts.block);
    out << ",\"stage\":";
    appendJsonNullable(out, parts.stage);
    out << ",\"version\":";
    appendJsonString(out, parts.version);
    if (file_state.has_value()) {
        out << ",\"file_state\":";
        appendJsonString(out, *file_state);
    }
    if (depth.has_value()) out << ",\"" << depth_name << "\":" << *depth;
    if (relation.has_value()) {
        out << ",\"relation\":";
        appendJsonString(out, *relation);
    }
    if (role.has_value()) {
        out << ",\"role\":";
        appendJsonString(out, *role);
    }
    out << "}";
}

void appendSubjectVersionJson(std::ostream& out, const std::filesystem::path& project,
                              const std::string& version) {
    const auto parts = parseVersionParts(version);
    out << "{\"canonical\":";
    appendJsonString(out, canonicalVersion(project, version));
    out << ",\"phase\":";
    appendJsonNullable(out, parts.phase);
    out << ",\"block\":";
    appendJsonNullable(out, parts.block);
    out << ",\"stage\":";
    appendJsonNullable(out, parts.stage);
    out << ",\"version\":";
    appendJsonString(out, parts.version);
    out << ",\"resolved\":true}";
}

std::filesystem::path projectFromOptionOrContext(const std::vector<std::string>& args) {
    if (const auto project = optionValue(args, "--project")) {
        return std::filesystem::absolute(*project);
    }
    return findProjectDirForSharedCommand();
}

void appendProvenanceErrorJson(std::ostream& out, const std::filesystem::path& project,
                               const std::string& type, const std::string& version,
                               const std::string& code, const std::string& message) {
    out << "{\"schema_version\":\"isvn.provenance.query.v0\","
           "\"authority\":\"declared_provenance\",\"content_verified\":false,"
           "\"status\":\"error\",\"query\":{\"type\":";
    appendJsonString(out, type);
    out << ",\"project\":";
    appendJsonString(out, project.string());
    out << ",\"version\":";
    appendJsonString(out, version);
    out << "},\"error\":{\"code\":";
    appendJsonString(out, code);
    out << ",\"message\":";
    appendJsonString(out, message);
    out << "},\"warnings\":[]}\n";
}

void appendGenericErrorJson(std::ostream& out, const std::string& code,
                            const std::string& message) {
    out << "{\"schema_version\":\"isvn.error.v0\",\"status\":\"error\","
           "\"error\":{\"code\":";
    appendJsonString(out, code);
    out << ",\"message\":";
    appendJsonString(out, message);
    out << "}}\n";
}

std::string relationForDepth(int depth, int root_depth) {
    if (depth == 1) return "parent";
    if (depth == root_depth) return "root";
    return "ancestor";
}

std::string fileStateForPath(const std::filesystem::path& working_copy,
                             const std::string& path) {
    const auto full = working_copy / path;
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::symlink_status(full, ec))) return "absent";
    const auto status = std::filesystem::status(full, ec);
    if (!ec && std::filesystem::is_directory(status)) return "directory";
    if (std::filesystem::is_symlink(full, ec)) return "checked_in";
    return "checked_in";
}

bool versionContainsPath(const std::filesystem::path& working_copy,
                         const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::symlink_status(working_copy / path, ec));
}

struct InventoryVersion {
    std::string version;
    std::filesystem::path working_copy;
    std::string file_state;
};

std::vector<InventoryVersion> inventoryVersionsForPath(const Runtime& runtime,
                                                       const std::string& path) {
    std::vector<InventoryVersion> result;
    for (const auto& entry : readWorkspaceMap(runtime.workspace)) {
        const auto working_copy = runtime.workspace / entry.relative_subdir;
        if (!std::filesystem::is_directory(working_copy)) continue;
        if (!versionContainsPath(working_copy, path)) continue;
        result.push_back(InventoryVersion{entry.version, working_copy,
                                          fileStateForPath(working_copy, path)});
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.version < rhs.version;
    });
    return result;
}

std::optional<InventoryVersion> inventorySubjectForPath(const Runtime& runtime,
                                                        const std::string& version,
                                                        const std::string& path) {
    const auto working_copy = mappedWorkingCopy(runtime, version);
    if (!versionContainsPath(working_copy, path)) return std::nullopt;
    return InventoryVersion{version, working_copy, fileStateForPath(working_copy, path)};
}

std::optional<std::pair<std::string, int>> fileParentCandidate(Lineage& lineage,
                                                               const Runtime& runtime,
                                                               const std::string& subject,
                                                               const std::string& path) {
    const auto chain = lineage.ancestry(subject);
    for (std::size_t index = 0; index < chain.size(); ++index) {
        try {
            const auto working_copy = mappedWorkingCopy(runtime, chain[index]);
            if (versionContainsPath(working_copy, path)) {
                return std::make_pair(chain[index], static_cast<int>(index + 1));
            }
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, int>> fileRootCandidate(Lineage& lineage,
                                                             const Runtime& runtime,
                                                             const std::string& subject,
                                                             const std::string& path) {
    std::optional<std::pair<std::string, int>> result;
    if (inventorySubjectForPath(runtime, subject, path).has_value()) {
        result = std::make_pair(subject, 0);
    }
    const auto chain = lineage.ancestry(subject);
    for (std::size_t index = 0; index < chain.size(); ++index) {
        try {
            const auto working_copy = mappedWorkingCopy(runtime, chain[index]);
            if (versionContainsPath(working_copy, path)) {
                result = std::make_pair(chain[index], static_cast<int>(index + 1));
            }
        } catch (const std::exception&) {
        }
    }
    return result;
}

std::optional<std::string> svnStatusLogicalPath(const CurrentVersion& current,
                                                const std::string& raw_path) {
    if (raw_path.empty()) return std::nullopt;
    const std::filesystem::path path(raw_path);
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path);
    if (!isSameOrUnderLexical(absolute, current.working_copy)) return std::nullopt;
    return lexicalRelativeString(current.working_copy, absolute);
}

std::vector<StatusDisplayRow> statusRowsForCurrentVersion(
    const Runtime& runtime, const CurrentVersion& current,
    const std::set<std::string>& local_paths,
    const std::vector<version::SvnStatusEntry>& svn_entries) {
    std::map<std::string, std::string> svn_state_by_path;
    for (const auto& entry : svn_entries) {
        const auto logical = svnStatusLogicalPath(current, entry.path);
        if (logical.has_value()) svn_state_by_path[*logical] = entry.status;
    }

    std::vector<StatusDisplayRow> rows;
    std::set<std::string> seen;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(current.working_copy)) {
        if (shouldSkipPath(current.working_copy, entry.path())) continue;
        std::error_code error;
        const bool symlink = std::filesystem::is_symlink(entry.path(), error);
        if (error) continue;
        if (!symlink && !entry.is_regular_file(error)) continue;

        const auto path = std::filesystem::absolute(entry.path());
        const auto logical = lexicalRelativeString(current.working_copy, path);
        const auto workspace_relative = lexicalRelativeString(runtime.workspace, path);
        const bool linked = isPoolSymlink(path, runtime.config.pool_root);
        const bool is_local = local_paths.count(workspace_relative) != 0 ||
                              (!linked && std::filesystem::exists(path));
        const auto svn_it = svn_state_by_path.find(logical);
        rows.push_back(StatusDisplayRow{
            logical,
            workspace_relative,
            linked ? "LINKED" : (is_local ? "LOCAL" : ""),
            svn_it == svn_state_by_path.end() ? "normal" : svn_it->second});
        seen.insert(logical);
    }

    for (const auto& [logical, state] : svn_state_by_path) {
        if (seen.count(logical) != 0) continue;
        const auto path = current.working_copy / logical;
        rows.push_back(StatusDisplayRow{
            logical,
            lexicalRelativeString(runtime.workspace, path),
            "",
            state});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.path < rhs.path;
    });
    return rows;
}

// Replaces a regular file with a link to its pool object.  The reference is registered
// before the file is touched: an object without references may be reclaimed by GC at
// any time, and once the reference exists it no longer can.  If GC reclaimed the object
// between storing and registering, it is stored again.  Until the link is in place the
// file itself still holds the content.
void relinkFile(ObjectStore& store, const std::filesystem::path& file) {
    if (!std::filesystem::is_regular_file(file) || std::filesystem::is_symlink(file)) return;
    const auto absolute = std::filesystem::absolute(file);
    try {
        store.unregister_ref(absolute.string());
    } catch (const std::exception&) {
    }
    auto result = store.put_object(file);
    try {
        store.register_ref(result.hash, absolute.string());
    } catch (const std::exception&) {
        result = store.put_object(file);
        store.register_ref(result.hash, absolute.string());
    }
    const auto object = store.object_path(result.hash);
    const auto temp = absolute.string() + ".link." + std::to_string(::getpid());
    try {
        std::filesystem::remove(temp);
        std::filesystem::create_symlink(object, temp);
        std::filesystem::rename(temp, absolute);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        try {
            store.unregister_ref(absolute.string());
        } catch (const std::exception&) {
        }
        throw;
    }
}

bool repairDanglingLink(Runtime& runtime, const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path);
    if (!runtime.store.is_dangling_link(absolute.string())) return false;
    try {
        runtime.store.unregister_ref(absolute.string());
    } catch (const std::exception&) {
    }
    std::filesystem::remove(absolute);
    runtime.svn.revert(absolute);
    if (!std::filesystem::is_regular_file(absolute) || std::filesystem::is_symlink(absolute)) {
        runtime.svn.update(absolute.parent_path());
    }
    relinkFile(runtime.store, absolute);
    const std::string hash = poolHashFromSymlink(absolute, runtime.config.pool_root);
    runtime.store.touch_last_access(hash);
    return true;
}

int repairDanglingLinksUnder(Runtime& runtime, const std::filesystem::path& root) {
    int repaired = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (shouldSkipPath(runtime.workspace, entry.path())) continue;
        std::error_code error;
        if (!std::filesystem::is_symlink(entry.path(), error)) continue;
        if (runtime.store.is_dangling_link(std::filesystem::absolute(entry.path()).string())) {
            if (repairDanglingLink(runtime, entry.path())) ++repaired;
        }
    }
    return repaired;
}

int repairDanglingWorkspaceLinks(Runtime& runtime) {
    return repairDanglingLinksUnder(runtime, runtime.workspace);
}

// Absolute, normalized form of a path given on the command line.  A folder named with a
// trailing separator ("rtl/") is the folder itself.
std::filesystem::path absoluteArgumentPath(const std::filesystem::path& argument) {
    auto path = std::filesystem::absolute(argument).lexically_normal();
    if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    return path;
}

// Working-copy nodes at or below `root` that carry a scheduled change, reduced to
// the top-most ones.  Unmodified managed files rest as pool symlinks, which
// Subversion reports as obstructed; they are never returned, so committing these
// targets cannot fail on them the way a commit of the whole working copy does.
std::vector<std::filesystem::path> scheduledCommitTargets(const Runtime& runtime,
                                                          const std::filesystem::path& root) {
    struct Scheduled {
        std::filesystem::path path;
        bool with_subtree;
    };
    std::vector<Scheduled> scheduled;
    for (const auto& entry : runtime.svn.status(root)) {
        // An added, replaced or deleted node is committed together with its children.
        const bool with_subtree =
            entry.status == "A" || entry.status == "R" || entry.status == "D";
        if (!with_subtree && entry.status != "M") continue;
        const auto path = std::filesystem::absolute(entry.path).lexically_normal();
        // A directory that only has a property change would be committed with its
        // whole subtree, obstructed nodes included; it is left out.
        std::error_code error;
        if (!with_subtree && std::filesystem::is_directory(path, error)) continue;
        scheduled.push_back(Scheduled{path, with_subtree});
    }
    std::sort(scheduled.begin(), scheduled.end(),
              [](const Scheduled& lhs, const Scheduled& rhs) { return lhs.path < rhs.path; });
    std::vector<std::filesystem::path> targets;
    std::optional<std::filesystem::path> covering;
    for (const auto& item : scheduled) {
        if (covering.has_value() && isSameOrUnderLexical(item.path, *covering)) continue;
        targets.push_back(item.path);
        if (item.with_subtree) covering = item.path;
    }
    return targets;
}

// True when Subversion tracks `path` and nothing at or below it differs from the
// repository.  Managed files resting as pool symlinks count as unchanged.
bool isTrackedAndUnchanged(const Runtime& runtime, const std::filesystem::path& path) {
    try {
        if (runtime.svn.info(path).revision <= 0) return false;
    } catch (const std::exception&) {
        return false;
    }
    const auto entries = runtime.svn.status(path);
    return std::all_of(entries.begin(), entries.end(),
                       [](const auto& entry) { return entry.status == "obstructed"; });
}

// Replaces a pool symlink with a writable regular copy of the object it points to and
// drops the pool reference held for the link.
void restoreRegularFileFromPoolLink(Runtime& runtime, const std::filesystem::path& link) {
    const auto target = std::filesystem::read_symlink(link);
    const auto resolved = target.is_absolute() ? target : link.parent_path() / target;
    const auto temp = link.string() + ".local." + std::to_string(::getpid());
    std::filesystem::copy_file(resolved, temp, std::filesystem::copy_options::overwrite_existing);
    try {
        runtime.store.unregister_ref(link.string());
    } catch (const std::exception&) {
    }
    std::filesystem::remove(link);
    std::filesystem::rename(temp, link);
    std::error_code error;
    std::filesystem::permissions(link, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, error);
}

// A pool symlink that Subversion does not track as a regular file would be added as a
// symbolic link, and the repository would then hold the link instead of the content.
// Before anything at or below `root` is scheduled, every such link is given its regular
// file back: untracked links, links inside untracked folders, and links that are already
// scheduled for addition.
void restoreLinksPendingAddition(Runtime& runtime, const std::filesystem::path& root) {
    const auto& pool = runtime.config.pool_root;
    for (const auto& entry : runtime.svn.status(root)) {
        const auto path = std::filesystem::absolute(entry.path).lexically_normal();
        if (entry.status == "A") {
            if (!isPoolSymlink(path, pool)) continue;
            // Scheduled as a link: schedule it again as a regular file.
            runtime.svn.revert(path);
            restoreRegularFileFromPoolLink(runtime, path);
            runtime.svn.add(path);
            continue;
        }
        if (entry.status != "?" && entry.status != "I") continue;
        if (isPoolSymlink(path, pool)) {
            restoreRegularFileFromPoolLink(runtime, path);
            continue;
        }
        // Any other link is added as the link it is; what it points to is not looked at.
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(path, error))) continue;
        std::vector<std::filesystem::path> links;
        for (const auto& child : std::filesystem::recursive_directory_iterator(path)) {
            const auto absolute = std::filesystem::absolute(child.path());
            if (isPoolSymlink(absolute, pool)) links.push_back(absolute);
        }
        for (const auto& link : links) restoreRegularFileFromPoolLink(runtime, link);
    }
}

// Schedules what Subversion does not track yet below a folder it already tracks: every
// new file, and every new folder together with its content.  A `root` that is itself
// untracked is scheduled as a whole by the caller and has nothing left to do here.
void scheduleNewContentBelow(Runtime& runtime, const std::filesystem::path& root) {
    const auto normalized_root = absoluteArgumentPath(root);
    for (const auto& entry : runtime.svn.status(root)) {
        if (entry.status != "?") continue;
        const auto path = std::filesystem::absolute(entry.path).lexically_normal();
        if (path == normalized_root) continue;
        runtime.svn.add(path);
    }
}

// Folders between the version working copy and `path` that are not in the repository
// yet, outermost first.  They are scheduled without their other content, so that `path`
// can be scheduled below them; a commit of the outermost folder then carries the folders
// and whatever is scheduled inside them.
std::vector<std::filesystem::path> scheduleNewFoldersAbove(
    Runtime& runtime, const std::filesystem::path& working_copy,
    const std::filesystem::path& path) {
    std::error_code root_error;
    if (std::filesystem::equivalent(path, working_copy, root_error)) return {};
    std::vector<std::filesystem::path> folders;
    for (auto folder = path.parent_path();; folder = folder.parent_path()) {
        std::error_code error;
        if (std::filesystem::equivalent(folder, working_copy, error)) break;
        // Not below the working copy: nothing is scheduled.
        if (error || folder == folder.parent_path()) return {};
        // A folder with a committed revision exists in the repository, and so does
        // everything above it.
        bool committed = false;
        try {
            committed = runtime.svn.info(folder).revision > 0;
        } catch (const std::exception&) {
        }
        if (committed) break;
        folders.push_back(folder);
    }
    if (folders.empty()) return {};
    std::reverse(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        // A folder left scheduled by an earlier attempt is kept as it is.
        try {
            runtime.svn.add(folder, true);
        } catch (const std::exception&) {
        }
    }
    // The commit is widened to the outermost folder only when that folder really is a
    // scheduled addition.
    for (const auto& entry : runtime.svn.status(folders.front())) {
        if (entry.status == "A" &&
            std::filesystem::absolute(entry.path).lexically_normal() == folders.front()) {
            return folders;
        }
    }
    return {};
}

// Undoes a deletion scheduled at or below `root`.  Subversion reports only the top of
// a deleted subtree and reverts one node at a time, so the walk repeats level by level
// until nothing is left scheduled for deletion.
void revertScheduledDeletion(Runtime& runtime, const std::filesystem::path& root) {
    constexpr int kMaxLevels = 256;
    for (int level = 0; level < kMaxLevels; ++level) {
        std::vector<std::filesystem::path> deleted;
        for (const auto& entry : runtime.svn.status(root)) {
            if (entry.status == "D") {
                deleted.push_back(std::filesystem::absolute(entry.path).lexically_normal());
            }
        }
        if (deleted.empty()) return;
        std::sort(deleted.begin(), deleted.end());
        for (const auto& node : deleted) runtime.svn.revert(node);
    }
}

// relinkFile leaves existing links alone, so a working link whose ref row is
// missing (for example one released by an older reconcile run from another
// account) would otherwise stay unrecorded and its object could look unused.
// Only the calling user's own workspace and own links are considered; refs that
// other accounts lost cannot be recorded from here.
void registerUnrecordedWorkspaceLinks(Runtime& runtime, const std::filesystem::path& scope) {
    struct stat root {};
    struct stat marker {};
    if (::lstat(runtime.workspace.c_str(), &root) != 0 || !S_ISDIR(root.st_mode) ||
        root.st_uid != ::geteuid() ||
        ::lstat((runtime.workspace / ".isvn_local").c_str(), &marker) != 0 ||
        !S_ISDIR(marker.st_mode) || marker.st_uid != ::geteuid()) {
        return;
    }
    std::error_code walk_error;
    std::filesystem::recursive_directory_iterator entries(
        scope, std::filesystem::directory_options::skip_permission_denied, walk_error);
    for (; !walk_error && entries != std::filesystem::recursive_directory_iterator();
         entries.increment(walk_error)) {
        const auto& entry = *entries;
        if (shouldSkipPath(runtime.workspace, entry.path())) continue;
        struct stat link {};
        if (::lstat(entry.path().c_str(), &link) != 0 || !S_ISLNK(link.st_mode) ||
            link.st_uid != ::geteuid()) {
            continue;
        }
        const auto absolute = std::filesystem::absolute(entry.path());
        const auto hash = poolHashFromSymlinkIfAny(absolute, runtime.config.pool_root);
        if (!hash || !runtime.store.has_object(*hash)) continue;
        if (runtime.store.has_ref(absolute.string())) continue;
        runtime.store.register_ref(*hash, absolute.string());
    }
}

void registerUnrecordedWorkspaceLinks(Runtime& runtime) {
    registerUnrecordedWorkspaceLinks(runtime, runtime.workspace);
}

// Re-links the regular files of one version working copy and records its links that
// have no reference.  Other versions in the workspace are left alone: an operation may
// be running on them, and a file that is new there must not become a pool link under it.
void relinkVersion(Runtime& runtime, const std::filesystem::path& working_copy) {
    repairDanglingLinksUnder(runtime, working_copy);
    const auto local = readWorkspaceState(runtime.workspace);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(working_copy)) {
        if (shouldSkipPath(runtime.workspace, entry.path())) continue;
        std::error_code error;
        if (!entry.is_regular_file(error) || std::filesystem::is_symlink(entry.path(), error)) {
            continue;
        }
        if (local.count(relativeString(runtime.workspace, entry.path())) != 0) continue;
        files.push_back(entry.path());
    }
    for (const auto& file : files) relinkFile(runtime.store, file);
    registerUnrecordedWorkspaceLinks(runtime, working_copy);
}

std::string usage() {
    return "usage: isvn <admin init|member init|create|checkout|checkin|remove|delete|checkout-ancestor|"
           "ancestry|provenance query|file query|file locate|reconcile|edit|status|work-status|workspaces|work-local-op|remove-batch|checkin-batch|import|work-recovery|renew-status|update|revert|branches|log|gc|renew|pool-stat|lineage verify|lineage audit|version>\n";
}

std::optional<std::string> optionValue(const std::vector<std::string>& args,
                                       const std::string& name) {
    for (std::size_t index = 0; index + 1 < args.size(); ++index) {
        if (args[index] == name) return args[index + 1];
    }
    return std::nullopt;
}

bool hasFlag(const std::vector<std::string>& args, const std::string& name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

std::vector<std::string> positionalArgs(const std::vector<std::string>& args) {
    std::vector<std::string> positional;
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (args[index].rfind("--", 0) == 0) {
            if (index + 1 < args.size() && args[index + 1].rfind("--", 0) != 0) {
                ++index;
            }
            continue;
        }
        positional.push_back(args[index]);
    }
    return positional;
}

gid_t gidFromGroupName(const std::string& name) {
    const group* entry = ::getgrnam(name.c_str());
    if (entry == nullptr) throw std::runtime_error("unknown project group: " + name);
    return entry->gr_gid;
}

bool currentUserInGroup(gid_t target) {
    if (::getgid() == target || ::getegid() == target) return true;
    int count = ::getgroups(0, nullptr);
    if (count < 0) throw std::runtime_error("unable to read current user groups");
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (count != 0 && ::getgroups(count, groups.data()) < 0) {
        throw std::runtime_error("unable to read current user groups");
    }
    return std::find(groups.begin(), groups.end(), target) != groups.end();
}

void ensureFileGroupMode(const std::filesystem::path& path, mode_t mode, gid_t group) {
    common::ensure_file_mode_group(path, mode, group);
}

bool hasRootAuditRecord(Lineage& lineage) {
    const auto records = lineage.auditTrail();
    return std::any_of(records.begin(), records.end(), [](const auto& record) {
        return record.child == "ROOT";
    });
}

std::string fileTypeName(const std::filesystem::file_status status) {
    if (std::filesystem::is_directory(status)) return "directory";
    if (std::filesystem::is_regular_file(status)) return "file";
    if (std::filesystem::is_symlink(status)) return "symlink";
    if (status.type() == std::filesystem::file_type::not_found) return "missing";
    return "other";
}

void appendPathHealthJson(std::ostream& out, const std::string& name,
                          const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    const bool exists = !error && std::filesystem::exists(status);
    out << "{\"name\":";
    appendJsonString(out, name);
    out << ",\"path\":";
    appendJsonString(out, path.string());
    out << ",\"exists\":" << (exists ? "true" : "false");
    out << ",\"type\":";
    appendJsonString(out, error ? "unknown" : fileTypeName(status));
    if (exists) {
        const auto space = std::filesystem::space(path, error);
        out << ",\"space_available_bytes\":";
        if (error) {
            out << "null";
        } else {
            out << space.available;
        }
        out << ",\"space_capacity_bytes\":";
        if (error) {
            out << "null";
        } else {
            out << space.capacity;
        }
        struct stat path_status {};
        struct stat parent_status {};
        const auto parent_path = path.parent_path().empty() ? std::filesystem::path(".")
                                                            : path.parent_path();
        const bool mount_root =
            ::stat(path.c_str(), &path_status) == 0 &&
            ::stat(parent_path.c_str(), &parent_status) == 0 &&
            path_status.st_dev != parent_status.st_dev;
        out << ",\"mount_root\":" << (mount_root ? "true" : "false");
    } else {
        out << ",\"space_available_bytes\":null,\"space_capacity_bytes\":null,"
               "\"mount_root\":false";
    }
    out << "}";
}

void appendPermissionJson(std::ostream& out, const std::string& name,
                          const std::filesystem::path& path,
                          std::optional<mode_t> expected_mode = std::nullopt) {
    struct stat status {};
    const bool ok = ::lstat(path.c_str(), &status) == 0;
    out << "{\"name\":";
    appendJsonString(out, name);
    out << ",\"path\":";
    appendJsonString(out, path.string());
    out << ",\"exists\":" << (ok ? "true" : "false");
    if (!ok) {
        out << ",\"mode\":null,\"expected_mode\":";
        if (expected_mode.has_value()) {
            appendJsonString(out, std::to_string(*expected_mode));
        } else {
            out << "null";
        }
        out << ",\"group\":null,\"setgid\":false,\"sticky\":false,\"matches_expected\":false}";
        return;
    }
    const mode_t mode = status.st_mode & 07777;
    out << ",\"mode\":";
    appendJsonString(out, std::to_string(mode));
    out << ",\"expected_mode\":";
    if (expected_mode.has_value()) {
        appendJsonString(out, std::to_string(*expected_mode));
    } else {
        out << "null";
    }
    const group* group_entry = ::getgrgid(status.st_gid);
    out << ",\"group\":";
    appendJsonString(out, group_entry != nullptr && group_entry->gr_name != nullptr
                              ? group_entry->gr_name
                              : std::to_string(status.st_gid));
    out << ",\"setgid\":" << ((mode & S_ISGID) ? "true" : "false")
        << ",\"sticky\":" << ((mode & S_ISVTX) ? "true" : "false")
        << ",\"matches_expected\":"
        << (!expected_mode.has_value() || mode == *expected_mode ? "true" : "false")
        << "}";
}

std::filesystem::path pathFromFileUrlOrRawPath(const std::string& value) {
    constexpr const char* prefix = "file://";
    if (value.rfind(prefix, 0) == 0) return std::filesystem::path(value.substr(7));
    return std::filesystem::path(value);
}

int cmdAdminInit(const std::vector<std::string>& args, std::ostream& out) {
    const auto project_dir = optionValue(args, "--project-dir");
    const auto repo = optionValue(args, "--repo");
    const auto pool = optionValue(args, "--pool");
    const auto group_name = optionValue(args, "--group");
    if (!project_dir || !repo || !pool || !group_name) {
        throw std::invalid_argument(
            "admin init requires --project-dir, --repo, --pool, and --group");
    }
    const gid_t group = gidFromGroupName(*group_name);
    const auto project = std::filesystem::absolute(*project_dir);
    const auto isvn = project / ".isvn";
    common::ensure_directory(project, 02770, group);
    common::ensure_directory(isvn, 02770, group);
    common::ensure_directory(*pool, 02770, group);
    Lineage lineage(project);
    ObjectStore store(*pool);
    SvnOps svn(trimTrailingSlash(*repo));
    svn.list(trimTrailingSlash(*repo), false);
    try {
        svn.list(trimTrailingSlash(*repo) + "/branches", false);
    } catch (const std::exception&) {
        svn.mkdir(trimTrailingSlash(*repo) + "/branches", "[isvn] init branches");
    }
    if (!hasRootAuditRecord(lineage)) {
        lineage.recordParent("ROOT", std::nullopt, "init");
    } else {
        const auto integrity = lineage.verifyIntegrity();
        if (!integrity.ok) {
            throw std::runtime_error("existing Lineage ROOT failed integrity verification");
        }
    }
    writeTextFile(isvn / "config",
                  "# isvn project config\nrepo_url = " + trimTrailingSlash(*repo) +
                      "\npool_root = " + std::filesystem::absolute(*pool).string() +
                      "\nbranch_prefix = branches/\nproject_group = " + *group_name + "\n",
                  0640);
    ensureFileGroupMode(isvn / "config", 0640, group);
    out << "initialized isvn project at " << project.string() << "\n";
    return 0;
}

int cmdMemberInit(const std::vector<std::string>& args, std::ostream& out) {
    const auto project_dir = optionValue(args, "--project-dir");
    const auto workspace_arg = optionValue(args, "--workspace");
    if (!project_dir || !workspace_arg) {
        throw std::invalid_argument("member init requires --project-dir and --workspace");
    }
    const auto project = std::filesystem::absolute(*project_dir);
    const Config config = readConfig(project);
    if (!config.project_group.empty()) {
        const gid_t group = gidFromGroupName(config.project_group);
        if (!currentUserInGroup(group)) {
            throw std::runtime_error("current user is not in project group: " +
                                     config.project_group);
        }
    }
    if (!std::filesystem::is_directory(config.pool_root / "objects") ||
        !std::filesystem::is_directory(config.pool_root / "locks") ||
        !std::filesystem::exists(config.pool_root / "meta.db")) {
        throw std::runtime_error("pool is not initialized or not readable: " +
                                 config.pool_root.string());
    }
    // Joining a project does not reconcile pool refs.  Other members' links may be
    // unreadable from this account or not mounted on this server, and this
    // account cannot tell that apart from a removed link.
    SvnOps svn(config.repo_url);
    svn.list(config.repo_url, false);

    const auto workspace = std::filesystem::absolute(*workspace_arg);
    std::filesystem::create_directories(workspace);
    ::chmod(workspace.c_str(), 0750);
    const auto local = workspace / ".isvn_local";
    std::filesystem::create_directories(local);
    ::chmod(local.c_str(), 0700);
    writeTextFile(local / "config", "project_dir = " + project.string() + "\n", 0600);
    if (!std::filesystem::exists(local / "workspaces")) writeTextFile(local / "workspaces", "", 0600);
    if (!std::filesystem::exists(local / "workspace_state")) {
        writeTextFile(local / "workspace_state", "", 0600);
    }
    out << "initialized isvn workspace at " << workspace.string() << "\n";
    return 0;
}

int cmdCreate(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() < 2) throw std::invalid_argument("create requires a version");
    Runtime runtime = workspaceRuntime();
    if (hasFlag(args, "--root")) {
        throw std::invalid_argument("create --root is obsolete; use --parent ROOT");
    }
    const auto positional = positionalArgs(args);
    if (positional.empty()) throw std::invalid_argument("create requires a version");
    if (positional.size() > 1) throw std::invalid_argument("create accepts exactly one version");
    const std::string child = positional.front();
    validateVersionId(child);
    if (hasLineageRecord(runtime.lineage, child)) {
        throw std::runtime_error("version already exists in Lineage: " + child);
    }

    std::optional<std::string> parent;
    const auto explicit_parent = optionValue(args, "--parent");
    if (hasFlag(args, "--parent") && !explicit_parent.has_value()) {
        throw std::invalid_argument("--parent requires ROOT or a version path");
    }
    if (explicit_parent.has_value()) {
        if (explicit_parent->empty()) {
            throw std::invalid_argument("--parent requires ROOT or a version path");
        }
        if (*explicit_parent == "ROOT") {
            parent = std::nullopt;
        } else {
            validateVersionId(*explicit_parent);
            if (!hasLineageRecord(runtime.lineage, *explicit_parent)) {
                throw std::runtime_error("parent does not exist in Lineage: " +
                                         *explicit_parent);
            }
            runtime.svn.info(std::filesystem::path(runtime.svn.versionToUrl(*explicit_parent)));
            parent = *explicit_parent;
        }
    } else {
        const auto current = requireCurrentVersion(runtime);
        verifyWorkingCopyMatchesMapping(runtime, current);
        parent = current.version;
    }

    const auto destination = runtime.workspace / versionSubdir(child);
    if (std::filesystem::exists(destination)) {
        throw std::runtime_error("target workspace already exists: " + destination.string());
    }
    if (parent.has_value()) {
        runtime.svn.createBranchFromParent(
            *parent, child, "[isvn] create " + child + " from " + *parent);
    } else {
        runtime.svn.createRootBranch(child, "[isvn] create " + child + " from ROOT");
    }
    try {
        runtime.lineage.recordParent(
            child, parent, "create",
            parent.has_value() ? "created from parent " + *parent
                               : "created from explicit parent ROOT");
    } catch (const std::exception& error) {
        err << "warning: svn branch was created but Lineage was not recorded: "
            << error.what() << "\nrun isvn reconcile before relying on ancestry\n";
        return 2;
    }
    std::filesystem::create_directories(destination.parent_path());
    runtime.svn.checkout(runtime.svn.versionToUrl(child), destination);
    upsertWorkspaceEntry(runtime.workspace, child);
    relinkVersion(runtime, destination);
    if (hasFlag(args, "--json")) {
        out << "{\"result\":\"created\",\"version\":";
        appendJsonString(out, child);
        out << ",\"parent\":";
        appendJsonString(out, parent.has_value() ? *parent : std::string("ROOT"));
        out << "}\n";
    } else {
        out << "created " << child << " from "
            << (parent.has_value() ? *parent : "ROOT") << "\n";
    }
    return 0;
}

int cmdCheckout(const std::vector<std::string>& args, std::ostream& out) {
    if (args.size() < 2) throw std::invalid_argument("checkout requires a version");
    Runtime runtime = workspaceRuntime();
    validateVersionId(args[1]);
    const auto destination = runtime.workspace / versionSubdir(args[1]);
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::is_directory(destination / ".svn")) {
        runtime.svn.update(destination);
    } else {
        runtime.svn.checkout(runtime.svn.versionToUrl(args[1]), destination);
    }
    upsertWorkspaceEntry(runtime.workspace, args[1]);
    relinkVersion(runtime, destination);
    if (hasFlag(args, "--json")) {
        out << "{\"result\":\"checked_out\",\"version\":";
        appendJsonString(out, args[1]);
        out << ",\"path\":";
        appendJsonString(out, destination.string());
        out << "}\n";
    } else {
        out << "checked out " << args[1] << " to "
            << destination.string() << "\n";
    }
    return 0;
}

int cmdCheckin(const std::vector<std::string>& args, std::ostream& out) {
    if (args.size() < 2) throw std::invalid_argument("checkin requires a file or folder");
    Runtime runtime = workspaceRuntime();
    const auto target = optionValue(args, "--to");
    const auto current = target.has_value()
                             ? CurrentVersion{*target, mappedWorkingCopy(runtime, *target)}
                             : requireCurrentVersion(runtime);
    const auto target_version = current.version;
    validateVersionId(target_version);
    verifyWorkingCopyMatchesMapping(runtime, current);
    runtime.svn.info(std::filesystem::path(runtime.svn.versionToUrl(target_version)));
    const auto source = absoluteArgumentPath(args[1]);
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("checkin path does not exist: " + source.string());
    }
    // The path that actually gets committed. If the user pointed at a file that already
    // lives inside the version working copy, check it in as-is. Otherwise copy it into the
    // version folder (same name) first, then check in — so the member does not have to copy
    // the file by hand. Containment resolves directory symlinks but not the leaf (a committed
    // file is a pool symlink whose target lives outside the working copy).
    std::filesystem::path path = source;
    bool copied_in = false;
    if (!isSameOrUnderResolvingParents(source, current.working_copy)) {
        const auto dest = current.working_copy / source.filename();
        // Clear any existing entry at the destination: a committed file rests as a
        // read-only pool symlink, so drop its ref and remove it before overwriting.
        if (std::filesystem::is_symlink(dest) || std::filesystem::exists(dest)) {
            try {
                runtime.store.unregister_ref(std::filesystem::absolute(dest).string());
            } catch (const std::exception&) {
            }
            std::filesystem::remove_all(dest);
        }
        if (std::filesystem::is_directory(source)) {
            std::filesystem::copy(source, dest,
                                  std::filesystem::copy_options::recursive |
                                      std::filesystem::copy_options::overwrite_existing);
        } else {
            std::filesystem::copy_file(source, dest,
                                       std::filesystem::copy_options::overwrite_existing);
            // The source (or a pool object) may be read-only; keep the working copy writable.
            std::error_code perm_ec;
            std::filesystem::permissions(dest, std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, perm_ec);
        }
        path = dest;
        copied_in = true;
    }
    const auto new_folders = scheduleNewFoldersAbove(runtime, current.working_copy, path);
    restoreLinksPendingAddition(runtime, path);
    try {
        runtime.svn.add(path);
    } catch (const std::exception&) {
    }
    // A folder that is already tracked is not scheduled again by the call above, so
    // the files and folders that are new inside it are scheduled here.
    scheduleNewContentBelow(runtime, path);
    const auto targets =
        scheduledCommitTargets(runtime, new_folders.empty() ? path : new_folders.front());
    // A path that does not differ from the repository is not an error: like a Subversion
    // commit without changes it creates no revision, and the files go back to being
    // pool symlinks.  The result says so instead of reporting a revision.
    std::optional<std::int64_t> revision;
    if (!targets.empty()) {
        revision = runtime.svn.commitPaths(
            targets, "[isvn] checkin " + path.filename().string() + " to " + target_version);
    } else if (!isTrackedAndUnchanged(runtime, path)) {
        throw std::runtime_error("nothing to check in under " + path.string());
    }
    // The state key is taken before relinking: afterwards the path resolves into the pool.
    std::vector<std::string> no_longer_local;
    if (std::filesystem::is_regular_file(path)) {
        no_longer_local.push_back(relativeString(runtime.workspace, path));
        relinkFile(runtime.store, path);
    } else if (std::filesystem::is_directory(path)) {
        for (const auto& file : regularWorkspaceFiles(path, {})) {
            no_longer_local.push_back(relativeString(runtime.workspace, file));
            relinkFile(runtime.store, file);
        }
    }
    mutateWorkspaceState(runtime.workspace, [&](std::set<std::string>& state) {
        for (const auto& key : no_longer_local) state.erase(key);
    });
    if (hasFlag(args, "--json")) {
        out << "{\"result\":\"" << (revision.has_value() ? "committed" : "unchanged")
            << "\",\"revision\":";
        if (revision.has_value()) {
            out << *revision;
        } else {
            out << "null";
        }
        out << ",\"file\":";
        appendJsonString(out, path.filename().string());
        out << ",\"version\":";
        appendJsonString(out, target_version);
        out << ",\"copied\":" << (copied_in ? "true" : "false") << "}\n";
    } else if (!revision.has_value()) {
        out << "no changes in " << path.filename().string() << " compared with "
            << target_version << "; nothing was committed\n";
    } else if (copied_in) {
        out << "copied " << path.filename().string() << " into " << target_version
            << " and committed r" << *revision << "\n";
    } else {
        out << "committed r" << *revision << "\n";
    }
    return 0;
}

int cmdRemove(const std::vector<std::string>& args, std::ostream& out) {
    if (args.size() < 2) throw std::invalid_argument("remove requires a file or folder");
    Runtime runtime = workspaceRuntime();
    // Optional explicit version target (mirrors checkin --to); without it, fall back to the
    // current directory's mapped version. This lets a GUI (whose cwd is not the version
    // working copy) remove a single file from a named version.
    const auto target = optionValue(args, "--from");
    const auto current = target.has_value()
                             ? CurrentVersion{*target, mappedWorkingCopy(runtime, *target)}
                             : requireCurrentVersion(runtime);
    validateVersionId(current.version);
    verifyWorkingCopyMatchesMapping(runtime, current);
    const auto path = absoluteArgumentPath(args[1]);
    // Lexical containment so pool-linked files (symlinks targeting the pool) are not
    // wrongly resolved outside the working copy; matches cmdEdit/cmdStatus/cmdCheckin.
    if (!isSameOrUnderResolvingParents(path, current.working_copy)) {
        throw std::runtime_error("remove path is not under current working copy");
    }
    // Committed files rest as pool symlinks while svn still tracks a regular file, so a
    // direct svn delete is obstructed ("changed kind"). Restore the tracked regular file
    // first (mirrors repairDanglingLink) so svn delete operates on a matching node. For a
    // folder this applies to every tracked file below it. A link that Subversion does not
    // track is left alone: nothing could bring its content back.
    std::vector<std::filesystem::path> links;
    for (const auto& entry : runtime.svn.status(path)) {
        if (entry.status != "obstructed") continue;
        const auto node = std::filesystem::absolute(entry.path).lexically_normal();
        std::error_code link_error;
        if (std::filesystem::is_symlink(node, link_error)) links.push_back(node);
    }
    for (const auto& link : links) {
        try {
            runtime.store.unregister_ref(link.string());
        } catch (const std::exception&) {
        }
        std::filesystem::remove(link);
        runtime.svn.revert(link);
    }
    // Only the removal is committed; other managed files in the working copy are
    // obstructed for Subversion and must stay out of the commit.
    std::int64_t revision = 0;
    bool scheduled = false;
    try {
        runtime.svn.remove(path);
        scheduled = true;
        revision = runtime.svn.commitPaths({path}, "[isvn] remove " + path.filename().string());
    } catch (...) {
        // Nothing reached the repository: undo the scheduled removal and turn the
        // restored files back into pool symlinks.
        try {
            if (scheduled) revertScheduledDeletion(runtime, path);
        } catch (const std::exception&) {
        }
        for (const auto& link : links) {
            try {
                relinkFile(runtime.store, link);
            } catch (const std::exception&) {
            }
        }
        throw;
    }
    const auto removed_relative = relativeString(runtime.workspace, path);
    const auto removed_prefix = removed_relative + "/";
    mutateWorkspaceState(runtime.workspace, [&](std::set<std::string>& state) {
        state.erase(removed_relative);
        for (auto it = state.lower_bound(removed_prefix);
             it != state.end() && it->compare(0, removed_prefix.size(), removed_prefix) == 0;) {
            it = state.erase(it);
        }
    });
    if (hasFlag(args, "--json")) {
        out << "{\"result\":\"removed\",\"revision\":" << revision << ",\"file\":";
        appendJsonString(out, path.filename().string());
        out << ",\"version\":";
        appendJsonString(out, current.version);
        out << "}\n";
    } else {
        out << "removed in r" << revision << "\n";
    }
    return 0;
}

int cmdDelete(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const auto positional = positionalArgs(args);
    if (positional.empty()) throw std::invalid_argument("delete requires a version");
    const bool json = hasFlag(args, "--json");
    Runtime runtime = workspaceRuntime();
    const std::string version = positional.front();
    validateVersionId(version);
    if (runtime.lineage.hasDescendants(version)) {
        if (json) {
            err << "{\"error\":\"has_descendants\",\"version\":";
            appendJsonString(err, version);
            err << "}\n";
        } else {
            err << "refusing to delete " << version << " because it has descendants:\n";
            for (const auto& child : runtime.lineage.directChildrenOf(version)) {
                err << "  " << child << "\n";
            }
        }
        return 2;
    }

    // Working links in this workspace whose ref row was lost are recorded first,
    // so releasing this version's refs cannot make a still-linked object look
    // unused.  Other accounts' lost rows cannot be recorded from here.
    registerUnrecordedWorkspaceLinks(runtime);

    // If this version is checked out in the local workspace, drop its pool references
    // and remove the working copy so the contents become reclaimable. Objects still
    // referenced by other versions/members keep a positive refcount and survive.
    // Only the object a released row recorded is a reclaim candidate, and only
    // when the link still points at that same object.
    int refs_released = 0;
    std::vector<std::string> released_hashes;
    std::vector<std::string> reclaim_warnings;
    std::optional<std::filesystem::path> local_wc;
    for (const auto& entry : readWorkspaceMap(runtime.workspace)) {
        if (entry.version == version) {
            local_wc = runtime.workspace / entry.relative_subdir;
            break;
        }
    }
    if (local_wc.has_value() && std::filesystem::exists(*local_wc)) {
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(*local_wc, ec)) {
            if (shouldSkipPath(*local_wc, entry.path())) continue;
            if (!std::filesystem::is_symlink(entry.path())) continue;
            const auto absolute = std::filesystem::absolute(entry.path());
            if (!isPoolSymlink(absolute, runtime.config.pool_root)) continue;
            const auto linked = poolHashFromSymlinkIfAny(absolute, runtime.config.pool_root);
            try {
                const auto recorded = runtime.store.ref_hash(absolute.string());
                if (!recorded) continue;
                runtime.store.unregister_ref(absolute.string());
                ++refs_released;
                if (linked && *linked == *recorded) {
                    released_hashes.push_back(*recorded);
                } else {
                    const std::string target = linked ? *linked : "no pool object";
                    reclaim_warnings.push_back(*recorded + ": recorded for " +
                                               absolute.string() + " but the link points at " +
                                               target + "; neither object was reclaimed");
                }
            } catch (const std::exception&) {
            }
        }
        std::filesystem::remove_all(*local_wc, ec);
    }
    if (local_wc.has_value()) {
        mutateWorkspaceMap(runtime.workspace, [&](std::vector<WorkspaceEntry>& entries) {
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&](const WorkspaceEntry& entry) {
                                             return entry.version == version;
                                         }),
                          entries.end());
        });
    }

    runtime.lineage.removeEntry(version, "delete by user");
    try {
        runtime.svn.branchDelete(runtime.svn.versionToUrl(version), "[isvn] delete " + version);
    } catch (const std::exception& error) {
        if (json) {
            err << "{\"error\":\"svn_delete_failed\",\"version\":";
            appendJsonString(err, version);
            err << ",\"detail\":";
            appendJsonString(err, std::string(error.what()));
            err << "}\n";
        } else {
            err << "warning: Lineage was marked removed but svn branch delete failed: "
                << error.what() << "\n";
        }
        return 2;
    }

    // Free only the objects this delete stopped referencing, and only when no
    // ref is left.  Refs of other members are never released from here, and
    // objects that were already unreferenced are left to explicit maintenance.
    int objects_reclaimed = 0;
    int stale_refs_released = 0;
    std::int64_t bytes_freed = 0;
    try {
        GarbageCollector gc(runtime.config.pool_root,
                            storage::ReconcileScope{common::system_user(), runtime.workspace});
        const auto report = gc.reclaimReleased(released_hashes);
        objects_reclaimed = report.objects_reclaimed;
        stale_refs_released = report.refs_removed;
        bytes_freed = report.bytes_freed;
        for (const auto& failure : report.reclaim_failures) {
            reclaim_warnings.push_back(failure.hash + ": " + failure.detail);
        }
    } catch (const std::exception& error) {
        reclaim_warnings.push_back(std::string("pool cleanup failed: ") + error.what());
    }

    if (json) {
        out << "{\"result\":\"deleted\",\"version\":";
        appendJsonString(out, version);
        out << ",\"refs_released\":" << refs_released
            << ",\"objects_reclaimed\":" << objects_reclaimed
            << ",\"bytes_freed\":" << bytes_freed
            << ",\"stale_refs_released\":" << stale_refs_released
            << ",\"objects_not_reclaimed\":" << reclaim_warnings.size()
            << ",\"reclaim_warnings\":[";
        for (std::size_t index = 0; index < reclaim_warnings.size(); ++index) {
            if (index != 0) out << ",";
            appendJsonString(out, reclaim_warnings[index]);
        }
        out << "]}\n";
    } else {
        out << "deleted " << version << " (refs_released=" << refs_released
            << " objects_reclaimed=" << objects_reclaimed << " bytes_freed=" << bytes_freed
            << ")\n";
    }
    if (!reclaim_warnings.empty()) {
        err << "warning: " << version << " was deleted, but " << reclaim_warnings.size()
            << " pool object(s) it no longer references were left in the pool:\n";
        for (const auto& warning : reclaim_warnings) err << "  " << warning << "\n";
    }
    return 0;
}

int cmdProvenanceQuery(const std::vector<std::string>& args, std::ostream& out) {
    if (!hasFlag(args, "--json")) {
        throw std::invalid_argument("provenance query requires --json");
    }
    const auto type = optionValue(args, "--type");
    const auto version = optionValue(args, "--version");
    if (!type) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT",
                               "--type is required for provenance query.");
        return 2;
    }
    if (!version) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT",
                               "--version is required for provenance query.");
        return 2;
    }
    if (*type != "parent" && *type != "ancestor-chain" && *type != "root" &&
        *type != "pedigree") {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "invalid provenance query --type");
        return 2;
    }
    int max_depth = 20;
    try {
        if (const auto value = optionValue(args, "--max-depth")) max_depth = std::stoi(*value);
    } catch (const std::exception&) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "--max-depth must be an integer.");
        return 2;
    }
    if (max_depth <= 0) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "--max-depth must be positive.");
        return 2;
    }

    const auto project = projectFromOptionOrContext(args);
    Lineage lineage(project);
    try {
        validateVersionId(*version);
    } catch (const std::exception& error) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", error.what());
        return 2;
    }
    if (!hasActiveLineageRecord(lineage, *version)) {
        appendProvenanceErrorJson(out, project, *type, *version, "VERSION_NOT_FOUND",
                                  "Version not found: " + *version);
        return 2;
    }

    std::vector<std::string> full_chain;
    try {
        full_chain = lineage.ancestry(*version);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        appendProvenanceErrorJson(
            out, project, *type, *version,
            message.find("Cycle") != std::string::npos ? "PROVENANCE_CYCLE_DETECTED"
                                                       : "PROVENANCE_QUERY_FAILED",
            message);
        return 2;
    }
    const bool truncated = (*type == "ancestor-chain" || *type == "pedigree") &&
                           full_chain.size() > static_cast<std::size_t>(max_depth);
    std::vector<std::pair<std::string, std::string>> warnings;
    if (truncated) {
        warnings.emplace_back("MAX_DEPTH_REACHED",
                              "The provenance traversal reached --max-depth.");
    }
    if (*type == "parent" && full_chain.empty()) {
        warnings.emplace_back("ROOT_VERSION", "This version has no declared parent.");
    }

    out << "{\"schema_version\":\"isvn.provenance.query.v0\","
           "\"authority\":\"declared_provenance\",\"content_verified\":false,"
           "\"status\":\"ok\",\"query\":{\"type\":";
    appendJsonString(out, *type);
    out << ",\"project\":";
    appendJsonString(out, project.string());
    out << ",\"version\":";
    appendJsonString(out, *version);
    if (*type == "ancestor-chain" || *type == "pedigree") {
        out << ",\"max_depth\":" << max_depth;
    }
    out << "},\"subject\":";
    appendSubjectVersionJson(out, project, *version);

    if (*type == "parent") {
        out << ",\"parent\":";
        if (full_chain.empty()) {
            out << "null";
        } else {
            appendVersionObjectJson(out, project, full_chain.front(), 1, "parent");
        }
    } else if (*type == "ancestor-chain") {
        const std::size_t limit =
            std::min(full_chain.size(), static_cast<std::size_t>(max_depth));
        out << ",\"ancestor_chain\":[";
        for (std::size_t index = 0; index < limit; ++index) {
            if (index != 0) out << ",";
            const int depth = static_cast<int>(index + 1);
            appendVersionObjectJson(out, project, full_chain[index], depth,
                                    relationForDepth(depth,
                                                     static_cast<int>(full_chain.size())));
        }
        out << "],\"root\":";
        if (full_chain.empty()) {
            appendVersionObjectJson(out, project, *version, 0, "root");
        } else if (truncated) {
            out << "null";
        } else {
            appendVersionObjectJson(out, project, full_chain.back(),
                                    static_cast<int>(full_chain.size()), "root");
        }
        out << ",\"truncated\":" << (truncated ? "true" : "false");
    } else if (*type == "root") {
        out << ",\"root\":";
        if (full_chain.empty()) {
            appendVersionObjectJson(out, project, *version, 0, "root");
        } else {
            appendVersionObjectJson(out, project, full_chain.back(),
                                    static_cast<int>(full_chain.size()), "root");
        }
    } else {
        std::vector<std::string> chain = full_chain;
        if (truncated) chain.resize(static_cast<std::size_t>(max_depth));
        std::reverse(chain.begin(), chain.end());
        chain.push_back(*version);
        out << ",\"pedigree\":{\"model\":\"single_parent_chain\","
               "\"direction\":\"root_to_subject\",\"chain\":[";
        for (std::size_t index = 0; index < chain.size(); ++index) {
            if (index != 0) out << ",";
            int depth_from_subject = 0;
            std::string relation = "subject";
            if (chain[index] != *version) {
                auto found = std::find(full_chain.begin(), full_chain.end(), chain[index]);
                depth_from_subject =
                    found == full_chain.end()
                        ? 0
                        : static_cast<int>(std::distance(full_chain.begin(), found) + 1);
                relation = relationForDepth(depth_from_subject,
                                            static_cast<int>(full_chain.size()));
            }
            appendVersionObjectJson(out, project, chain[index], depth_from_subject,
                                    relation, std::nullopt, std::nullopt,
                                    "depth_from_subject");
        }
        out << "]},\"truncated\":" << (truncated ? "true" : "false");
    }
    out << ",\"warnings\":";
    appendWarningsJson(out, warnings);
    out << "}\n";
    return 0;
}

void appendFileBaseJson(std::ostream& out, const std::string& type,
                        const std::filesystem::path& project,
                        const std::optional<std::string>& version,
                        const std::string& path, bool managed) {
    out << "{\"schema_version\":\"isvn.file.query.v0\","
           "\"authority\":\"declared_inventory\",\"content_verified\":false,"
           "\"status\":\"ok\",\"query\":{\"type\":";
    appendJsonString(out, type);
    out << ",\"project\":";
    appendJsonString(out, project.string());
    if (version.has_value()) {
        out << ",\"version\":";
        appendJsonString(out, *version);
    }
    out << ",\"path\":";
    appendJsonString(out, path);
    out << "},\"file\":{\"path\":";
    appendJsonString(out, path);
    out << ",\"managed\":" << (managed ? "true" : "false") << "}";
}

void appendCompactVersionJson(std::ostream& out, const std::filesystem::path& project,
                              const std::string& version,
                              const std::optional<int>& depth = std::nullopt,
                              const std::optional<std::string>& relation = std::nullopt) {
    out << "{\"canonical\":";
    appendJsonString(out, canonicalVersion(project, version));
    out << ",\"version\":";
    appendJsonString(out, parseVersionParts(version).version);
    if (depth.has_value()) out << ",\"depth\":" << *depth;
    if (relation.has_value()) {
        out << ",\"relation\":";
        appendJsonString(out, *relation);
    }
    out << "}";
}

// The Version working copy, inside one workspace, that holds `path`.
struct LocatedPath {
    std::filesystem::path workspace;
    std::filesystem::path project;
    std::string version;
    std::filesystem::path working_copy;
    std::string relative;
};

std::optional<LocatedPath> locateInWorkspace(const std::filesystem::path& path) {
    const auto workspace = maybeWorkspaceRoot(path.parent_path());
    if (!workspace.has_value()) return std::nullopt;
    const auto root = std::filesystem::absolute(*workspace).lexically_normal();
    for (const auto& entry : readWorkspaceMap(root)) {
        const auto working_copy = (root / entry.relative_subdir).lexically_normal();
        if (!isSameOrUnderLexical(path, working_copy)) continue;
        return LocatedPath{root, readProjectDirFromWorkspace(root), entry.version, working_copy,
                           path.lexically_relative(working_copy).generic_string()};
    }
    return std::nullopt;
}

// `isvn file locate`: which checked-out Version a path belongs to, and its coordinate.
// The answer comes from the position of the path and the workspace's own record of
// checked-out Versions.  The path's own link is never followed, and nothing is read from
// file content, SVN or Lineage; the command changes nothing.
int cmdFileLocate(const std::vector<std::string>& args, std::ostream& out) {
    if (!hasFlag(args, "--json")) throw std::invalid_argument("file locate requires --json");
    const auto path_arg = optionValue(args, "--path");
    if (!path_arg || path_arg->empty()) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "--path is required for file locate.");
        return 2;
    }
    const auto requested = absoluteArgumentPath(*path_arg);
    // The folders above the path may be reached through a link (a workspace opened
    // through a linked directory); only they are resolved, never the last component.
    std::optional<LocatedPath> located = locateInWorkspace(requested);
    if (!located.has_value()) {
        std::error_code ec;
        const auto parent = std::filesystem::weakly_canonical(requested.parent_path(), ec);
        if (!ec && parent != requested.parent_path()) {
            located = locateInWorkspace(parent / requested.filename());
        }
    }
    if (located.has_value()) {
        if (const auto expected = optionValue(args, "--project")) {
            std::error_code a_ec;
            std::error_code b_ec;
            const auto want = std::filesystem::weakly_canonical(std::filesystem::absolute(*expected), a_ec);
            const auto have = std::filesystem::weakly_canonical(located->project, b_ec);
            if (a_ec || b_ec || want != have) {
                appendGenericErrorJson(out, "PROJECT_MISMATCH",
                                       "The path belongs to a workspace of another project.");
                return 2;
            }
        }
    }

    std::error_code status_ec;
    const auto status = located.has_value()
                            ? std::filesystem::symlink_status(located->working_copy /
                                                                  located->relative,
                                                              status_ec)
                            : std::filesystem::file_status{};
    const bool exists = located.has_value() && std::filesystem::exists(status);
    std::string kind = "absent";
    std::string link_state;
    if (exists) {
        if (std::filesystem::is_symlink(status)) {
            kind = "file";
            link_state = isPoolSymlink(located->working_copy / located->relative,
                                       readConfig(located->project).pool_root)
                             ? "LINKED"
                             : "";
            if (link_state.empty()) kind = "link";
        } else if (std::filesystem::is_directory(status)) {
            kind = "directory";
        } else {
            kind = "file";
            link_state = "LOCAL";
        }
    }
    const bool at_version_root = located.has_value() && located->relative == ".";

    out << "{\"schema_version\":\"isvn.file.locate.v0\","
           "\"authority\":\"declared_inventory\",\"content_verified\":false,"
           "\"status\":\"ok\",\"query\":{\"type\":\"locate\",\"path\":";
    appendJsonString(out, requested.string());
    out << "},\"managed\":" << (exists && !at_version_root ? "true" : "false");
    out << ",\"project\":";
    if (located.has_value()) {
        out << "{\"dir\":";
        appendJsonString(out, located->project.string());
        out << ",\"name\":";
        appendJsonString(out, projectIdentity(located->project));
        out << "}";
    } else {
        out << "null";
    }
    out << ",\"version\":";
    if (located.has_value()) {
        appendVersionObjectJson(out, located->project, located->version);
    } else {
        out << "null";
    }
    out << ",\"file\":";
    if (located.has_value()) {
        out << "{\"path\":";
        appendJsonString(out, at_version_root ? std::string() : located->relative);
        out << ",\"kind\":";
        appendJsonString(out, kind);
        out << ",\"link_state\":";
        appendJsonString(out, link_state);
        out << ",\"working_copy\":";
        appendJsonString(out, located->working_copy.string());
        out << "}";
    } else {
        out << "null";
    }
    std::vector<std::pair<std::string, std::string>> warnings;
    if (!located.has_value()) {
        warnings.emplace_back("PATH_NOT_IN_VERSION",
                              "The path is not inside a Version checked out in an isvn workspace.");
    } else if (!exists) {
        warnings.emplace_back("FILE_NOT_FOUND",
                              "The path is inside a Version but does not exist.");
    } else if (at_version_root) {
        warnings.emplace_back("VERSION_ROOT", "The path is the root folder of the Version.");
    }
    out << ",\"warnings\":";
    appendWarningsJson(out, warnings);
    out << "}\n";
    return 0;
}

int cmdFileQuery(const std::vector<std::string>& args, std::ostream& out) {
    if (!hasFlag(args, "--json")) throw std::invalid_argument("file query requires --json");
    const auto type = optionValue(args, "--type");
    const auto path = optionValue(args, "--path");
    const auto version = optionValue(args, "--version");
    if (!type) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "--type is required for file query.");
        return 2;
    }
    if (!path) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "--path is required for file query.");
        return 2;
    }
    if (*type != "versions" && *type != "parent" && *type != "root") {
        appendGenericErrorJson(out, "INVALID_ARGUMENT", "invalid file query --type");
        return 2;
    }
    const std::filesystem::path query_path(*path);
    if (query_path.is_absolute() || query_path.lexically_normal().generic_string().find("..") !=
                                        std::string::npos) {
        appendGenericErrorJson(out, "INVALID_ARGUMENT",
                               "--path must be a relative project file path.");
        return 2;
    }
    const auto project = projectFromOptionOrContext(args);
    const auto workspace = maybeWorkspaceRoot(std::filesystem::current_path()).value_or(
        std::filesystem::current_path());
    Runtime runtime(project, workspace);
    std::vector<InventoryVersion> subjects;
    if (version.has_value()) {
        validateVersionId(*version);
        if (const auto subject = inventorySubjectForPath(runtime, *version, *path)) {
            subjects.push_back(*subject);
        }
    } else {
        subjects = inventoryVersionsForPath(runtime, *path);
    }
    const bool managed = !subjects.empty() || (!version.has_value() &&
                                              !inventoryVersionsForPath(runtime, *path).empty());
    std::vector<std::pair<std::string, std::string>> warnings;
    if (!managed) {
        warnings.emplace_back("FILE_NOT_MANAGED",
                              "The file path is not managed by isvn.");
    }

    appendFileBaseJson(out, *type, project, version, *path, managed);
    if (*type == "versions") {
        out << ",\"versions\":[";
        for (std::size_t index = 0; index < subjects.size(); ++index) {
            if (index != 0) out << ",";
            appendVersionObjectJson(out, project, subjects[index].version, std::nullopt,
                                    std::nullopt, subjects[index].file_state,
                                    index == 0 ? "first_seen" : "seen");
        }
        out << "],\"summary\":{\"version_count\":" << subjects.size()
            << ",\"first_seen\":";
        if (subjects.empty()) {
            out << "null";
        } else {
            appendCompactVersionJson(out, project, subjects.front().version);
        }
        out << ",\"latest_seen\":";
        if (subjects.empty()) {
            out << "null";
        } else {
            appendCompactVersionJson(out, project, subjects.back().version);
        }
        out << "},\"warnings\":";
        appendWarningsJson(out, warnings);
        out << "}\n";
        return 0;
    }

    out << ",\"subjects\":[";
    for (std::size_t index = 0; index < subjects.size(); ++index) {
        if (index != 0) out << ",";
        appendVersionObjectJson(out, project, subjects[index].version, std::nullopt,
                                std::nullopt, subjects[index].file_state);
    }
    out << "]";

    Lineage lineage(project);
    std::map<std::string, std::pair<std::string, int>> dedup;
    std::vector<std::pair<InventoryVersion, std::vector<std::pair<std::string, int>>>> per_subject;
    for (const auto& subject : subjects) {
        std::vector<std::pair<std::string, int>> candidates;
        const auto candidate = *type == "parent"
                                   ? fileParentCandidate(lineage, runtime, subject.version, *path)
                                   : fileRootCandidate(lineage, runtime, subject.version, *path);
        if (candidate.has_value()) {
            candidates.push_back(*candidate);
            dedup[candidate->first] = *candidate;
        }
        per_subject.emplace_back(subject, candidates);
    }

    const std::string result_key = *type == "parent" ? "parent_versions" : "root_versions";
    const std::string relation = *type == "parent" ? "file_parent" : "file_root";
    out << ",\"" << result_key << "\":[";
    bool first = true;
    for (const auto& [candidate_version, candidate] : dedup) {
        if (!first) out << ",";
        first = false;
        appendVersionObjectJson(out, project, candidate_version, candidate.second, relation,
                                fileStateForPath(mappedWorkingCopy(runtime, candidate_version),
                                                 *path));
    }
    out << "],\"per_subject\":[";
    for (std::size_t index = 0; index < per_subject.size(); ++index) {
        if (index != 0) out << ",";
        out << "{\"subject\":";
        appendCompactVersionJson(out, project, per_subject[index].first.version);
        out << ",\"" << result_key << "\":[";
        for (std::size_t sub_index = 0; sub_index < per_subject[index].second.size();
             ++sub_index) {
            if (sub_index != 0) out << ",";
            appendCompactVersionJson(out, project,
                                     per_subject[index].second[sub_index].first,
                                     per_subject[index].second[sub_index].second, relation);
        }
        out << "]}";
    }
    out << "],\"warnings\":";
    appendWarningsJson(out, warnings);
    out << "}\n";
    return 0;
}

int cmdCheckoutAncestor(const std::vector<std::string>& args, std::istream& in,
                        std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const auto positional = positionalArgs(args);
    if (positional.size() > 1) {
        throw std::invalid_argument("checkout-ancestor accepts at most one version");
    }
    const bool json = hasFlag(args, "--json");
    int depth = 1;
    if (const auto value = optionValue(args, "--depth")) depth = std::stoi(*value);
    if (depth <= 0) throw std::invalid_argument("--depth must be positive");
    std::optional<std::string> explicit_version;
    if (!positional.empty()) explicit_version = positional.front();
    const auto current = currentOrExplicitVersion(runtime, explicit_version);
    const auto chain = runtime.lineage.ancestry(current.version);
    if (static_cast<std::size_t>(depth) > chain.size()) {
        if (json) {
            out << "{\"result\":\"at_root\",\"depth\":" << depth << "}\n";
        } else {
            out << "already at root; ancestry depth " << depth << " is unavailable\n";
        }
        return 0;
    }
    const auto target = chain[static_cast<std::size_t>(depth - 1)];
    if (!json) out << "ancestor depth " << depth << ": " << target << "\n";
    if (!hasFlag(args, "--yes")) {
        out << "checkout this ancestor content? (y/n) ";
        std::string answer;
        std::getline(in, answer);
        if (answer != "y" && answer != "Y" && answer != "yes") {
            out << "no content checked out\n";
            return 0;
        }
    }
    const auto destination = runtime.workspace / ("ancestor-" + std::to_string(depth));
    runtime.svn.checkout(runtime.svn.versionToUrl(target), destination);
    if (json) {
        out << "{\"result\":\"checked_out\",\"depth\":" << depth << ",\"version\":";
        appendJsonString(out, target);
        out << ",\"path\":";
        appendJsonString(out, destination.string());
        out << "}\n";
    } else {
        out << "checked out ancestor content to " << destination.string() << "\n";
    }
    return 0;
}

int cmdAncestry(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    std::string version;
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (args[index].rfind("--", 0) != 0) {
            version = args[index];
            break;
        }
    }
    if (version.empty()) version = requireCurrentVersion(runtime).version;
    validateVersionId(version);
    if (json) {
        out << "{\"child\":";
        appendJsonString(out, version);
        out << ",\"chain\":" << runtime.lineage.ancestryJson(version) << "}\n";
        return 0;
    }
    out << version << "\n";
    for (const auto& ancestor : runtime.lineage.ancestry(version)) {
        out << "  <- " << ancestor << "\n";
    }
    return 0;
}

int cmdReconcile(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    std::string child;
    for (std::size_t index = 1; index < args.size(); ++index) {
        if (args[index].rfind("--", 0) != 0) {
            child = args[index];
            break;
        }
    }
    std::optional<std::string> lineage_parent;
    if (!child.empty()) {
        validateVersionId(child);
        lineage_parent = runtime.lineage.parentOf(child);
    }
    const auto integrity = runtime.lineage.verifyIntegrity();
    if (json) {
        out << "{\"child\":";
        if (child.empty()) {
            out << "null";
        } else {
            appendJsonString(out, child);
        }
        out << ",\"lineage_parent\":";
        appendJsonNullable(out, lineage_parent);
        out << ",\"svn_copyfrom_hint\":null,\"consistent\":null,\"verify\":{\"intact\":"
            << (integrity.ok ? "true" : "false") << ",\"break_at\":";
        if (integrity.first_bad_seq.has_value()) {
            out << *integrity.first_bad_seq;
        } else {
            out << "null";
        }
        out << ",\"detail\":";
        appendJsonString(out, integrity.detail);
        out << "}}\n";
        return integrity.ok ? 0 : 2;
    }
    out << "svn copy-source comparison is not available in this MVP\n";
    out << "Lineage integrity: " << (integrity.ok ? "ok" : "failed") << "\n";
    if (!integrity.ok) {
        out << integrity.detail << "\n";
        return 2;
    }
    return 0;
}

int cmdEdit(const std::vector<std::string>& args, std::ostream& out) {
    if (args.size() < 2) throw std::invalid_argument("edit requires a file");
    Runtime runtime = workspaceRuntime();
    const auto positional = positionalArgs(args);
    if (positional.empty()) throw std::invalid_argument("edit requires a file");
    if (positional.size() > 1) throw std::invalid_argument("edit accepts exactly one file");
    const auto explicit_version = optionValue(args, "--in");
    if (hasFlag(args, "--in") && !explicit_version.has_value()) {
        throw std::invalid_argument("--in requires a version path");
    }
    const auto current = currentOrExplicitVersion(runtime, explicit_version);
    const auto file_arg = std::filesystem::path(positional.front());
    // Strip version prefix if the caller already prefixed the relative path with the
    // version path (e.g. "Trail/Block1/RTL/v1/top.v" --in Trail/Block1/RTL/v1 should
    // resolve identically to "top.v" --in Trail/Block1/RTL/v1).
    auto file_in_wkcp = file_arg;
    if (explicit_version.has_value() && file_arg.is_relative()) {
        const std::filesystem::path version_path(current.version);
        auto it_f = file_arg.begin();
        auto it_v = version_path.begin();
        bool prefix_match = true;
        while (it_v != version_path.end()) {
            if (it_f == file_arg.end() || *it_f != *it_v) { prefix_match = false; break; }
            ++it_f; ++it_v;
        }
        if (prefix_match && it_f != file_arg.end()) {
            std::filesystem::path stripped;
            for (; it_f != file_arg.end(); ++it_f) stripped /= *it_f;
            file_in_wkcp = stripped;
        }
    }
    const auto path = std::filesystem::absolute(
        explicit_version.has_value() && file_in_wkcp.is_relative()
            ? current.working_copy / file_in_wkcp
            : file_arg);
    if (!isSameOrUnderResolvingParents(path, current.working_copy)) {
        throw std::runtime_error("edit path is not under target working copy");
    }
    repairDanglingLink(runtime, path);
    if (!isPoolSymlink(path, runtime.config.pool_root)) {
        throw std::runtime_error("file is not a [LINKED] pool symlink: " + path.string());
    }
    const auto target = std::filesystem::read_symlink(path);
    const auto resolved = target.is_absolute() ? target : path.parent_path() / target;
    const auto temp = path.string() + ".local." + std::to_string(::getpid());
    std::filesystem::copy_file(resolved, temp, std::filesystem::copy_options::overwrite_existing);
    try {
        runtime.store.unregister_ref(path.string());
    } catch (const std::exception&) {
    }
    std::filesystem::remove(path);
    std::filesystem::rename(temp, path);
    // Pool objects are stored read-only (0440); copy_file carried that mode onto the
    // local copy, leaving the "editable" working file unwritable. Restore owner write
    // so the member can actually modify the file before checkin.
    std::error_code perm_ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::add, perm_ec);
    const auto local_key = relativeString(runtime.workspace, path);
    mutateWorkspaceState(runtime.workspace,
                         [&](std::set<std::string>& state) { state.insert(local_key); });
    if (hasFlag(args, "--json")) {
        out << "{\"result\":\"local\",\"state\":\"LOCAL\",\"file\":";
        appendJsonString(out, relativeString(runtime.workspace, path));
        out << "}\n";
    } else {
        out << "[LOCAL] " << relativeString(runtime.workspace, path) << "\n";
    }
    return 0;
}

int cmdStatus(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    const auto positional = positionalArgs(args);
    if (positional.size() > 1) throw std::invalid_argument("status accepts at most one version");
    const auto local = readWorkspaceState(runtime.workspace);
    std::optional<std::string> explicit_version;
    if (!positional.empty()) explicit_version = positional.front();
    const auto current = currentOrExplicitVersion(runtime, explicit_version);
    verifyWorkingCopyMatchesMapping(runtime, current);
    const int repaired = explicit_version.has_value()
                             ? repairDanglingLinksUnder(runtime, current.working_copy)
                             : repairDanglingWorkspaceLinks(runtime);
    if (json) {
        out << "{\"version\":";
        appendJsonString(out, current.version);
        out << ",\"files\":[";
    } else if (repaired != 0) {
        out << "repaired_dangling_links=" << repaired << "\n";
    }
    // statusRowsForCurrentVersion uses shouldSkipPath to hide .isvn/.isvn_local/.svn entries.
    const auto rows = statusRowsForCurrentVersion(runtime, current, local,
                                                  runtime.svn.status(current.working_copy));
    bool first = true;
    for (const auto& row : rows) {
        if (json) {
            if (!first) out << ",";
            first = false;
            out << "{\"path\":";
            appendJsonString(out, row.path);
            out << ",\"link_state\":";
            appendJsonString(out, row.link_state);
            out << ",\"svn_state\":";
            appendJsonString(out, row.svn_state);
            out << "}";
        } else {
            out << row.svn_state << " " << row.workspace_path << " ";
            if (row.link_state == "LINKED") {
                out << "[LINKED]";
            } else if (row.link_state == "LOCAL") {
                out << "[LOCAL]";
            }
            out << "\n";
        }
    }
    if (json) {
        out << "],\"working_copy\":";
        appendJsonString(out, current.working_copy.string());
        out << "}\n";
    }
    return 0;
}

int cmdRenewStatus(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    const auto positional = positionalArgs(args);
    if (positional.size() > 1) {
        throw std::invalid_argument("renew-status accepts at most one version");
    }
    std::optional<std::string> explicit_version;
    if (!positional.empty()) explicit_version = positional.front();
    const auto current = currentOrExplicitVersion(runtime, explicit_version);
    const auto local = readWorkspaceState(runtime.workspace);

    if (json) {
        out << "{\"version\":";
        appendJsonString(out, current.version);
        out << ",\"files\":[";
    }

    bool first = true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(current.working_copy)) {
        if (shouldSkipPath(runtime.workspace, entry.path())) continue;
        std::error_code error;
        const bool symlink = std::filesystem::is_symlink(entry.path(), error);
        if (error) continue;
        if (!symlink && !entry.is_regular_file(error)) continue;

        const auto path = std::filesystem::absolute(entry.path());
        const auto relative = lexicalRelativeString(current.working_copy, path);
        const auto workspace_relative = lexicalRelativeString(runtime.workspace, path);
        const auto hash = poolHashFromSymlinkIfAny(path, runtime.config.pool_root);
        const bool linked = hash.has_value();
        const bool is_local = local.count(workspace_relative) != 0 || !linked;

        std::optional<std::string> last_access;
        std::optional<int> refcount;
        if (linked) {
            try {
                last_access = runtime.store.last_access_of(*hash);
                refcount = runtime.store.refcount(*hash);
            } catch (const std::exception&) {
                last_access = std::nullopt;
                refcount = std::nullopt;
            }
        }

        if (json) {
            if (!first) out << ",";
            first = false;
            out << "{\"file\":";
            appendJsonString(out, relative);
            out << ",\"state\":";
            appendJsonString(out, linked ? "LINKED" : (is_local ? "LOCAL" : ""));
            out << ",\"hash\":";
            appendJsonNullable(out, hash);
            out << ",\"last_access\":";
            appendJsonNullable(out, last_access);
            out << ",\"refcount\":";
            if (refcount.has_value()) {
                out << *refcount;
            } else {
                out << "null";
            }
            out << "}";
        } else {
            out << relative << " " << (linked ? "[LINKED]" : (is_local ? "[LOCAL]" : ""));
            if (linked) {
                out << " hash=" << *hash << " last_access=";
                if (last_access.has_value()) {
                    out << *last_access;
                } else {
                    out << "null";
                }
                out << " refcount=";
                if (refcount.has_value()) {
                    out << *refcount;
                } else {
                    out << "null";
                }
            }
            out << "\n";
        }
    }
    if (json) out << "]}\n";
    return 0;
}

// ---------------------------------------------------------------------------
// Read-only Work model (workspaces / work-status).
//
// These commands are additive and strictly read-only. They never launch an SVN
// or filesystem mutation, never advance a revision, and never alter the local
// workspace registry or pool metadata. They operate only in the current fixed
// project/workspace context and accept no project path or cross-project
// selector.
// ---------------------------------------------------------------------------

// Stable, opaque, deterministic per-checkout id derived from the version id.
// It is stable within the same user/project/local checkout and is safe to
// persist; it is never an array index. (FNV-1a 64-bit over the version string.)
std::string stableWorkspaceId(const std::string& version) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : version) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << "ws_" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

// Tolerant registry reader for the read model: skips malformed rows and reports
// them as issues instead of throwing, so a single bad row cannot crash the
// command or make it silently target another project.
std::vector<WorkspaceEntry> readWorkspaceMapTolerant(
    const std::filesystem::path& workspace,
    std::vector<std::pair<std::string, std::string>>& issues) {
    std::vector<WorkspaceEntry> entries;
    const auto path = localStatePath(workspace, "workspaces");
    std::ifstream input(path);
    if (!input) return entries;
    std::string line;
    std::set<std::string> seen;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            issues.emplace_back("MALFORMED_REGISTRY_ROW",
                                "workspace registry row is missing a tab separator");
            continue;
        }
        WorkspaceEntry entry{line.substr(0, tab), std::filesystem::path(line.substr(tab + 1))};
        try {
            validateVersionId(entry.version);
        } catch (const std::exception&) {
            issues.emplace_back("MALFORMED_REGISTRY_ROW",
                                "workspace registry row has an invalid version id");
            continue;
        }
        if (entry.relative_subdir.is_absolute() ||
            entry.relative_subdir.generic_string().find("..") != std::string::npos) {
            issues.emplace_back("MALFORMED_REGISTRY_ROW",
                                "workspace registry row has an invalid subdir");
            continue;
        }
        if (!seen.insert(entry.version).second) {
            issues.emplace_back("DUPLICATE_REGISTRY_ROW",
                                "workspace registry contains a duplicate version");
            continue;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

// One tested adapter from raw (svn_state, link_state) to the Work UI vocabulary:
// clean | modified | editable | new | conflict | missing | removed.
std::string mapUiState(const std::string& svn_state, const std::string& link_state) {
    if (svn_state == "C") return "conflict";
    if (svn_state == "!") return "missing";
    if (svn_state == "D") return "removed";
    if (svn_state == "M" || svn_state == "R") return "modified";
    if (svn_state == "A" || svn_state == "?") return "new";
    // svn_state is normal/empty/obstructed here.
    if (link_state == "LOCAL") return "editable";  // made-editable copy, unmodified
    return "clean";
}

struct ActionAvailability {
    bool allowed = false;
    std::string reason_code = "ALLOWED";
};

struct WorkFileEntry {
    std::string path;         // logical relative path
    std::string kind;         // "file" | "folder"
    std::optional<std::int64_t> size;
    std::string svn_state;    // raw machine field
    std::string link_state;   // LINKED | LOCAL | ""
    std::string ui_state;
    std::vector<std::string> issues;
    // folder aggregate counts (only meaningful when kind == "folder")
    int agg_files = 0;
    int agg_changed = 0;
    int agg_conflict = 0;
    int agg_local = 0;
};

ActionAvailability availMakeEditable(const WorkFileEntry& e) {
    if (e.kind == "folder") return {false, "UNSUPPORTED_TYPE"};
    if (e.ui_state == "missing") return {false, "MISSING_ITEM"};
    if (e.ui_state == "removed") return {false, "REMOVED_ITEM"};
    if (e.link_state == "LINKED") return {true, "ALLOWED"};
    if (e.link_state == "LOCAL") return {false, "ALREADY_LOCAL"};
    return {false, "UNMANAGED_ITEM"};
}

ActionAvailability availAddToBatch(const WorkFileEntry& e) {
    if (e.ui_state == "removed") return {false, "REMOVED_ITEM"};
    if (e.ui_state == "missing") return {false, "MISSING_ITEM"};
    if (e.ui_state == "conflict") return {true, "ALLOWED"};  // selectable, blocking
    if (e.ui_state == "modified" || e.ui_state == "new" || e.ui_state == "editable") {
        return {true, "ALLOWED"};
    }
    if (e.kind == "folder") {
        return e.agg_changed > 0 ? ActionAvailability{true, "ALLOWED"}
                                 : ActionAvailability{false, "NO_LOCAL_CHANGE"};
    }
    return {false, "CLEAN_ITEM"};
}

ActionAvailability availRevert(const WorkFileEntry& e) {
    if (e.kind == "folder") {
        return (e.agg_changed > 0 || e.agg_conflict > 0)
                   ? ActionAvailability{true, "ALLOWED"}
                   : ActionAvailability{false, "NO_LOCAL_CHANGE"};
    }
    if (e.ui_state == "modified" || e.ui_state == "conflict" ||
        e.ui_state == "editable" || e.ui_state == "removed" || e.ui_state == "missing") {
        return {true, "ALLOWED"};
    }
    return {false, "NO_LOCAL_CHANGE"};
}

ActionAvailability availRemove(const WorkFileEntry& e) {
    if (e.ui_state == "removed") return {false, "REMOVED_ITEM"};
    if (e.ui_state == "missing") return {false, "MISSING_ITEM"};
    return {true, "ALLOWED"};
}

void appendActionJson(std::ostream& out, const std::string& name,
                      const ActionAvailability& avail) {
    out << "\"" << name << "\":{\"allowed\":" << (avail.allowed ? "true" : "false")
        << ",\"reason_code\":";
    appendJsonString(out, avail.reason_code);
    out << ",\"reason_params\":{}}";
}

// Build the per-file Work entries for one checked-out version. Read-only: svn
// status is attempted but a non-working-copy or svn failure degrades to an
// explicit issue rather than throwing.
std::vector<WorkFileEntry> buildWorkFileEntries(
    const Runtime& runtime, const CurrentVersion& current,
    const std::set<std::string>& local_paths, bool& svn_available,
    std::vector<std::pair<std::string, std::string>>& issues) {
    std::map<std::string, std::string> svn_state_by_path;
    svn_available = false;
    try {
        for (const auto& entry : runtime.svn.status(current.working_copy)) {
            const auto logical = svnStatusLogicalPath(current, entry.path);
            if (logical.has_value()) svn_state_by_path[*logical] = entry.status;
        }
        svn_available = true;
    } catch (const std::exception&) {
        issues.emplace_back("SVN_STATUS_UNAVAILABLE",
                            "svn status was unavailable for this working copy");
    }

    std::vector<WorkFileEntry> entries;
    std::error_code error;
    if (!std::filesystem::is_directory(current.working_copy, error)) {
        return entries;
    }
    for (const auto& dir_entry :
         std::filesystem::recursive_directory_iterator(current.working_copy, error)) {
        if (shouldSkipPath(current.working_copy, dir_entry.path())) continue;
        const bool symlink = std::filesystem::is_symlink(dir_entry.path(), error);
        if (error) continue;
        const bool regular = dir_entry.is_regular_file(error);
        if (!symlink && !regular) continue;

        const auto path = std::filesystem::absolute(dir_entry.path());
        const auto logical = lexicalRelativeString(current.working_copy, path);
        const auto workspace_relative = lexicalRelativeString(runtime.workspace, path);
        const auto hash = poolHashFromSymlinkIfAny(path, runtime.config.pool_root);
        const bool linked = hash.has_value();
        const bool is_local = local_paths.count(workspace_relative) != 0 ||
                              (!linked && std::filesystem::exists(path, error));

        WorkFileEntry file;
        file.path = logical;
        file.kind = "file";
        file.link_state = linked ? "LINKED" : (is_local ? "LOCAL" : "");
        const auto svn_it = svn_state_by_path.find(logical);
        file.svn_state = svn_it == svn_state_by_path.end() ? "" : svn_it->second;
        file.ui_state = mapUiState(file.svn_state, file.link_state);

        // Size: LINKED -> validated pool object size; LOCAL/regular -> local
        // metadata (no content read). Broken/missing -> null + issue.
        if (linked) {
            try {
                const auto object = runtime.store.object_path(*hash);
                file.size = static_cast<std::int64_t>(std::filesystem::file_size(object, error));
                if (error) {
                    file.size = std::nullopt;
                    file.issues.push_back("MISSING_ITEM");
                }
            } catch (const std::exception&) {
                file.size = std::nullopt;
                file.issues.push_back("STATE_INCONSISTENT");
            }
        } else if (regular && !symlink) {
            const auto sz = std::filesystem::file_size(path, error);
            file.size = error ? std::optional<std::int64_t>()
                              : std::optional<std::int64_t>(static_cast<std::int64_t>(sz));
        } else {
            file.size = std::nullopt;
        }

        if (file.ui_state == "conflict") file.issues.push_back("CONFLICT_PRESENT");
        if (file.ui_state == "missing") file.issues.push_back("MISSING_ITEM");
        entries.push_back(std::move(file));
    }

    // svn-only rows (e.g. missing/removed) that no longer exist on disk.
    std::set<std::string> seen;
    for (const auto& e : entries) seen.insert(e.path);
    for (const auto& [logical, state] : svn_state_by_path) {
        if (seen.count(logical) != 0) continue;
        WorkFileEntry file;
        file.path = logical;
        file.kind = "file";
        file.link_state = "";
        file.svn_state = state;
        file.ui_state = mapUiState(state, "");
        file.size = std::nullopt;
        if (file.ui_state == "missing") file.issues.push_back("MISSING_ITEM");
        if (file.ui_state == "removed") file.issues.push_back("REMOVED_ITEM");
        entries.push_back(std::move(file));
    }

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });
    return entries;
}

struct WorkCounts {
    int changed = 0;
    int conflict = 0;
    int local = 0;
};

WorkCounts countFileEntries(const std::vector<WorkFileEntry>& entries) {
    WorkCounts counts;
    for (const auto& e : entries) {
        if (e.kind != "file") continue;
        if (e.ui_state == "conflict") ++counts.conflict;
        if (e.ui_state == "modified" || e.ui_state == "new" || e.ui_state == "editable") {
            ++counts.changed;
        }
        if (e.link_state == "LOCAL") ++counts.local;
    }
    return counts;
}

std::string healthFromCounts(const WorkCounts& counts, bool working_copy_ok) {
    if (!working_copy_ok || counts.conflict > 0) return "needs_attention";
    if (counts.changed > 0) return "changed";
    return "clean";
}

int cmdWorkspaces(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("workspaces does not accept --project; it is fixed-project");
    }
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    std::vector<std::pair<std::string, std::string>> top_issues;
    const auto entries = readWorkspaceMapTolerant(runtime.workspace, top_issues);
    const auto local = readWorkspaceState(runtime.workspace);

    // Deterministic order: hierarchy (phase/block/stage/version), then version.
    std::vector<WorkspaceEntry> ordered(entries.begin(), entries.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return a.version < b.version;
    });

    if (!json) {
        out << "project " << projectIdentity(runtime.project_dir) << "\n";
        for (const auto& e : ordered) out << e.version << "\n";
        return 0;
    }

    out << "{\"schema_version\":\"workspaces/v1\",\"project\":{\"name\":";
    appendJsonString(out, projectIdentity(runtime.project_dir));
    out << "},\"workspaces\":[";
    bool first = true;
    for (const auto& e : ordered) {
        const auto parts = parseVersionParts(e.version);
        const auto working_copy = runtime.workspace / e.relative_subdir;
        std::error_code error;
        const bool wc_ok = std::filesystem::is_directory(working_copy, error);
        WorkCounts counts;
        std::vector<std::pair<std::string, std::string>> ws_issues;
        if (wc_ok) {
            CurrentVersion current{e.version, std::filesystem::absolute(working_copy)};
            bool svn_available = false;
            const auto files = buildWorkFileEntries(runtime, current, local, svn_available,
                                                    ws_issues);
            counts = countFileEntries(files);
        } else {
            ws_issues.emplace_back("WORKING_COPY_MISSING",
                                   "mapped working copy directory does not exist");
        }

        if (!first) out << ",";
        first = false;
        out << "{\"workspace_id\":";
        appendJsonString(out, stableWorkspaceId(e.version));
        out << ",\"version\":";
        appendJsonString(out, parts.version);
        out << ",\"phase\":";
        appendJsonNullable(out, parts.phase);
        out << ",\"block\":";
        appendJsonNullable(out, parts.block);
        out << ",\"stage\":";
        appendJsonNullable(out, parts.stage);
        out << ",\"repository_path\":";
        appendJsonString(out, e.version);
        out << ",\"subdir\":";
        appendJsonString(out, e.relative_subdir.generic_string());
        out << ",\"working_copy\":";
        appendJsonString(out, working_copy.string());
        out << ",\"counts\":{\"changed\":" << counts.changed
            << ",\"conflict\":" << counts.conflict
            << ",\"local\":" << counts.local << "}";
        out << ",\"health\":";
        appendJsonString(out, healthFromCounts(counts, wc_ok));
        out << ",\"issues\":";
        appendWarningsJson(out, ws_issues);
        out << "}";
    }
    out << "],\"issues\":";
    appendWarningsJson(out, top_issues);
    out << "}\n";
    return 0;
}

// Structured, machine-readable Work error.  It is declared before the read
// lane too, because an in-flight Version transaction makes a Work Status
// response explicitly non-authoritative.
struct WorkError {
    std::string code;
    bool retryable = false;
    bool recovery_required = false;
    std::string operation_id;
};

// A WorkError with a human-readable cause.  The JSON envelope is unchanged; the
// cause is written to stderr.
struct DetailedWorkError : WorkError {
    std::string detail;
};

int cmdWorkStatus(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("work-status does not accept --project; it is fixed-project");
    }
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    const auto positional = positionalArgs(args);
    if (positional.size() != 1) {
        throw std::invalid_argument("work-status requires exactly one version");
    }
    const std::string version = positional.front();

    // Reject a version that is not in the current project's local inventory.
    std::vector<std::pair<std::string, std::string>> reg_issues;
    const auto registry = readWorkspaceMapTolerant(runtime.workspace, reg_issues);
    const auto match = std::find_if(registry.begin(), registry.end(),
                                    [&](const auto& e) { return e.version == version; });
    if (match == registry.end()) {
        if (json) {
            out << "{\"schema_version\":\"work-status/v1\",\"status\":\"error\",\"version\":";
            appendJsonString(out, version);
            out << ",\"reason_code\":\"INVALID_VERSION\",\"issues\":[";
            appendWarningJson(out, "INVALID_VERSION",
                              "version is not checked out in this workspace");
            out << "]}\n";
        }
        throw std::invalid_argument("version is not checked out in this workspace: " + version);
    }

    const auto working_copy = std::filesystem::absolute(runtime.workspace / match->relative_subdir);
    CurrentVersion current{version, working_copy};
    const auto local = readWorkspaceState(runtime.workspace);
    bool svn_available = false;
    std::vector<std::pair<std::string, std::string>> issues;
    auto entries = buildWorkFileEntries(runtime, current, local, svn_available, issues);

    // Synthesize folder aggregate entries from file paths.
    std::map<std::string, WorkFileEntry> folders;
    for (const auto& e : entries) {
        std::filesystem::path p(e.path);
        std::filesystem::path prefix;
        std::vector<std::string> parts;
        for (const auto& part : p.parent_path()) parts.push_back(part.string());
        std::string acc;
        for (const auto& part : parts) {
            acc = acc.empty() ? part : acc + "/" + part;
            auto& folder = folders[acc];
            folder.path = acc;
            folder.kind = "folder";
            folder.agg_files += (e.kind == "file" ? 1 : 0);
            if (e.ui_state == "conflict") folder.agg_conflict += 1;
            if (e.ui_state == "modified" || e.ui_state == "new" || e.ui_state == "editable") {
                folder.agg_changed += 1;
            }
            if (e.link_state == "LOCAL") folder.agg_local += 1;
            if (e.size.has_value()) {
                folder.size = folder.size.value_or(0) + *e.size;
            }
        }
    }

    // Compute the summary from the file entries BEFORE they are merged/moved.
    const auto counts = countFileEntries(entries);
    std::int64_t total_size = 0;
    int file_count = 0;
    const int folder_count = static_cast<int>(folders.size());
    for (const auto& e : entries) {
        if (e.kind != "file") continue;
        ++file_count;
        if (e.size.has_value()) total_size += *e.size;
    }

    std::vector<WorkFileEntry> all;
    for (auto& [key, folder] : folders) all.push_back(folder);
    for (auto& e : entries) all.push_back(std::move(e));
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
        if (a.path != b.path) return a.path < b.path;
        return a.kind < b.kind;
    });

    if (!json) {
        out << "version " << version << " files=" << file_count << " changed=" << counts.changed
            << " conflict=" << counts.conflict << " local=" << counts.local << "\n";
        return 0;
    }

    out << "{\"schema_version\":\"work-status/v1\",\"workspace_id\":";
    appendJsonString(out, stableWorkspaceId(version));
    out << ",\"version\":";
    appendJsonString(out, version);
    out << ",\"repository_path\":";
    appendJsonString(out, version);
    out << ",\"working_copy\":";
    appendJsonString(out, working_copy.string());
    out << ",\"svn_available\":" << (svn_available ? "true" : "false");
    out << ",\"summary\":{\"files\":" << file_count << ",\"folders\":" << folder_count
        << ",\"changed\":" << counts.changed << ",\"conflict\":" << counts.conflict
        << ",\"local\":" << counts.local << ",\"total_size_bytes\":" << total_size << "}";
    out << ",\"entries\":[";
    bool first = true;
    for (const auto& e : all) {
        if (!first) out << ",";
        first = false;
        out << "{\"path\":";
        appendJsonString(out, e.path);
        out << ",\"kind\":";
        appendJsonString(out, e.kind);
        out << ",\"size_bytes\":";
        if (e.size.has_value()) out << *e.size; else out << "null";
        out << ",\"svn_state\":";
        appendJsonString(out, e.svn_state);
        out << ",\"link_state\":";
        appendJsonString(out, e.link_state);
        out << ",\"source_kind\":";
        appendJsonString(out, e.link_state);
        out << ",\"ui_state\":";
        appendJsonString(out, e.kind == "folder" ? std::string("") : e.ui_state);
        out << ",\"counts\":";
        if (e.kind == "folder") {
            out << "{\"files\":" << e.agg_files << ",\"changed\":" << e.agg_changed
                << ",\"conflict\":" << e.agg_conflict << ",\"local\":" << e.agg_local << "}";
        } else {
            out << "null";
        }
        out << ",\"actions\":{";
        appendActionJson(out, "make_editable", availMakeEditable(e));
        out << ",";
        appendActionJson(out, "add_to_checkin_batch", availAddToBatch(e));
        out << ",";
        appendActionJson(out, "revert", availRevert(e));
        out << ",";
        appendActionJson(out, "remove", availRemove(e));
        out << "},\"issues\":";
        std::vector<std::pair<std::string, std::string>> entry_issues;
        for (const auto& code : e.issues) entry_issues.emplace_back(code, code);
        appendWarningsJson(out, entry_issues);
        out << "}";
    }
    out << "],\"version_actions\":{";
    appendActionJson(out, "create_derived_version", {true, "ALLOWED"});
    out << ",";
    appendActionJson(out, "inspect_in_explore", {true, "ALLOWED"});
    out << "},\"issues\":";
    appendWarningsJson(out, issues);
    out << "}\n";
    return 0;
}

// ===========================================================================
// Transactional Work file actions, atomic Remove, and recovery.
//
// All new Work mutation paths share a project-local transaction contract: a
// schema-versioned journal + backups under .isvn_local, a deterministic plan
// hash revalidated at apply, all-or-none application with rollback, and
// explicit crash recovery. QML is never allowed to perform the
// filesystem/SVN work; these commands do it, revalidating at execution time.
// ===========================================================================

void appendWorkErrorJson(std::ostream& out, const WorkError& error) {
    out << "{\"schema_version\":\"work-error/v1\",\"ok\":false,\"operation_id\":";
    appendJsonString(out, error.operation_id);
    out << ",\"error\":{\"code\":";
    appendJsonString(out, error.code);
    out << ",\"reason_params\":{},\"retryable\":" << (error.retryable ? "true" : "false")
        << ",\"recovery_required\":" << (error.recovery_required ? "true" : "false")
        << "}}\n";
}

// Maintenance is project-owner authority, not a QML presentation choice.  A
// MEMBER may issue proven read-only diagnostics, but every GC/reclaim/repair
// mutation is denied again at the CLI boundary.  The opt-in test variable can
// only reduce authority and exists to exercise the direct-CLI rejection path.
void requirePmMaintenance(const Runtime& runtime) {
    struct stat status {};
    const bool owner = ::stat((runtime.project_dir / ".isvn").c_str(), &status) == 0 &&
                       status.st_uid == ::geteuid();
    const bool forced_member = [] {
        const char* value = std::getenv("ISVN_TEST_FORCE_MEMBER_MAINTENANCE");
        return value != nullptr && std::string(value) == "1";
    }();
    if (!owner || forced_member) throw WorkError{"AUTHORIZATION_DENIED", false, false, ""};
}

// Deterministic test-only fault injection. ISVN_WORK_FAULT=<point> throws (the
// operation rolls back); ISVN_WORK_CRASH=<point> aborts abruptly leaving the
// journal for recovery. Both are inert unless the env var matches the point.
void workFault(const std::string& point) {
    if (const char* crash = std::getenv("ISVN_WORK_CRASH"); crash != nullptr && point == crash) {
        std::_Exit(137);
    }
    if (const char* fault = std::getenv("ISVN_WORK_FAULT"); fault != nullptr && point == fault) {
        throw std::runtime_error("injected fault at " + point);
    }
}

// Deterministic test barrier for exercising overlapping operations of several
// processes and SVN attribution overlap.  It is opt-in through an exact test-only
// environment pair, bounded to five seconds, and never changes production
// control flow when unset.
void workTestPause(const std::string& point) {
    const char* expected = std::getenv("ISVN_WORK_TEST_PAUSE_AT");
    const char* directory = std::getenv("ISVN_WORK_TEST_PAUSE_DIR");
    if (expected == nullptr || directory == nullptr || point != expected) return;
    std::error_code ec;
    const std::filesystem::path root(directory);
    std::filesystem::create_directories(root, ec);
    if (ec) return;
    {
        std::ofstream ready(root / "ready", std::ios::binary | std::ios::trunc);
        ready << "ready";
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(root / "continue", ec) &&
           std::chrono::steady_clock::now() < deadline) {
        ::usleep(5 * 1000);
    }
}

// Narrow deterministic synchronization used only by the command fault tests.
// It is inert unless both test-only environment variables are set, does not
// accept a source or destination path, and has a short monotonic timeout.
void importTestPause(const std::string& point) {
    const char* expected = std::getenv("ISVN_IMPORT_TEST_PAUSE_AT");
    const char* directory = std::getenv("ISVN_IMPORT_TEST_PAUSE_DIR");
    if (expected == nullptr || directory == nullptr || point != expected) return;
    std::error_code ec;
    const std::filesystem::path marker_root(directory);
    std::filesystem::create_directories(marker_root, ec);
    if (ec) return;
    {
        std::ofstream ready(marker_root / "ready", std::ios::binary | std::ios::trunc);
        ready << "ready";
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::filesystem::exists(marker_root / "continue", ec) &&
           std::chrono::steady_clock::now() < deadline) {
        ::usleep(5 * 1000);
    }
}

std::filesystem::path workOpsRoot(const std::filesystem::path& workspace) {
    return localStatePath(workspace, "work_operations");
}

constexpr std::size_t kMaxPendingOperationJournals = 256;
// Held open by the process that owns an operation journal; see OperationLiveness.
constexpr const char* kOperationOwnerMarker = "running";

// ---- operation liveness ----------------------------------------------------
//
// Mutations are not serialized against each other: members do not change the
// same Version at the same time, and Subversion and the pool protect their own
// state.  What remains is crash recovery.  A journal that is still on disk is
// either interrupted or simply not finished yet, and recovery must never roll
// back the second kind.  The process that owns a journal therefore holds an
// advisory flock on a file inside its private journal directory for as long as
// it runs; the kernel drops it when the process ends, also after a crash.
// Nothing waits for this token and no operation is refused because another
// operation holds one.
class OperationLiveness {
public:
    OperationLiveness() = default;
    ~OperationLiveness() { release(); }
    OperationLiveness(const OperationLiveness&) = delete;
    OperationLiveness& operator=(const OperationLiveness&) = delete;
    OperationLiveness(OperationLiveness&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    OperationLiveness& operator=(OperationLiveness&& other) noexcept {
        if (this != &other) {
            release();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Empty when another running process already owns the journal.
    static std::optional<OperationLiveness> tryClaim(const std::filesystem::path& journal_dir) {
        const int fd = ::open((journal_dir / kOperationOwnerMarker).c_str(),
                              O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw WorkError{"STATE_INCONSISTENT", false, false, ""};
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int saved = errno;
            ::close(fd);
            if (saved == EWOULDBLOCK || saved == EAGAIN) return std::nullopt;
            throw WorkError{"STATE_INCONSISTENT", false, false, ""};
        }
        OperationLiveness token;
        token.fd_ = fd;
        return token;
    }

private:
    void release() noexcept {
        if (fd_ >= 0) {
            (void)::flock(fd_, LOCK_UN);
            (void)::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

// Removes an operation journal directory.  The journal itself goes first, so that a
// cleanup that stops halfway never leaves a journal without its running owner.
void removeOperationJournal(const std::filesystem::path& journal_dir, std::error_code& ec) {
    std::filesystem::remove(journal_dir / "journal.json", ec);
    std::filesystem::remove_all(journal_dir, ec);
}

// Creates the private journal directory of a new operation and marks this
// process as its owner.
OperationLiveness claimNewOperationJournal(const std::filesystem::path& journal_dir) {
    std::error_code ec;
    std::filesystem::create_directories(journal_dir, ec);
    if (ec) throw WorkError{"STATE_INCONSISTENT", false, false, ""};
    ::chmod(journal_dir.c_str(), 0700);
    auto token = OperationLiveness::tryClaim(journal_dir);
    if (!token.has_value()) throw WorkError{"STATE_INCONSISTENT", false, false, ""};
    return std::move(*token);
}

// True while the process that owns the journal in `journal_dir` is running.  A
// journal without a token was left by an interrupted operation.
bool operationOwnerIsLive(const std::filesystem::path& journal_dir) {
    const int fd = ::open((journal_dir / kOperationOwnerMarker).c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    // A shared probe conflicts only with the owner's exclusive lock, not with other probes.
    const bool live = ::flock(fd, LOCK_SH | LOCK_NB) != 0;
    if (!live) (void)::flock(fd, LOCK_UN);
    ::close(fd);
    return live;
}

// A monotonic-ish operation id: time + pid. Deterministic enough for uniqueness
// within a workspace; never used as a security token.
// 128 bits of unpredictable uniqueness from the OS CSPRNG (/dev/urandom). Not a
// PID/timestamp/counter; used as the multi-host SVN attribution key.
std::string secureRandomHex(std::size_t bytes) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    std::vector<unsigned char> buffer(bytes);
    if (!urandom.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(bytes))) {
        throw std::runtime_error("unable to read /dev/urandom for operation id");
    }
    std::ostringstream out;
    for (const unsigned char c : buffer) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return out.str();
}

std::string makeOperationId(const std::string& op) {
    return "op-" + op + "-" + secureRandomHex(16);  // 128-bit random suffix
}

// A well-formed operation id: op-<type>-<32 lowercase hex>.
bool isValidOperationId(const std::string& id) {
    const auto pos = id.rfind('-');
    if (id.rfind("op-", 0) != 0 || pos == std::string::npos) return false;
    const std::string suffix = id.substr(pos + 1);
    if (suffix.size() != 32) return false;
    return suffix.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// The canonical trailer written once at the end of a Remove commit message.
std::string operationTrailer(const std::string& op_id) {
    return "\n\n[isvn-op:" + op_id + "]";
}

// Parse the operation id from the EXACT end-of-message trailer `[isvn-op:<id>]`
// (only whitespace may follow). Not a substring search.
std::optional<std::string> parseOperationTrailer(const std::string& message) {
    const std::string open = "[isvn-op:";
    const auto pos = message.rfind(open);
    if (pos == std::string::npos) return std::nullopt;
    const auto close = message.find(']', pos + open.size());
    if (close == std::string::npos) return std::nullopt;
    for (std::size_t i = close + 1; i < message.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(message[i])) == 0) return std::nullopt;
    }
    return message.substr(pos + open.size(), close - (pos + open.size()));
}

// The SVN commit author expected for this operation. For the file:// repository
// deployment the commit author is the process OS user (verified live: svn logs
// the OS username). Returns empty if it cannot be determined.
std::string expectedSvnAuthor() {
    if (const passwd* pw = ::getpwuid(::geteuid())) {
        return pw->pw_name != nullptr ? std::string(pw->pw_name) : std::string();
    }
    return {};
}

// Repository-absolute path of a version-relative item, matching the paths that
// `svn log --verbose` reports (e.g. "/branches/T/B/RTL/v1/keep.v").
std::string repoAbsolutePath(const Runtime& runtime, const std::string& version,
                             const std::string& relative) {
    const std::string url = runtime.svn.versionToUrl(version);
    const std::string root = runtime.config.repo_url;
    std::string prefix = url.rfind(root, 0) == 0 ? url.substr(root.size()) : url;
    if (!prefix.empty() && prefix.front() != '/') prefix = "/" + prefix;
    return prefix + "/" + relative;
}

std::string projectIdentityHash(const Runtime& runtime) {
    return common::blake3_hex(std::filesystem::absolute(runtime.project_dir).string());
}

// ---- plan model -----------------------------------------------------------

struct PlanItem {
    std::string path;          // working-copy-relative
    std::string svn_state;
    std::string link_state;    // LINKED | LOCAL | ""
    std::string source_kind;   // mirror of link_state
    std::string ui_state;
    bool exists = false;
    std::string kind = "file";
    std::string fingerprint;   // content/link fingerprint
    bool eligible = false;
    std::string reason_code = "ALLOWED";
    bool destructive = false;
    std::string before_source_kind;
    std::string after_source_kind;
};

// Validate a selector is a safe, contained, project-relative path.
void validateSelector(const std::string& selector) {
    if (selector.empty()) throw WorkError{"OUTSIDE_WORKSPACE", false, false, ""};
    if (selector.front() == '/') throw WorkError{"OUTSIDE_WORKSPACE", false, false, ""};
    if (selector.find('\0') != std::string::npos) throw WorkError{"OUTSIDE_WORKSPACE", false, false, ""};
    for (const auto& part : std::filesystem::path(selector)) {
        if (part == "..") throw WorkError{"OUTSIDE_WORKSPACE", false, false, ""};
    }
}

std::string itemFingerprint(const std::filesystem::path& path,
                            const std::filesystem::path& pool,
                            const std::string& link_state) {
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec)) {
        if (const auto hash = poolHashFromSymlinkIfAny(path, pool)) return "link:" + *hash;
        return "symlink:" + std::filesystem::read_symlink(path, ec).string();
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        return "file:" + common::blake3_file_hex(path);
    }
    (void)link_state;
    return "absent";
}

// Build the plan for a local operation or removal over the expanded selection.
struct WorkPlan {
    std::string op;
    std::string version;
    std::string plan_hash;
    std::vector<PlanItem> items;
    std::int64_t backup_bytes = 0;
    bool ready = false;
    bool local_discard_required = false;
    int local_discard_count = 0;
    std::vector<std::pair<std::string, std::string>> issues;
};

std::string computePlanHash(const Runtime& runtime, const CurrentVersion& current,
                            const std::string& op, const std::vector<PlanItem>& items) {
    // The plan hash covers project/workspace/version/op identity and the sorted
    // item content/state. Authorization flags (e.g. discard acknowledgment) are
    // NOT plan content and must not perturb the hash.
    std::ostringstream serialized;
    serialized << projectIdentityHash(runtime) << '\n'
               << stableWorkspaceId(current.version) << '\n'
               << current.version << '\n'
               << op << '\n';
    for (const auto& item : items) {
        serialized << item.path << '|' << item.svn_state << '|' << item.link_state << '|'
                   << (item.exists ? '1' : '0') << '|' << item.kind << '|' << item.fingerprint
                   << '\n';
    }
    return common::blake3_hex(serialized.str());
}

WorkPlan buildWorkPlan(const Runtime& runtime, const CurrentVersion& current,
                       const std::string& op, const std::vector<std::string>& selectors) {
    for (const auto& selector : selectors) validateSelector(selector);

    const auto local = readWorkspaceState(runtime.workspace);
    bool svn_available = false;
    std::vector<std::pair<std::string, std::string>> entry_issues;
    const auto entries = buildWorkFileEntries(runtime, current, local, svn_available, entry_issues);
    std::map<std::string, const WorkFileEntry*> by_path;
    for (const auto& entry : entries) {
        if (entry.kind == "file") by_path[entry.path] = &entry;
    }

    // Expand selectors: a folder selector matches every file beneath it.
    std::set<std::string> expanded;
    for (const auto& selector : selectors) {
        const std::string norm =
            std::filesystem::path(selector).lexically_normal().generic_string();
        if (by_path.count(norm) != 0) {
            expanded.insert(norm);
            continue;
        }
        const std::string prefix = norm + "/";
        bool matched = false;
        for (const auto& [path, entry] : by_path) {
            if (path.rfind(prefix, 0) == 0) {
                expanded.insert(path);
                matched = true;
            }
        }
        if (!matched) {
            // A selector that matches nothing on disk is reported per-item below.
            expanded.insert(norm);
        }
    }

    WorkPlan plan;
    plan.op = op;
    plan.version = current.version;
    bool has_managed = false;
    bool has_new_local = false;
    for (const auto& path : expanded) {  // std::set → deterministic order
        PlanItem item;
        item.path = path;
        const auto abs = current.working_copy / path;
        std::error_code ec;
        item.exists = std::filesystem::exists(abs, ec);
        const auto it = by_path.find(path);
        if (it != by_path.end()) {
            item.svn_state = it->second->svn_state;
            item.link_state = it->second->link_state;
            item.source_kind = it->second->link_state;
            item.ui_state = it->second->ui_state;
        }
        item.fingerprint = itemFingerprint(abs, runtime.config.pool_root, item.link_state);
        const bool new_local = item.link_state == "LOCAL" &&
                               (item.svn_state == "?" || item.svn_state == "A");
        const bool managed = !new_local && (item.link_state == "LINKED" ||
                                            item.svn_state == "M" || item.svn_state == "!" ||
                                            item.svn_state == "D" || item.svn_state == "C" ||
                                            item.svn_state.empty() ||
                                            item.svn_state == "normal");
        if (new_local) has_new_local = true;
        if (managed) has_managed = true;

        item.before_source_kind = item.link_state;
        if (op == "make-editable") {
            item.after_source_kind = "LOCAL";
            if (!item.exists) { item.eligible = false; item.reason_code = "MISSING_ITEM"; }
            else if (item.link_state == "LINKED") { item.eligible = true; item.reason_code = "ALLOWED"; }
            else if (item.link_state == "LOCAL") { item.eligible = false; item.reason_code = "ALREADY_LOCAL"; }
            else { item.eligible = false; item.reason_code = "UNMANAGED_ITEM"; }
        } else if (op == "revert") {
            item.destructive = true;
            item.after_source_kind = "LINKED";
            if (new_local) { item.eligible = false; item.reason_code = "NO_LOCAL_CHANGE"; }
            else if (item.svn_state == "M" || item.svn_state == "!" || item.svn_state == "D" ||
                     item.ui_state == "editable") { item.eligible = true; item.reason_code = "ALLOWED"; }
            else { item.eligible = false; item.reason_code = "NO_LOCAL_CHANGE"; }
        } else if (op == "discard-local") {
            item.destructive = true;
            item.after_source_kind = "";
            if (new_local) { item.eligible = true; item.reason_code = "ALLOWED"; }
            else { item.eligible = false; item.reason_code = "NOT_NEW_LOCAL"; }
        } else if (op == "remove-batch") {
            item.destructive = true;
            if (new_local) { item.eligible = false; item.reason_code = "USE_DISCARD_LOCAL"; }
            else if (!item.exists && item.svn_state.empty()) {
                item.eligible = false; item.reason_code = "STATE_INCONSISTENT";
            } else if (item.svn_state == "C") { item.eligible = false; item.reason_code = "CONFLICT_PRESENT"; }
            else { item.eligible = true; item.reason_code = "ALLOWED"; }
            if (item.eligible && item.svn_state == "M") {
                plan.local_discard_required = true;
                plan.local_discard_count += 1;
            }
        }

        if (item.destructive && item.eligible) {
            if (std::filesystem::is_regular_file(abs, ec) &&
                !std::filesystem::is_symlink(abs, ec)) {
                plan.backup_bytes += static_cast<std::int64_t>(std::filesystem::file_size(abs, ec));
            }
        }
        plan.items.push_back(std::move(item));
    }

    // Whole-selection eligibility rules.
    if (op == "discard-local" && has_managed) {
        plan.ready = false;
        plan.issues.emplace_back("MIXED_SELECTION", "selection mixes managed and NEW LOCAL");
    } else if (op == "remove-batch" && has_new_local && has_managed) {
        plan.ready = false;
        plan.issues.emplace_back("MIXED_SELECTION", "selection mixes managed and NEW LOCAL");
    } else if (op == "remove-batch" && has_new_local && !has_managed) {
        plan.ready = false;
        plan.issues.emplace_back("USE_DISCARD_LOCAL", "NEW LOCAL content uses Discard Local");
    } else {
        plan.ready = !plan.items.empty() &&
                     std::all_of(plan.items.begin(), plan.items.end(),
                                 [](const PlanItem& i) { return i.eligible; });
    }
    plan.plan_hash = computePlanHash(runtime, current, op, plan.items);
    return plan;
}

// ---- journal --------------------------------------------------------------

std::filesystem::path journalDir(const Runtime& runtime, const std::string& op_id) {
    return workOpsRoot(runtime.workspace) / op_id;
}

std::string journalScalar(const std::string& content, const std::string& key) {
    const auto marker = "\"" + key + "\":";
    auto pos = content.find(marker);
    if (pos == std::string::npos) return "";
    pos += marker.size();
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos])) != 0) ++pos;
    if (pos >= content.size()) return "";
    if (content[pos] == '"') {
        const auto end = content.find('"', pos + 1);
        return end == std::string::npos ? "" : content.substr(pos + 1, end - pos - 1);
    }
    const auto end = content.find_first_of(",\n", pos);
    return content.substr(pos, end - pos);
}

struct PendingJournal {
    std::string operation_id;
    std::string operation_type;
    std::string target_version;
    std::string schema;
    std::filesystem::path directory;
};

std::vector<PendingJournal> discoverPendingJournals(const Runtime& runtime) {
    std::vector<PendingJournal> result;
    std::error_code ec;
    const auto root = workOpsRoot(runtime.workspace);
    if (!std::filesystem::is_directory(root, ec)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        const auto journal = entry.path() / "journal.json";
        if (!std::filesystem::exists(journal, ec) || ec) continue;
        const auto content = readTextFile(journal);
        const auto id = journalScalar(content, "operation_id");
        const auto op = journalScalar(content, "operation_type");
        const auto version = journalScalar(content, "target_version");
        const auto schema = journalScalar(content, "schema_version");
        if (!isValidOperationId(id) || op.empty() || version.empty() || schema.empty()) {
            throw WorkError{"OPERATION_REGISTRY_CORRUPT", false, true, ""};
        }
        result.push_back(PendingJournal{id, op, version, schema, entry.path()});
        // A private journal root is still durable input after a crash.  Keep
        // discovery bounded so corrupted/unexpected state cannot turn every
        // mutation into an unbounded directory walk.
        if (result.size() > kMaxPendingOperationJournals) {
            throw WorkError{"OPERATION_REGISTRY_CORRUPT", false, true, ""};
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.operation_id < rhs.operation_id;
    });
    return result;
}

void enforceJournalAdmission(const Runtime& runtime, const std::string& target_version) {
    for (const auto& pending : discoverPendingJournals(runtime)) {
        // A journal whose owner is running is unfinished work, not a recovery case.
        if (operationOwnerIsLive(pending.directory)) continue;
        // Any historical record without immutable v5 target pins remains a
        // deliberate Project-wide recovery barrier.  It is never guessed into
        // a Version scope merely because its old target text looks plausible.
        if (pending.schema != "work-journal/v5") {
            throw WorkError{"PROJECT_LEGACY_RECOVERY_REQUIRED", false, true,
                            pending.operation_id};
        }
        if (pending.target_version == target_version) {
            throw WorkError{"VERSION_RECOVERY_REQUIRED", false, true, pending.operation_id};
        }
    }
}

struct TargetPin {
    std::string version;
    std::filesystem::path root;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::string target_url;
};

TargetPin captureTargetPin(const Runtime& runtime, const std::string& version) {
    validateVersionId(version);
    const auto root = std::filesystem::absolute(mappedWorkingCopy(runtime, version)).lexically_normal();
    struct stat st {};
    if (::lstat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, ""};
    }
    const int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, ""};
    struct stat opened {};
    const int status = ::fstat(fd, &opened);
    ::close(fd);
    if (status != 0 || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, ""};
    }
    return TargetPin{version, root, static_cast<std::uintmax_t>(st.st_dev),
                     static_cast<std::uintmax_t>(st.st_ino), runtime.svn.versionToUrl(version)};
}

bool targetPinMatches(const TargetPin& pin) {
    struct stat st {};
    if (::lstat(pin.root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) return false;
    const int fd = ::open(pin.root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat opened {};
    const int status = ::fstat(fd, &opened);
    ::close(fd);
    return status == 0 && static_cast<std::uintmax_t>(st.st_dev) == pin.device &&
           static_cast<std::uintmax_t>(st.st_ino) == pin.inode &&
           opened.st_dev == st.st_dev && opened.st_ino == st.st_ino;
}

CurrentVersion currentFromPinnedJournal(const Runtime& runtime, const std::string& content,
                                        const std::string& op_id) {
    const auto version = jsonUnescape(journalScalar(content, "target_version"));
    const auto root = jsonUnescape(journalScalar(content, "target_root"));
    const auto dev = journalScalar(content, "target_root_device");
    const auto ino = journalScalar(content, "target_root_inode");
    const auto target_url = jsonUnescape(journalScalar(content, "target_url"));
    if (version.empty() || root.empty() || dev.empty() || ino.empty() || target_url.empty()) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    TargetPin pin;
    try {
        validateVersionId(version);
        pin = TargetPin{version, std::filesystem::path(root).lexically_normal(),
                        static_cast<std::uintmax_t>(std::stoull(dev)),
                        static_cast<std::uintmax_t>(std::stoull(ino)), target_url};
    } catch (const std::exception&) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    if (!pin.root.is_absolute() || !targetPinMatches(pin) ||
        pin.target_url != runtime.svn.versionToUrl(pin.version)) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    // The current map can corroborate a modern pin but never silently replaces
    // it.  A changed map is Manual Review for this target, not a redirect.
    try {
        const auto mapped = std::filesystem::absolute(mappedWorkingCopy(runtime, pin.version)).lexically_normal();
        if (mapped != pin.root) {
            throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
        }
    } catch (const WorkError&) {
        throw;
    } catch (const std::exception&) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    try {
        // Recovery target corroboration is a current authoritative SVN query,
        // but it must never reintroduce an unbounded runner path merely
        // because recovery is already blocked.  This exact one-root info call
        // shares the established bounded query policy.
        SvnQueryLimits limits;
        SvnQueryBudget budget;
        const auto info = runtime.svn.infoBounded(pin.root, limits, budget);
        if (info.url != pin.target_url) {
            throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
        }
    } catch (const WorkError&) {
        throw;
    } catch (const std::exception&) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    return CurrentVersion{pin.version, pin.root};
}

void mutateWorkspaceState(const Runtime& runtime,
                          const std::function<void(std::set<std::string>&)>& mutation) {
    mutateWorkspaceState(runtime.workspace, mutation);
}

void mergeLocalOperationWorkspaceState(const Runtime& runtime, const CurrentVersion& current,
                                       const std::string& op, const WorkPlan& plan,
                                       bool restore_original) {
    mutateWorkspaceState(runtime, [&](std::set<std::string>& state) {
        for (const auto& item : plan.items) {
            const auto rel = relativeString(runtime.workspace, current.working_copy / item.path);
            const bool original_local = item.link_state == "LOCAL";
            const bool final_local = op == "make-editable";
            const bool should_be_local = restore_original ? original_local : final_local;
            if (should_be_local) state.insert(rel);
            else state.erase(rel);
        }
    });
}

// Controlled-format JSON (one field per line) so recovery can read it back with
// simple, robust substring extraction without a full JSON parser.
void writeJournal(const Runtime& runtime, const std::string& op_id, const std::string& op,
                  const std::string& version, const std::string& state,
                  const std::string& plan_hash, const WorkPlan& plan,
                  const std::vector<bool>& applied, const std::string& message,
                  const std::string& op_trailer, const std::string& schema = "work-journal/v5",
                  const std::string& evidence_fields = "") {
    const auto dir = journalDir(runtime, op_id);
    std::error_code ec;
    std::filesystem::create_directories(dir / "backup", ec);
    ::chmod(dir.c_str(), 0700);
    std::ostringstream out;
    out << "{\n";
    out << "\"schema_version\": \"" << jsonEscape(schema) << "\",\n";
    out << "\"operation_id\": \"" << jsonEscape(op_id) << "\",\n";
    out << "\"operation_type\": \"" << jsonEscape(op) << "\",\n";
    out << "\"project_hash\": \"" << jsonEscape(projectIdentityHash(runtime)) << "\",\n";
    out << "\"workspace_id\": \"" << jsonEscape(stableWorkspaceId(version)) << "\",\n";
    out << "\"target_version\": \"" << jsonEscape(version) << "\",\n";
    const auto existing = dir / "journal.json";
    std::optional<TargetPin> prior_pin;
    if (std::filesystem::exists(existing, ec)) {
        const auto prior = readTextFile(existing);
        const auto root = journalScalar(prior, "target_root");
        const auto dev = journalScalar(prior, "target_root_device");
        const auto ino = journalScalar(prior, "target_root_inode");
        const auto url = jsonUnescape(journalScalar(prior, "target_url"));
        if (!root.empty() && !dev.empty() && !ino.empty() && !url.empty()) {
            try {
                prior_pin = TargetPin{version, std::filesystem::path(jsonUnescape(root)),
                                      static_cast<std::uintmax_t>(std::stoull(dev)),
                                      static_cast<std::uintmax_t>(std::stoull(ino)), url};
            } catch (const std::exception&) {
                throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
            }
        }
    }
    // Once a mutation has started, journal updates must never resolve the
    // mutable workspace map/Active hint again.  The first PREPARED write pins
    // the target; later writes merely prove that exact pinned root still is
    // present, otherwise they stop without retargeting anything.
    TargetPin pin = prior_pin.has_value() ? *prior_pin : captureTargetPin(runtime, version);
    if (pin.version != version || pin.target_url != runtime.svn.versionToUrl(version)) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    if (!targetPinMatches(pin)) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    out << "\"lock_scope\": \"VERSION\",\n";
    out << "\"target_root\": \"" << jsonEscape(pin.root.string()) << "\",\n";
    out << "\"target_root_device\": " << pin.device << ",\n";
    out << "\"target_root_inode\": " << pin.inode << ",\n";
    out << "\"target_url\": \"" << jsonEscape(pin.target_url) << "\",\n";
    out << "\"state\": \"" << jsonEscape(state) << "\",\n";
    out << "\"plan_hash\": \"" << jsonEscape(plan_hash) << "\",\n";
    out << "\"message\": \"" << jsonEscape(message) << "\",\n";
    out << "\"op_trailer\": \"" << jsonEscape(op_trailer) << "\",\n";
    if (!evidence_fields.empty()) out << evidence_fields;  // v2 attribution evidence
    out << "\"item_count\": " << plan.items.size() << ",\n";
    out << "\"items\": [\n";
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const auto& item = plan.items[i];
        out << "{\"index\": " << i << ", \"path\": \"" << jsonEscape(item.path)
            << "\", \"link_state\": \"" << jsonEscape(item.link_state)
            << "\", \"svn_state\": \"" << jsonEscape(item.svn_state)
            << "\", \"fingerprint\": \"" << jsonEscape(item.fingerprint)
            << "\", \"applied\": " << (i < applied.size() && applied[i] ? "true" : "false")
            << "}" << (i + 1 < plan.items.size() ? "," : "") << "\n";
    }
    out << "]\n}\n";
    const auto tmp = dir / "journal.json.tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file << out.str();
        file.flush();
    }
    std::filesystem::rename(tmp, dir / "journal.json");
    common::fsync_directory(dir);
}

// ---- editable-provenance receipts ------------------------------------------
//
// A managed repository path may be checked in as an UPDATE only when it carries
// durable, backend-verifiable provenance from a controlled Make Editable. The
// receipt is project-local (never in SVN/Git), written atomically only after the
// Make Editable transaction succeeds, and cleared on verified Revert / Remove /
// Checkin. Repository existence or "the file is LOCAL" is NOT provenance.

struct EditableReceipt {
    std::string schema;
    std::string project_hash;
    std::string workspace_id;
    std::string version;
    std::string path;
    std::string make_editable_op_id;
    std::string original_fingerprint;  // managed object identity when made editable
    bool valid = false;
};

std::filesystem::path editableReceiptsRoot(const std::filesystem::path& workspace) {
    return localStatePath(workspace, "editable_receipts");
}

// Content-addressed receipt filename bound to (version, project-relative path).
std::string editableReceiptKey(const std::string& version, const std::string& relpath) {
    return common::blake3_hex(version + "\n" + relpath);
}

std::string isoNow() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    ::gmtime_r(&now, &tm_buf);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buffer;
}

void writeEditableReceipt(const Runtime& runtime, const std::string& version,
                          const std::string& relpath, const std::string& op_id,
                          const std::string& original_fingerprint) {
    const auto root = editableReceiptsRoot(runtime.workspace);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    ::chmod(root.c_str(), 0700);
    const auto file = root / (editableReceiptKey(version, relpath) + ".json");
    std::ostringstream out;
    out << "{\n"
        << "\"schema_version\": \"editable-receipt/v1\",\n"
        << "\"project_hash\": \"" << jsonEscape(projectIdentityHash(runtime)) << "\",\n"
        << "\"workspace_id\": \"" << jsonEscape(stableWorkspaceId(version)) << "\",\n"
        << "\"target_version\": \"" << jsonEscape(version) << "\",\n"
        << "\"path\": \"" << jsonEscape(relpath) << "\",\n"
        << "\"make_editable_op_id\": \"" << jsonEscape(op_id) << "\",\n"
        << "\"original_fingerprint\": \"" << jsonEscape(original_fingerprint) << "\",\n"
        << "\"created_at\": \"" << jsonEscape(isoNow()) << "\"\n}\n";
    const auto tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << out.str();
        f.flush();
    }
    std::filesystem::rename(tmp, file);
    ::chmod(file.c_str(), 0600);
    common::fsync_directory(root);
}

// Read + validate. Any malformed/unknown/mismatched receipt fails closed
// (valid=false); a missing receipt returns nullopt.
std::optional<EditableReceipt> readEditableReceipt(const Runtime& runtime,
                                                   const std::string& version,
                                                   const std::string& relpath) {
    const auto file = editableReceiptsRoot(runtime.workspace) /
                      (editableReceiptKey(version, relpath) + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;
    const auto content = readTextFile(file);
    EditableReceipt r;
    r.schema = jsonUnescape(journalScalar(content, "schema_version"));
    r.project_hash = jsonUnescape(journalScalar(content, "project_hash"));
    r.workspace_id = jsonUnescape(journalScalar(content, "workspace_id"));
    r.version = jsonUnescape(journalScalar(content, "target_version"));
    r.path = jsonUnescape(journalScalar(content, "path"));
    r.make_editable_op_id = jsonUnescape(journalScalar(content, "make_editable_op_id"));
    r.original_fingerprint = jsonUnescape(journalScalar(content, "original_fingerprint"));
    r.valid = r.schema == "editable-receipt/v1" &&
              r.project_hash == projectIdentityHash(runtime) &&
              r.workspace_id == stableWorkspaceId(version) && r.version == version &&
              r.path == relpath && isValidOperationId(r.make_editable_op_id) &&
              !r.original_fingerprint.empty();
    return r;
}

void clearEditableReceipt(const Runtime& runtime, const std::string& version,
                          const std::string& relpath) {
    std::error_code ec;
    std::filesystem::remove(
        editableReceiptsRoot(runtime.workspace) / (editableReceiptKey(version, relpath) + ".json"),
        ec);
}

// ---- local-operation apply engine -----------------------------------------

void backupItemContent(const std::filesystem::path& dir, std::size_t index,
                       const std::filesystem::path& abs) {
    std::error_code ec;
    const auto backup = dir / "backup" / std::to_string(index);
    if (std::filesystem::is_symlink(abs, ec)) {
        std::ofstream link_backup(backup.string() + ".link", std::ios::binary | std::ios::trunc);
        link_backup << std::filesystem::read_symlink(abs, ec).string();
    } else if (std::filesystem::is_regular_file(abs, ec)) {
        std::filesystem::copy_file(abs, backup,
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }
}

void restoreItemFromBackup(Runtime& runtime, const std::string& op,
                           const std::filesystem::path& dir, std::size_t index,
                           const std::filesystem::path& abs, const std::string& link_state,
                           std::set<std::string>& state) {
    std::error_code ec;
    const auto backup = dir / "backup" / std::to_string(index);
    const auto rel = relativeString(runtime.workspace, abs);
    // Clear whatever is currently there.
    try { runtime.store.unregister_ref(abs.string()); } catch (...) {}
    std::filesystem::remove(abs, ec);
    if (op == "make-editable") {
        // Restore the original LINKED symlink + reference.
        std::ifstream link_backup(backup.string() + ".link", std::ios::binary);
        std::string target((std::istreambuf_iterator<char>(link_backup)),
                           std::istreambuf_iterator<char>());
        if (!target.empty()) {
            std::filesystem::create_symlink(target, abs);
            if (const auto hash = poolHashFromSymlinkIfAny(abs, runtime.config.pool_root)) {
                try { runtime.store.register_ref(*hash, abs.string()); } catch (...) {}
            }
        }
        state.erase(rel);  // pre-op LINKED files are not LOCAL
    } else {
        // revert / discard-local: restore the backed-up LOCAL content.
        if (std::filesystem::exists(backup, ec)) {
            std::filesystem::copy_file(backup, abs,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::permissions(abs, std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::add, ec);
            state.insert(rel);
        }
    }
    (void)link_state;
}

void applyLocalItem(Runtime& runtime, const std::string& op, const std::filesystem::path& abs,
                    std::set<std::string>& state) {
    std::error_code ec;
    const auto rel = relativeString(runtime.workspace, abs);
    if (op == "make-editable") {
        const auto target = std::filesystem::read_symlink(abs, ec);
        const auto resolved = target.is_absolute() ? target : abs.parent_path() / target;
        const auto temp = abs.string() + ".local." + std::to_string(::getpid());
        std::filesystem::copy_file(resolved, temp,
                                   std::filesystem::copy_options::overwrite_existing);
        try { runtime.store.unregister_ref(abs.string()); } catch (...) {}
        std::filesystem::remove(abs);
        std::filesystem::rename(temp, abs);
        std::filesystem::permissions(abs, std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ec);
        state.insert(rel);
    } else if (op == "revert") {
        try { runtime.store.unregister_ref(abs.string()); } catch (...) {}
        if (std::filesystem::is_symlink(abs, ec)) {
            std::filesystem::remove(abs);
            runtime.svn.revert(abs);
        } else {
            runtime.svn.revert(abs);
        }
        if (std::filesystem::is_regular_file(abs, ec) && !std::filesystem::is_symlink(abs, ec)) {
            relinkFile(runtime.store, abs);
        }
        state.erase(rel);
    } else if (op == "discard-local") {
        std::filesystem::remove(abs, ec);
        state.erase(rel);
    }
}

// Preflight same-filesystem backup space; refuse to start if a recoverable
// backup cannot be created.
void preflightBackupSpace(const Runtime& runtime, std::int64_t backup_bytes) {
    std::error_code ec;
    const auto space = std::filesystem::space(workOpsRoot(runtime.workspace), ec);
    if (ec) return;
    if (space.available < static_cast<std::uintmax_t>(backup_bytes) * 2 + 4096) {
        throw WorkError{"INSUFFICIENT_BACKUP_SPACE", false, false, ""};
    }
}

// Emit the work-operation-result/v1 success/failed shape.
void appendOperationResult(std::ostream& out, const std::string& op_id, const std::string& op,
                           const std::string& status, const std::string& version,
                           const std::vector<PlanItem>& items, bool recovery_required) {
    out << "{\"schema_version\":\"work-operation-result/v1\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"operation\":";
    appendJsonString(out, op);
    out << ",\"status\":";
    appendJsonString(out, status);
    out << ",\"target_version\":";
    appendJsonString(out, version);
    out << ",\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out << ",";
        out << "{\"path\":";
        appendJsonString(out, items[i].path);
        out << ",\"status\":";
        appendJsonString(out, status == "succeeded" ? "succeeded" : "rolled_back");
        out << "}";
    }
    out << "],\"svn_revision\":null,\"recovery_required\":" << (recovery_required ? "true" : "false")
        << "}\n";
}

int cmdWorkLocalOp(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("work-local-op does not accept --project; it is fixed-project");
    }
    if (args.size() < 2) throw std::invalid_argument("work-local-op requires preview|apply");
    const std::string phase = args[1];
    const auto action = optionValue(args, "--action");
    const auto version = optionValue(args, "--in");
    if (!action.has_value() || !version.has_value()) {
        throw std::invalid_argument("work-local-op requires --action and --in");
    }
    const std::string op = *action;
    if (op != "make-editable" && op != "revert" && op != "discard-local") {
        throw std::invalid_argument("unknown --action: " + op);
    }
    std::vector<std::string> selectors;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--path") selectors.push_back(args[i + 1]);
    }
    if (selectors.empty()) throw std::invalid_argument("work-local-op requires at least one --path");

    Runtime runtime = workspaceRuntime();
    CurrentVersion current;
    try {
        current = explicitVersionInWorkspace(runtime, *version);
    } catch (const std::exception&) {
        throw WorkError{"INVALID_VERSION", false, false, ""};
    }

    if (phase == "preview") {
        enforceJournalAdmission(runtime, current.version);
        const auto plan = buildWorkPlan(runtime, current, op, selectors);
        out << "{\"schema_version\":\"work-local-op/v1\",\"operation\":";
        appendJsonString(out, op);
        out << ",\"target_version\":";
        appendJsonString(out, current.version);
        out << ",\"plan_hash\":";
        appendJsonString(out, plan.plan_hash);
        out << ",\"items\":[";
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            const auto& item = plan.items[i];
            if (i != 0) out << ",";
            out << "{\"path\":";
            appendJsonString(out, item.path);
            out << ",\"eligible\":" << (item.eligible ? "true" : "false") << ",\"reason_code\":";
            appendJsonString(out, item.reason_code);
            out << ",\"before_source_kind\":";
            appendJsonString(out, item.before_source_kind);
            out << ",\"after_source_kind\":";
            appendJsonString(out, item.after_source_kind);
            out << ",\"destructive\":" << (item.destructive ? "true" : "false") << "}";
        }
        out << "],\"summary\":{\"selected\":" << selectors.size()
            << ",\"expanded\":" << plan.items.size() << ",\"backup_bytes\":" << plan.backup_bytes
            << "},\"ready\":" << (plan.ready ? "true" : "false") << ",\"issues\":";
        appendWarningsJson(out, plan.issues);
        out << "}\n";
        return 0;
    }

    if (phase != "apply") throw std::invalid_argument("work-local-op phase must be preview|apply");

    const auto expected_plan = optionValue(args, "--expected-plan");
    if (!expected_plan.has_value()) throw std::invalid_argument("apply requires --expected-plan");

    enforceJournalAdmission(runtime, current.version);
    const auto plan = buildWorkPlan(runtime, current, op, selectors);
    if (!plan.ready) {
        std::string code = "STATE_INCONSISTENT";
        if (!plan.issues.empty()) code = plan.issues.front().first;
        else {
            for (const auto& item : plan.items)
                if (!item.eligible) { code = item.reason_code; break; }
        }
        throw WorkError{code, false, false, ""};
    }
    if (plan.plan_hash != *expected_plan) throw WorkError{"PLAN_STALE", true, false, ""};
    preflightBackupSpace(runtime, plan.backup_bytes);

    const auto op_id = makeOperationId(op);
    const auto dir = journalDir(runtime, op_id);
    const OperationLiveness owner = claimNewOperationJournal(dir);
    std::vector<bool> applied(plan.items.size(), false);

    // Create the backup area, then back up every item, then the PREPARED journal.
    {
        std::error_code dec;
        std::filesystem::create_directories(dir / "backup", dec);
        ::chmod(dir.c_str(), 0700);
    }
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        backupItemContent(dir, i, current.working_copy / plan.items[i].path);
    }
    writeJournal(runtime, op_id, op, current.version, "PREPARED", plan.plan_hash, plan, applied,
                 "", "");
    workFault("journal_prepared");
    writeJournal(runtime, op_id, op, current.version, "APPLYING", plan.plan_hash, plan, applied,
                 "", "");
    workTestPause("local_before_apply");
    workFault("backup_created");

    std::set<std::string> state;
    try {
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            applyLocalItem(runtime, op, current.working_copy / plan.items[i].path, state);
            applied[i] = true;
            workFault("item_applied_" + std::to_string(i));
        }
        mergeLocalOperationWorkspaceState(runtime, current, op, plan, false);
    } catch (const std::exception&) {
        // Roll back every already-applied item, in reverse.
        writeJournal(runtime, op_id, op, current.version, "ROLLING_BACK", plan.plan_hash, plan,
                     applied, "", "");
        std::set<std::string> rollback_state;
        for (std::size_t i = plan.items.size(); i-- > 0;) {
            if (!applied[i]) continue;
            restoreItemFromBackup(runtime, op, dir, i,
                                  current.working_copy / plan.items[i].path,
                                  plan.items[i].link_state, rollback_state);
        }
        mergeLocalOperationWorkspaceState(runtime, current, op, plan, true);
        std::error_code ec;
        removeOperationJournal(dir, ec);
        appendOperationResult(out, op_id, op, "failed", current.version, plan.items, false);
        return 0;
    }

    writeJournal(runtime, op_id, op, current.version, "COMPLETED", plan.plan_hash, plan, applied,
                 "", "");
    // Editable-provenance receipts are mutated only after the whole local
    // transaction has succeeded, so a rollback never has to restore them.
    if (op == "make-editable") {
        for (const auto& item : plan.items) {
            writeEditableReceipt(runtime, current.version, item.path, op_id, item.fingerprint);
        }
    } else if (op == "revert") {
        for (const auto& item : plan.items) {
            clearEditableReceipt(runtime, current.version, item.path);
        }
    }
    std::error_code ec;
    removeOperationJournal(dir, ec);
    appendOperationResult(out, op_id, op, "succeeded", current.version, plan.items, false);
    return 0;
}

// ---- atomic Remove from Version -------------------------------------------

struct JournalItem {
    std::size_t index = 0;
    std::string path;
    std::string link_state;
};

// Parse a top-level JSON string array field (e.g. "expected_changed") from the
// controlled-format journal.
std::set<std::string> journalStringArray(const std::string& content, const std::string& key) {
    std::set<std::string> values;
    const auto marker = "\"" + key + "\":";
    auto pos = content.find(marker);
    if (pos == std::string::npos) return values;
    pos += marker.size();
    while (pos < content.size() && std::isspace(static_cast<unsigned char>(content[pos])) != 0) ++pos;
    if (pos >= content.size() || content[pos] != '[') return values;
    ++pos;
    const auto end = content.find(']', pos);
    if (end == std::string::npos) return values;
    const std::string body = content.substr(pos, end - pos);
    std::size_t q = 0;
    while ((q = body.find('"', q)) != std::string::npos) {
        const auto close = body.find('"', q + 1);
        if (close == std::string::npos) break;
        values.insert(body.substr(q + 1, close - q - 1));
        q = close + 1;
    }
    return values;
}

std::vector<JournalItem> parseJournalItems(const std::string& content) {
    std::vector<JournalItem> items;
    std::size_t pos = 0;
    while ((pos = content.find("{\"index\": ", pos)) != std::string::npos) {
        const auto end = content.find('}', pos);
        if (end == std::string::npos) break;
        const auto line = content.substr(pos, end - pos + 1);
        JournalItem item;
        item.index = static_cast<std::size_t>(std::stoul(journalScalar(line, "index")));
        item.path = journalScalar(line, "path");
        item.link_state = journalScalar(line, "link_state");
        items.push_back(item);
        pos = end + 1;
    }
    return items;
}

// Positive, typed attribution of a Remove commit by exact SVN-log
// evidence. Path existence is NEVER the primary proof (superseded).
enum class RemoveVerifyState {
    Committed,       // exactly one matching commit with exact evidence
    NotCommitted,    // no matching commit and target paths remain → safe rollback
    Mismatch,        // one candidate but author/message/target/changed-set differ
    Ambiguous,       // more than one candidate trailer
    Diverged,        // no candidate but paths already gone (another actor?)
    Insufficient     // author unavailable / log query/parse failed
};

struct RemoveEvidence {
    std::string op_id;
    std::string expected_author;
    std::string target_url;
    std::int64_t pre_commit_head = -1;
    std::string full_message;                 // exact committed message incl. trailer
    std::set<std::string> expected_changed;   // "<action>|<repo-absolute path>"
    std::vector<std::string> item_paths;      // version-relative, for existence fallback
};

struct RemoveVerifyResult {
    RemoveVerifyState state = RemoveVerifyState::Insufficient;
    std::int64_t revision = 0;
    std::string author;
    std::string code;  // stable reason code for non-Committed states
};

RemoveVerifyResult verifyRemoveOutcome(Runtime& runtime, const RemoveEvidence& ev) {
    RemoveVerifyResult result;
    if (ev.expected_author.empty()) {
        result.state = RemoveVerifyState::Insufficient;
        result.code = "EXPECTED_AUTHOR_UNAVAILABLE";
        return result;
    }
    std::int64_t head = 0;
    try {
        head = runtime.svn.headRevision(runtime.config.repo_url);
    } catch (const std::exception&) {
        result.code = "SVN_LOG_QUERY_FAILED";
        return result;
    }
    std::vector<version::SvnVerboseLogEntry> entries;
    const std::int64_t from = ev.pre_commit_head < 0 ? 0 : ev.pre_commit_head + 1;
    if (head >= from) {
        // Only query when new revisions exist since the frozen pre-commit HEAD.
        try {
            entries = runtime.svn.logVerbose(ev.target_url, from, head);
        } catch (const std::exception&) {
            result.code = "SVN_LOG_PARSE_FAILED";
            return result;
        }
    }

    std::vector<const version::SvnVerboseLogEntry*> candidates;
    for (const auto& entry : entries) {
        const auto trailer = parseOperationTrailer(entry.message);
        if (trailer.has_value() && *trailer == ev.op_id) candidates.push_back(&entry);
    }
    if (candidates.size() > 1) {
        result.state = RemoveVerifyState::Ambiguous;
        result.code = "OPERATION_EVIDENCE_AMBIGUOUS";
        return result;
    }
    if (candidates.empty()) {
        // No positive attribution. Use current path existence ONLY to distinguish
        // "safe local rollback" from "another actor may have changed the target".
        std::set<std::string> present;
        try {
            for (const auto& p : runtime.svn.list(ev.target_url, true)) {
                std::string trimmed = p;
                while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
                present.insert(trimmed);
            }
        } catch (const std::exception&) {
            result.code = "SVN_LOG_QUERY_FAILED";
            return result;
        }
        bool any_absent = false;
        for (const auto& rel : ev.item_paths) {
            if (present.count(rel) == 0) any_absent = true;
        }
        if (!any_absent) {
            result.state = RemoveVerifyState::NotCommitted;  // all still present → rollback
            return result;
        }
        result.state = RemoveVerifyState::Diverged;
        result.code = "REMOTE_STATE_DIVERGED";
        return result;
    }

    // Exactly one candidate: require ALL exact evidence to match.
    const auto& candidate = *candidates.front();
    std::set<std::string> actual;
    for (const auto& changed : candidate.paths) {
        actual.insert(changed.action + "|" + changed.path);
    }
    const bool ok = candidate.revision > ev.pre_commit_head &&
                    candidate.author == ev.expected_author &&
                    candidate.message == ev.full_message && actual == ev.expected_changed;
    if (!ok) {
        result.state = RemoveVerifyState::Mismatch;
        result.code = "OPERATION_EVIDENCE_MISMATCH";
        return result;
    }
    result.state = RemoveVerifyState::Committed;
    result.revision = candidate.revision;
    result.author = candidate.author;
    return result;
}

int cmdRemoveBatch(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("remove-batch does not accept --project; it is fixed-project");
    }
    if (args.size() < 2) throw std::invalid_argument("remove-batch requires preview|apply");
    const std::string phase = args[1];
    const auto version = optionValue(args, "--from");
    if (!version.has_value()) throw std::invalid_argument("remove-batch requires --from");
    std::vector<std::string> selectors;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--path") selectors.push_back(args[i + 1]);
    }
    if (selectors.empty()) throw std::invalid_argument("remove-batch requires at least one --path");

    Runtime runtime = workspaceRuntime();
    CurrentVersion current;
    try {
        current = explicitVersionInWorkspace(runtime, *version);
    } catch (const std::exception&) {
        throw WorkError{"INVALID_VERSION", false, false, ""};
    }
    const bool allow_discard = hasFlag(args, "--allow-discard-local");

    if (phase == "preview") {
        enforceJournalAdmission(runtime, current.version);
        const auto plan = buildWorkPlan(runtime, current, "remove-batch", selectors);
        out << "{\"schema_version\":\"remove-batch/v1\",\"target_version\":";
        appendJsonString(out, current.version);
        out << ",\"plan_hash\":";
        appendJsonString(out, plan.plan_hash);
        out << ",\"local_discard_required\":" << (plan.local_discard_required ? "true" : "false")
            << ",\"local_discard_item_count\":" << plan.local_discard_count << ",\"items\":[";
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            const auto& item = plan.items[i];
            if (i != 0) out << ",";
            out << "{\"path\":";
            appendJsonString(out, item.path);
            out << ",\"eligible\":" << (item.eligible ? "true" : "false") << ",\"reason_code\":";
            appendJsonString(out, item.reason_code);
            out << ",\"modified_local\":" << (item.svn_state == "M" ? "true" : "false") << "}";
        }
        out << "],\"ready\":" << (plan.ready ? "true" : "false") << ",\"issues\":";
        appendWarningsJson(out, plan.issues);
        out << "}\n";
        return 0;
    }
    if (phase != "apply") throw std::invalid_argument("remove-batch phase must be preview|apply");

    const auto expected_plan = optionValue(args, "--expected-plan");
    if (!expected_plan.has_value()) throw std::invalid_argument("apply requires --expected-plan");
    const auto message_opt = optionValue(args, "--message");
    const std::string message = message_opt.has_value() ? trim(*message_opt) : "";
    if (message.empty()) throw WorkError{"MESSAGE_REQUIRED", false, false, ""};
    // The operation trailer syntax is reserved; a user message may not spoof it.
    if (message.find("[isvn-op:") != std::string::npos) {
        throw WorkError{"RESERVED_OPERATION_TRAILER", false, false, ""};
    }

    enforceJournalAdmission(runtime, current.version);
    const auto plan = buildWorkPlan(runtime, current, "remove-batch", selectors);
    if (!plan.ready) {
        std::string code = "STATE_INCONSISTENT";
        if (!plan.issues.empty()) code = plan.issues.front().first;
        else for (const auto& item : plan.items)
            if (!item.eligible) { code = item.reason_code; break; }
        throw WorkError{code, false, false, ""};
    }
    if (plan.plan_hash != *expected_plan) throw WorkError{"PLAN_STALE", true, false, ""};
    if (plan.local_discard_required && !allow_discard) {
        throw WorkError{"DISCARD_ACK_REQUIRED", false, false, ""};
    }
    preflightBackupSpace(runtime, plan.backup_bytes);

    const auto op_id = makeOperationId("remove-batch");
    const auto dir = journalDir(runtime, op_id);
    const OperationLiveness owner = claimNewOperationJournal(dir);
    const std::string trailer = operationTrailer(op_id);
    const std::string full_message = "[isvn] " + message + trailer;
    std::vector<bool> applied(plan.items.size(), false);
    {
        std::error_code dec;
        std::filesystem::create_directories(dir / "backup", dec);
        ::chmod(dir.c_str(), 0700);
    }
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        backupItemContent(dir, i, current.working_copy / plan.items[i].path);
    }

    // Freeze the attribution evidence BEFORE staging/commit.
    std::int64_t pre_commit_head = -1;
    try {
        pre_commit_head = runtime.svn.headRevision(runtime.config.repo_url);
    } catch (const std::exception&) {
        { std::error_code rec; removeOperationJournal(dir, rec); }
        throw WorkError{"SVN_LOG_QUERY_FAILED", true, false, op_id};
    }
    const std::string expected_author = expectedSvnAuthor();
    const std::string target_url = runtime.svn.versionToUrl(current.version);
    std::vector<std::filesystem::path> commit_targets;
    std::set<std::string> expected_changed;
    for (const auto& item : plan.items) {
        commit_targets.push_back(current.working_copy / item.path);
        expected_changed.insert("D|" + repoAbsolutePath(runtime, current.version, item.path));
    }

    // Build the v2 evidence block for the journal.
    std::ostringstream evidence;
    evidence << "\"pre_commit_head_revision\": " << pre_commit_head << ",\n";
    evidence << "\"expected_author\": \"" << jsonEscape(expected_author) << "\",\n";
    evidence << "\"attribution_target_url\": \"" << jsonEscape(target_url) << "\",\n";
    evidence << "\"full_message\": \"" << jsonEscape(full_message) << "\",\n";
    evidence << "\"expected_changed\": [";
    { bool first = true; for (const auto& c : expected_changed) { if (!first) evidence << ", "; first = false; evidence << "\"" << jsonEscape(c) << "\""; } }
    evidence << "],\n";
    evidence << "\"commit_targets\": [";
    { bool first = true; for (const auto& t : commit_targets) { if (!first) evidence << ", "; first = false; evidence << "\"" << jsonEscape(t.string()) << "\""; } }
    evidence << "],\n";
    const std::string evidence_fields = evidence.str();

    writeJournal(runtime, op_id, "remove-batch", current.version, "PREPARED", plan.plan_hash, plan,
                 applied, message, trailer, "work-journal/v5", evidence_fields);
    workFault("journal_prepared");
    writeJournal(runtime, op_id, "remove-batch", current.version, "APPLYING", plan.plan_hash, plan,
                 applied, message, trailer, "work-journal/v5", evidence_fields);
    workFault("backup_created");

    try {
        // Stage all SVN removals without committing. A committed file rests as a
        // pool symlink and may carry uncommitted LOCAL edits; revert first (which
        // discards the acknowledged LOCAL modification and restores the tracked
        // regular node) so `svn delete` operates on a matching, unmodified node.
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            const auto abs = current.working_copy / plan.items[i].path;
            try { runtime.store.unregister_ref(abs.string()); } catch (...) {}
            std::error_code ec;
            if (std::filesystem::is_symlink(abs, ec)) {
                std::filesystem::remove(abs, ec);
            }
            runtime.svn.revert(abs);
            runtime.svn.remove(abs);
        }
        workFault("svn_staged");
    } catch (const std::exception&) {
        // Pre-commit failure: revert staging + restore local content; no revision.
        writeJournal(runtime, op_id, "remove-batch", current.version, "ROLLING_BACK",
                     plan.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                     evidence_fields);
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            const auto abs = current.working_copy / plan.items[i].path;
            try { runtime.svn.revert(abs); } catch (const std::exception&) {}
            std::error_code ec;
            const auto backup = dir / "backup" / std::to_string(i);
            if (std::filesystem::exists(backup, ec)) {
                std::filesystem::copy_file(backup, abs,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        // This is a Version-scoped rollback.  Never invoke the historical
        // whole-workspace relink walker here: an unrelated Version may be
        // changing at the same time.  Re-link only restored managed
        // selected files; acknowledged LOCAL originals stay regular LOCAL
        // files exactly as they were before this Remove attempt.
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            if (plan.items[i].link_state == "LOCAL") continue;
            const auto restored = current.working_copy / plan.items[i].path;
            std::error_code restore_ec;
            if (std::filesystem::is_regular_file(restored, restore_ec) &&
                !std::filesystem::is_symlink(restored, restore_ec)) {
                relinkFile(runtime.store, restored);
            }
        }
        { std::error_code rec; removeOperationJournal(dir, rec); }
        out << "{\"schema_version\":\"remove-batch-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"status\":\"failed\",\"target_version\":";
        appendJsonString(out, current.version);
        out << ",\"message\":";
        appendJsonString(out, message);
        out << ",\"removed_paths\":[],\"discarded_local_item_count\":0,\"revision\":null,"
               "\"author\":null,\"changed_paths_verified\":false,\"recovery_required\":false}\n";
        return 0;
    }

    // Commit boundary: a crash here leaves VERIFYING_REPOSITORY for recovery to
    // verify by exact SVN-log evidence (never a blind retry).
    writeJournal(runtime, op_id, "remove-batch", current.version, "VERIFYING_REPOSITORY",
                 plan.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                 evidence_fields);
    workFault("svn_commit_boundary");
    try {
        // Commit EXACTLY the selected removal targets, never the whole working
        // copy, so unrelated changes cannot be swept into the revision.
        runtime.svn.commitPaths(commit_targets, full_message);
    } catch (const std::exception&) {
        appendWorkErrorJson(out, WorkError{"OUTCOME_UNKNOWN", false, true, op_id});
        return 1;
    }
    workFault("repo_success_precleanup");

    // Positive attribution of the just-created revision (exact scope + evidence).
    RemoveEvidence ev{op_id, expected_author, target_url, pre_commit_head, full_message,
                      expected_changed, {}};
    for (const auto& item : plan.items) ev.item_paths.push_back(item.path);
    const auto verify = verifyRemoveOutcome(runtime, ev);
    if (verify.state != RemoveVerifyState::Committed) {
        // The commit landed but its evidence does not match exactly: an integrity
        // event. Keep the journal/backups for manual review; do not finalize.
        writeJournal(runtime, op_id, "remove-batch", current.version, "RECOVERY_REQUIRED",
                     plan.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                     evidence_fields);
        appendWorkErrorJson(out,
                            WorkError{verify.code.empty() ? "COMMIT_SCOPE_MISMATCH" : verify.code,
                                      false, true, op_id});
        return 1;
    }

    mutateWorkspaceState(runtime, [&](std::set<std::string>& state) {
        for (const auto& item : plan.items) {
            state.erase(relativeString(runtime.workspace, current.working_copy / item.path));
        }
    });
    for (const auto& item : plan.items) {
        clearEditableReceipt(runtime, current.version, item.path);
    }
    { std::error_code rec; removeOperationJournal(dir, rec); }

    out << "{\"schema_version\":\"remove-batch-result/v2\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"status\":\"succeeded\",\"target_version\":";
    appendJsonString(out, current.version);
    out << ",\"message\":";
    appendJsonString(out, message);
    out << ",\"removed_paths\":[";
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        if (i != 0) out << ",";
        appendJsonString(out, plan.items[i].path);
    }
    out << "],\"discarded_local_item_count\":" << plan.local_discard_count << ",\"revision\":"
        << verify.revision << ",\"author\":";
    appendJsonString(out, verify.author);
    out << ",\"changed_paths_verified\":true,\"recovery_required\":false}\n";
    return 0;
}

// ===========================================================================
// Atomic Checkin Batch.
//
// Explicit selection -> backend concrete Batch -> Review -> Confirm -> one exact
// SVN commit -> Success / Failed / Outcome Unknown / Manual Review. The backend
// is authoritative for selection expansion, eligibility (NEW LOCAL add vs
// managed update with Make Editable provenance), plan identity, atomic
// prepare-all/commit-once, exact commit scope, and positive outcome attribution.
// ===========================================================================

struct CheckinItem {
    std::string path;                 // working-copy-relative
    std::string change_kind;          // "add" | "update" | ""
    std::string source_kind = "LOCAL";
    std::int64_t size = 0;
    std::string fingerprint;
    std::string editable_provenance;  // verified | not_required | required | stale
    bool allowed = false;
    std::string reason_code = "OK";
    bool blocking = false;            // conflicted/missing/removed dangerous state
};

struct CheckinBatch {
    std::string version;
    std::string target_url;
    std::vector<std::string> selectors;
    std::vector<CheckinItem> items;            // sorted by path, deduplicated
    std::vector<std::string> supporting_dirs;  // version-relative, shallow-first
    std::int64_t total_size = 0;
    int add_count = 0;
    int update_count = 0;
    int blocked_count = 0;
    int ignored_clean_count = 0;
    bool can_apply = false;
    bool svn_available = false;
    std::string plan_hash;
    std::string expected_author;
    std::vector<std::pair<std::string, std::string>> issues;
    struct QueryScope {
        std::size_t selector_count = 0;
        std::size_t scan_root_count = 0;
        std::size_t local_entries_examined = 0;
        std::size_t remote_nodes_queried = 0;
        std::size_t svn_process_count = 0;
        std::uint64_t elapsed_ms = 0;
        bool root_explicitly_selected = false;
    } query_scope;
};

// Fixed backend-owned query ceilings. They deliberately live beside the
// Checkin plan builder, not in QML or a user-facing option.  Any incomplete
// query throws before a plan can become applicable.
constexpr std::size_t kCheckinMaxSelectors = 4096;
constexpr std::size_t kCheckinMaxScanRoots = 4096;
constexpr std::size_t kCheckinMaxItems = 50000;
constexpr std::size_t kCheckinMaxLocalEntries = 250000;
constexpr std::size_t kCheckinMaxRemoteNodes = 100000;
constexpr std::size_t kCheckinMaxSupportingDirs = 50000;

struct CheckinQueryContext {
    version::SvnQueryLimits svn_limits;
    version::SvnQueryBudget svn_budget;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::size_t selector_count = 0;
    std::size_t scan_root_count = 0;
    std::size_t local_entries_examined = 0;
    std::size_t remote_nodes_queried = 0;
    bool root_explicitly_selected = false;

    void ensureWithinDeadline() const {
        if (std::chrono::steady_clock::now() - started >= svn_limits.total_timeout) {
            throw WorkError{"SVN_QUERY_TIMEOUT", false, false, ""};
        }
    }
    void add(std::size_t& value, std::size_t amount, std::size_t limit,
             const char* code) {
        ensureWithinDeadline();
        if (amount > limit || value > limit - amount) throw WorkError{code, false, false, ""};
        value += amount;
    }
    void localEntry() { add(local_entries_examined, 1, kCheckinMaxLocalEntries,
                            "LOCAL_SCAN_LIMIT_EXCEEDED"); }
    void remoteNodes(std::size_t count) { add(remote_nodes_queried, count,
                                               kCheckinMaxRemoteNodes,
                                               "REMOTE_QUERY_LIMIT_EXCEEDED"); }
};

struct CheckinScanRoot {
    std::string path;              // empty = explicitly selected Active root
    bool directory = false;
};

struct CheckinCandidate {
    WorkFileEntry entry;
    bool special_or_escaping = false;
    bool explicit_selector = false;
};

enum class RemoteNodeKind { File, Directory, Absent };

std::vector<std::string> ancestorDirs(const std::string& relpath);

bool isValidUtf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) { ++i; continue; }
        std::size_t follow = 0;
        if (c >= 0xc2 && c <= 0xdf) follow = 1;
        else if (c >= 0xe0 && c <= 0xef) follow = 2;
        else if (c >= 0xf0 && c <= 0xf4) follow = 3;
        else return false;
        if (i + follow >= value.size()) return false;
        for (std::size_t j = 1; j <= follow; ++j) {
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        }
        const unsigned char first = static_cast<unsigned char>(value[i + 1]);
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return false;
        i += follow + 1;
    }
    return true;
}

std::string normalizeCheckinSelector(const std::string& raw) {
    if (raw.empty() || !isValidUtf8(raw)) throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    std::string selector = raw;
    std::replace(selector.begin(), selector.end(), '\\', '/');
    if (selector.front() == '/' || selector.find('\0') != std::string::npos) {
        throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    }
    const auto normalized = std::filesystem::path(selector).lexically_normal().generic_string();
    if (normalized == ".") return "";  // explicit Active-root selector
    if (normalized.empty() || normalized.front() == '/' || normalized.rfind("../", 0) == 0 ||
        normalized == "..") {
        throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    }
    for (const auto& part : std::filesystem::path(normalized)) {
        const auto text = part.generic_string();
        if (text.empty() || text == "." || text == ".." || text == ".svn" ||
            text == ".isvn" || text == ".isvn_local") {
            throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
        }
    }
    return normalized;
}

bool isSameOrUnderRelative(const std::string& path, const std::string& root) {
    return root.empty() || path == root ||
           (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
            path[root.size()] == '/');
}

std::string relativeParent(const std::string& path) {
    const auto parent = std::filesystem::path(path).parent_path().generic_string();
    return parent == "." ? "" : parent;
}

std::string relativeLeaf(const std::string& path) {
    return std::filesystem::path(path).filename().generic_string();
}

std::string urlForRelative(const std::string& root_url, const std::string& relative) {
    return relative.empty() ? root_url : root_url + "/" + relative;
}

[[noreturn]] void throwCheckinSvnFailure(const version::SvnQueryError& error,
                                         bool recovery = false) {
    switch (error.failure()) {
        case version::SvnQueryFailure::ProcessLimit:
        case version::SvnQueryFailure::ArgvLimit:
            throw WorkError{"REMOTE_QUERY_LIMIT_EXCEEDED", false, recovery, ""};
        case version::SvnQueryFailure::Timeout:
            throw WorkError{recovery ? "RECOVERY_QUERY_INCOMPLETE" : "SVN_QUERY_TIMEOUT",
                            false, recovery, ""};
        case version::SvnQueryFailure::OutputLimit:
            throw WorkError{recovery ? "RECOVERY_QUERY_INCOMPLETE" : "SVN_OUTPUT_LIMIT_EXCEEDED",
                            false, recovery, ""};
        case version::SvnQueryFailure::Failed:
            throw WorkError{recovery ? "RECOVERY_QUERY_INCOMPLETE" : "SVN_QUERY_FAILED",
                            false, recovery, ""};
    }
    throw WorkError{"SVN_QUERY_FAILED", false, recovery, ""};
}

std::vector<CheckinScanRoot> normalizeCheckinScanRoots(
    const CurrentVersion& current, const std::vector<std::string>& selectors,
    CheckinQueryContext& context, std::vector<std::string>& normalized_selectors) {
    context.selector_count = selectors.size();
    if (context.selector_count > kCheckinMaxSelectors) {
        throw WorkError{"SELECTOR_LIMIT_EXCEEDED", false, false, ""};
    }
    std::set<std::string> unique;
    for (const auto& raw : selectors) unique.insert(normalizeCheckinSelector(raw));
    if (unique.empty()) throw WorkError{"SELECTION_REQUIRED", false, false, ""};
    normalized_selectors.assign(unique.begin(), unique.end());
    context.root_explicitly_selected = unique.count("") != 0;

    std::vector<CheckinScanRoot> candidates;
    for (const auto& path : unique) {
        std::error_code ec;
        const auto abs = current.working_copy / path;
        const auto state = std::filesystem::symlink_status(abs, ec);
        const bool directory = !ec && std::filesystem::is_directory(state) &&
                               !std::filesystem::is_symlink(state);
        candidates.push_back(CheckinScanRoot{path, directory});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
        return a.path < b.path;
    });
    std::vector<CheckinScanRoot> roots;
    for (const auto& candidate : candidates) {
        bool covered = false;
        for (const auto& root : roots) {
            if (root.directory && isSameOrUnderRelative(candidate.path, root.path)) {
                covered = true;
                break;
            }
        }
        if (!covered) roots.push_back(candidate);
    }
    context.scan_root_count = roots.size();
    if (context.scan_root_count > kCheckinMaxScanRoots) {
        throw WorkError{"SELECTOR_LIMIT_EXCEEDED", false, false, ""};
    }
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return roots;
}

CheckinCandidate inspectCheckinPath(const Runtime& runtime,
                                    const std::set<std::string>& local_paths,
                                    const std::filesystem::path& absolute,
                                    const std::string& relative) {
    CheckinCandidate candidate;
    candidate.entry.path = relative;
    candidate.entry.kind = "file";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(absolute, ec);
    const bool symlink = !ec && std::filesystem::is_symlink(status);
    const bool regular = !ec && std::filesystem::is_regular_file(status);
    const auto workspace_relative = lexicalRelativeString(runtime.workspace, absolute);
    const auto hash = symlink ? poolHashFromSymlinkIfAny(absolute, runtime.config.pool_root)
                              : std::optional<std::string>();
    const bool linked = hash.has_value();
    candidate.entry.link_state = linked ? "LINKED" :
        ((regular || symlink) && (local_paths.count(workspace_relative) != 0 || !linked) ? "LOCAL" : "");
    candidate.special_or_escaping = !regular && !symlink;
    if (linked) {
        try {
            candidate.entry.size = static_cast<std::int64_t>(
                std::filesystem::file_size(runtime.store.object_path(*hash), ec));
            if (ec) candidate.entry.size = std::nullopt;
        } catch (const std::exception&) {
            candidate.entry.size = std::nullopt;
        }
    } else if (regular) {
        const auto size = std::filesystem::file_size(absolute, ec);
        candidate.entry.size = ec ? std::optional<std::int64_t>()
                                  : std::optional<std::int64_t>(static_cast<std::int64_t>(size));
    }
    return candidate;
}

void discoverCheckinRoot(const Runtime& runtime, const CurrentVersion& current,
                         const std::set<std::string>& local_paths, const CheckinScanRoot& root,
                         CheckinQueryContext& context,
                         std::map<std::string, CheckinCandidate>& candidates) {
    const auto absolute_root = current.working_copy / root.path;
    auto add_path = [&](const std::filesystem::path& absolute) {
        const auto relative = lexicalRelativeString(current.working_copy, absolute);
        candidates.try_emplace(relative, inspectCheckinPath(runtime, local_paths,
                                                             absolute, relative));
    };
    if (!root.directory) {
        std::error_code ec;
        if (std::filesystem::exists(absolute_root, ec) || std::filesystem::is_symlink(absolute_root, ec)) {
            context.localEntry();
            add_path(absolute_root);
        } else if (!root.path.empty()) {
            CheckinCandidate missing;
            missing.entry.path = root.path;
            missing.entry.kind = "file";
            missing.entry.ui_state = "missing";
            candidates.try_emplace(root.path, std::move(missing));
        }
        return;
    }
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        absolute_root, std::filesystem::directory_options::skip_permission_denied, ec), end;
    if (ec) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
    for (; it != end; it.increment(ec)) {
        if (ec) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
        context.localEntry();
        const auto path = it->path();
        if (shouldSkipPath(current.working_copy, path)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            if (ec) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
            continue;
        }
        const auto state = std::filesystem::symlink_status(path, ec);
        if (ec) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
        if (std::filesystem::is_directory(state) && !std::filesystem::is_symlink(state)) continue;
        add_path(path);
    }
}

std::map<std::string, RemoteNodeKind> querySelectedRemoteNodes(
    const Runtime& runtime, const CheckinBatch& batch,
    const std::map<std::string, CheckinCandidate>& candidates,
    CheckinQueryContext& context) {
    std::set<std::string> nodes;
    auto add_node = [&](const std::string& node) {
        if (nodes.insert(node).second && nodes.size() > kCheckinMaxRemoteNodes) {
            throw WorkError{"REMOTE_QUERY_LIMIT_EXCEEDED", false, false, ""};
        }
    };
    for (const auto& [path, candidate] : candidates) {
        (void)candidate;
        context.ensureWithinDeadline();
        add_node(path);
        for (const auto& parent : ancestorDirs(path)) add_node(parent);
    }
    context.remoteNodes(nodes.size());
    std::map<std::string, std::set<std::string>> wanted_by_parent;
    for (const auto& node : nodes) wanted_by_parent[relativeParent(node)].insert(relativeLeaf(node));
    std::map<std::string, RemoteNodeKind> result;
    try {
        for (const auto& [parent, wanted] : wanted_by_parent) {
            const auto listed = runtime.svn.listImmediateBounded(
                urlForRelative(batch.target_url, parent), context.svn_limits, context.svn_budget);
            std::map<std::string, bool> present;
            if (listed.exists) {
                for (const auto& entry : listed.entries) {
                    if (!present.emplace(entry.name, entry.is_directory).second) {
                        throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
                    }
                }
            }
            for (const auto& leaf : wanted) {
                const std::string node = parent.empty() ? leaf : parent + "/" + leaf;
                const auto it = present.find(leaf);
                result[node] = (!listed.exists || it == present.end()) ? RemoteNodeKind::Absent
                           : (it->second ? RemoteNodeKind::Directory : RemoteNodeKind::File);
            }
        }
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error);
    }
    return result;
}

// Recovery's no-candidate path needs only a conclusive answer for the exact
// NEW LOCAL add targets.  It never reconstructs a version-wide inventory.
bool anyExactAddTargetExists(const Runtime& runtime, const std::string& target_url,
                             const std::vector<std::string>& add_relpaths,
                             CheckinQueryContext& context) {
    std::map<std::string, std::set<std::string>> wanted_by_parent;
    for (const auto& path : add_relpaths) wanted_by_parent[relativeParent(path)].insert(relativeLeaf(path));
    context.remoteNodes(add_relpaths.size() + wanted_by_parent.size());
    try {
        for (const auto& [parent, wanted] : wanted_by_parent) {
            const auto listed = runtime.svn.listImmediateBounded(
                urlForRelative(target_url, parent), context.svn_limits, context.svn_budget);
            if (!listed.exists) continue;  // stable not-found proves every direct child absent
            std::set<std::string> names;
            for (const auto& entry : listed.entries) names.insert(entry.name);
            for (const auto& leaf : wanted) if (names.count(leaf) != 0) return true;
        }
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error, true);
    }
    return false;
}

// Ancestor directory components of a working-copy-relative file path, shallowest
// first (e.g. "a/b/c.v" -> {"a","a/b"}).
std::vector<std::string> ancestorDirs(const std::string& relpath) {
    std::vector<std::string> dirs;
    std::filesystem::path p(relpath);
    std::string acc;
    std::vector<std::string> parts;
    for (const auto& part : p.parent_path()) parts.push_back(part.string());
    for (const auto& part : parts) {
        acc = acc.empty() ? part : acc + "/" + part;
        dirs.push_back(acc);
    }
    return dirs;
}

std::string checkinPlanHash(const Runtime& runtime, const CheckinBatch& batch) {
    std::ostringstream s;
    s << "checkin-batch/v1\n"
      << projectIdentityHash(runtime) << '\n'
      << stableWorkspaceId(batch.version) << '\n'
      << batch.version << '\n'
      << batch.target_url << '\n'
      << batch.expected_author << '\n';
    for (const auto& item : batch.items) {
        s << item.path << '|' << item.change_kind << '|' << item.source_kind << '|' << item.size
          << '|' << item.fingerprint << '|' << item.editable_provenance << '|'
          << (item.allowed ? '1' : '0') << '|' << item.reason_code << '\n';
    }
    s << "--dirs--\n";
    for (const auto& d : batch.supporting_dirs) s << d << '\n';
    return common::blake3_hex(s.str());
}

// Build the authoritative concrete Batch from the explicit selector set.
// This planner deliberately does not call buildWorkFileEntries(), svn list -R, or
// a working-copy-root status/iterator unless the user explicitly selected ".".
CheckinBatch buildCheckinBatch(const Runtime& runtime, const CurrentVersion& current,
                               const std::vector<std::string>& selectors,
                               CheckinQueryContext& context) {
    CheckinBatch batch;
    batch.version = current.version;
    batch.expected_author = expectedSvnAuthor();
    try {
        batch.target_url = runtime.svn.versionToUrl(current.version);
    } catch (const std::exception&) {
        throw WorkError{"INVALID_VERSION", false, false, ""};
    }

    std::vector<std::string> normalized_selectors;
    const auto roots = normalizeCheckinScanRoots(current, selectors, context, normalized_selectors);
    batch.selectors = normalized_selectors;
    const auto local = readWorkspaceState(runtime.workspace);
    std::map<std::string, CheckinCandidate> candidates;
    std::vector<std::filesystem::path> status_targets;
    for (const auto& root : roots) {
        discoverCheckinRoot(runtime, current, local, root, context, candidates);
        // Ask SVN about every explicit minimal root, including a missing
        // selected path.  SVN metadata can distinguish a missing managed node
        // from a never-versioned one without widening the query to its parent
        // or the Active working-copy root.
        const auto absolute = current.working_copy / root.path;
        status_targets.push_back(absolute);
    }

    try {
        for (const auto& status : runtime.svn.statusBounded(status_targets, context.svn_limits,
                                                            context.svn_budget)) {
            const auto logical = svnStatusLogicalPath(current, status.path);
            if (!logical.has_value()) continue;
            bool covered = false;
            for (const auto& root : roots) {
                if (isSameOrUnderRelative(*logical, root.path)) { covered = true; break; }
            }
            if (!covered || shouldSkipPath(current.working_copy, current.working_copy / *logical)) continue;
            std::error_code status_ec;
            const auto status_fs = std::filesystem::symlink_status(
                current.working_copy / *logical, status_ec);
            if (!status_ec && std::filesystem::is_directory(status_fs) &&
                !std::filesystem::is_symlink(status_fs)) {
                // Directories are traversal/support context.  Their file
                // descendants are already indexed by discoverCheckinRoot(),
                // and an unversioned directory's `?` status is never itself
                // checkin content.
                continue;
            }
            // `svn status <folder>` reports the clean folder node itself.  It
            // is scan context, not a content candidate: inserting it would
            // turn a valid selected subtree into a spurious NOT_CHECKIN_ELIGIBLE
            // item.  Non-clean/missing/conflicted nodes remain visible even
            // when the filesystem has no entry for them.
            auto found = candidates.find(*logical);
            if (found == candidates.end() && status.status.empty()) continue;
            auto [it, inserted] = found == candidates.end()
                                      ? candidates.try_emplace(*logical)
                                      : std::pair{found, false};
            if (inserted) {
                it->second.entry.path = *logical;
                it->second.entry.kind = "file";
            }
            it->second.entry.svn_state = status.status;
        }
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error);
    }
    for (auto& [path, candidate] : candidates) {
        candidate.explicit_selector = std::binary_search(normalized_selectors.begin(),
                                                          normalized_selectors.end(), path);
        candidate.entry.ui_state = candidate.entry.ui_state == "missing" &&
                                   candidate.entry.svn_state.empty()
                                       ? "missing"
                                       : mapUiState(candidate.entry.svn_state,
                                                    candidate.entry.link_state);
    }
    const auto remote = querySelectedRemoteNodes(runtime, batch, candidates, context);
    std::set<std::string> folder_selectors;
    for (const auto& root : roots) if (root.directory) folder_selectors.insert(root.path);
    std::map<std::string, int> folder_actionable;
    for (const auto& folder : folder_selectors) folder_actionable.emplace(folder, 0);
    std::set<std::string> supporting;

    for (const auto& [path, candidate] : candidates) {
        context.ensureWithinDeadline();
        const auto& e = candidate.entry;
        const auto abs = current.working_copy / path;
        const auto remote_it = remote.find(path);
        if (remote_it == remote.end()) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
        CheckinItem item;
        item.path = path;
        item.source_kind = "LOCAL";
        item.size = e.size.value_or(0);
        item.fingerprint = itemFingerprint(abs, runtime.config.pool_root, e.link_state);

        bool actionable = false;
        std::error_code link_ec;
        const bool local_symlink = std::filesystem::is_symlink(abs, link_ec);
        if (candidate.special_or_escaping || link_ec ||
            (e.link_state == "LOCAL" && local_symlink)) {
            item.allowed = false;
            item.blocking = true;
            item.reason_code = "UNSUPPORTED_FILE_TYPE";
            actionable = true;
        } else if (e.ui_state == "conflict") {
            item.allowed = false;
            item.blocking = true;
            item.reason_code = "CONFLICT_BLOCKED";
            actionable = true;
        } else if (e.ui_state == "missing" || e.ui_state == "removed") {
            item.allowed = false;
            item.blocking = true;
            item.reason_code = e.svn_state.empty() ? "PATH_NOT_FOUND" : "REMOVAL_REQUIRES_REMOVE";
            actionable = true;
        } else if (e.link_state == "LOCAL" && remote_it->second == RemoteNodeKind::Absent) {
            item.change_kind = "add";
            item.editable_provenance = "not_required";
            item.allowed = true;
            item.reason_code = "OK";
            actionable = true;
            for (const auto& dir : ancestorDirs(path)) {
                const auto dir_it = remote.find(dir);
                if (dir_it == remote.end()) throw WorkError{"SVN_QUERY_FAILED", false, false, ""};
                if (dir_it->second == RemoteNodeKind::File) {
                    item.allowed = false;
                    item.blocking = true;
                    item.change_kind.clear();
                    item.reason_code = "MANAGED_OVERWRITE_BLOCKED";
                    break;
                }
                if (dir_it->second == RemoteNodeKind::Absent && supporting.insert(dir).second &&
                    supporting.size() > kCheckinMaxSupportingDirs) {
                    throw WorkError{"REMOTE_QUERY_LIMIT_EXCEEDED", false, false, ""};
                }
            }
        } else if (remote_it->second != RemoteNodeKind::Absent && e.svn_state == "M") {
            const auto receipt = readEditableReceipt(runtime, current.version, path);
            if (!receipt.has_value()) {
                item.reason_code = "EDIT_PROVENANCE_REQUIRED";
                item.editable_provenance = "required";
            } else if (!receipt->valid) {
                item.reason_code = "EDIT_PROVENANCE_STALE";
                item.editable_provenance = "stale";
            } else {
                item.change_kind = "update";
                item.editable_provenance = "verified";
                item.allowed = true;
                item.reason_code = "OK";
            }
            actionable = true;
        } else if (remote_it->second != RemoteNodeKind::Absent && e.link_state == "LOCAL") {
            // A local non-M collision with a repository node is never converted
            // to a NEW LOCAL add merely because the filesystem currently has it.
            item.allowed = false;
            item.blocking = true;
            item.reason_code = "MANAGED_OVERWRITE_BLOCKED";
            actionable = true;
        } else {
            if (!candidate.explicit_selector) {
                ++batch.ignored_clean_count;
                continue;
            }
            item.allowed = false;
            item.reason_code = "NOT_CHECKIN_ELIGIBLE";
        }

        if (actionable) {
            if (folder_selectors.count("") != 0) ++folder_actionable[""];
            for (const auto& ancestor : ancestorDirs(path)) {
                if (folder_selectors.count(ancestor) != 0) ++folder_actionable[ancestor];
            }
        }
        batch.items.push_back(std::move(item));
        if (batch.items.size() > kCheckinMaxItems) {
            throw WorkError{"BATCH_ITEM_LIMIT_EXCEEDED", false, false, ""};
        }
    }
    if (supporting.size() > kCheckinMaxSupportingDirs) {
        throw WorkError{"REMOTE_QUERY_LIMIT_EXCEEDED", false, false, ""};
    }
    batch.supporting_dirs.assign(supporting.begin(), supporting.end());
    std::sort(batch.supporting_dirs.begin(), batch.supporting_dirs.end(),
              [](const std::string& a, const std::string& b) {
                  const auto da = std::count(a.begin(), a.end(), '/');
                  const auto db = std::count(b.begin(), b.end(), '/');
                  return da == db ? a < b : da < db;
              });
    for (const auto& item : batch.items) {
        if (item.blocking || !item.allowed) ++batch.blocked_count;
        if (item.allowed && item.change_kind == "add") {
            ++batch.add_count;
            batch.total_size += item.size;
        }
        if (item.allowed && item.change_kind == "update") {
            ++batch.update_count;
            batch.total_size += item.size;
        }
    }
    for (const auto& [folder, count] : folder_actionable) {
        if (count == 0) batch.issues.emplace_back("FOLDER_EMPTY", "selected folder has no checkin-eligible item");
    }
    batch.svn_available = true;
    batch.can_apply = !batch.items.empty() && batch.blocked_count == 0;
    batch.query_scope.selector_count = context.selector_count;
    batch.query_scope.scan_root_count = context.scan_root_count;
    batch.query_scope.local_entries_examined = context.local_entries_examined;
    batch.query_scope.remote_nodes_queried = context.remote_nodes_queried;
    batch.query_scope.svn_process_count = context.svn_budget.process_count;
    batch.query_scope.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                context.started).count());
    batch.query_scope.root_explicitly_selected = context.root_explicitly_selected;
    batch.plan_hash = checkinPlanHash(runtime, batch);  // metrics intentionally excluded
    return batch;
}

// Positive, typed attribution of a Checkin Batch commit (reuses the Remove
// evidence model). `selected_repo_paths` are the repository-absolute targets of
// the operation; `add_relpaths` are version-relative NEW LOCAL targets that must
// remain absent for a safe no-commit proof.
RemoveVerifyResult verifyCheckinOutcome(Runtime& runtime, const RemoveEvidence& ev,
                                        const std::set<std::string>& selected_repo_paths,
                                        const std::vector<std::string>& add_relpaths,
                                        CheckinQueryContext& query) {
    RemoveVerifyResult result;
    if (ev.expected_author.empty()) {
        result.state = RemoveVerifyState::Insufficient;
        result.code = "EXPECTED_AUTHOR_UNAVAILABLE";
        return result;
    }
    std::int64_t head = 0;
    try {
        head = runtime.svn.headRevisionBounded(runtime.config.repo_url, query.svn_limits,
                                              query.svn_budget);
    } catch (const version::SvnQueryError& error) {
        switch (error.failure()) {
            case version::SvnQueryFailure::Timeout: result.code = "RECOVERY_QUERY_INCOMPLETE"; break;
            case version::SvnQueryFailure::OutputLimit: result.code = "RECOVERY_QUERY_INCOMPLETE"; break;
            case version::SvnQueryFailure::ProcessLimit:
            case version::SvnQueryFailure::ArgvLimit: result.code = "RECOVERY_QUERY_INCOMPLETE"; break;
            case version::SvnQueryFailure::Failed: result.code = "RECOVERY_QUERY_INCOMPLETE"; break;
        }
        return result;
    } catch (const std::exception&) {
        result.code = "RECOVERY_QUERY_INCOMPLETE";
        return result;
    }
    std::vector<version::SvnVerboseLogEntry> entries;
    const std::int64_t from = ev.pre_commit_head < 0 ? 0 : ev.pre_commit_head + 1;
    if (head >= from) {
        try {
            entries = runtime.svn.logVerboseBounded(ev.target_url, from, head, query.svn_limits,
                                                    query.svn_budget);
        } catch (const version::SvnQueryError&) {
            result.code = "RECOVERY_QUERY_INCOMPLETE";
            return result;
        } catch (const std::exception&) {
            result.code = "RECOVERY_QUERY_INCOMPLETE";
            return result;
        }
    }
    std::vector<const version::SvnVerboseLogEntry*> candidates;
    for (const auto& entry : entries) {
        const auto trailer = parseOperationTrailer(entry.message);
        if (trailer.has_value() && *trailer == ev.op_id) candidates.push_back(&entry);
    }
    if (candidates.size() > 1) {
        result.state = RemoveVerifyState::Ambiguous;
        result.code = "OPERATION_EVIDENCE_AMBIGUOUS";
        return result;
    }
    if (candidates.empty()) {
        // Safe NOT_COMMITTED only if no selected repository path was touched by any
        // intervening revision and every NEW LOCAL target is still absent.
        bool diverged = false;
        for (const auto& entry : entries) {
            for (const auto& changed : entry.paths) {
                if (selected_repo_paths.count(changed.path) != 0) diverged = true;
            }
        }
        if (!diverged && !add_relpaths.empty()) {
            try {
                if (anyExactAddTargetExists(runtime, ev.target_url, add_relpaths, query)) diverged = true;
            } catch (const WorkError&) {
                result.code = "RECOVERY_QUERY_INCOMPLETE";
                return result;
            }
        }
        if (diverged) {
            result.state = RemoveVerifyState::Diverged;
            result.code = "REMOTE_STATE_DIVERGED";
            return result;
        }
        result.state = RemoveVerifyState::NotCommitted;
        return result;
    }
    const auto& candidate = *candidates.front();
    std::set<std::string> actual;
    for (const auto& changed : candidate.paths) actual.insert(changed.action + "|" + changed.path);
    const bool ok = candidate.revision > ev.pre_commit_head &&
                    candidate.author == ev.expected_author &&
                    candidate.message == ev.full_message && actual == ev.expected_changed;
    if (!ok) {
        result.state = RemoveVerifyState::Mismatch;
        result.code = "OPERATION_EVIDENCE_MISMATCH";
        return result;
    }
    result.state = RemoveVerifyState::Committed;
    result.revision = candidate.revision;
    result.author = candidate.author;
    return result;
}

// Finalize committed files into the authoritative LINKED managed form, clear the
// LOCAL state entry, and consume any editable receipt. Idempotent.
void finalizeCheckinFiles(Runtime& runtime, const std::string& version,
                          const std::filesystem::path& working_copy,
                          const std::vector<std::string>& relpaths) {
    std::error_code ec;
    for (const auto& rel : relpaths) {
        const auto abs = working_copy / rel;
        if (std::filesystem::is_regular_file(abs, ec) && !std::filesystem::is_symlink(abs, ec)) {
            relinkFile(runtime.store, abs);
        }
        clearEditableReceipt(runtime, version, rel);
    }
    mutateWorkspaceState(runtime, [&](std::set<std::string>& state) {
        for (const auto& rel : relpaths) {
            state.erase(relativeString(runtime.workspace, working_copy / rel));
        }
    });
}

// Undo a pre-commit / not-committed preparation: unschedule the staged NEW LOCAL
// file adds and supporting directory adds only. Existing managed UPDATE targets
// were never staged (they are the member's own modifications) and are left
// exactly as they were.
void rollbackCheckinStaging(Runtime& runtime, const std::filesystem::path& working_copy,
                            const std::vector<std::string>& add_relpaths,
                            const std::vector<std::string>& supporting_dirs) {
    for (const auto& rel : add_relpaths) {
        try { runtime.svn.revert(working_copy / rel); } catch (const std::exception&) {}
    }
    // Deepest directory first so a parent is never unscheduled before its child.
    for (auto it = supporting_dirs.rbegin(); it != supporting_dirs.rend(); ++it) {
        try { runtime.svn.revert(working_copy / *it); } catch (const std::exception&) {}
    }
}

void appendCheckinBatchResult(std::ostream& out, const std::string& op_id,
                              const std::string& version, const std::string& status,
                              std::int64_t revision, const std::string& message_summary,
                              const std::vector<CheckinItem>& items, bool recovery_required) {
    out << "{\"schema_version\":\"checkin-batch-result/v1\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"target_version\":";
    appendJsonString(out, version);
    out << ",\"status\":";
    appendJsonString(out, status);
    out << ",\"revision\":" << (revision > 0 ? std::to_string(revision) : std::string("null"));
    out << ",\"message_summary\":";
    appendJsonString(out, message_summary);
    out << ",\"item_count\":" << items.size() << ",\"items\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out << ",";
        out << "{\"path\":";
        appendJsonString(out, items[i].path);
        out << ",\"change_kind\":";
        appendJsonString(out, items[i].change_kind);
        out << ",\"status\":";
        appendJsonString(out, status == "committed" ? "committed" : "not_committed");
        out << "}";
    }
    out << "],\"recovery_required\":" << (recovery_required ? "true" : "false") << "}\n";
}

int cmdCheckinBatch(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("checkin-batch does not accept --project; it is fixed-project");
    }
    if (args.size() < 2) throw std::invalid_argument("checkin-batch requires preview|apply");
    const std::string phase = args[1];
    const auto positional = positionalArgs(args);
    if (positional.size() < 2) throw std::invalid_argument("checkin-batch requires a version");
    const std::string version = positional[1];
    std::vector<std::string> selectors;
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--path") selectors.push_back(args[i + 1]);
    }

    Runtime runtime = workspaceRuntime();
    CurrentVersion current;
    try {
        current = explicitVersionInWorkspace(runtime, version);
    } catch (const std::exception&) {
        throw WorkError{"INVALID_VERSION", false, false, ""};
    }

    if (phase == "preview") {
        enforceJournalAdmission(runtime, current.version);
        if (selectors.empty()) throw WorkError{"SELECTION_REQUIRED", false, false, ""};
        CheckinQueryContext query;
        const auto batch = buildCheckinBatch(runtime, current, selectors, query);
        out << "{\"schema\":\"checkin-batch/v1\",\"target\":{\"workspace_id\":";
        appendJsonString(out, stableWorkspaceId(current.version));
        out << ",\"version\":";
        appendJsonString(out, current.version);
        out << ",\"path\":";
        appendJsonString(out, current.version);
        out << "},\"selectors\":[";
        for (std::size_t i = 0; i < batch.selectors.size(); ++i) {
            if (i != 0) out << ",";
            appendJsonString(out, batch.selectors[i]);
        }
        out << "],\"items\":[";
        for (std::size_t i = 0; i < batch.items.size(); ++i) {
            const auto& item = batch.items[i];
            if (i != 0) out << ",";
            out << "{\"path\":";
            appendJsonString(out, item.path);
            out << ",\"kind\":\"file\",\"source_kind\":";
            appendJsonString(out, item.source_kind);
            out << ",\"change_kind\":";
            appendJsonString(out, item.change_kind);
            out << ",\"size\":" << item.size << ",\"fingerprint\":";
            appendJsonString(out, item.fingerprint);
            out << ",\"editable_provenance\":";
            appendJsonString(out, item.editable_provenance);
            out << ",\"allowed\":" << (item.allowed ? "true" : "false") << ",\"blocking\":"
                << (item.blocking ? "true" : "false") << ",\"reason_code\":";
            appendJsonString(out, item.reason_code);
            out << ",\"reason_params\":{}}";
        }
        out << "],\"supporting_repository_changes\":[";
        for (std::size_t i = 0; i < batch.supporting_dirs.size(); ++i) {
            if (i != 0) out << ",";
            out << "{\"path\":";
            appendJsonString(out, batch.supporting_dirs[i]);
            out << ",\"kind\":\"directory\",\"change_kind\":\"add\"}";
        }
        out << "],\"summary\":{\"item_count\":" << batch.items.size()
            << ",\"add_count\":" << batch.add_count << ",\"update_count\":" << batch.update_count
            << ",\"total_size\":" << batch.total_size << ",\"blocked_count\":" << batch.blocked_count
            << ",\"ignored_clean_count\":" << batch.ignored_clean_count << "}";
        out << ",\"query_scope\":{\"selector_count\":" << batch.query_scope.selector_count
            << ",\"scan_root_count\":" << batch.query_scope.scan_root_count
            << ",\"local_entries_examined\":" << batch.query_scope.local_entries_examined
            << ",\"remote_nodes_queried\":" << batch.query_scope.remote_nodes_queried
            << ",\"svn_process_count\":" << batch.query_scope.svn_process_count
            << ",\"elapsed_ms\":" << batch.query_scope.elapsed_ms
            << ",\"root_explicitly_selected\":"
            << (batch.query_scope.root_explicitly_selected ? "true" : "false") << "}";
        out << ",\"can_apply\":" << (batch.can_apply ? "true" : "false") << ",\"plan_hash\":";
        appendJsonString(out, batch.plan_hash);
        out << ",\"issues\":";
        appendWarningsJson(out, batch.issues);
        out << "}\n";
        return 0;
    }
    if (phase != "apply") throw std::invalid_argument("checkin-batch phase must be preview|apply");

    if (selectors.empty()) throw WorkError{"SELECTION_REQUIRED", false, false, ""};
    const auto expected_plan = optionValue(args, "--expected-plan");
    if (!expected_plan.has_value()) throw std::invalid_argument("apply requires --expected-plan");
    const auto message_opt = optionValue(args, "--message");
    const std::string message = message_opt.has_value() ? trim(*message_opt) : "";
    if (message.empty()) throw WorkError{"MESSAGE_REQUIRED", false, false, ""};
    if (message.find("[isvn-op:") != std::string::npos) {
        throw WorkError{"RESERVED_OPERATION_TRAILER", false, false, ""};
    }

    enforceJournalAdmission(runtime, current.version);
    CheckinQueryContext query;
    const auto batch = buildCheckinBatch(runtime, current, selectors, query);
    if (!batch.can_apply) {
        std::string code = "STATE_INCONSISTENT";
        if (batch.items.empty() && !batch.issues.empty()) code = batch.issues.front().first;
        else if (batch.items.empty()) code = "FOLDER_EMPTY";
        else for (const auto& item : batch.items)
            if (!item.allowed) { code = item.reason_code; break; }
        throw WorkError{code, false, false, ""};
    }
    if (batch.plan_hash != *expected_plan) throw WorkError{"PLAN_STALE", true, false, ""};

    // Freeze attribution evidence and build the exact commit plan.
    std::int64_t pre_commit_head = -1;
    try {
        pre_commit_head = runtime.svn.headRevisionBounded(runtime.config.repo_url,
                                                          query.svn_limits, query.svn_budget);
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error);
    }
    const std::string expected_author = expectedSvnAuthor();
    const std::string op_id = makeOperationId("checkin-batch");
    const auto dir = journalDir(runtime, op_id);
    const OperationLiveness owner = claimNewOperationJournal(dir);
    const std::string trailer = operationTrailer(op_id);
    const std::string full_message = "[isvn] " + message + trailer;

    std::vector<std::string> add_relpaths;
    std::vector<std::string> update_relpaths;
    std::vector<std::string> commit_relpaths;  // adds + updates (finalization set)
    std::set<std::string> expected_changed;
    std::set<std::string> selected_repo_paths;
    // Commit targets, dirs first (shallow->deep) then files, all at empty depth.
    std::vector<std::filesystem::path> commit_targets;
    for (const auto& d : batch.supporting_dirs) {
        commit_targets.push_back(current.working_copy / d);
        const auto repo_abs = repoAbsolutePath(runtime, current.version, d);
        expected_changed.insert("A|" + repo_abs);
        selected_repo_paths.insert(repo_abs);
    }
    for (const auto& item : batch.items) {
        const auto repo_abs = repoAbsolutePath(runtime, current.version, item.path);
        selected_repo_paths.insert(repo_abs);
        commit_relpaths.push_back(item.path);
        commit_targets.push_back(current.working_copy / item.path);
        if (item.change_kind == "add") {
            add_relpaths.push_back(item.path);
            expected_changed.insert("A|" + repo_abs);
        } else {
            update_relpaths.push_back(item.path);
            expected_changed.insert("M|" + repo_abs);
        }
    }

    // Build the v3 checkin evidence block for the journal.
    auto jsonArray = [](const std::vector<std::string>& values) {
        std::ostringstream a;
        bool first = true;
        for (const auto& v : values) {
            if (!first) a << ", ";
            first = false;
            a << "\"" << jsonEscape(v) << "\"";
        }
        return a.str();
    };
    std::vector<std::string> expected_changed_vec(expected_changed.begin(), expected_changed.end());
    std::ostringstream evidence;
    evidence << "\"pre_commit_head_revision\": " << pre_commit_head << ",\n";
    evidence << "\"expected_author\": \"" << jsonEscape(expected_author) << "\",\n";
    evidence << "\"attribution_target_url\": \"" << jsonEscape(batch.target_url) << "\",\n";
    evidence << "\"full_message\": \"" << jsonEscape(full_message) << "\",\n";
    evidence << "\"user_message\": \"" << jsonEscape(message) << "\",\n";
    evidence << "\"expected_changed\": [" << jsonArray(expected_changed_vec) << "],\n";
    evidence << "\"commit_files\": [" << jsonArray(commit_relpaths) << "],\n";
    evidence << "\"add_targets\": [" << jsonArray(add_relpaths) << "],\n";
    evidence << "\"update_targets\": [" << jsonArray(update_relpaths) << "],\n";
    evidence << "\"supporting_dirs\": [" << jsonArray(batch.supporting_dirs) << "],\n";
    const std::string evidence_fields = evidence.str();

    // Build a PlanItem-shaped view + backups (defensive; updates are also backed up).
    WorkPlan plan;
    plan.op = "checkin-batch";
    plan.version = current.version;
    plan.plan_hash = batch.plan_hash;
    for (const auto& item : batch.items) {
        PlanItem pi;
        pi.path = item.path;
        pi.link_state = item.source_kind;
        pi.svn_state = item.change_kind == "add" ? "?" : "M";
        pi.fingerprint = item.fingerprint;
        plan.items.push_back(pi);
    }
    std::vector<bool> applied(plan.items.size(), false);
    {
        std::error_code dec;
        std::filesystem::create_directories(dir / "backup", dec);
        ::chmod(dir.c_str(), 0700);
    }
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        backupItemContent(dir, i, current.working_copy / plan.items[i].path);
    }
    writeJournal(runtime, op_id, "checkin-batch", current.version, "PREPARING_WORKSPACE",
                 batch.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                 evidence_fields);
    workFault("journal_prepared");

    try {
        // Stage supporting directories (shallow->deep) then NEW LOCAL file adds,
        // each at empty depth. Existing managed UPDATE targets are already
        // modified in the working copy and need no staging.
        for (const auto& d : batch.supporting_dirs) {
            runtime.svn.add(current.working_copy / d, /*depth_empty=*/true);
        }
        for (const auto& rel : add_relpaths) {
            runtime.svn.add(current.working_copy / rel, /*depth_empty=*/true);
        }
        workFault("svn_staged");
    } catch (const std::exception&) {
        writeJournal(runtime, op_id, "checkin-batch", current.version, "ROLLED_BACK",
                     batch.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                     evidence_fields);
        rollbackCheckinStaging(runtime, current.working_copy, add_relpaths, batch.supporting_dirs);
        { std::error_code rec; removeOperationJournal(dir, rec); }
        appendCheckinBatchResult(out, op_id, current.version, "failed", 0, message, batch.items,
                                 false);
        return 0;
    }

    writeJournal(runtime, op_id, "checkin-batch", current.version, "COMMITTING", batch.plan_hash,
                 plan, applied, message, trailer, "work-journal/v5", evidence_fields);
    writeJournal(runtime, op_id, "checkin-batch", current.version, "VERIFYING_REPOSITORY",
                 batch.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                 evidence_fields);
    workTestPause("checkin_before_commit");
    workFault("svn_commit_boundary");
    try {
        runtime.svn.commitPaths(commit_targets, full_message, /*depth_empty=*/true);
    } catch (const std::exception&) {
        appendWorkErrorJson(out, WorkError{"OUTCOME_UNKNOWN", false, true, op_id});
        return 1;
    }
    workFault("repo_success_precleanup");

    RemoveEvidence ev{op_id, expected_author, batch.target_url, pre_commit_head, full_message,
                      expected_changed, {}};
    // Verification is a separate bounded operation.  A failure here is still
    // Outcome Unknown/Manual Review: commit invocation has already happened.
    CheckinQueryContext verify_query;
    const auto verify = verifyCheckinOutcome(runtime, ev, selected_repo_paths, add_relpaths,
                                             verify_query);
    if (verify.state != RemoveVerifyState::Committed) {
        writeJournal(runtime, op_id, "checkin-batch", current.version, "MANUAL_REVIEW_REQUIRED",
                     batch.plan_hash, plan, applied, message, trailer, "work-journal/v5",
                     evidence_fields);
        appendWorkErrorJson(out, WorkError{verify.code.empty() ? "COMMIT_SCOPE_MISMATCH" : verify.code,
                                           false, true, op_id});
        return 1;
    }

    writeJournal(runtime, op_id, "checkin-batch", current.version, "FINALIZING", batch.plan_hash,
                 plan, applied, message, trailer, "work-journal/v5", evidence_fields);
    finalizeCheckinFiles(runtime, current.version, current.working_copy, commit_relpaths);
    { std::error_code rec; removeOperationJournal(dir, rec); }

    out << "{\"schema\":\"checkin-batch-result/v1\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"target_version\":";
    appendJsonString(out, current.version);
    out << ",\"status\":\"committed\",\"revision\":" << verify.revision << ",\"message_summary\":";
    appendJsonString(out, message);
    out << ",\"author\":";
    appendJsonString(out, verify.author);
    out << ",\"item_count\":" << batch.items.size() << ",\"add_count\":" << batch.add_count
        << ",\"update_count\":" << batch.update_count << ",\"items\":[";
    for (std::size_t i = 0; i < batch.items.size(); ++i) {
        if (i != 0) out << ",";
        out << "{\"path\":";
        appendJsonString(out, batch.items[i].path);
        out << ",\"change_kind\":";
        appendJsonString(out, batch.items[i].change_kind);
        out << ",\"status\":\"committed\"}";
    }
    out << "],\"changed_paths_verified\":true,\"recovery_required\":false}\n";
    return 0;
}

// ---- recovery -------------------------------------------------------------

int recoverImportJournal(const std::string& op_id, const std::filesystem::path& dir,
                         const std::string& content,
                         const CurrentVersion& current, std::ostream& out);
std::size_t importJournalItemCount(const std::string& content);

int cmdWorkRecovery(const std::vector<std::string>& args, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("work-recovery does not accept --project; it is fixed-project");
    }
    if (args.size() < 2) throw std::invalid_argument("work-recovery requires status|restore");
    const std::string phase = args[1];
    Runtime runtime = workspaceRuntime();

    if (phase == "status") {
        out << "{\"schema_version\":\"work-recovery/v1\",\"recovery_required\":";
        std::vector<std::string> blocks;
        for (const auto& pending : discoverPendingJournals(runtime)) {
            if (operationOwnerIsLive(pending.directory)) continue;
            const auto content = readTextFile(pending.directory / "journal.json");
            const auto state = journalScalar(content, "state");
            const auto item_count = pending.operation_type == "import"
                                        ? importJournalItemCount(content)
                                        : parseJournalItems(content).size();
            std::ostringstream block;
            block << "{\"operation_id\":";
            appendJsonString(block, pending.operation_id);
            block << ",\"operation\":";
            appendJsonString(block, pending.operation_type);
            block << ",\"target_version\":";
            appendJsonString(block, pending.target_version);
            block << ",\"lock_scope\":";
            appendJsonString(block, pending.schema == "work-journal/v5" ? "VERSION" : "PROJECT");
            block << ",\"legacy_project_blocking\":"
                  << (pending.schema == "work-journal/v5" ? "false" : "true");
            block << ",\"state\":";
            appendJsonString(block, state);
            block << ",\"item_count\":" << item_count << ",\"safe_summary\":";
            appendJsonString(block, "Interrupted " + pending.operation_type);
            block << "}";
            blocks.push_back(block.str());
        }
        out << (blocks.empty() ? "false" : "true") << ",\"operations\":[";
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            if (i != 0) out << ",";
            out << blocks[i];
        }
        out << "]}\n";
        return 0;
    }

    if (phase != "restore" && phase != "verify") {
        throw std::invalid_argument("work-recovery phase must be status|restore|verify");
    }
    // positionalArgs()[0] is the subcommand token; the operation id follows.
    const auto positional = positionalArgs(args);
    if (positional.size() < 2) throw std::invalid_argument("requires an operation_id");
    const std::string op_id = positional[1];
    if (!isValidOperationId(op_id)) throw WorkError{"STATE_INCONSISTENT", false, false, op_id};
    const auto dir = journalDir(runtime, op_id);
    const auto journal = dir / "journal.json";
    std::error_code ec;
    if (!std::filesystem::exists(journal, ec)) {
        // Idempotent: nothing to recover for this id (already finalized/rolled back).
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"status\":\"not_found\",\"revision\":null,\"recovery_required\":false,"
               "\"manual_review_required\":false}\n";
        return 0;
    }
    const auto content = readTextFile(journal);
    const auto schema = journalScalar(content, "schema_version");
    if ((schema != "work-journal/v1" && schema != "work-journal/v2" &&
         schema != "work-journal/v3" && schema != "work-journal/v4" &&
         schema != "work-journal/v5") ||
        journalScalar(content, "project_hash") != projectIdentityHash(runtime)) {
        throw WorkError{"STATE_INCONSISTENT", false, false, op_id};
    }
    const auto op = journalScalar(content, "operation_type");
    const auto version = journalScalar(content, "target_version");
    const auto items = parseJournalItems(content);
    CurrentVersion current;
    // The operation may still be running in another process.  Recovery only takes
    // over a journal whose owner is gone, and then owns it itself.
    const auto owner = OperationLiveness::tryClaim(dir);
    if (!owner.has_value()) throw WorkError{"OPERATION_IN_PROGRESS", true, false, op_id};
    if (schema == "work-journal/v5") {
        try {
            validateVersionId(version);
        } catch (const std::exception&) {
            throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
        }
        try {
            current = currentFromPinnedJournal(runtime, content, op_id);
        } catch (const WorkError& error) {
            if (error.code != "OPERATION_TARGET_DIVERGED") throw;
            out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
            appendJsonString(out, op_id);
            out << ",\"operation\":";
            appendJsonString(out, op);
            out << ",\"target_version\":";
            appendJsonString(out, version);
            out << ",\"status\":\"manual_review_required\",\"revision\":null,"
                   "\"recovery_required\":true,\"manual_review_required\":true,\"reason_code\":";
            appendJsonString(out, error.code);
            out << "}\n";
            return 0;
        }
    } else {
        // Historical records lack every immutable pin required for isolated
        // recovery.  They keep blocking the whole Project until resolved.
        try {
            current = CurrentVersion{version, mappedWorkingCopy(runtime, version)};
        } catch (const std::exception&) {
            throw WorkError{"PROJECT_LEGACY_RECOVERY_REQUIRED", false, true, op_id};
        }
    }

    // --- import: no external source is ever resumed.  Recovery verifies a
    // completed destination-only journal or restores the pre-Import topology.
    if (op == "import") {
        if (schema != "work-journal/v4" && schema != "work-journal/v5") {
            throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id};
        }
        return recoverImportJournal(op_id, dir, content, current, out);
    }

    // --- checkin-batch: positive SVN-log attribution + idempotent finalize.
    if (op == "checkin-batch") {
        if (schema != "work-journal/v5") {
            throw WorkError{"LEGACY_EVIDENCE_INSUFFICIENT", false, true, op_id};
        }
        RemoveEvidence ev;
        ev.op_id = op_id;
        ev.expected_author = jsonUnescape(journalScalar(content, "expected_author"));
        ev.target_url = jsonUnescape(journalScalar(content, "attribution_target_url"));
        ev.full_message = jsonUnescape(journalScalar(content, "full_message"));
        const auto pre = journalScalar(content, "pre_commit_head_revision");
        ev.pre_commit_head = pre.empty() ? -1 : std::stoll(pre);
        std::set<std::string> selected_repo_paths;
        for (const auto& c : journalStringArray(content, "expected_changed")) {
            const auto unesc = jsonUnescape(c);
            ev.expected_changed.insert(unesc);
            const auto bar = unesc.find('|');
            if (bar != std::string::npos) selected_repo_paths.insert(unesc.substr(bar + 1));
        }
        std::vector<std::string> add_relpaths;
        for (const auto& p : journalStringArray(content, "add_targets"))
            add_relpaths.push_back(jsonUnescape(p));
        std::vector<std::string> supporting_dirs;
        for (const auto& p : journalStringArray(content, "supporting_dirs"))
            supporting_dirs.push_back(jsonUnescape(p));
        std::vector<std::string> commit_files;
        for (const auto& p : journalStringArray(content, "commit_files"))
            commit_files.push_back(jsonUnescape(p));

        // Recovery has its own whole-operation deadline and never reuses
        // Preview/Apply query evidence.
        CheckinQueryContext recovery_query;
        const auto verify = verifyCheckinOutcome(runtime, ev, selected_repo_paths, add_relpaths,
                                                 recovery_query);
        std::string status;
        bool manual_review = false;
        std::int64_t revision = 0;
        std::string author;
        if (verify.state == RemoveVerifyState::Committed) {
            finalizeCheckinFiles(runtime, version, current.working_copy, commit_files);
            removeOperationJournal(dir, ec);
            status = "verified_committed";
            revision = verify.revision;
            author = verify.author;
        } else if (verify.state == RemoveVerifyState::NotCommitted) {
            rollbackCheckinStaging(runtime, current.working_copy, add_relpaths, supporting_dirs);
            removeOperationJournal(dir, ec);
            status = "verified_not_committed";
        } else {
            manual_review = true;
            status = "manual_review_required";
        }
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"operation\":\"checkin-batch\",\"status\":";
        appendJsonString(out, status);
        out << ",\"revision\":" << (revision != 0 ? std::to_string(revision) : std::string("null"))
            << ",\"author\":";
        appendJsonString(out, author);
        out << ",\"changed_paths_verified\":"
            << (verify.state == RemoveVerifyState::Committed ? "true" : "false")
            << ",\"reason_code\":";
        appendJsonString(out, verify.code);
        out << ",\"recovery_required\":" << (manual_review ? "true" : "false")
            << ",\"manual_review_required\":" << (manual_review ? "true" : "false") << "}\n";
        return 0;
    }

    // --- remove-batch: positive SVN-log attribution (never path-absence guessing).
    if (op == "remove-batch") {
        if (schema != "work-journal/v5") {
            // Legacy journal lacks the evidence needed for safe attribution.
            throw WorkError{"LEGACY_EVIDENCE_INSUFFICIENT", false, true, op_id};
        }
        RemoveEvidence ev;
        ev.op_id = op_id;
        ev.expected_author = jsonUnescape(journalScalar(content, "expected_author"));
        ev.target_url = jsonUnescape(journalScalar(content, "attribution_target_url"));
        ev.full_message = jsonUnescape(journalScalar(content, "full_message"));
        const auto pre = journalScalar(content, "pre_commit_head_revision");
        ev.pre_commit_head = pre.empty() ? -1 : std::stoll(pre);
        for (const auto& c : journalStringArray(content, "expected_changed"))
            ev.expected_changed.insert(jsonUnescape(c));
        for (const auto& item : items) ev.item_paths.push_back(item.path);

        const auto verify = verifyRemoveOutcome(runtime, ev);
        std::string status;
        bool manual_review = false;
        std::int64_t revision = 0;
        std::string author;
        if (verify.state == RemoveVerifyState::Committed) {
            // Finalize: the removal is proven; update workspace state and clean up.
            mutateWorkspaceState(runtime, [&](std::set<std::string>& state) {
                for (const auto& item : items) {
                    state.erase(relativeString(runtime.workspace, current.working_copy / item.path));
                }
            });
            for (const auto& item : items) {
                clearEditableReceipt(runtime, version, item.path);
            }
            removeOperationJournal(dir, ec);
            status = "verified_committed";
            revision = verify.revision;
            author = verify.author;
        } else if (verify.state == RemoveVerifyState::NotCommitted) {
            // Safe rollback: no matching commit and the target paths still exist.
            // svn revert is per-target (a directory revert is non-recursive), so
            // un-schedule each deleted file individually, then re-link.
            for (const auto& item : items) {
                const auto abs = current.working_copy / item.path;
                try { runtime.svn.revert(abs); } catch (const std::exception&) {}
                const auto backup = dir / "backup" / std::to_string(item.index);
                if (std::filesystem::exists(backup, ec)) {
                    std::filesystem::copy_file(backup, abs,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
            for (const auto& item : items) {
                if (item.link_state == "LOCAL") continue;
                const auto restored = current.working_copy / item.path;
                std::error_code restore_ec;
                if (std::filesystem::is_regular_file(restored, restore_ec) &&
                    !std::filesystem::is_symlink(restored, restore_ec)) {
                    relinkFile(runtime.store, restored);
                }
            }
            removeOperationJournal(dir, ec);
            status = "verified_not_committed";
        } else {
            // Mismatch / ambiguous / diverged / insufficient: keep journal+backups,
            // block, require manual review. Never auto-finalize or auto-rollback.
            manual_review = true;
            status = "manual_review_required";
        }
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"operation\":\"remove-batch\",\"status\":";
        appendJsonString(out, status);
        out << ",\"revision\":" << (revision != 0 ? std::to_string(revision) : std::string("null"))
            << ",\"author\":";
        appendJsonString(out, author);
        out << ",\"changed_paths_verified\":"
            << (verify.state == RemoveVerifyState::Committed ? "true" : "false")
            << ",\"reason_code\":";
        appendJsonString(out, verify.code);
        out << ",\"recovery_required\":" << (manual_review ? "true" : "false")
            << ",\"manual_review_required\":" << (manual_review ? "true" : "false") << "}\n";
        return 0;
    }

    if (phase == "verify") {
        // verify is defined for remove-batch outcomes only.
        throw WorkError{"STATE_INCONSISTENT", false, false, op_id};
    }

    // --- local operation restore: roll every item back to its pre-op state.
    std::set<std::string> state;
    for (const auto& item : items) {
        restoreItemFromBackup(runtime, op, dir, item.index,
                              current.working_copy / item.path, item.link_state, state);
    }
    WorkPlan restore_plan;
    restore_plan.items.reserve(items.size());
    for (const auto& item : items) {
        PlanItem plan_item;
        plan_item.path = item.path;
        plan_item.link_state = item.link_state;
        restore_plan.items.push_back(plan_item);
    }
    mergeLocalOperationWorkspaceState(runtime, current, op, restore_plan, true);
    removeOperationJournal(dir, ec);

    out << "{\"schema_version\":\"work-recovery-result/v1\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"operation\":";
    appendJsonString(out, op);
    out << ",\"terminal_state\":\"rolled_back\",\"revision\":null}\n";
    return 0;
}

// ===========================================================================
// Transactional external Import.
//
// Absolute external source names are deliberately confined to ImportRequest /
// ImportItem while a command is executing.  They are never rendered, returned,
// logged, or written to the destination-only recovery journal.
// ===========================================================================

constexpr std::size_t kImportMaxRequestBytes = 8U * 1024U * 1024U;
constexpr std::size_t kImportMaxSources = 4096;
constexpr std::size_t kImportMaxEntries = 250000;
constexpr std::size_t kImportMaxFiles = 50000;
constexpr std::size_t kImportMaxDirectories = 50000;
constexpr std::size_t kImportMaxMappings = 50000;
constexpr std::uint64_t kImportMaxTotalBytes = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kImportMaxOneFileBytes = 256ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr auto kImportPreviewWallBudget = std::chrono::minutes(30);
constexpr auto kImportApplyWallBudget = std::chrono::hours(4);

void ensureImportDeadline(const std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() > deadline) {
        throw WorkError{"IMPORT_READ_TIMEOUT", true, false, ""};
    }
}

// A small strict JSON reader keeps Import's stdin transport dependency-free.
// It rejects duplicate object keys, malformed escapes and trailing bytes; only
// the object/array/string subset needed by import-request/v1 is accepted by the
// request decoder below.
struct ImportJson {
    enum class Type { String, Object, Array, Boolean, Null, Number };
    Type type = Type::Null;
    std::string string;
    std::map<std::string, ImportJson> object;
    std::vector<ImportJson> array;
};

class ImportJsonReader {
public:
    explicit ImportJsonReader(const std::string& text) : text_(text) {}

    ImportJson parse() {
        skip();
        ImportJson value = valueAt();
        skip();
        if (pos_ != text_.size()) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        return value;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void skip() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            ++pos_;
    }
    char take() {
        if (pos_ >= text_.size()) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        return text_[pos_++];
    }
    void require(char expected) {
        if (take() != expected) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    }
    static void appendUtf8(std::string& output, unsigned codepoint) {
        if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    std::string stringAt() {
        require('"');
        std::string output;
        while (true) {
            const char c = take();
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20) {
                throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            }
            if (c != '\\') { output.push_back(c); continue; }
            const char escaped = take();
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
                    }
                    unsigned value = 0;
                    for (int i = 0; i != 4; ++i) {
                        const char h = text_[pos_++];
                        value <<= 4;
                        if (h >= '0' && h <= '9') value += static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') value += static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') value += static_cast<unsigned>(h - 'A' + 10);
                        else throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
                    }
                    if (value >= 0xd800 && value <= 0xdfff) {
                        throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
                    }
                    appendUtf8(output, value);
                    break;
                }
                default: throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            }
        }
    }
    ImportJson objectAt() {
        ImportJson result;
        result.type = ImportJson::Type::Object;
        require('{'); skip();
        if (pos_ < text_.size() && text_[pos_] == '}') { ++pos_; return result; }
        while (true) {
            skip();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            }
            const std::string key = stringAt();
            skip(); require(':'); skip();
            auto [it, inserted] = result.object.emplace(key, valueAt());
            if (!inserted) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            skip();
            const char next = take();
            if (next == '}') return result;
            if (next != ',') throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            skip();
        }
    }
    ImportJson arrayAt() {
        ImportJson result;
        result.type = ImportJson::Type::Array;
        require('['); skip();
        if (pos_ < text_.size() && text_[pos_] == ']') { ++pos_; return result; }
        while (true) {
            result.array.push_back(valueAt());
            skip();
            const char next = take();
            if (next == ']') return result;
            if (next != ',') throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            skip();
        }
    }
    ImportJson valueAt() {
        skip();
        if (pos_ >= text_.size()) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        if (text_[pos_] == '"') return ImportJson{ImportJson::Type::String, stringAt(), {}, {}};
        if (text_[pos_] == '{') return objectAt();
        if (text_[pos_] == '[') return arrayAt();
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4; return ImportJson{ImportJson::Type::Boolean, "true", {}, {}};
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5; return ImportJson{ImportJson::Type::Boolean, "false", {}, {}};
        }
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4; return ImportJson{};
        }
        // Numeric values are parsed only so unexpected known fields can still
        // be rejected by schema validation without accepting malformed JSON.
        const std::size_t begin = pos_;
        if (text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) ++pos_;
        if (begin == pos_) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        return ImportJson{ImportJson::Type::Number, text_.substr(begin, pos_ - begin), {}, {}};
    }
};

const ImportJson& importJsonField(const ImportJson& object, const std::string& key) {
    if (object.type != ImportJson::Type::Object) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    const auto it = object.object.find(key);
    if (it == object.object.end()) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    return it->second;
}

void importJsonOnlyKeys(const ImportJson& object, const std::set<std::string>& allowed) {
    if (object.type != ImportJson::Type::Object) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    for (const auto& [key, value] : object.object) {
        (void)value;
        if (allowed.count(key) == 0) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    }
}

bool importValidUtf8(const std::string& value) {
    // Reuse the bounded selector validator's strict UTF-8 policy without
    // applying its workspace-path policy to an external source.
    return isValidUtf8(value);
}

std::string decodeFileUrl(const std::string& url) {
    constexpr const char prefix[] = "file://";
    if (url.rfind(prefix, 0) != 0) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    const std::string encoded = url.substr(sizeof(prefix) - 1);
    if (encoded.empty() || encoded.front() != '/') {
        // file://host/path and file:relative are intentionally unsupported.
        throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    }
    std::string result;
    result.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') { result.push_back(encoded[i]); continue; }
        if (i + 2 >= encoded.size()) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        const auto hex = encoded.substr(i + 1, 2);
        unsigned value = 0;
        for (const char c : hex) {
            value <<= 4;
            if (c >= '0' && c <= '9') value += static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value += static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value += static_cast<unsigned>(c - 'A' + 10);
            else throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        }
        if (value == 0) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        result.push_back(static_cast<char>(value));
        i += 2;
    }
    if (!importValidUtf8(result)) throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    return result;
}

struct ImportRequest {
    std::string version;
    std::string destination;  // project-relative, empty = Active root
    std::vector<std::filesystem::path> sources;  // transient absolute paths only
    std::set<std::string> replace_local;
    std::string expected_plan;
};

std::string readImportRequestBytes(std::istream& in) {
    std::string request;
    std::array<char, 65536> buffer{};
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = in.gcount();
        if (count <= 0) break;
        if (static_cast<std::size_t>(count) > kImportMaxRequestBytes ||
            request.size() > kImportMaxRequestBytes - static_cast<std::size_t>(count)) {
            throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
        }
        request.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (request.empty() || !importValidUtf8(request)) {
        throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    }
    return request;
}

std::string normalizeImportDestination(const std::string& raw) {
    if (!importValidUtf8(raw) || raw.find('\0') != std::string::npos ||
        (!raw.empty() && raw.front() == '/')) {
        throw WorkError{"INVALID_DROP_TARGET", false, false, ""};
    }
    if (raw.empty()) return "";  // File Status blank-space target = Active root.
    const std::string normalized = std::filesystem::path(raw).lexically_normal().generic_string();
    if (normalized == ".") return "";
    if (normalized.empty() || normalized == ".." || normalized.rfind("../", 0) == 0) {
        throw WorkError{"INVALID_DROP_TARGET", false, false, ""};
    }
    for (const auto& component : std::filesystem::path(normalized)) {
        const auto text = component.generic_string();
        if (text.empty() || text == "." || text == ".." || text == ".svn" ||
            text == ".isvn" || text == ".isvn_local") {
            throw WorkError{"INVALID_DROP_TARGET", false, false, ""};
        }
    }
    return normalized;
}

ImportRequest parseImportRequest(std::istream& in, bool apply) {
    const ImportJson root = ImportJsonReader(readImportRequestBytes(in)).parse();
    importJsonOnlyKeys(root, apply
        ? std::set<std::string>{"schema", "target_version", "destination", "sources",
                                "expected_plan", "replace_local"}
        : std::set<std::string>{"schema", "target_version", "destination", "sources"});
    const auto schema = importJsonField(root, "schema");
    const auto version = importJsonField(root, "target_version");
    const auto destination = importJsonField(root, "destination");
    const auto sources = importJsonField(root, "sources");
    if (schema.type != ImportJson::Type::String ||
        schema.string != (apply ? "import-apply-request/v1" : "import-request/v1") ||
        version.type != ImportJson::Type::String || destination.type != ImportJson::Type::String ||
        sources.type != ImportJson::Type::Array || sources.array.empty()) {
        throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
    }
    ImportRequest request;
    request.version = version.string;
    request.destination = normalizeImportDestination(destination.string);
    if (sources.array.size() > kImportMaxSources) {
        throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
    }
    std::set<std::string> seen;
    for (const auto& source : sources.array) {
        importJsonOnlyKeys(source, {"source_url"});
        const auto value = importJsonField(source, "source_url");
        if (value.type != ImportJson::Type::String) {
            throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        }
        const auto absolute = std::filesystem::absolute(decodeFileUrl(value.string)).lexically_normal();
        const auto key = absolute.generic_string();
        if (seen.insert(key).second) request.sources.push_back(absolute);
    }
    if (request.sources.empty()) throw WorkError{"IMPORT_SOURCE_REQUIRED", false, false, ""};
    if (apply) {
        const auto expected = importJsonField(root, "expected_plan");
        const auto replacements = importJsonField(root, "replace_local");
        if (expected.type != ImportJson::Type::String || expected.string.empty() ||
            replacements.type != ImportJson::Type::Array) {
            throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
        }
        request.expected_plan = expected.string;
        for (const auto& replacement : replacements.array) {
            if (replacement.type != ImportJson::Type::String) {
                throw WorkError{"UNSUPPORTED_SOURCE_URL", false, false, ""};
            }
            request.replace_local.insert(normalizeImportDestination(replacement.string));
        }
    }
    return request;
}

bool importStatSame(const struct stat& lhs, const struct stat& rhs) {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
           lhs.st_size == rhs.st_size && lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec &&
           lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec &&
           lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec && lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec;
}

class ImportFd {
public:
    explicit ImportFd(int fd = -1) : fd_(fd) {}
    ~ImportFd() { if (fd_ >= 0) ::close(fd_); }
    ImportFd(const ImportFd&) = delete;
    ImportFd& operator=(const ImportFd&) = delete;
    ImportFd(ImportFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    ImportFd& operator=(ImportFd&& other) noexcept {
        if (this == &other) return *this;
        if (fd_ >= 0) ::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }
    int get() const { return fd_; }
    int release() { const int result = fd_; fd_ = -1; return result; }

private:
    int fd_;
};

// Resolve an absolute path from the root directory one component at a time.
// `O_NOFOLLOW` on only the final open is insufficient: an attacker could swap
// any ancestor after Preview.  Import uses this primitive for every source root
// and every re-open of the Active working-copy root.
std::vector<std::string> importAbsoluteComponents(const std::filesystem::path& path,
                                                   const std::string& failure_code) {
    if (!path.is_absolute()) throw WorkError{failure_code, false, false, ""};
    std::vector<std::string> result;
    for (const auto& component_path : path) {
        const std::string component = component_path.generic_string();
        if (component == "/" || component.empty()) continue;
        if (component == "." || component == ".." || component.find('\0') != std::string::npos) {
            throw WorkError{failure_code, false, false, ""};
        }
        result.push_back(component);
    }
    return result;
}

ImportFd openImportAbsoluteNoFollow(const std::filesystem::path& path, bool terminal_directory,
                                    const std::string& absent_code,
                                    const std::string& symlink_code,
                                    const std::string& type_code,
                                    const std::string& failure_code) {
    ImportFd current(::open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (current.get() < 0) throw WorkError{failure_code, false, false, ""};
    const auto components = importAbsoluteComponents(path, failure_code);
    for (std::size_t i = 0; i < components.size(); ++i) {
        struct stat status {};
        if (::fstatat(current.get(), components[i].c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            throw WorkError{errno == ENOENT ? absent_code : failure_code, false, false, ""};
        }
        if (S_ISLNK(status.st_mode)) throw WorkError{symlink_code, false, false, ""};
        const bool needs_directory = i + 1 < components.size() || terminal_directory;
        if (needs_directory && !S_ISDIR(status.st_mode)) {
            throw WorkError{type_code, false, false, ""};
        }
        const int next = ::openat(current.get(), components[i].c_str(),
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                                      (needs_directory ? O_DIRECTORY : 0));
        if (next < 0) throw WorkError{failure_code, false, false, ""};
        current = ImportFd(next);
    }
    if (terminal_directory) {
        struct stat status {};
        if (::fstat(current.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
            throw WorkError{type_code, false, false, ""};
        }
    }
    return current;
}

bool importLstatBelowNoFollowFd(int root_fd, const std::string& relative, struct stat& status) {
    ImportFd current(::dup(root_fd));
    if (current.get() < 0) throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    std::vector<std::string> components;
    for (const auto& component_path : std::filesystem::path(relative)) {
        const std::string component = component_path.generic_string();
        if (component.empty() || component == "." || component == "..") {
            throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
        }
        components.push_back(component);
    }
    if (components.empty()) return ::fstat(current.get(), &status) == 0;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        struct stat parent_status {};
        if (::fstatat(current.get(), components[i].c_str(), &parent_status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) return false;
            throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
        }
        if (S_ISLNK(parent_status.st_mode)) {
            throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
        }
        if (!S_ISDIR(parent_status.st_mode)) {
            throw WorkError{"DESTINATION_TYPE_CONFLICT", false, false, ""};
        }
        const int next = ::openat(current.get(), components[i].c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
        current = ImportFd(next);
    }
    if (::fstatat(current.get(), components.back().c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) return true;
    if (errno == ENOENT) return false;
    throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
}

std::string importRelativeJoin(const std::string& root, const std::string& child) {
    if (root.empty()) return child;
    if (child.empty()) return root;
    return root + "/" + child;
}

struct ImportItem {
    std::string source_id;
    std::string source_relative;             // safe root-relative display label
    std::filesystem::path source_absolute;   // transient; never serialized
    std::filesystem::path source_root;       // transient; reopened from / with no-follow
    struct stat source_root_stat {};
    struct stat source_stat {};
    std::string source_fingerprint;
    std::string destination;
    std::uint64_t size = 0;
    bool destination_exists = false;
    std::uint64_t destination_size = 0;
    bool destination_managed = false;
    bool destination_replaceable = false;
    bool replace_selected = false;
    std::string destination_fingerprint;
    bool allowed = true;
    std::string reason_code = "OK";
};

struct ImportSource {
    std::string id;
    std::string label;
    std::string kind;
    std::filesystem::path absolute;  // transient only, never emitted/journaled
    struct stat root_stat {};
};

struct ImportPlan {
    std::string version;
    std::string destination;
    struct stat target_root_stat {};
    std::vector<ImportSource> sources;
    std::vector<ImportItem> items;
    std::size_t directory_count = 0;
    std::uint64_t total_bytes = 0;
    std::size_t duplicate_roots = 0;
    std::string plan_hash;
    bool can_apply = false;
};

std::string importPlanHash(const Runtime& runtime, const CurrentVersion& current,
                           const ImportPlan& plan) {
    std::ostringstream serialized;
    serialized << "import/v1\n" << projectIdentityHash(runtime) << '\n'
               << stableWorkspaceId(current.version) << '\n' << current.version << '\n'
               << plan.destination << '\n' << plan.target_root_stat.st_dev << ':'
               << plan.target_root_stat.st_ino << ':' << plan.target_root_stat.st_mtim.tv_sec << ':'
               << plan.target_root_stat.st_mtim.tv_nsec << ':' << plan.target_root_stat.st_ctim.tv_sec << ':'
               << plan.target_root_stat.st_ctim.tv_nsec << '\n' << "resource-policy/import-v1\n";
    for (const auto& source : plan.sources) {
        serialized << source.id << '|' << source.label << '|' << source.kind << '|'
                   << source.absolute.generic_string() << '|'
                   << source.root_stat.st_dev << ':' << source.root_stat.st_ino << ':'
                   << source.root_stat.st_mtim.tv_sec << ':' << source.root_stat.st_mtim.tv_nsec << ':'
                   << source.root_stat.st_ctim.tv_sec << ':' << source.root_stat.st_ctim.tv_nsec << '\n';
    }
    for (const auto& item : plan.items) {
        serialized << item.source_id << '|' << item.source_root.generic_string() << '|'
                   << item.source_relative << '|' << item.destination << '|'
                   << item.source_stat.st_dev << ':' << item.source_stat.st_ino << ':'
                   << item.source_stat.st_size << ':' << item.source_stat.st_mtim.tv_sec << ':'
                   << item.source_stat.st_mtim.tv_nsec << ':' << item.source_stat.st_ctim.tv_sec << ':'
                   << item.source_stat.st_ctim.tv_nsec << '|' << item.source_fingerprint << '|'
                   << (item.destination_exists ? '1' : '0') << '|'
                   << (item.destination_managed ? '1' : '0') << '|'
                   << (item.destination_replaceable ? '1' : '0') << '|'
                   << item.destination_size << '|' << item.destination_fingerprint << '\n';
    }
    return common::blake3_hex(serialized.str());
}

std::map<std::string, std::string> importStatusEvidence(
    const Runtime& runtime, const CurrentVersion& current,
    const std::vector<std::filesystem::path>& destinations,
    CheckinQueryContext& query) {
    std::map<std::string, std::string> result;
    try {
        for (const auto& entry : runtime.svn.statusBounded(destinations, query.svn_limits,
                                                            query.svn_budget)) {
            const auto logical = svnStatusLogicalPath(current, entry.path);
            if (logical.has_value()) result[*logical] = entry.status;
        }
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error);
    }
    return result;
}

std::map<std::string, bool> importRemoteEvidence(const Runtime& runtime,
                                                  const std::string& target_url,
                                                  const std::set<std::string>& paths,
                                                  CheckinQueryContext& query) {
    std::map<std::string, std::set<std::string>> by_parent;
    for (const auto& path : paths) by_parent[relativeParent(path)].insert(relativeLeaf(path));
    query.remoteNodes(paths.size() + by_parent.size());
    std::map<std::string, bool> result;
    try {
        for (const auto& [parent, leaves] : by_parent) {
            const auto listed = runtime.svn.listImmediateBounded(
                urlForRelative(target_url, parent), query.svn_limits, query.svn_budget);
            std::set<std::string> present;
            if (listed.exists) {
                for (const auto& entry : listed.entries) present.insert(entry.name);
            }
            for (const auto& leaf : leaves) {
                result[importRelativeJoin(parent, leaf)] =
                    listed.exists && present.count(leaf) != 0;
            }
        }
    } catch (const version::SvnQueryError& error) {
        throwCheckinSvnFailure(error);
    }
    return result;
}

bool importPathInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    const auto c = std::filesystem::absolute(child).lexically_normal().generic_string();
    const auto p = std::filesystem::absolute(parent).lexically_normal().generic_string();
    return c == p || (c.size() > p.size() && c.compare(0, p.size(), p) == 0 && c[p.size()] == '/');
}

// Inspect a target-relative path without ever following a destination symlink.
// This is used during Preview as well as again immediately before publication;
// lexical containment alone is never sufficient for Import.
bool importLstatBelowNoFollow(const std::filesystem::path& root, const std::string& relative,
                              struct stat& status) {
    ImportFd root_fd(openImportAbsoluteNoFollow(root, true, "PATH_OUTSIDE_WORKSPACE",
                                                 "DESTINATION_SYMLINK_BLOCKED",
                                                 "DESTINATION_TYPE_CONFLICT",
                                                 "PATH_OUTSIDE_WORKSPACE"));
    return importLstatBelowNoFollowFd(root_fd.get(), relative, status);
}

void addImportRegular(ImportPlan& plan, const std::string& source_id,
                      const std::filesystem::path& source_root,
                      const struct stat& source_root_stat, const std::string& relative,
                      int file_fd, const std::string& destination,
                      const std::chrono::steady_clock::time_point deadline) {
    ensureImportDeadline(deadline);
    if (plan.items.size() >= kImportMaxFiles || plan.items.size() >= kImportMaxMappings) {
        throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
    }
    struct stat status {};
    if (::fstat(file_fd, &status) != 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    if (!S_ISREG(status.st_mode)) throw WorkError{"SOURCE_SPECIAL_FILE_UNSUPPORTED", false, false, ""};
    if (status.st_size < 0) throw WorkError{"IMPORT_SIZE_LIMIT_EXCEEDED", false, false, ""};
    const auto bytes = static_cast<std::uint64_t>(status.st_size);
    if (bytes > kImportMaxOneFileBytes || plan.total_bytes > kImportMaxTotalBytes - bytes) {
        throw WorkError{"IMPORT_SIZE_LIMIT_EXCEEDED", false, false, ""};
    }
    ImportItem item;
    item.source_id = source_id;
    item.source_relative = relative;
    item.source_root = source_root;
    item.source_root_stat = source_root_stat;
    item.source_absolute = relative.empty() ? source_root : source_root / relative;
    item.source_stat = status;
    try {
        item.source_fingerprint = common::blake3_fd_hex(file_fd);
    } catch (...) {
        // Do not let a path-bearing filesystem exception escape into a GUI
        // stderr surface.  Source paths are transient/private inputs.
        throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    }
    ensureImportDeadline(deadline);
    item.destination = destination;
    item.size = bytes;
    plan.total_bytes += bytes;
    plan.items.push_back(std::move(item));
}

void collectImportDirectory(ImportPlan& plan, const std::string& source_id,
                            const std::filesystem::path& source_root,
                            const struct stat& source_root_stat, int directory_fd,
                            const std::string& source_relative,
                            const std::string& mapped_root, std::size_t& visited,
                            const std::chrono::steady_clock::time_point deadline) {
    ImportFd iterator_fd(::dup(directory_fd));
    if (iterator_fd.get() < 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    DIR* raw_directory = ::fdopendir(iterator_fd.release());
    if (raw_directory == nullptr) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    std::vector<std::string> names;
    errno = 0;
    while (dirent* entry = ::readdir(raw_directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    const int read_error = errno;
    ::closedir(raw_directory);  // closes the duplicated descriptor
    if (read_error != 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    if (names.empty()) throw WorkError{"EMPTY_DIRECTORY_UNSUPPORTED", false, false, ""};
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        ensureImportDeadline(deadline);
        if (++visited > kImportMaxEntries) {
            throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
        }
        struct stat status {};
        if (::fstatat(directory_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            throw WorkError{errno == ENOENT ? "SOURCE_NOT_FOUND" : "SOURCE_UNREADABLE", false, false, ""};
        }
        if (S_ISLNK(status.st_mode)) throw WorkError{"SOURCE_SYMLINK_UNSUPPORTED", false, false, ""};
        const std::string relative = source_relative.empty() ? name
                                                              : source_relative + "/" + name;
        if (S_ISDIR(status.st_mode)) {
            if (++plan.directory_count > kImportMaxDirectories) {
                throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
            }
            ImportFd child(::openat(directory_fd, name.c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.get() < 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
            collectImportDirectory(plan, source_id, source_root, source_root_stat, child.get(), relative,
                                   mapped_root, visited, deadline);
            continue;
        }
        if (!S_ISREG(status.st_mode)) throw WorkError{"SOURCE_SPECIAL_FILE_UNSUPPORTED", false, false, ""};
        ImportFd file(::openat(directory_fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (file.get() < 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
        addImportRegular(plan, source_id, source_root, source_root_stat, relative, file.get(),
                         importRelativeJoin(mapped_root, relative), deadline);
    }
}

void collectImportSource(ImportPlan& plan, const std::filesystem::path& source,
                         const std::string& source_id, const std::string& destination_root,
                         std::size_t& visited,
                         const std::chrono::steady_clock::time_point deadline) {
    ensureImportDeadline(deadline);
    struct stat root_status {};
    ImportFd root(openImportAbsoluteNoFollow(source, false, "SOURCE_NOT_FOUND",
                                             "SOURCE_SYMLINK_UNSUPPORTED",
                                             "SOURCE_SPECIAL_FILE_UNSUPPORTED", "SOURCE_UNREADABLE"));
    if (::fstat(root.get(), &root_status) != 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
    const std::string basename = source.filename().generic_string();
    if (basename.empty() || basename == "." || basename == "..") {
        throw WorkError{"SOURCE_NOT_FOUND", false, false, ""};
    }
    if (S_ISLNK(root_status.st_mode)) throw WorkError{"SOURCE_SYMLINK_UNSUPPORTED", false, false, ""};
    if (S_ISREG(root_status.st_mode)) {
        plan.sources.push_back(ImportSource{source_id, basename, "file", source, root_status});
        if (++visited > kImportMaxEntries) throw WorkError{"IMPORT_ENTRY_LIMIT_EXCEEDED", false, false, ""};
        addImportRegular(plan, source_id, source, root_status, "", root.get(),
                         importRelativeJoin(destination_root, basename), deadline);
        return;
    }
    if (!S_ISDIR(root_status.st_mode)) throw WorkError{"SOURCE_SPECIAL_FILE_UNSUPPORTED", false, false, ""};
    plan.sources.push_back(ImportSource{source_id, basename, "directory", source, root_status});
    const auto mapped_root = importRelativeJoin(destination_root, basename);
    ++plan.directory_count;
    collectImportDirectory(plan, source_id, source, root_status, root.get(), "", mapped_root, visited,
                           deadline);
}

ImportPlan buildImportPlan(const Runtime& runtime, const CurrentVersion& current,
                           const ImportRequest& request, bool applying_choices,
                           const std::chrono::steady_clock::time_point deadline) {
    ensureImportDeadline(deadline);
    if (request.version != current.version) throw WorkError{"TARGET_MISMATCH", false, false, ""};
    ImportPlan plan;
    plan.version = current.version;
    plan.destination = request.destination;
    ImportFd active_root(openImportAbsoluteNoFollow(current.working_copy, true, "PATH_OUTSIDE_WORKSPACE",
                                                    "DESTINATION_SYMLINK_BLOCKED",
                                                    "DESTINATION_TYPE_CONFLICT",
                                                    "PATH_OUTSIDE_WORKSPACE"));
    if (::fstat(active_root.get(), &plan.target_root_stat) != 0) {
        throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    }
    struct stat destination_status {};
    if (!importLstatBelowNoFollowFd(active_root.get(), request.destination, destination_status) ||
        !S_ISDIR(destination_status.st_mode) ||
        S_ISLNK(destination_status.st_mode)) {
        throw WorkError{"INVALID_DROP_TARGET", false, false, ""};
    }

    std::vector<std::filesystem::path> sources = request.sources;
    std::sort(sources.begin(), sources.end());
    std::size_t visited_sources = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = i + 1; j < sources.size(); ++j) {
            if (importPathInside(sources[j], sources[i])) {
                throw WorkError{"OVERLAPPING_SOURCE_ROOTS", false, false, ""};
            }
        }
        if (importPathInside(sources[i], current.working_copy) ||
            importPathInside(current.working_copy, sources[i])) {
            throw WorkError{"SOURCE_TARGET_OVERLAP", false, false, ""};
        }
        std::ostringstream source_id;
        source_id << "source-" << std::setw(4) << std::setfill('0') << i + 1;
        collectImportSource(plan, sources[i], source_id.str(), request.destination, visited_sources, deadline);
    }
    std::sort(plan.items.begin(), plan.items.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.destination != rhs.destination) return lhs.destination < rhs.destination;
        return lhs.source_id < rhs.source_id;
    });
    for (std::size_t i = 1; i < plan.items.size(); ++i) {
        if (plan.items[i - 1].destination == plan.items[i].destination) {
            plan.items[i - 1].allowed = false;
            plan.items[i - 1].reason_code = "IMPORT_MAPPING_COLLISION";
            plan.items[i].allowed = false;
            plan.items[i].reason_code = "IMPORT_MAPPING_COLLISION";
        }
    }

    std::vector<std::filesystem::path> destination_paths;
    std::set<std::string> destination_names;
    for (const auto& item : plan.items) {
        destination_paths.push_back(current.working_copy / item.destination);
        destination_names.insert(item.destination);
    }
    CheckinQueryContext query;
    const auto statuses = importStatusEvidence(runtime, current, destination_paths, query);
    ensureImportDeadline(deadline);
    const auto remote = importRemoteEvidence(runtime, runtime.svn.versionToUrl(current.version),
                                             destination_names, query);
    const auto local = readWorkspaceState(runtime.workspace);
    for (auto& item : plan.items) {
        ensureImportDeadline(deadline);
        if (!item.allowed) continue;
        const auto destination = current.working_copy / item.destination;
        struct stat status {};
        item.destination_exists = importLstatBelowNoFollow(current.working_copy, item.destination, status);
        const bool symlink = item.destination_exists && S_ISLNK(status.st_mode);
        const bool regular = item.destination_exists && S_ISREG(status.st_mode);
        const auto workspace_relative = lexicalRelativeString(runtime.workspace, destination);
        const bool linked = symlink &&
            poolHashFromSymlinkIfAny(destination, runtime.config.pool_root).has_value();
        const auto status_it = statuses.find(item.destination);
        const std::string svn_state = status_it == statuses.end() ? "" : status_it->second;
        const bool remote_exists = remote.count(item.destination) != 0 && remote.at(item.destination);
        const bool receipt = readEditableReceipt(runtime, current.version, item.destination).has_value();
        const bool managed = linked || remote_exists || receipt ||
                             local.count(workspace_relative) != 0 ||
                             (!svn_state.empty() && svn_state != "?");
        item.destination_managed = managed;
        if (symlink) {
            item.allowed = false;
            item.reason_code = linked ? "MANAGED_DESTINATION_BLOCKED"
                                      : "DESTINATION_SYMLINK_BLOCKED";
        } else if (item.destination_exists && !regular) {
            item.allowed = false;
            item.reason_code = "DESTINATION_TYPE_CONFLICT";
        } else if (!item.destination_exists && managed) {
            item.allowed = false;
            item.reason_code = "MANAGED_DESTINATION_BLOCKED";
        } else if (item.destination_exists && managed) {
            item.allowed = false;
            item.reason_code = "MANAGED_DESTINATION_BLOCKED";
        } else if (item.destination_exists) {
            item.destination_replaceable = true;
            item.destination_size = static_cast<std::uint64_t>(status.st_size);
            try {
                item.destination_fingerprint = common::blake3_file_hex(destination);
            } catch (const std::exception&) {
                throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
            }
            item.replace_selected = applying_choices && request.replace_local.count(item.destination) != 0;
            if (!item.replace_selected) {
                item.allowed = false;
                item.reason_code = "REPLACE_LOCAL_REQUIRED";
            }
        }
    }
    if (applying_choices) {
        for (const auto& replacement : request.replace_local) {
            const auto found = std::find_if(plan.items.begin(), plan.items.end(), [&](const auto& item) {
                return item.destination == replacement;
            });
            if (found == plan.items.end() || !found->destination_replaceable) {
                throw WorkError{"REPLACE_LOCAL_NOT_ALLOWED", false, false, ""};
            }
        }
    }
    plan.can_apply = !plan.items.empty() &&
        std::all_of(plan.items.begin(), plan.items.end(), [](const auto& item) {
            return item.allowed;
        });
    plan.plan_hash = importPlanHash(runtime, current, plan);
    return plan;
}

void appendImportPlan(std::ostream& out, const Runtime&, const CurrentVersion& current,
                      const ImportPlan& plan) {
    out << "{\"schema\":\"import/v1\",\"target\":{\"workspace_id\":";
    appendJsonString(out, stableWorkspaceId(current.version));
    out << ",\"version\":"; appendJsonString(out, current.version);
    out << ",\"destination\":"; appendJsonString(out, plan.destination);
    out << "},\"sources\":[";
    for (std::size_t i = 0; i < plan.sources.size(); ++i) {
        if (i) out << ",";
        out << "{\"source_id\":"; appendJsonString(out, plan.sources[i].id);
        out << ",\"label\":"; appendJsonString(out, plan.sources[i].label);
        out << ",\"kind\":"; appendJsonString(out, plan.sources[i].kind); out << "}";
    }
    out << "],\"items\":[";
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        if (i) out << ",";
        const auto& item = plan.items[i];
        out << "{\"source_id\":"; appendJsonString(out, item.source_id);
        out << ",\"source_relative\":"; appendJsonString(out, item.source_relative);
        out << ",\"destination\":"; appendJsonString(out, item.destination);
        out << ",\"kind\":\"file\",\"size\":" << item.size;
        out << ",\"destination_state\":";
        appendJsonString(out, item.destination_managed ? "managed"
                         : item.destination_exists ? "local" : "absent");
        out << ",\"allowed\":" << (item.allowed ? "true" : "false");
        out << ",\"replace_local_available\":"
            << (item.destination_replaceable ? "true" : "false");
        out << ",\"reason_code\":"; appendJsonString(out, item.reason_code);
        out << ",\"reason_params\":{}}";
    }
    const auto blocked = std::count_if(plan.items.begin(), plan.items.end(),
                                       [](const auto& item) { return !item.allowed; });
    const auto replaceable = std::count_if(plan.items.begin(), plan.items.end(),
                                           [](const auto& item) { return item.destination_replaceable; });
    out << "],\"summary\":{\"source_root_count\":" << plan.sources.size()
        << ",\"file_count\":" << plan.items.size()
        << ",\"directory_count\":" << plan.directory_count
        << ",\"total_size\":" << plan.total_bytes
        << ",\"new_count\":" << (plan.items.size() - replaceable)
        << ",\"replaceable_local_count\":" << replaceable
        << ",\"blocked_count\":" << blocked
        << "},\"can_apply\":" << (plan.can_apply ? "true" : "false")
        << ",\"plan_hash\":"; appendJsonString(out, plan.plan_hash);
    out << "}\n";
}

struct ImportTargetRoot {
    std::filesystem::path path;
    ImportFd fd;
    struct stat identity {};
};

ImportTargetRoot openImportTargetRootNoFollow(const std::filesystem::path& path) {
    ImportTargetRoot result;
    result.path = path;
    result.fd = openImportAbsoluteNoFollow(path, true, "PATH_OUTSIDE_WORKSPACE",
                                           "DESTINATION_SYMLINK_BLOCKED",
                                           "DESTINATION_TYPE_CONFLICT",
                                           "PATH_OUTSIDE_WORKSPACE");
    if (::fstat(result.fd.get(), &result.identity) != 0) {
        throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    }
    return result;
}

void verifyImportTargetRootNoFollow(const ImportTargetRoot& root, bool recovery) {
    ImportFd reopened(openImportAbsoluteNoFollow(root.path, true, "PATH_OUTSIDE_WORKSPACE",
                                                 "DESTINATION_SYMLINK_BLOCKED",
                                                 "DESTINATION_TYPE_CONFLICT",
                                                 "PATH_OUTSIDE_WORKSPACE"));
    struct stat current {};
    if (::fstat(reopened.get(), &current) != 0 ||
        current.st_dev != root.identity.st_dev || current.st_ino != root.identity.st_ino) {
        throw WorkError{recovery ? "RECOVERY_EVIDENCE_MISMATCH" : "PLAN_STALE", !recovery,
                        recovery, ""};
    }
}

int openImportParentNoFollow(int root_fd, const std::string& relative,
                             bool create, std::set<std::string>& created_dirs) {
    ImportFd current(::dup(root_fd));
    if (current.get() < 0) throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
    std::string accumulated;
    const auto parent = std::filesystem::path(relative).parent_path();
    for (const auto& component_path : parent) {
        const std::string component = component_path.generic_string();
        if (component.empty() || component == "." || component == "..") {
            throw WorkError{"PATH_OUTSIDE_WORKSPACE", false, false, ""};
        }
        struct stat status {};
        if (::fstatat(current.get(), component.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT || !create || ::mkdirat(current.get(), component.c_str(), 0750) != 0) {
                throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
            }
            accumulated = importRelativeJoin(accumulated, component);
            created_dirs.insert(accumulated);
        } else if (S_ISLNK(status.st_mode) || !S_ISDIR(status.st_mode)) {
            throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
        } else {
            accumulated = importRelativeJoin(accumulated, component);
        }
        const int next = ::openat(current.get(), component.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, ""};
        current = ImportFd(next);
    }
    return current.release();
}

ImportFd openImportSourceItemNoFollow(const ImportItem& item) {
    ImportFd root(openImportAbsoluteNoFollow(item.source_root, false, "SOURCE_NOT_FOUND",
                                             "SOURCE_SYMLINK_UNSUPPORTED",
                                             "SOURCE_SPECIAL_FILE_UNSUPPORTED", "SOURCE_UNREADABLE"));
    struct stat root_status {};
    if (::fstat(root.get(), &root_status) != 0 || !importStatSame(root_status, item.source_root_stat)) {
        throw WorkError{"PLAN_STALE", true, false, ""};
    }
    if (item.source_relative.empty()) return root;
    if (!S_ISDIR(root_status.st_mode)) throw WorkError{"PLAN_STALE", true, false, ""};
    ImportFd current(std::move(root));
    std::vector<std::string> components;
    for (const auto& component_path : std::filesystem::path(item.source_relative)) {
        const std::string component = component_path.generic_string();
        if (component.empty() || component == "." || component == "..") {
            throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
        }
        components.push_back(component);
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        struct stat status {};
        if (::fstatat(current.get(), components[i].c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            throw WorkError{errno == ENOENT ? "PLAN_STALE" : "SOURCE_UNREADABLE", true, false, ""};
        }
        if (S_ISLNK(status.st_mode)) throw WorkError{"SOURCE_SYMLINK_UNSUPPORTED", false, false, ""};
        const bool needs_directory = i + 1 < components.size();
        if (needs_directory && !S_ISDIR(status.st_mode)) throw WorkError{"PLAN_STALE", true, false, ""};
        const int next = ::openat(current.get(), components[i].c_str(),
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                                      (needs_directory ? O_DIRECTORY : 0));
        if (next < 0) throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
        current = ImportFd(next);
    }
    return current;
}

void verifyImportSourceRootsNoFollow(const ImportPlan& plan) {
    for (const auto& source : plan.sources) {
        ImportFd root(openImportAbsoluteNoFollow(source.absolute, false, "SOURCE_NOT_FOUND",
                                                 "SOURCE_SYMLINK_UNSUPPORTED",
                                                 "SOURCE_SPECIAL_FILE_UNSUPPORTED", "SOURCE_UNREADABLE"));
        struct stat status {};
        if (::fstat(root.get(), &status) != 0 || !importStatSame(status, source.root_stat)) {
            throw WorkError{"PLAN_STALE", true, false, ""};
        }
    }
}

std::string copyImportRegularNoFollow(const ImportItem& item, int temp_fd, const std::string& temp_name,
                                      const std::chrono::steady_clock::time_point deadline) {
    ImportFd source(openImportSourceItemNoFollow(item));
    struct stat before {};
    if (::fstat(source.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
        !importStatSame(before, item.source_stat)) {
        throw WorkError{"PLAN_STALE", true, false, ""};
    }
    ImportFd output(::openat(temp_fd, temp_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                             0640));
    if (output.get() < 0) throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    std::array<char, 65536> buffer{};
    auto last_progress = std::chrono::steady_clock::now();
    while (true) {
        ensureImportDeadline(deadline);
        const ssize_t read_count = ::read(source.get(), buffer.data(), buffer.size());
        if (read_count == 0) break;
        if (read_count < 0) {
            if (errno == EINTR) continue;
            throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
        }
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(read_count)) {
            const ssize_t write_count = ::write(output.get(), buffer.data() + written,
                                                 static_cast<std::size_t>(read_count) - written);
            if (write_count < 0) {
                if (errno == EINTR) continue;
                throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
            }
            written += static_cast<std::size_t>(write_count);
        }
        if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(60)) {
            throw WorkError{"IMPORT_READ_TIMEOUT", true, false, ""};
        }
        last_progress = std::chrono::steady_clock::now();
    }
    if (::fsync(output.get()) != 0) throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    struct stat after {};
    if (::fstat(source.get(), &after) != 0 || !importStatSame(before, after)) {
        throw WorkError{"PLAN_STALE", true, false, ""};
    }
    ImportFd verified_temp(::openat(temp_fd, temp_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (verified_temp.get() < 0) throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    const auto fingerprint = common::blake3_fd_hex(verified_temp.get());
    ensureImportDeadline(deadline);
    if (fingerprint != item.source_fingerprint) throw WorkError{"PLAN_STALE", true, false, ""};
    return fingerprint;
}

struct ImportJournalItem {
    std::size_t index = 0;
    std::string destination;
    std::string fingerprint;
    bool existed = false;
    bool backup_taken = false;
    bool published = false;
};

void writeImportJournal(const Runtime& runtime, const std::string& operation_id,
                        const CurrentVersion& current, const ImportTargetRoot& target_root,
                        const ImportPlan& plan,
                        const std::string& state, const std::vector<ImportJournalItem>& items,
                        const std::set<std::string>& created_dirs) {
    const auto dir = journalDir(runtime, operation_id);
    std::error_code ec;
    std::filesystem::create_directories(dir / "backup", ec);
    std::filesystem::create_directories(dir / "temp", ec);
    if (ec) throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, operation_id};
    ::chmod(dir.c_str(), 0700); ::chmod((dir / "backup").c_str(), 0700); ::chmod((dir / "temp").c_str(), 0700);
    std::ostringstream content;
    const TargetPin pin{current.version,
                        std::filesystem::absolute(target_root.path).lexically_normal(),
                        static_cast<std::uintmax_t>(target_root.identity.st_dev),
                        static_cast<std::uintmax_t>(target_root.identity.st_ino),
                        runtime.svn.versionToUrl(current.version)};
    if (!targetPinMatches(pin)) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, operation_id};
    }
    content << "{\n\"schema_version\":\"work-journal/v5\",\n\"operation_id\":";
    appendJsonString(content, operation_id);
    content << ",\n\"operation_type\":\"import\",\n\"project_hash\":";
    appendJsonString(content, projectIdentityHash(runtime));
    content << ",\n\"workspace_id\":"; appendJsonString(content, stableWorkspaceId(current.version));
    content << ",\n\"target_version\":"; appendJsonString(content, current.version);
    content << ",\n\"lock_scope\":\"VERSION\"";
    content << ",\n\"target_root\":"; appendJsonString(content, pin.root.string());
    content << ",\n\"target_root_device\":" << pin.device;
    content << ",\n\"target_root_inode\":" << pin.inode;
    content << ",\n\"target_url\":"; appendJsonString(content, pin.target_url);
    content << ",\n\"working_copy_dev\":"
            << static_cast<std::uintmax_t>(target_root.identity.st_dev);
    content << ",\n\"working_copy_ino\":"
            << static_cast<std::uintmax_t>(target_root.identity.st_ino);
    content << ",\n\"destination\":"; appendJsonString(content, plan.destination);
    content << ",\n\"state\":"; appendJsonString(content, state);
    content << ",\n\"plan_hash\":"; appendJsonString(content, plan.plan_hash);
    content << ",\n\"items\":[\n";
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        content << "{\"index\":" << item.index << ",\"destination\":";
        appendJsonString(content, item.destination);
        content << ",\"fingerprint\":"; appendJsonString(content, item.fingerprint);
        content << ",\"existed\":" << (item.existed ? "true" : "false")
                << ",\"backup_taken\":" << (item.backup_taken ? "true" : "false")
                << ",\"published\":" << (item.published ? "true" : "false") << "}"
                << (i + 1 == items.size() ? "" : ",") << "\n";
    }
    content << "],\n\"created_dirs\":[";
    bool first = true;
    for (const auto& created : created_dirs) {
        if (!first) content << ",";
        first = false; appendJsonString(content, created);
    }
    content << "]\n}\n";
    const auto tmp = dir / "journal.json.tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file << content.str(); file.flush();
        if (!file) throw WorkError{"STATE_INCONSISTENT", false, false, operation_id};
    }
    ::chmod(tmp.c_str(), 0600);
    const int journal_fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
    if (journal_fd < 0 || ::fsync(journal_fd) != 0) {
        if (journal_fd >= 0) ::close(journal_fd);
        throw WorkError{"STATE_INCONSISTENT", false, false, operation_id};
    }
    ::close(journal_fd);
    std::filesystem::rename(tmp, dir / "journal.json", ec);
    if (ec) throw WorkError{"STATE_INCONSISTENT", false, false, operation_id};
    common::fsync_directory(dir);
}

void preflightImportSpace(const Runtime& runtime, const CurrentVersion& current,
                          const ImportPlan& plan) {
    // Fail-only test seam for the pre-publication capacity gate.  It cannot
    // relax a production ceiling and is ignored unless a test explicitly sets
    // the marker before spawning the CLI.
    if (const char* forced = std::getenv("ISVN_IMPORT_TEST_FORCE_NO_SPACE");
        forced != nullptr && std::string(forced) == "1") {
        throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    }
    struct stat workspace_status {}, ops_status {};
    const auto ops = workOpsRoot(runtime.workspace);
    std::error_code ec;
    std::filesystem::create_directories(ops, ec);
    if (ec || ::stat(current.working_copy.c_str(), &workspace_status) != 0 ||
        ::stat(ops.c_str(), &ops_status) != 0 || workspace_status.st_dev != ops_status.st_dev) {
        throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    }
    std::uint64_t replacements = 0;
    for (const auto& item : plan.items) {
        if (item.destination_replaceable) {
            if (item.destination_fingerprint.empty()) throw WorkError{"PLAN_STALE", true, false, ""};
            if (replacements > std::numeric_limits<std::uint64_t>::max() - item.destination_size) {
                throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
            }
            replacements += item.destination_size;
        }
    }
    const std::uint64_t reserve = std::max<std::uint64_t>(1024ULL * 1024ULL * 1024ULL,
                                                           plan.total_bytes / 20ULL);
    if (plan.total_bytes > std::numeric_limits<std::uint64_t>::max() - replacements ||
        plan.total_bytes + replacements > std::numeric_limits<std::uint64_t>::max() - reserve) {
        throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    }
    const auto space = std::filesystem::space(ops, ec);
    if (ec || space.available < plan.total_bytes + replacements + reserve) {
        throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, ""};
    }
}

std::string importFingerprintAtNoFollow(int parent_fd, const std::string& leaf,
                                        const WorkError& failure) {
    ImportFd file(::openat(parent_fd, leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (file.get() < 0) throw failure;
    struct stat status {};
    if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw failure;
    }
    try {
        return common::blake3_fd_hex(file.get());
    } catch (const std::exception&) {
        throw failure;
    }
}

void rollbackImport(const ImportTargetRoot& target_root, const std::filesystem::path& dir,
                    const std::vector<ImportJournalItem>& journal,
                    const std::set<std::string>& created_dirs, bool strict) {
    verifyImportTargetRootNoFollow(target_root, true);
    ImportFd backup(::open((dir / "backup").c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (backup.get() < 0) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
    for (std::size_t i = journal.size(); i-- > 0;) {
        const auto& item = journal[i];
        std::set<std::string> no_created_dirs;
        ImportFd parent(openImportParentNoFollow(target_root.fd.get(), item.destination, false,
                                                  no_created_dirs));
        const std::string leaf = std::filesystem::path(item.destination).filename().generic_string();
        struct stat status {};
        const bool exists = ::fstatat(parent.get(), leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0;
        if (exists && S_ISLNK(status.st_mode)) {
            if (strict) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
            continue;
        }
        if (exists && (item.published || (S_ISREG(status.st_mode) &&
                                          importFingerprintAtNoFollow(parent.get(), leaf,
                                                                      {"RECOVERY_EVIDENCE_MISMATCH", false, true, ""}) == item.fingerprint))) {
            if (!S_ISREG(status.st_mode) ||
                importFingerprintAtNoFollow(parent.get(), leaf,
                                             {"RECOVERY_EVIDENCE_MISMATCH", false, true, ""}) != item.fingerprint) {
                if (strict) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
            } else {
                if (::unlinkat(parent.get(), leaf.c_str(), 0) != 0 && strict) {
                    throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
                }
                if (::fsync(parent.get()) != 0 && strict) {
                    throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
                }
            }
        }
        struct stat backup_status {};
        const std::string backup_name = std::to_string(item.index);
        if (item.existed && ::fstatat(backup.get(), backup_name.c_str(), &backup_status,
                                      AT_SYMLINK_NOFOLLOW) == 0) {
            if (::fstatat(parent.get(), leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0) {
                if (strict) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
            } else {
                if (::renameat(backup.get(), backup_name.c_str(), parent.get(), leaf.c_str()) != 0 && strict) {
                    throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
                }
                if (::fsync(parent.get()) != 0 && strict) {
                    throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
                }
            }
        }
    }
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) {
        std::set<std::string> no_created_dirs;
        ImportFd parent(openImportParentNoFollow(target_root.fd.get(), *it, false, no_created_dirs));
        const std::string leaf = std::filesystem::path(*it).filename().generic_string();
        // AT_REMOVEDIR removes only empty directories; a non-empty directory is
        // intentionally left intact rather than sweeping an unrelated sibling.
        if (::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR) != 0 && errno != ENOTEMPTY &&
            errno != EEXIST && errno != ENOENT && strict) {
            throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
        }
    }
}

void appendImportResult(std::ostream& out, const std::string& operation_id,
                        const CurrentVersion& current, const ImportPlan& plan,
                        const std::string& status, bool recovery_required) {
    std::vector<std::string> imported;
    if (status == "imported") {
        for (const auto& item : plan.items) imported.push_back(item.destination);
    }
    out << "{\"schema\":\"import-result/v1\",\"operation_id\":";
    appendJsonString(out, operation_id);
    out << ",\"target_version\":"; appendJsonString(out, current.version);
    out << ",\"destination\":"; appendJsonString(out, plan.destination);
    out << ",\"status\":"; appendJsonString(out, status);
    out << ",\"revision\":null,\"item_count\":" << imported.size()
        << ",\"new_count\":" << (imported.size() -
               std::count_if(plan.items.begin(), plan.items.end(), [](const auto& item) {
                   return item.destination_replaceable;
               }))
        << ",\"replaced_local_count\":"
        << std::count_if(plan.items.begin(), plan.items.end(), [](const auto& item) {
               return item.destination_replaceable;
           })
        << ",\"imported_paths\":[";
    for (std::size_t i = 0; i < imported.size(); ++i) {
        if (i) out << ",";
        appendJsonString(out, imported[i]);
    }
    out << "],\"batch_paths\":[";
    if (status == "imported") {
        for (std::size_t i = 0; i < imported.size(); ++i) {
            if (i) out << ",";
            appendJsonString(out, imported[i]);
        }
    }
    out << "],\"recovery_required\":" << (recovery_required ? "true" : "false") << "}\n";
}

int cmdImport(const std::vector<std::string>& args, std::istream& in, std::ostream& out) {
    if (hasFlag(args, "--project")) {
        throw std::invalid_argument("import does not accept --project; it is fixed-project");
    }
    if (args.size() < 2 || (args[1] != "preview" && args[1] != "apply") ||
        !hasFlag(args, "--request-stdin")) {
        throw std::invalid_argument("import requires preview|apply --request-stdin");
    }
    const bool apply = args[1] == "apply";
    ImportRequest request = parseImportRequest(in, apply);
    Runtime runtime = workspaceRuntime();
    CurrentVersion current;
    try {
        current = explicitVersionInWorkspace(runtime, request.version);
    } catch (const std::exception&) {
        throw WorkError{"NO_ACTIVE_VERSION", false, false, ""};
    }
    if (!apply) {
        // A transactional intermediate state never yields an Apply-capable
        // plan for the same Version.  A modern journal on B does not block A;
        // an unpinned historical journal remains deliberately Project-blocking.
        enforceJournalAdmission(runtime, current.version);
        const auto plan = buildImportPlan(runtime, current, request, false,
                                          std::chrono::steady_clock::now() + kImportPreviewWallBudget);
        appendImportPlan(out, runtime, current, plan);
        return 0;
    }

    // Re-scan right before applying so a journal left by a crash since the
    // preview cannot be mistaken for a clean plan.
    enforceJournalAdmission(runtime, current.version);
    const auto apply_deadline = std::chrono::steady_clock::now() + kImportApplyWallBudget;
    const auto plan = buildImportPlan(runtime, current, request, true, apply_deadline);
    if (plan.plan_hash != request.expected_plan) throw WorkError{"PLAN_STALE", true, false, ""};
    if (!plan.can_apply) {
        const auto blocked = std::find_if(plan.items.begin(), plan.items.end(), [](const auto& item) {
            return !item.allowed;
        });
        throw WorkError{blocked == plan.items.end() ? "STATE_INCONSISTENT" : blocked->reason_code,
                        false, false, ""};
    }
    ImportTargetRoot target_root = openImportTargetRootNoFollow(current.working_copy);
    if (!importStatSame(target_root.identity, plan.target_root_stat)) {
        throw WorkError{"PLAN_STALE", true, false, ""};
    }
    verifyImportTargetRootNoFollow(target_root, false);
    preflightImportSpace(runtime, current, plan);

    const std::string operation_id = makeOperationId("import");
    const auto dir = journalDir(runtime, operation_id);
    const OperationLiveness owner = claimNewOperationJournal(dir);
    std::vector<ImportJournalItem> journal;
    journal.reserve(plan.items.size());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        journal.push_back(ImportJournalItem{i, plan.items[i].destination,
                                            plan.items[i].source_fingerprint,
                                            plan.items[i].destination_exists, false, false});
    }
    std::set<std::string> created_dirs;
    writeImportJournal(runtime, operation_id, current, target_root, plan, "PREPARED", journal, created_dirs);
    try {
        workFault("import_journal_prepared");
        // Copy/verify every source before publication.  Temp names never include
        // external source names and the operation directory is on the target FS.
        writeImportJournal(runtime, operation_id, current, target_root, plan, "COPYING_TO_TEMP", journal, created_dirs);
        ImportFd temp_fd(::open((dir / "temp").c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (temp_fd.get() < 0) throw WorkError{"INSUFFICIENT_TARGET_SPACE", false, false, operation_id};
        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            ensureImportDeadline(apply_deadline);
            (void)copyImportRegularNoFollow(plan.items[i], temp_fd.get(), std::to_string(i), apply_deadline);
            workFault("import_temp_copied_" + std::to_string(i));
            importTestPause("import_temp_copied_" + std::to_string(i));
        }
        verifyImportSourceRootsNoFollow(plan);
        verifyImportTargetRootNoFollow(target_root, false);
        writeImportJournal(runtime, operation_id, current, target_root, plan, "READY_TO_PUBLISH", journal,
                           created_dirs);
        workFault("import_ready_to_publish");
        ImportFd backup_fd(::open((dir / "backup").c_str(),
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (backup_fd.get() < 0) throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};

        for (std::size_t i = 0; i < plan.items.size(); ++i) {
            ensureImportDeadline(apply_deadline);
            const auto& item = plan.items[i];
            verifyImportTargetRootNoFollow(target_root, false);
            ImportFd parent(openImportParentNoFollow(target_root.fd.get(), item.destination, true,
                                                      created_dirs));
            const auto leaf = std::filesystem::path(item.destination).filename().generic_string();
            struct stat current_status {};
            const bool currently_exists =
                ::fstatat(parent.get(), leaf.c_str(), &current_status, AT_SYMLINK_NOFOLLOW) == 0;
            if (currently_exists && (S_ISLNK(current_status.st_mode) || !S_ISREG(current_status.st_mode))) {
                throw WorkError{"DESTINATION_SYMLINK_BLOCKED", false, false, operation_id};
            }
            if (item.destination_exists) {
                if (!currently_exists || importFingerprintAtNoFollow(parent.get(), leaf,
                                                                     {"PLAN_STALE", true, false, operation_id}) !=
                                              item.destination_fingerprint) {
                    throw WorkError{"PLAN_STALE", true, false, operation_id};
                }
                writeImportJournal(runtime, operation_id, current, target_root, plan, "PUBLISHING", journal,
                                   created_dirs);
                const std::string backup_name = std::to_string(i);
                if (::renameat(parent.get(), leaf.c_str(), backup_fd.get(), backup_name.c_str()) != 0) {
                    throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};
                }
                if (::fsync(parent.get()) != 0) {
                    throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};
                }
                if (::fsync(backup_fd.get()) != 0) {
                    throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};
                }
                journal[i].backup_taken = true;
                writeImportJournal(runtime, operation_id, current, target_root, plan, "PUBLISHING", journal,
                                   created_dirs);
                workFault("import_backup_renamed_" + std::to_string(i));
            } else if (currently_exists) {
                throw WorkError{"PLAN_STALE", true, false, operation_id};
            }
            const std::string temp_name = std::to_string(i);
            if (::renameat(temp_fd.get(), temp_name.c_str(), parent.get(), leaf.c_str()) != 0) {
                throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};
            }
            if (::fsync(parent.get()) != 0) {
                throw WorkError{"STATE_INCONSISTENT", false, true, operation_id};
            }
            journal[i].published = true;
            writeImportJournal(runtime, operation_id, current, target_root, plan, "PUBLISHING", journal,
                               created_dirs);
            workFault("import_item_published_" + std::to_string(i));
        }
        writeImportJournal(runtime, operation_id, current, target_root, plan, "COMPLETED", journal, created_dirs);
        workFault("import_completed_before_cleanup");
        std::error_code cleanup_ec;
        removeOperationJournal(dir, cleanup_ec);
        appendImportResult(out, operation_id, current, plan, "imported", false);
    } catch (const WorkError& failure) {
        const bool publication_started = std::any_of(journal.begin(), journal.end(), [](const auto& item) {
            return item.backup_taken || item.published;
        });
        if (!publication_started) {
            std::error_code cleanup_ec;
            removeOperationJournal(dir, cleanup_ec);
            // A no-follow source/destination failure before the first rename
            // has no local mutation to recover.  Do not turn it into a
            // misleading Recovery Required state merely because nested target
            // parents have not been created yet.
            throw;
        }
        try {
            writeImportJournal(runtime, operation_id, current, target_root, plan, "ROLLING_BACK", journal,
                               created_dirs);
            rollbackImport(target_root, dir, journal, created_dirs, true);
            std::error_code cleanup_ec;
            removeOperationJournal(dir, cleanup_ec);
        } catch (const WorkError&) {
            throw WorkError{"RECOVERY_REQUIRED", false, true, operation_id};
        }
        appendImportResult(out, operation_id, current, plan, "rolled_back", false);
    } catch (const std::exception&) {
        const bool publication_started = std::any_of(journal.begin(), journal.end(), [](const auto& item) {
            return item.backup_taken || item.published;
        });
        if (!publication_started) {
            std::error_code cleanup_ec;
            removeOperationJournal(dir, cleanup_ec);
            appendImportResult(out, operation_id, current, plan, "rolled_back", false);
            return 0;
        }
        try {
            writeImportJournal(runtime, operation_id, current, target_root, plan, "ROLLING_BACK", journal,
                               created_dirs);
            rollbackImport(target_root, dir, journal, created_dirs, true);
            std::error_code cleanup_ec;
            removeOperationJournal(dir, cleanup_ec);
        } catch (const WorkError&) {
            throw WorkError{"RECOVERY_REQUIRED", false, true, operation_id};
        }
        appendImportResult(out, operation_id, current, plan, "rolled_back", false);
    }
    return 0;
}

std::vector<ImportJournalItem> parseImportJournalItems(const std::string& content) {
    std::vector<ImportJournalItem> result;
    std::size_t pos = 0;
    while ((pos = content.find("{\"index\":", pos)) != std::string::npos) {
        const auto end = content.find('}', pos);
        if (end == std::string::npos) break;
        const auto line = content.substr(pos, end - pos + 1);
        const auto index = journalScalar(line, "index");
        const auto destination = journalScalar(line, "destination");
        const auto fingerprint = journalScalar(line, "fingerprint");
        if (index.empty() || destination.empty() || fingerprint.size() != 64) {
            throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, ""};
        }
        result.push_back(ImportJournalItem{
            static_cast<std::size_t>(std::stoull(index)), destination, fingerprint,
            journalScalar(line, "existed") == "true",
            journalScalar(line, "backup_taken") == "true",
            journalScalar(line, "published") == "true"});
        pos = end + 1;
    }
    return result;
}

std::size_t importJournalItemCount(const std::string& content) {
    return parseImportJournalItems(content).size();
}

int recoverImportJournal(const std::string& op_id, const std::filesystem::path& dir,
                         const std::string& content,
                         const CurrentVersion& current, std::ostream& out) {
    const auto version = journalScalar(content, "target_version");
    const auto state = journalScalar(content, "state");
    const auto destination = journalScalar(content, "destination");
    if (version.empty() || !isValidOperationId(op_id)) {
        throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id};
    }
    if (current.version != version) {
        throw WorkError{"OPERATION_TARGET_DIVERGED", false, true, op_id};
    }
    auto journal = parseImportJournalItems(content);
    const auto created = journalStringArray(content, "created_dirs");
    if (journal.empty()) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id};

    // The journal deliberately contains only destination evidence.  Pin the
    // current Active root through every ancestor without following links, then
    // require the same inode that was durable at PREPARED.  Old v4 Import
    // journals without this identity are manual-review-only rather than being
    // restored through a path that may now point elsewhere.
    ImportTargetRoot target_root;
    try {
        const auto dev_text = journalScalar(content, "working_copy_dev");
        const auto ino_text = journalScalar(content, "working_copy_ino");
        if (dev_text.empty() || ino_text.empty()) throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id};
        target_root = openImportTargetRootNoFollow(current.working_copy);
        if (static_cast<std::uintmax_t>(target_root.identity.st_dev) != std::stoull(dev_text) ||
            static_cast<std::uintmax_t>(target_root.identity.st_ino) != std::stoull(ino_text)) {
            throw WorkError{"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id};
        }
        verifyImportTargetRootNoFollow(target_root, true);
    } catch (...) {
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"operation\":\"import\",\"status\":\"manual_review_required\","
               "\"revision\":null,\"reason_code\":\"RECOVERY_EVIDENCE_MISMATCH\","
               "\"recovery_required\":true,\"manual_review_required\":true}\n";
        return 0;
    }

    if (state == "COMPLETED") {
        bool complete = true;
        for (const auto& item : journal) {
            try {
                std::set<std::string> no_created_dirs;
                ImportFd parent(openImportParentNoFollow(target_root.fd.get(), item.destination, false,
                                                          no_created_dirs));
                const std::string leaf = std::filesystem::path(item.destination).filename().generic_string();
                struct stat status {};
                if (::fstatat(parent.get(), leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0 ||
                    !S_ISREG(status.st_mode) ||
                    importFingerprintAtNoFollow(parent.get(), leaf,
                                                 {"RECOVERY_EVIDENCE_MISMATCH", false, true, op_id}) !=
                        item.fingerprint) {
                    complete = false;
                    break;
                }
            } catch (const WorkError&) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
            appendJsonString(out, op_id);
            out << ",\"operation\":\"import\",\"status\":\"manual_review_required\","
                   "\"revision\":null,\"reason_code\":\"RECOVERY_EVIDENCE_MISMATCH\","
                   "\"recovery_required\":true,\"manual_review_required\":true}\n";
            return 0;
        }
        std::error_code ec;
        removeOperationJournal(dir, ec);
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"operation\":\"import\",\"status\":\"verified_completed\","
               "\"revision\":null,\"recovery_required\":false,\"manual_review_required\":false}\n";
        return 0;
    }

    try {
        rollbackImport(target_root, dir, journal, created, true);
    } catch (const WorkError&) {
        out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
        appendJsonString(out, op_id);
        out << ",\"operation\":\"import\",\"status\":\"manual_review_required\","
               "\"revision\":null,\"reason_code\":\"RECOVERY_EVIDENCE_MISMATCH\","
               "\"recovery_required\":true,\"manual_review_required\":true}\n";
        return 0;
    }
    std::error_code ec;
    removeOperationJournal(dir, ec);
    out << "{\"schema_version\":\"work-recovery-result/v2\",\"operation_id\":";
    appendJsonString(out, op_id);
    out << ",\"operation\":\"import\",\"status\":\"rolled_back\",\"revision\":null,"
           "\"recovery_required\":false,\"manual_review_required\":false}\n";
    return 0;
}

int cmdUpdate(std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const auto current = requireCurrentVersion(runtime);
    verifyWorkingCopyMatchesMapping(runtime, current);
    repairDanglingLinksUnder(runtime, current.working_copy);
    runtime.svn.update(current.working_copy);
    relinkVersion(runtime, current.working_copy);
    out << "updated " << current.version << "\n";
    return 0;
}

int cmdRevert(const std::vector<std::string>& args, std::ostream& out) {
    if (args.size() < 2) throw std::invalid_argument("revert requires a file");
    Runtime runtime = workspaceRuntime();
    const auto current = requireCurrentVersion(runtime);
    verifyWorkingCopyMatchesMapping(runtime, current);
    const auto path = std::filesystem::absolute(args[1]);
    if (!isSameOrUnder(path, current.working_copy)) {
        throw std::runtime_error("revert path is not under current working copy");
    }
    repairDanglingLink(runtime, path);
    runtime.svn.revert(path);
    // The state key is taken before relinking: afterwards the path resolves into the pool.
    const auto reverted_key = relativeString(runtime.workspace, path);
    if (std::filesystem::is_regular_file(path)) relinkFile(runtime.store, path);
    mutateWorkspaceState(runtime.workspace,
                         [&](std::set<std::string>& state) { state.erase(reverted_key); });
    out << "reverted " << relativeString(runtime.workspace, path) << "\n";
    return 0;
}

int cmdBranches(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    std::optional<std::string> head;
    if (const auto workspace = maybeWorkspaceRoot(std::filesystem::current_path())) {
        Runtime local_runtime(findProjectDirForSharedCommand(), *workspace);
        if (const auto current = currentVersionFromCwd(local_runtime)) head = current->version;
    }
    // List first: when svn fails, nothing may reach stdout, so a caller never
    // receives a truncated document alongside the error.
    const auto entries = runtime.svn.list(runtime.config.repo_url + "/branches", true);
    if (json) out << "{\"branches\":[";
    bool first = true;
    for (const auto& entry : entries) {
        const auto trimmed = entry.back() == '/' ? entry.substr(0, entry.size() - 1) : entry;
        const auto parts = split(trimmed, '/');
        if (parts.size() == 4) {
            if (json) {
                if (!first) out << ",";
                first = false;
                out << "{\"phase\":";
                appendJsonString(out, parts[0]);
                out << ",\"block\":";
                appendJsonString(out, parts[1]);
                out << ",\"stage\":";
                appendJsonString(out, parts[2]);
                out << ",\"version\":";
                appendJsonString(out, parts[3]);
                out << ",\"path\":";
                appendJsonString(out, trimmed);
                out << ",\"is_head\":"
                    << (head.has_value() && *head == trimmed ? "true" : "false")
                    << ",\"removed\":false}";
            } else {
                out << trimmed << "\n";
            }
        }
    }
    if (json) out << "]}\n";
    return 0;
}

int cmdLog(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    std::string target;
    if (args.size() > 1) {
        for (std::size_t index = 1; index < args.size(); ++index) {
            if (args[index].rfind("--", 0) != 0) {
                validateVersionId(args[index]);
                target = runtime.svn.versionToUrl(args[index]);
                break;
            }
        }
    }
    if (target.empty()) target = requireCurrentVersion(runtime).working_copy.string();
    const auto entries = runtime.svn.log(target, 50);
    if (json) {
        out << "{\"entries\":[";
        bool first = true;
        for (const auto& entry : entries) {
            if (!first) out << ",";
            first = false;
            out << "{\"rev\":" << entry.revision << ",\"author\":";
            appendJsonString(out, entry.author);
            out << ",\"time\":";
            appendJsonString(out, entry.date);
            out << ",\"message\":";
            appendJsonString(out, entry.message);
            out << "}";
        }
        out << "]}\n";
    } else {
        for (const auto& entry : entries) {
            out << "r" << entry.revision << " | " << entry.author << " | " << entry.date
                << "\n" << entry.message << "\n";
        }
    }
    return 0;
}

int cmdLineageVerify(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    const auto result = runtime.lineage.verifyIntegrity();
    if (json) {
        out << "{\"intact\":" << (result.ok ? "true" : "false") << ",\"break_at\":";
        if (result.first_bad_seq.has_value()) {
            out << *result.first_bad_seq;
        } else {
            out << "null";
        }
        out << ",\"detail\":{\"expected_hash\":null,\"actual_hash\":null,"
               "\"actor\":null,\"ts\":null,\"message\":";
        appendJsonString(out, result.detail);
        out << "}}\n";
        return result.ok ? 0 : 2;
    }
    out << (result.ok ? "Lineage integrity ok" : "Lineage integrity failed") << "\n";
    if (!result.ok) out << result.detail << "\n";
    return result.ok ? 0 : 2;
}

int cmdLineageAudit(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    std::optional<std::string> child;
    for (std::size_t index = 2; index < args.size(); ++index) {
        if (args[index].rfind("--", 0) != 0) {
            validateVersionId(args[index]);
            child = args[index];
            break;
        }
    }
    const auto records = runtime.lineage.auditTrail(child);
    if (json) {
        out << "{\"records\":[";
        bool first = true;
        for (const auto& record : records) {
            if (!first) out << ",";
            first = false;
            out << "{\"seq\":" << record.seq << ",\"ts\":";
            appendJsonString(out, record.ts);
            out << ",\"actor\":";
            appendJsonString(out, record.actor);
            out << ",\"action\":";
            appendJsonString(out, record.action);
            out << ",\"child\":";
            appendJsonString(out, record.child);
            out << ",\"old_parent\":";
            appendJsonNullable(out, record.old_parent);
            out << ",\"new_parent\":";
            appendJsonNullable(out, record.new_parent);
            out << ",\"reason\":";
            appendJsonNullable(out, record.reason);
            out << "}";
        }
        out << "]}\n";
        return 0;
    }
    for (const auto& record : records) {
        out << record.seq << " | " << record.ts << " | " << record.actor << " | "
            << record.action << " | " << record.child << " | ";
        out << (record.old_parent.has_value() ? *record.old_parent : "null") << " -> "
            << (record.new_parent.has_value() ? *record.new_parent : "null");
        if (record.reason.has_value()) out << " | " << *record.reason;
        out << "\n";
    }
    return 0;
}

double optionDouble(const std::vector<std::string>& args, const std::string& name,
                    double fallback) {
    if (const auto value = optionValue(args, name)) return std::stod(*value);
    return fallback;
}

struct RenewEventRow {
    std::string hash;
    std::string actor;
    std::string renewed_at;
    std::int64_t refcount = 0;
    std::int64_t size = 0;
};

int cmdGc(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    const bool json = hasFlag(args, "--json");
    const bool dry_run = hasFlag(args, "--dry-run");
    if (dry_run && !hasFlag(args, "--reconcile")) {
        err << "--dry-run only supports --reconcile; reclaim/full GC dry-run is unavailable. "
               "No reclamation was executed.\n";
        return 2;
    }

    Runtime runtime = sharedRuntime();
    requirePmMaintenance(runtime);
    // Maintenance may release only refs it can prove gone: this account's own
    // links inside the workspace it runs from.  Everything else is kept and
    // listed in retained_refs.
    GarbageCollector gc(runtime.config.pool_root,
                        storage::ReconcileScope{common::system_user(), runtime.workspace});
    const double high = optionDouble(args, "--high", 0.9);
    const double low = optionDouble(args, "--low", 0.7);
    storage::GcReport report;
    if (hasFlag(args, "--reconcile")) {
        report = dry_run ? gc.reconcileDryRun() : gc.reconcile();
    } else if (hasFlag(args, "--reclaim-orphans")) {
        report = gc.reclaimOrphans();
    } else if (hasFlag(args, "--reclaim")) {
        report = gc.reclaim(high, low);
    } else {
        report = gc.run(high, low);
    }
    if (json) {
        out << "{\"dry_run\":" << (dry_run ? "true" : "false")
            << ",\"refs_removed\":" << report.refs_removed
            << ",\"objects_reclaimed\":" << report.objects_reclaimed
            << ",\"objects_force_evicted\":" << report.objects_force_evicted
            << ",\"bytes_freed\":" << report.bytes_freed
            << ",\"low_watermark_reached\":"
            << (report.low_watermark_reached ? "true" : "false")
            << ",\"phantom_refs\":[";
        bool first = true;
        for (const auto& phantom : report.phantom_refs) {
            if (!first) out << ",";
            first = false;
            out << "{\"link_path\":";
            appendJsonString(out, phantom.link_path);
            out << ",\"hash\":";
            appendJsonString(out, phantom.hash);
            out << "}";
        }
        out << "],\"refs_retained\":" << report.retained_refs.size() << ",\"retained_refs\":[";
        first = true;
        for (const auto& retained : report.retained_refs) {
            if (!first) out << ",";
            first = false;
            out << "{\"link_path\":";
            appendJsonString(out, retained.link_path);
            out << ",\"hash\":";
            appendJsonString(out, retained.hash);
            out << ",\"owner\":";
            appendJsonString(out, retained.owner);
            out << ",\"reason\":";
            appendJsonString(out, retained.reason);
            out << ",\"scope\":";
            appendJsonString(out, retained.scope);
            out << "}";
        }
        out << "],\"objects_not_reclaimed\":" << report.reclaim_failures.size()
            << ",\"reclaim_failures\":[";
        first = true;
        for (const auto& failure : report.reclaim_failures) {
            if (!first) out << ",";
            first = false;
            out << "{\"hash\":";
            appendJsonString(out, failure.hash);
            out << ",\"detail\":";
            appendJsonString(out, failure.detail);
            out << "}";
        }
        out << "]}\n";
    } else {
        out << "refs_removed=" << report.refs_removed
            << " objects_reclaimed=" << report.objects_reclaimed
            << " objects_force_evicted=" << report.objects_force_evicted
            << " bytes_freed=" << report.bytes_freed << "\n";
        out << "low_watermark_reached="
            << (report.low_watermark_reached ? "true" : "false") << "\n";
        if (dry_run) {
            for (const auto& phantom : report.phantom_refs) {
                out << "phantom_ref " << phantom.hash << " " << phantom.link_path << "\n";
            }
            for (const auto& retained : report.retained_refs) {
                out << "retained_ref " << retained.hash << " " << retained.link_path << " owner="
                    << retained.owner << " reason=" << retained.reason
                    << " scope=" << retained.scope << "\n";
            }
        } else if (!report.retained_refs.empty()) {
            err << "warning: " << report.retained_refs.size()
                << " pool reference(s) could not be verified from this account and were kept; "
                   "run 'isvn gc --reconcile --dry-run' to list them\n";
        }
    }
    if (!report.reclaim_failures.empty()) {
        // The other candidates were still processed; the run is incomplete, not
        // a success.
        err << "isvn: cleanup incomplete: " << report.reclaim_failures.size()
            << " unreferenced pool object(s) could not be removed and were left in the pool:\n";
        for (const auto& failure : report.reclaim_failures) {
            err << "  " << failure.hash << ": " << failure.detail << "\n";
        }
        return 1;
    }
    return 0;
}

class SqliteDatabase {
public:
    explicit SqliteDatabase(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown error";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error("unable to open pool metadata: " + message);
        }
    }
    ~SqliteDatabase() { sqlite3_close(db_); }
    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;

    std::int64_t integer(const char* sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_ROW) {
            sqlite3_finalize(statement);
            throw std::runtime_error("SQLite query returned no row");
        }
        const auto result = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return result;
    }

    std::string text(const char* sql) const {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
        std::string result;
        if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_text(statement, 0) != nullptr) {
            result = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        }
        sqlite3_finalize(statement);
        return result;
    }

    std::vector<std::pair<std::int64_t, std::int64_t>> refcountDistribution() const {
        sqlite3_stmt* statement = nullptr;
        const char* sql =
            "SELECT refcount, COUNT(*) FROM object GROUP BY refcount ORDER BY refcount;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
        std::vector<std::pair<std::int64_t, std::int64_t>> rows;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            rows.emplace_back(sqlite3_column_int64(statement, 0),
                              sqlite3_column_int64(statement, 1));
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::vector<std::pair<std::string, std::string>> evictEvents() const {
        sqlite3_stmt* statement = nullptr;
        const char* sql =
            "SELECT hash, evicted_at FROM evict_event ORDER BY evicted_at DESC, event_id DESC;";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
        std::vector<std::pair<std::string, std::string>> rows;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* hash = sqlite3_column_text(statement, 0);
            const auto* evicted_at = sqlite3_column_text(statement, 1);
            rows.emplace_back(hash == nullptr ? "" : reinterpret_cast<const char*>(hash),
                              evicted_at == nullptr ? "" : reinterpret_cast<const char*>(evicted_at));
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::vector<RenewEventRow> recentRenewEvents(int limit = 20) const {
        sqlite3_stmt* statement = nullptr;
        const char* sql = R"sql(
            SELECT renew_event.hash, renew_event.actor, renew_event.renewed_at,
                   COALESCE(object.refcount, 0), COALESCE(object.size, 0)
            FROM renew_event
            LEFT JOIN object ON object.hash = renew_event.hash
            ORDER BY renew_event.renewed_at DESC, renew_event.event_id DESC
            LIMIT ?;
        )sql";
        if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("SQLite prepare failed: ") +
                                     sqlite3_errmsg(db_));
        }
        sqlite3_bind_int(statement, 1, limit);
        std::vector<RenewEventRow> rows;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* hash = sqlite3_column_text(statement, 0);
            const auto* actor = sqlite3_column_text(statement, 1);
            const auto* renewed_at = sqlite3_column_text(statement, 2);
            rows.push_back(RenewEventRow{
                hash == nullptr ? "" : reinterpret_cast<const char*>(hash),
                actor == nullptr ? "" : reinterpret_cast<const char*>(actor),
                renewed_at == nullptr ? "" : reinterpret_cast<const char*>(renewed_at),
                sqlite3_column_int64(statement, 3),
                sqlite3_column_int64(statement, 4)});
        }
        sqlite3_finalize(statement);
        return rows;
    }

private:
    sqlite3* db_ = nullptr;
};

int cmdRenew(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = workspaceRuntime();
    const bool json = hasFlag(args, "--json");
    const auto in_version = optionValue(args, "--in");
    if (hasFlag(args, "--in") && !in_version.has_value()) {
        throw std::invalid_argument("--in requires a version path");
    }

    std::vector<std::filesystem::path> targets;
    if (in_version.has_value()) {
        // Renew every content object of one version. First repair dangling links
        // (swap-in restores any content no longer present in the pool), then renew
        // each pool symlink so the whole version's contents get a fresh life.
        validateVersionId(*in_version);
        const auto wc = mappedWorkingCopy(runtime, *in_version);
        repairDanglingLinksUnder(runtime, wc);
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(wc, ec)) {
            if (shouldSkipPath(wc, entry.path())) continue;
            if (!std::filesystem::is_symlink(entry.path())) continue;
            const auto absolute = std::filesystem::absolute(entry.path());
            if (!isPoolSymlink(absolute, runtime.config.pool_root)) continue;
            targets.push_back(absolute);
        }
    } else {
        const auto positional = positionalArgs(args);
        if (positional.empty()) throw std::invalid_argument("renew requires at least one path");
        for (const auto& entry : positional) {
            targets.push_back(std::filesystem::absolute(entry));
        }
    }

    int renewed = 0;
    std::vector<std::string> renewed_files;
    for (const auto& path : targets) {
        repairDanglingLink(runtime, path);
        const std::string hash = poolHashFromSymlink(path, runtime.config.pool_root);
        runtime.store.touch_last_access(hash);
        runtime.store.record_renew_event(hash);
        ++renewed;
        // Lexical (do not resolve the pool symlink) so we report the logical file path,
        // not the pool object target.
        const auto relative = lexicalRelativeString(runtime.workspace, path);
        if (json) renewed_files.push_back(relative);
        else out << "renewed " << relative << "\n";
    }
    if (json) {
        out << "{\"result\":\"renewed\",\"renewed_count\":" << renewed;
        if (in_version.has_value()) {
            out << ",\"version\":";
            appendJsonString(out, *in_version);
        }
        out << ",\"files\":[";
        for (std::size_t index = 0; index < renewed_files.size(); ++index) {
            if (index != 0) out << ",";
            appendJsonString(out, renewed_files[index]);
        }
        out << "]}\n";
    } else {
        out << "renewed_count=" << renewed << "\n";
    }
    return 0;
}

int cmdPoolStat(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    const SqliteDatabase db(runtime.config.pool_root / "meta.db");
    const auto space = std::filesystem::space(runtime.config.pool_root);
    const std::int64_t object_count = db.integer("SELECT COUNT(*) FROM object;");
    const std::int64_t ref_count = db.integer("SELECT COUNT(*) FROM ref;");
    const std::int64_t physical_bytes =
        db.integer("SELECT COALESCE(SUM(size), 0) FROM object;");
    const std::int64_t logical_bytes =
        db.integer("SELECT COALESCE(SUM(size * refcount), 0) FROM object;");
    const std::int64_t renewed_object_count =
        db.integer("SELECT COUNT(DISTINCT hash) FROM renew_event;");
    const std::int64_t renewed_bytes = db.integer(R"sql(
        SELECT COALESCE(SUM(size), 0)
        FROM object
        WHERE hash IN (SELECT DISTINCT hash FROM renew_event);
    )sql");
    const auto recent_renews = db.recentRenewEvents();
    const double dedupe_ratio =
        physical_bytes == 0 ? 1.0 : static_cast<double>(logical_bytes) /
                                      static_cast<double>(physical_bytes);
    const double use_ratio = space.capacity == 0
                                 ? 0.0
                                 : static_cast<double>(space.capacity - space.available) /
                                       static_cast<double>(space.capacity);
    const auto distribution = db.refcountDistribution();
    const bool include_events = hasFlag(args, "--evict-events");
    const std::int64_t events = include_events
                                    ? db.integer("SELECT COUNT(*) FROM evict_event;")
                                    : 0;
    const std::optional<std::string> threshold_arg = optionValue(args, "--evict-threshold");
    const std::optional<std::int64_t> threshold =
        threshold_arg.has_value()
            ? std::optional<std::int64_t>(static_cast<std::int64_t>(std::stoll(*threshold_arg)))
            : std::nullopt;
    const bool critical = threshold.has_value() && events > *threshold;
    if (json) {
        out << "{\"dedup_ratio\":" << dedupe_ratio
            << ",\"used_bytes\":" << (space.capacity - space.available)
            << ",\"capacity_bytes\":" << space.capacity
            << ",\"object_count\":" << object_count
            << ",\"ref_count\":" << ref_count
            << ",\"physical_bytes\":" << physical_bytes
            << ",\"logical_ref_bytes\":" << logical_bytes
            << ",\"atime_usable\":" << (runtime.store.atime_usable() ? "true" : "false")
            << ",\"renewed_object_count\":" << renewed_object_count
            << ",\"renewed_bytes\":" << renewed_bytes
            << ",\"recent_renews\":[";
        bool first = true;
        for (const auto& event : recent_renews) {
            if (!first) out << ",";
            first = false;
            out << "{\"hash\":";
            appendJsonString(out, event.hash);
            out << ",\"actor\":";
            appendJsonString(out, event.actor);
            out << ",\"renewed_at\":";
            appendJsonString(out, event.renewed_at);
            out << ",\"refcount\":" << event.refcount
                << ",\"size\":" << event.size << "}";
        }
        out << "]"
            << ",\"refcount_dist\":{";
        first = true;
        for (const auto& [refcount, count] : distribution) {
            if (!first) out << ",";
            first = false;
            appendJsonString(out, std::to_string(refcount));
            out << ":" << count;
        }
        out << "}";
        if (include_events) {
            out << ",\"events\":[";
            first = true;
            for (const auto& [hash, evicted_at] : db.evictEvents()) {
                if (!first) out << ",";
                first = false;
                out << "{\"hash\":";
                appendJsonString(out, hash);
                out << ",\"evicted_at\":";
                appendJsonString(out, evicted_at);
                out << "}";
            }
            out << "],\"density\":[{\"window\":\"all\",\"count\":" << events
                << "}],\"threshold\":";
            if (threshold.has_value()) {
                out << *threshold;
            } else {
                out << "null";
            }
            out << ",\"critical\":" << (critical ? "true" : "false");
        }
        out << "}\n";
        return 0;
    }
    out << "objects=" << object_count << "\n";
    out << "refs=" << ref_count << "\n";
    out << "physical_bytes=" << physical_bytes << "\n";
    out << "logical_ref_bytes=" << logical_bytes << "\n";
    out << "dedupe_ratio=" << dedupe_ratio << "\n";
    out << "pool_used_ratio=" << use_ratio << "\n";
    out << "atime_usable=" << (runtime.store.atime_usable() ? "true" : "false") << "\n";
    out << "renewed_objects=" << renewed_object_count << "\n";
    out << "renewed_bytes=" << renewed_bytes << "\n";
    out << "refcount_distribution=";
    bool first = true;
    for (const auto& [refcount, count] : distribution) {
        if (!first) out << ",";
        first = false;
        out << refcount << ":" << count;
    }
    out << "\n";
    if (include_events) {
        const std::string latest = db.text("SELECT evicted_at FROM evict_event ORDER BY evicted_at DESC LIMIT 1;");
        out << "evict_events=" << events << "\n";
        out << "latest_evict_event=" << latest << "\n";
        if (critical) out << "pool_health=critical_expand_pool\n";
    }
    return 0;
}

int cmdAdminStatus(const std::vector<std::string>& args, std::ostream& out) {
    Runtime runtime = sharedRuntime();
    const bool json = hasFlag(args, "--json");
    const auto integrity = runtime.lineage.verifyIntegrity();
    const auto audit = runtime.lineage.auditTrail();
    std::set<std::string> lineage_children;
    std::string latest_lineage_change;
    for (const auto& record : audit) {
        if (record.child != "ROOT") lineage_children.insert(record.child);
        if (record.ts > latest_lineage_change) latest_lineage_change = record.ts;
    }

    std::set<std::string> phases;
    std::set<std::string> blocks;
    std::set<std::string> stages;
    std::size_t version_count = 0;
    bool repo_reachable = true;
    try {
        for (const auto& entry : runtime.svn.list(runtime.config.repo_url + "/branches", true)) {
            const auto trimmed = entry.empty() || entry.back() != '/'
                                     ? entry
                                     : entry.substr(0, entry.size() - 1);
            const auto parts = split(trimmed, '/');
            if (parts.size() != 4) continue;
            phases.insert(parts[0]);
            blocks.insert(parts[0] + "/" + parts[1]);
            stages.insert(parts[0] + "/" + parts[1] + "/" + parts[2]);
            ++version_count;
        }
    } catch (const std::exception&) {
        repo_reachable = false;
    }
    std::int64_t revision = 0;
    try {
        const auto latest = runtime.svn.log(runtime.config.repo_url, 1);
        if (!latest.empty()) revision = latest.front().revision;
    } catch (const std::exception&) {
        repo_reachable = false;
        revision = 0;
    }
    std::size_t member_count = 0;
    if (!runtime.config.project_group.empty()) {
        if (const group* group_entry = ::getgrnam(runtime.config.project_group.c_str());
            group_entry != nullptr) {
            for (char** member = group_entry->gr_mem; member != nullptr && *member != nullptr;
                 ++member) {
                ++member_count;
            }
        }
    }

    std::optional<std::string> latest_backup;
    const std::filesystem::path backup_dir = runtime.project_dir / ".isvn" / "backup";
    std::error_code error;
    if (std::filesystem::is_directory(backup_dir, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(backup_dir, error)) {
            if (error) break;
            const auto write_time = std::filesystem::last_write_time(entry.path(), error);
            if (error) continue;
            const auto seconds = write_time.time_since_epoch().count();
            const std::string value = std::to_string(seconds);
            if (!latest_backup.has_value() || value > *latest_backup) latest_backup = value;
        }
    }

    if (json) {
        out << "{\"paths\":[";
        appendPathHealthJson(out, "repo", pathFromFileUrlOrRawPath(runtime.config.repo_url));
        out << ",";
        appendPathHealthJson(out, "pool", runtime.config.pool_root);
        out << ",";
        appendPathHealthJson(out, "project_dir", runtime.project_dir);
        out << "],\"permissions\":[";
        appendPermissionJson(out, "project_isvn", runtime.project_dir / ".isvn", 02770);
        out << ",";
        appendPermissionJson(out, "pool_root", runtime.config.pool_root, 02770);
        out << ",";
        appendPermissionJson(out, "pool_objects", runtime.config.pool_root / "objects", 03770);
        out << ",";
        appendPermissionJson(out, "pool_meta_db", runtime.config.pool_root / "meta.db", 0660);
        out << "],\"authz\":{\"available\":false,\"protected_stages\":[],\"issues\":[]}";
        out << ",\"scale\":{\"phase_count\":" << phases.size()
            << ",\"block_count\":" << blocks.size()
            << ",\"stage_count\":" << stages.size()
            << ",\"version_count\":" << version_count
            << ",\"member_count\":" << member_count
            << ",\"revision_count\":" << revision
            << ",\"repo_reachable\":" << (repo_reachable ? "true" : "false") << "}";
        out << ",\"lineage\":{\"intact\":" << (integrity.ok ? "true" : "false")
            << ",\"node_count\":" << lineage_children.size()
            << ",\"audit_record_count\":" << audit.size()
            << ",\"latest_change\":";
        if (latest_lineage_change.empty()) {
            out << "null";
        } else {
            appendJsonString(out, latest_lineage_change);
        }
        out << ",\"break_at\":";
        if (integrity.first_bad_seq.has_value()) {
            out << *integrity.first_bad_seq;
        } else {
            out << "null";
        }
        out << ",\"detail\":";
        appendJsonString(out, integrity.detail);
        out << "},\"backup\":{\"available\":"
            << (latest_backup.has_value() ? "true" : "false")
            << ",\"latest\":";
        if (latest_backup.has_value()) {
            appendJsonString(out, *latest_backup);
        } else {
            out << "null";
        }
        out << "}}\n";
        return integrity.ok ? 0 : 2;
    }
    out << "admin status requires --json for structured output\n";
    return integrity.ok ? 0 : 2;
}

}  // namespace

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out,
        std::ostream& err) {
    try {
        if (args.empty()) {
            err << usage();
            return 1;
        }
        const auto& command = args[0];
        if (command == "version" || command == "--version" || command == "-v") {
            if (hasFlag(args, "--json")) out << isvn::version::json("isvn");
            else out << isvn::version::plainText("isvn");
            return 0;
        }
        if (command == "admin" && args.size() > 1 && args[1] == "init") {
            return cmdAdminInit(args, out);
        }
        if (command == "admin" && args.size() > 1 && args[1] == "status") {
            return cmdAdminStatus(args, out);
        }
        if (command == "member" && args.size() > 1 && args[1] == "init") {
            return cmdMemberInit(args, out);
        }
        if (command == "init") {
            err << "use 'isvn admin init' or 'isvn member init'\n";
            return 1;
        }
        if (command == "create") return cmdCreate(args, out, err);
        if (command == "checkout") return cmdCheckout(args, out);
        if (command == "checkin") return cmdCheckin(args, out);
        if (command == "remove") return cmdRemove(args, out);
        if (command == "delete") return cmdDelete(args, out, err);
        if (command == "checkout-ancestor") return cmdCheckoutAncestor(args, in, out);
        if (command == "ancestry") return cmdAncestry(args, out);
        if (command == "provenance" && args.size() > 1 && args[1] == "query") {
            return cmdProvenanceQuery(args, out);
        }
        if (command == "file" && args.size() > 1 && args[1] == "query") {
            return cmdFileQuery(args, out);
        }
        if (command == "file" && args.size() > 1 && args[1] == "locate") {
            return cmdFileLocate(args, out);
        }
        if (command == "reconcile") return cmdReconcile(args, out);
        if (command == "edit") return cmdEdit(args, out);
        if (command == "status") return cmdStatus(args, out);
        if (command == "work-status") return cmdWorkStatus(args, out);
        if (command == "workspaces") return cmdWorkspaces(args, out);
        if (command == "work-local-op") return cmdWorkLocalOp(args, out);
        if (command == "remove-batch") return cmdRemoveBatch(args, out);
        if (command == "checkin-batch") return cmdCheckinBatch(args, out);
        if (command == "import") {
            try {
                return cmdImport(args, in, out);
            } catch (const WorkError&) {
                throw;
            } catch (const std::exception&) {
                // Import receives private external paths.  A generic filesystem
                // exception can include one, so never forward it to stderr/QML.
                throw WorkError{"SOURCE_UNREADABLE", false, false, ""};
            }
        }
        if (command == "work-recovery") return cmdWorkRecovery(args, out);
        if (command == "renew-status") return cmdRenewStatus(args, out);
        if (command == "update") return cmdUpdate(out);
        if (command == "revert") return cmdRevert(args, out);
        if (command == "branches") return cmdBranches(args, out);
        if (command == "log") return cmdLog(args, out);
        if (command == "gc") return cmdGc(args, out, err);
        if (command == "renew") return cmdRenew(args, out);
        if (command == "pool-stat") return cmdPoolStat(args, out);
        if (command == "lineage" && args.size() > 1 && args[1] == "verify") {
            return cmdLineageVerify(args, out);
        }
        if (command == "lineage" && args.size() > 1 && args[1] == "audit") {
            return cmdLineageAudit(args, out);
        }
        err << "unknown command: " << command << "\n" << usage();
        return 1;
    } catch (const DetailedWorkError& work_error) {
        appendWorkErrorJson(out, work_error);
        err << "isvn: " << work_error.detail << "\n";
        return 1;
    } catch (const WorkError& work_error) {
        // Structured machine-readable envelope for Work mutation paths.
        appendWorkErrorJson(out, work_error);
        return 1;
    } catch (const std::exception& error) {
        err << "isvn: " << error.what() << "\n";
        return 1;
    }
}

}  // namespace isvn::cli
