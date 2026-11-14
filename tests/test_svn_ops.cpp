#include "common/system.h"
#include "version/svn_ops.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using isvn::version::SvnInfo;
using isvn::version::SvnLogEntry;
using isvn::version::SvnOps;
using isvn::version::SvnQueryBudget;
using isvn::version::SvnQueryError;
using isvn::version::SvnQueryFailure;
using isvn::version::SvnQueryLimits;
using isvn::version::SvnStatusEntry;

using BranchCopySignature = void (SvnOps::*)(const std::string&, const std::string&,
                                             const std::string&);
using CreateBranchSignature = void (SvnOps::*)(const std::string&, const std::string&,
                                               const std::string&);
using CreateRootBranchSignature = void (SvnOps::*)(const std::string&,
                                                   const std::string&);
using CommitSignature = std::int64_t (SvnOps::*)(const std::filesystem::path&,
                                                 const std::string&);
using LogSignature = std::vector<SvnLogEntry> (SvnOps::*)(const std::string&, int) const;
using InfoSignature = SvnInfo (SvnOps::*)(const std::filesystem::path&) const;
using StatusSignature = std::vector<SvnStatusEntry> (SvnOps::*)(
    const std::filesystem::path&) const;
static_assert(std::is_same_v<decltype(&SvnOps::branchCopy), BranchCopySignature>);
static_assert(std::is_same_v<decltype(&SvnOps::createBranchFromParent),
                             CreateBranchSignature>);
static_assert(std::is_same_v<decltype(&SvnOps::createRootBranch),
                             CreateRootBranchSignature>);
static_assert(std::is_same_v<decltype(&SvnOps::commit), CommitSignature>);
static_assert(std::is_same_v<decltype(&SvnOps::log), LogSignature>);
static_assert(std::is_same_v<decltype(&SvnOps::info), InfoSignature>);
static_assert(std::is_same_v<decltype(&SvnOps::status), StatusSignature>);

template <typename T, typename = void>
struct HasQueryCopyFrom : std::false_type {};
template <typename T>
struct HasQueryCopyFrom<T, std::void_t<decltype(&T::queryCopyFrom)>> : std::true_type {};
static_assert(!HasQueryCopyFrom<SvnOps>::value,
              "MVP svn_ops must not expose svn copy source queries; reconcile may add "
              "a tightly scoped exception later after review");

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/isvn-svn-ops-test-XXXXXX";
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

struct CommandResult {
    int exit_code = 0;
    std::string output;
};

CommandResult runCommand(const std::vector<std::string>& args) {
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) throw std::runtime_error("pipe failed");
    const pid_t child = ::fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        std::vector<std::string> storage = args;
        std::vector<char*> argv;
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(output_pipe[1]);
    CommandResult result;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        throw std::runtime_error("read failed");
    }
    ::close(output_pipe[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("waitpid failed");
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

bool hasSvnTools() {
    return runCommand({"svn", "--version", "--quiet"}).exit_code == 0 &&
           runCommand({"svnadmin", "--version", "--quiet"}).exit_code == 0 &&
           runCommand({"svnmucc", "--help"}).exit_code == 0;
}

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

template <typename Callable>
void requireQueryFailure(SvnQueryFailure expected, Callable&& callable,
                         const std::string& message) {
    try {
        callable();
    } catch (const SvnQueryError& error) {
        require(error.failure() == expected, message + " (wrong failure type)");
        return;
    }
    throw std::runtime_error(message + " (no failure)");
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("write file failed");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string fileUrl(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).string();
}

void mustRun(const std::vector<std::string>& args) {
    const auto result = runCommand(args);
    if (result.exit_code != 0) {
        throw std::runtime_error("command failed: " + result.output);
    }
}

struct SvnFixture {
    TemporaryDirectory temporary;
    std::filesystem::path repo_path;
    std::string repo_url;
    std::string v1_url;
    std::string v2_url;
    SvnOps ops;

    SvnFixture()
        : repo_path(temporary.path() / "repo"),
          repo_url(fileUrl(repo_path)),
          v1_url(repo_url + "/branches/IMPL/cpu/FLOORPLAN/v1"),
          v2_url(repo_url + "/branches/IMPL/cpu/FLOORPLAN/v2"),
          ops(repo_url) {
        mustRun({"svnadmin", "create", repo_path.string()});
        mustRun({"svn", "mkdir", "--parents",
                 repo_url + "/branches/IMPL/cpu/FLOORPLAN", "-m",
                 "[isvn] create branch parents"});
        const auto import_dir = temporary.path() / "import";
        writeFile(import_dir / "design.txt", "base\n");
        mustRun({"svn", "import", import_dir.string(), v1_url, "-m",
                 "[isvn] import initial branch"});
    }
};

void testVersionUrlConversion() {
    SvnOps ops("file:///tmp/repo/");
    const std::string version = "IMPL/cpu_core/FLOORPLAN/v1";
    const std::string url = "file:///tmp/repo/branches/" + version;
    require(ops.versionToUrl(version) == url, "versionToUrl must use branches layout");
    require(ops.urlToVersion(url) == std::optional<std::string>(version),
            "urlToVersion must reverse valid branch URL");
    require(!ops.urlToVersion("file:///tmp/repo/trunk/IMPL/cpu/FLOORPLAN/v1"),
            "urlToVersion must reject non-branch layout");
    requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(ops.versionToUrl("IMPL/cpu/FLOORPLAN")); },
        "versionToUrl must reject missing segment");
    requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(ops.versionToUrl("IMPL/cpu//FLOORPLAN/v1")); },
        "versionToUrl must reject empty segment");
}

void testMarkerValidation() {
    SvnOps ops("file:///tmp/repo");
    requireThrows<std::invalid_argument>(
        [&] { ops.branchCopy("file:///tmp/a", "file:///tmp/b", "missing marker"); },
        "branchCopy must reject messages without marker before invoking svn");
    requireThrows<std::invalid_argument>(
        [&] { ops.branchDelete("file:///tmp/a", "missing marker"); },
        "branchDelete must reject messages without marker before invoking svn");
    requireThrows<std::invalid_argument>(
        [&] { static_cast<void>(ops.commit("/tmp/missing-wc", "missing marker")); },
        "commit must reject messages without marker before invoking svn");
}

void testNoForbiddenLayerIncludes() {
    const auto root = std::filesystem::path(ISVN_SOURCE_DIR);
    const std::string header = readFile(root / "src/version/svn_ops.h");
    const std::string source = readFile(root / "src/version/svn_ops.cpp");
    const std::string combined = header + "\n" + source;
    require(combined.find("lineage.h") == std::string::npos,
            "svn_ops must not include Lineage");
    require(combined.find("object_store.h") == std::string::npos,
            "svn_ops must not include ObjectStore");
    require(combined.find("queryCopyFrom") == std::string::npos,
            "MVP svn_ops must not expose copy source query API");
}

void testBranchCopyCheckoutInfoLogAndCommit() {
    SvnFixture fixture;
    fixture.ops.branchCopy(fixture.v1_url, fixture.v2_url, "[isvn] create v2");
    require(runCommand({"svn", "ls", fixture.v2_url}).exit_code == 0,
            "branchCopy must create destination branch");

    const auto wc = fixture.temporary.path() / "wc";
    fixture.ops.checkout(fixture.v2_url, wc);
    require(readFile(wc / "design.txt") == "base\n", "checkout must materialize files");

    SvnInfo info = fixture.ops.info(wc);
    require(info.url == fixture.v2_url, "info URL must match checked-out branch");
    require(info.repo_root == fixture.repo_url, "info repo root must match repository");
    require(info.relative_url.find("^/branches/IMPL/cpu/FLOORPLAN/v2") !=
                std::string::npos,
            "info relative URL must identify branch path");
    require(!info.last_author.empty(), "info must include last author");

    SvnQueryLimits bounded_limits;
    SvnQueryBudget bounded_budget;
    const SvnInfo bounded_info = fixture.ops.infoBounded(wc, bounded_limits, bounded_budget);
    require(bounded_info.url == fixture.v2_url && bounded_info.repo_root == fixture.repo_url &&
                bounded_budget.process_count == 1,
            "bounded info must return complete target identity within one query budget");

    std::vector<SvnLogEntry> entries = fixture.ops.log(fixture.v2_url, 10);
    require(!entries.empty(), "log must return entries");
    require(entries.front().message.find("[isvn] create v2") != std::string::npos,
            "log must include commit message");

    writeFile(wc / "design.txt", "base\nchange\n");
    const std::int64_t commit_rev =
        fixture.ops.commit(wc, "[isvn] update design data");
    require(commit_rev > entries.front().revision, "commit must return newer revision");
    entries = fixture.ops.log(fixture.v2_url, 1);
    require(entries.front().author == isvn::common::system_user(),
            "svn commit author must be the current system user");
}

std::size_t stopOnCopyRevisionCount(const std::string& url) {
    const auto result = runCommand({"svn", "log", "--stop-on-copy", "-q", url});
    if (result.exit_code != 0) {
        throw std::runtime_error("svn log --stop-on-copy failed: " + result.output);
    }
    std::size_t count = 0;
    std::stringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.front() == 'r') ++count;
    }
    return count;
}

void testCreateBranchFromParentUsesSingleSvnmuccCommit() {
    SvnFixture fixture;
    const std::string child = "IMPL/cpu/SYN/v1";
    const std::string child_url = fixture.repo_url + "/branches/" + child;
    fixture.ops.createBranchFromParent("IMPL/cpu/FLOORPLAN/v1", child,
                                       "[isvn] create " + child +
                                           " from IMPL/cpu/FLOORPLAN/v1");
    require(runCommand({"svn", "ls", child_url}).exit_code == 0,
            "createBranchFromParent must create the destination branch");
    require(stopOnCopyRevisionCount(child_url) == 1,
            "createBranchFromParent must be a single svnmucc revision");
}

void testCreateRootBranchUsesSingleSvnmuccCommitForMissingChain() {
    SvnFixture fixture;
    const std::string child = "IMPL/gpu/SYN/v1";
    const std::string child_url = fixture.repo_url + "/branches/" + child;
    fixture.ops.createRootBranch(child, "[isvn] create " + child + " from ROOT");
    require(runCommand({"svn", "ls", child_url}).exit_code == 0,
            "createRootBranch must create the destination branch");
    require(stopOnCopyRevisionCount(child_url) == 1,
            "createRootBranch must be a single svnmucc revision");
}

void testCreateBranchFromParentFailureLeavesNoOrphanParents() {
    SvnFixture fixture;
    const std::string child = "IMPL/cpu/ROUTE/v1";
    requireThrows([&] {
        fixture.ops.createBranchFromParent("IMPL/cpu/MISSING/v1", child,
                                           "[isvn] create " + child +
                                               " from IMPL/cpu/MISSING/v1");
    }, "createBranchFromParent must fail when source parent is absent");
    require(runCommand({"svn", "ls", fixture.repo_url + "/branches/IMPL/cpu/ROUTE"}).exit_code != 0,
            "failed svnmucc create must not leave orphan intermediate directories");
}

void testAddRemoveRevertStatusUpdateAndFailure() {
    SvnFixture fixture;
    fixture.ops.branchCopy(fixture.v1_url, fixture.v2_url, "[isvn] create v2");
    const auto wc1 = fixture.temporary.path() / "wc1";
    const auto wc2 = fixture.temporary.path() / "wc2";
    fixture.ops.checkout(fixture.v2_url, wc1);
    fixture.ops.checkout(fixture.v2_url, wc2);

    writeFile(wc1 / "new.txt", "new\n");
    fixture.ops.add(wc1 / "new.txt");
    auto entries = fixture.ops.status(wc1);
    require(!entries.empty(), "status must report added file");
    require(entries.front().status == "A", "status must map added file to A");
    fixture.ops.commit(wc1, "[isvn] add new file");
    fixture.ops.update(wc2);
    require(readFile(wc2 / "new.txt") == "new\n", "update must fetch remote commit");

    writeFile(wc2 / "new.txt", "local dirty\n");
    entries = fixture.ops.status(wc2);
    require(!entries.empty(), "status must report modified file");
    require(entries.front().status == "M", "status must map modified file to M");
    fixture.ops.revert(wc2 / "new.txt");
    require(readFile(wc2 / "new.txt") == "new\n", "revert must restore file");

    fixture.ops.remove(wc1 / "new.txt");
    fixture.ops.commit(wc1, "[isvn] remove new file");
    require(runCommand({"svn", "ls", fixture.v2_url + "/new.txt"}).exit_code != 0,
            "remove plus commit must delete repository file");

    requireThrows([&] { static_cast<void>(fixture.ops.log(fixture.repo_url + "/missing")); },
                  "svn failures must throw structured errors");
}

void testBoundedQueryRunnerFaults() {
    // This fake `svn` is deliberately a child process (not a mocked parser):
    // it exercises incremental dual-pipe capture, deadline termination, and
    // complete-XML gating through the public bounded APIs.
    TemporaryDirectory temporary;
    const auto bin = temporary.path() / "bin";
    std::filesystem::create_directories(bin);
    const auto fake = bin / "svn";
    writeFile(fake,
              "#!/bin/sh\n"
              "case \"${ISVN_FAKE_SVN_MODE:-status}\" in\n"
              " status) printf '%s' '<?xml version=\"1.0\"?><status><target path=\"/x\"><entry path=\"/x\"><wc-status item=\"modified\"/></entry></target></status>' ;;\n"
              " status-exit) printf '%s' '<?xml version=\"1.0\"?><status><target path=\"/x\"><entry path=\"/x\"><wc-status item=\"modified\"/></entry></target></status>'; exit 1 ;;\n"
              " list) printf '%s' '<?xml version=\"1.0\"?><lists><list path=\"/x\"><entry kind=\"file\"><name>only.v</name></entry></list></lists>' ;;\n"
              " malformed) printf '%s' '<status><target>' ;;\n"
              " stdout) i=0; while [ \"$i\" -lt 200 ]; do printf 0123456789; i=$((i + 1)); done ;;\n"
              " stderr) i=0; while [ \"$i\" -lt 200 ]; do printf 0123456789 >&2; i=$((i + 1)); done ;;\n"
              " both) i=0; while [ \"$i\" -lt 200 ]; do printf 0123456789; printf 0123456789 >&2; i=$((i + 1)); done ;;\n"
              " stall) trap '' TERM; while :; do sleep 1; done ;;\n"
              " fork-stall) (trap '' TERM; while :; do sleep 1; done) & exit 0 ;;\n"
              " *) exit 70 ;;\n"
              "esac\n");
    std::filesystem::permissions(fake,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add);
    const char* old_path = ::getenv("PATH");
    ScopedEnvironment path("PATH", bin.string() + ":" + (old_path ? old_path : ""));
    SvnOps ops("file:///unused");
    SvnQueryLimits limits;
    limits.per_process_timeout = std::chrono::milliseconds(120);
    limits.total_timeout = std::chrono::milliseconds(900);
    limits.termination_grace = std::chrono::milliseconds(20);
    limits.max_stdout_per_process = 4096;
    limits.max_stderr_per_process = 4096;
    limits.max_stdout_total = 8192;
    limits.max_stderr_total = 8192;

    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "status");
        SvnQueryBudget budget;
        const auto status = ops.statusBounded({"/selected/file.v"}, limits, budget);
        require(status.size() == 1 && status[0].path == "/x" && status[0].status == "M",
                "complete bounded status XML must parse");
        require(budget.process_count == 1 && budget.stdout_bytes > 0,
                "bounded status accounts for the spawned process and capture");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "list");
        SvnQueryBudget budget;
        const auto list = ops.listImmediateBounded("file:///unused/parent", limits, budget);
        require(list.exists && list.entries.size() == 1 && list.entries[0].name == "only.v" &&
                    !list.entries[0].is_directory,
                "complete immediate-list XML must parse exact child evidence");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "malformed");
        SvnQueryBudget budget;
        requireQueryFailure(SvnQueryFailure::Failed,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, limits, budget)); },
                            "truncated XML must never parse as bounded status success");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "status-exit");
        SvnQueryBudget budget;
        requireQueryFailure(SvnQueryFailure::Failed,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, limits, budget)); },
                            "non-zero status with partial-looking XML must fail closed");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "stdout");
        SvnQueryLimits small = limits;
        small.max_stdout_per_process = 128;
        small.max_stdout_total = 128;
        SvnQueryBudget budget;
        requireQueryFailure(SvnQueryFailure::OutputLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, small, budget)); },
                            "stdout cap must fail before a complete plan can parse");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "stderr");
        SvnQueryLimits small = limits;
        small.max_stderr_per_process = 128;
        small.max_stderr_total = 128;
        SvnQueryBudget budget;
        requireQueryFailure(SvnQueryFailure::OutputLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, small, budget)); },
                            "stderr cap must fail before a complete plan can parse");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "both");
        SvnQueryLimits small = limits;
        small.max_stdout_per_process = 256;
        small.max_stderr_per_process = 256;
        SvnQueryBudget budget;
        requireQueryFailure(SvnQueryFailure::OutputLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, small, budget)); },
                            "concurrent stdout/stderr pressure must not deadlock the bounded runner");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "stall");
        SvnQueryBudget budget;
        const auto started = std::chrono::steady_clock::now();
        requireQueryFailure(SvnQueryFailure::Timeout,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, limits, budget)); },
                            "stalled query must hit its monotonic deadline");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < std::chrono::seconds(2),
                "timeout must terminate and reap the stalled process group promptly");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "fork-stall");
        SvnQueryBudget budget;
        const auto started = std::chrono::steady_clock::now();
        requireQueryFailure(SvnQueryFailure::Timeout,
                            [&] { static_cast<void>(ops.statusBounded({"/x"}, limits, budget)); },
                            "descendant-held pipes must be terminated with the query process group");
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                "descendant-held pipes cannot leave the bounded runner hanging");
    }
    {
        ScopedEnvironment mode("ISVN_FAKE_SVN_MODE", "status");
        SvnQueryLimits small = limits;
        small.max_processes = 2;
        small.max_stdout_per_process = 4096;
        small.max_stdout_total = 8192;
        SvnQueryBudget budget;
        static_cast<void>(ops.statusBounded({"/one"}, small, budget));
        static_cast<void>(ops.statusBounded({"/two"}, small, budget));
        requireQueryFailure(SvnQueryFailure::ProcessLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/three"}, small, budget)); },
                            "process count is cumulative across a bounded operation");
        SvnQueryBudget argv_budget;
        small.max_processes = 10;
        small.max_argv_bytes = 8;
        requireQueryFailure(SvnQueryFailure::ArgvLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/too-long"}, small, argv_budget)); },
                            "argv ceiling must fail before spawning a process");
        SvnQueryLimits cumulative = limits;
        cumulative.max_processes = 10;
        cumulative.max_stdout_per_process = 4096;
        cumulative.max_stdout_total = 300;
        SvnQueryBudget cumulative_budget;
        static_cast<void>(ops.statusBounded({"/one"}, cumulative, cumulative_budget));
        static_cast<void>(ops.statusBounded({"/two"}, cumulative, cumulative_budget));
        requireQueryFailure(SvnQueryFailure::OutputLimit,
                            [&] { static_cast<void>(ops.statusBounded({"/three"}, cumulative,
                                                                        cumulative_budget)); },
                            "stdout accounting is cumulative across query chunks");
    }
}

void testBranchDelete() {
    SvnFixture fixture;
    fixture.ops.branchCopy(fixture.v1_url, fixture.v2_url, "[isvn] create v2");
    fixture.ops.branchDelete(fixture.v2_url, "[isvn] delete v2");
    require(runCommand({"svn", "ls", fixture.v2_url}).exit_code != 0,
            "branchDelete must remove branch URL");
}

}  // namespace

int main() {
    try {
        testVersionUrlConversion();
        testMarkerValidation();
        testNoForbiddenLayerIncludes();

        if (!hasSvnTools()) {
            std::cout << "svn/svnadmin not found; skipped live svn_ops integration tests\n";
            return 0;
        }

        testBranchCopyCheckoutInfoLogAndCommit();
        testCreateBranchFromParentUsesSingleSvnmuccCommit();
        testCreateRootBranchUsesSingleSvnmuccCommitForMissingChain();
        testCreateBranchFromParentFailureLeavesNoOrphanParents();
        testAddRemoveRevertStatusUpdateAndFailure();
        testBranchDelete();
        testBoundedQueryRunnerFaults();
    } catch (const std::exception& error) {
        std::cerr << "test_svn_ops failed: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
