#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

namespace isvn::version {

struct SvnResult {
    int exit_code = 0;
    int terminating_signal = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::size_t stdout_bytes = 0;
    std::size_t stderr_bytes = 0;
    std::uint64_t elapsed_ms = 0;
    bool timed_out = false;
    bool stdout_limit_exceeded = false;
    bool stderr_limit_exceeded = false;
    bool runner_error = false;
};

// Explicitly opt-in limits for a sequence of read-only SVN queries.  Legacy
// callers keep using the unbounded compatibility APIs below; bounded planning passes
// this object only for Checkin planning and recovery evidence.
struct SvnQueryLimits {
    std::size_t max_processes = 256;
    std::size_t max_argv_bytes = 128U * 1024U;
    std::size_t max_stdout_per_process = 64U * 1024U * 1024U;
    std::size_t max_stderr_per_process = 4U * 1024U * 1024U;
    std::size_t max_stdout_total = 128U * 1024U * 1024U;
    std::size_t max_stderr_total = 8U * 1024U * 1024U;
    std::chrono::milliseconds per_process_timeout{30000};
    std::chrono::milliseconds total_timeout{60000};
    std::chrono::milliseconds termination_grace{250};
};

struct SvnQueryBudget {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::size_t process_count = 0;
    std::size_t stdout_bytes = 0;
    std::size_t stderr_bytes = 0;
};

enum class SvnQueryFailure {
    ProcessLimit,
    ArgvLimit,
    Timeout,
    OutputLimit,
    Failed,
};

// Deliberately does not retain or expose command lines, URLs, stderr, or XML.
// Callers convert the typed failure to a sanitized Work error code.
class SvnQueryError : public std::runtime_error {
public:
    explicit SvnQueryError(SvnQueryFailure failure);
    SvnQueryFailure failure() const noexcept { return failure_; }

private:
    SvnQueryFailure failure_;
};

struct SvnLogEntry {
    std::int64_t revision = 0;
    std::string author;
    std::string date;
    std::string message;
};

struct SvnInfo {
    std::string url;
    std::string repo_root;
    std::string relative_url;
    std::int64_t revision = 0;
    std::string last_author;
    std::string last_date;
};

struct SvnStatusEntry {
    std::string path;
    std::string status;
};

struct SvnListEntry {
    std::string name;
    bool is_directory = false;
};

struct SvnImmediateListResult {
    // `exists=false` is returned only for the tested stable SVN not-found
    // outcome. All transport/auth/parse failures throw SvnQueryError instead.
    bool exists = true;
    std::vector<SvnListEntry> entries;
};

// One changed path from `svn log --verbose --xml`. `action` is the raw SVN
// action letter (A/D/M/R). `path` is the repository-absolute path.
struct SvnChangedPath {
    std::string action;
    std::string path;
};

struct SvnVerboseLogEntry {
    std::int64_t revision = 0;
    std::string author;
    std::string date;
    std::string message;
    std::vector<SvnChangedPath> paths;
};

class SvnOps {
public:
    explicit SvnOps(const std::string& repo_root_url);

    void branchCopy(const std::string& source_url, const std::string& dest_url,
                    const std::string& commit_message);
    void createBranchFromParent(const std::string& parent_version,
                                const std::string& child_version,
                                const std::string& commit_message);
    void createRootBranch(const std::string& child_version,
                          const std::string& commit_message);
    void branchDelete(const std::string& branch_url, const std::string& commit_message);
    void mkdir(const std::string& url, const std::string& commit_message);

    void checkout(const std::string& url, const std::filesystem::path& local_path,
                  std::optional<std::int64_t> revision = std::nullopt);
    void update(const std::filesystem::path& wc_path,
                std::optional<std::int64_t> revision = std::nullopt);
    std::int64_t commit(const std::filesystem::path& wc_path,
                        const std::string& commit_message);
    // `depth_empty` schedules exactly the named node (a directory is added
// without sweeping in its descendants); used by batch checkin to
    // stage supporting parent directories and files as explicit exact nodes.
    void add(const std::filesystem::path& path, bool depth_empty = false);
    void remove(const std::filesystem::path& path);
    void revert(const std::filesystem::path& path);

    std::vector<SvnStatusEntry> status(const std::filesystem::path& wc_path) const;
    std::vector<SvnLogEntry> log(const std::string& url_or_path, int limit = 50) const;
    SvnInfo info(const std::filesystem::path& wc_path) const;
    std::vector<std::string> list(const std::string& url, bool recursive = false) const;

// Bounded, typed query APIs. They are intentionally separate
    // from the legacy methods so the new resource ceilings are not silently
    // imposed on unrelated commands.
    std::vector<SvnStatusEntry> statusBounded(
        const std::vector<std::filesystem::path>& targets, const SvnQueryLimits& limits,
        SvnQueryBudget& budget) const;
    SvnInfo infoBounded(const std::filesystem::path& target, const SvnQueryLimits& limits,
                        SvnQueryBudget& budget) const;
    SvnImmediateListResult listImmediateBounded(const std::string& url,
                                                 const SvnQueryLimits& limits,
                                                 SvnQueryBudget& budget) const;
    std::vector<SvnVerboseLogEntry> logVerboseBounded(const std::string& target_url,
                                                       std::int64_t from_revision,
                                                       std::int64_t to_revision,
                                                       const SvnQueryLimits& limits,
                                                       SvnQueryBudget& budget) const;
    std::int64_t headRevisionBounded(const std::string& url, const SvnQueryLimits& limits,
                                     SvnQueryBudget& budget) const;

// Machine-readable verbose log evidence plus repository HEAD and
    // exact-scope commit. `logVerbose` runs `svn log --xml --verbose -r
    // <from>:<to> <url>`; malformed/incomplete XML throws (never silent
    // success). `headRevision` reads the repository's current revision.
    // `commitPaths` commits exactly the listed working-copy targets (never the
    // whole working copy), returning the new revision.
    std::vector<SvnVerboseLogEntry> logVerbose(const std::string& target_url,
                                               std::int64_t from_revision,
                                               std::int64_t to_revision) const;
    std::int64_t headRevision(const std::string& url) const;
    // `depth_empty` commits each listed target at empty depth (the node itself
    // only), so committing a newly added directory cannot sweep unselected
// descendants into the revision. Exact-scope batch checkin.
    std::int64_t commitPaths(const std::vector<std::filesystem::path>& paths,
                             const std::string& commit_message,
                             bool depth_empty = false) const;

    std::string versionToUrl(const std::string& version_path) const;
    std::optional<std::string> urlToVersion(const std::string& url) const;

private:
    SvnResult runTool(const std::string& program, const std::vector<std::string>& args) const;
    SvnResult runToolBounded(const std::string& program, const std::vector<std::string>& args,
                             std::size_t stdout_limit, std::size_t stderr_limit,
                             std::chrono::milliseconds timeout,
                             std::chrono::milliseconds termination_grace) const;
    SvnResult runSvn(const std::vector<std::string>& args) const;
    SvnResult runSvnBounded(const std::vector<std::string>& args, const SvnQueryLimits& limits,
                            SvnQueryBudget& budget) const;
    SvnResult runSvnmucc(const std::vector<std::string>& args) const;
    static void requireIsvnMarker(const std::string& commit_message);
    static void requireSuccess(const SvnResult& result, const std::string& operation);

    std::string repo_root_;
};

}  // namespace isvn::version
