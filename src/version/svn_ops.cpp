#include "version/svn_ops.h"

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace isvn::version {
namespace {

std::string trimTrailingSlash(std::string value) {
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string escapePath(const std::filesystem::path& path) {
    return path.string();
}

std::string decodeXml(std::string value) {
    struct Replacement {
        const char* from;
        const char* to;
    };
    const Replacement replacements[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"},
        {"&gt;", ">"},    {"&amp;", "&"},
    };
    for (const auto& replacement : replacements) {
        std::size_t position = 0;
        while ((position = value.find(replacement.from, position)) != std::string::npos) {
            value.replace(position, std::strlen(replacement.from), replacement.to);
            position += std::strlen(replacement.to);
        }
    }
    return value;
}

std::optional<std::string> tagText(const std::string& xml, const std::string& tag,
                                   std::size_t offset = 0) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::size_t begin = xml.find(open, offset);
    if (begin == std::string::npos) return std::nullopt;
    const std::size_t value_begin = begin + open.size();
    const std::size_t end = xml.find(close, value_begin);
    if (end == std::string::npos) return std::nullopt;
    return decodeXml(xml.substr(value_begin, end - value_begin));
}

std::optional<std::string> attributeText(const std::string& text,
                                         const std::string& attribute,
                                         std::size_t offset = 0) {
    const std::string pattern = attribute + "=\"";
    const std::size_t begin = text.find(pattern, offset);
    if (begin == std::string::npos) return std::nullopt;
    const std::size_t value_begin = begin + pattern.size();
    const std::size_t end = text.find('"', value_begin);
    if (end == std::string::npos) return std::nullopt;
    return decodeXml(text.substr(value_begin, end - value_begin));
}

void removeTagBlocks(std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";
    std::size_t position = 0;
    while ((position = xml.find(open, position)) != std::string::npos) {
        const std::size_t open_end = xml.find('>', position);
        if (open_end == std::string::npos) return;
        if (xml[open_end - 1] == '/') {
            xml.erase(position, open_end - position + 1);
            continue;
        }
        const std::size_t close_begin = xml.find(close, open_end + 1);
        if (close_begin == std::string::npos) return;
        xml.erase(position, close_begin + close.size() - position);
    }
}

std::string removeSvnCopySourceNodes(std::string xml) {
// Guard: svn XML may contain path-origin metadata. General log/info APIs
    // must never parse or return those nodes; Lineage remains the ancestry source.
    removeTagBlocks(xml, "copy-from-path");
    removeTagBlocks(xml, "copy-from-rev");
    return xml;
}

std::int64_t parseInteger(const std::string& value, const std::string& field) {
    std::size_t consumed = 0;
    const long long parsed = std::stoll(value, &consumed);
    if (consumed != value.size()) throw std::runtime_error("Invalid SVN " + field);
    return static_cast<std::int64_t>(parsed);
}

std::string statusCodeFromXmlItem(const std::string& item) {
    if (item == "modified") return "M";
    if (item == "added") return "A";
    if (item == "deleted") return "D";
    if (item == "unversioned") return "?";
    if (item == "missing") return "!";
    if (item == "conflicted") return "C";
    if (item == "replaced") return "R";
    if (item == "external") return "X";
    if (item == "ignored") return "I";
    if (item == "incomplete") return "!";
    if (item == "normal" || item == "none") return "";
    return item;
}

std::string firstNonemptyError(const SvnResult& result) {
    if (!result.stderr_text.empty()) return result.stderr_text;
    if (!result.stdout_text.empty()) return result.stdout_text;
    return "svn exited with code " + std::to_string(result.exit_code);
}

void setCloseOnExec(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw std::runtime_error("Unable to set close-on-exec on pipe");
    }
}

void setNonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("Unable to set nonblocking pipe");
    }
}

std::vector<std::string> splitVersionPath(const std::string& version_path) {
    if (version_path.empty() || version_path.front() == '/' ||
        version_path.find("//") != std::string::npos) {
        throw std::invalid_argument("Version path must be four non-empty relative segments");
    }
    std::vector<std::string> parts;
    std::stringstream stream(version_path);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == "." || part == "..") {
            throw std::invalid_argument("Version path contains an invalid segment");
        }
        parts.push_back(part);
    }
    if (parts.size() != 4) {
        throw std::invalid_argument("Version path must contain phase/block/stage/version");
    }
    return parts;
}

std::vector<std::string> parentChainUrls(const std::string& repo_root,
                                         const std::string& version_path,
                                         bool include_version) {
    const auto parts = splitVersionPath(version_path);
    std::vector<std::string> urls;
    std::string url = repo_root + "/branches";
    const std::size_t limit = include_version ? parts.size() : parts.size() - 1;
    for (std::size_t index = 0; index < limit; ++index) {
        url += "/" + parts[index];
        urls.push_back(url);
    }
    return urls;
}

std::vector<std::string> nonemptyLines(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

bool hasCompleteXmlRoot(const std::string& xml, const std::string& root) {
    return xml.find("<" + root) != std::string::npos &&
           xml.rfind("</" + root + ">") != std::string::npos;
}

bool isStableNotFound(const SvnResult& result) {
    // SVN's repository-side missing-node diagnostic is stable for the file://
    // repositories used by isvn.  Do not treat any other non-zero exit as
    // absence: auth, transport, malformed URLs, and incomplete output fail
    // closed in the bounded Checkin path.
    return result.exit_code != 0 &&
           (result.stderr_text.find("E160013") != std::string::npos ||
            result.stderr_text.find("E200009") != std::string::npos);
}

std::string queryFailureText(SvnQueryFailure failure) {
    switch (failure) {
        case SvnQueryFailure::ProcessLimit: return "SVN query process budget exceeded";
        case SvnQueryFailure::ArgvLimit: return "SVN query argv budget exceeded";
        case SvnQueryFailure::Timeout: return "SVN query timed out";
        case SvnQueryFailure::OutputLimit: return "SVN query output budget exceeded";
        case SvnQueryFailure::Failed: return "SVN query failed";
    }
    return "SVN query failed";
}

}  // namespace

SvnQueryError::SvnQueryError(SvnQueryFailure failure)
    : std::runtime_error(queryFailureText(failure)), failure_(failure) {}

SvnOps::SvnOps(const std::string& repo_root_url)
    : repo_root_(trimTrailingSlash(repo_root_url)) {
    if (repo_root_.empty()) throw std::invalid_argument("SVN repo root URL is required");
}

void SvnOps::branchCopy(const std::string& source_url, const std::string& dest_url,
                        const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    requireSuccess(runSvn({"copy", source_url, dest_url, "-m", commit_message}),
                   "svn copy");
}

void SvnOps::createBranchFromParent(const std::string& parent_version,
                                    const std::string& child_version,
                                    const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    const std::string source_url = versionToUrl(parent_version);
    const std::string dest_url = versionToUrl(child_version);
    std::vector<std::string> args;
    for (const auto& url : parentChainUrls(repo_root_, child_version, false)) {
        try {
            static_cast<void>(list(url, false));
        } catch (const std::exception&) {
            args.push_back("mkdir");
            args.push_back(url);
        }
    }
    args.push_back("cp");
    args.push_back("HEAD");
    args.push_back(source_url);
    args.push_back(dest_url);
    args.push_back("-m");
    args.push_back(commit_message);
    requireSuccess(runSvnmucc(args), "svnmucc create branch");
}

void SvnOps::createRootBranch(const std::string& child_version,
                              const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    std::vector<std::string> args;
    for (const auto& url : parentChainUrls(repo_root_, child_version, true)) {
        try {
            static_cast<void>(list(url, false));
        } catch (const std::exception&) {
            args.push_back("mkdir");
            args.push_back(url);
        }
    }
    args.push_back("-m");
    args.push_back(commit_message);
    requireSuccess(runSvnmucc(args), "svnmucc create root branch");
}

void SvnOps::branchDelete(const std::string& branch_url,
                          const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    requireSuccess(runSvn({"delete", branch_url, "-m", commit_message}), "svn delete");
}

void SvnOps::mkdir(const std::string& url, const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    requireSuccess(runSvn({"mkdir", "--parents", url, "-m", commit_message}),
                   "svn mkdir");
}

void SvnOps::checkout(const std::string& url, const std::filesystem::path& local_path,
                      std::optional<std::int64_t> revision) {
    std::vector<std::string> args{"checkout"};
    if (revision.has_value()) {
        args.push_back("-r");
        args.push_back(std::to_string(*revision));
    }
    args.push_back(url);
    args.push_back(escapePath(local_path));
    requireSuccess(runSvn(args), "svn checkout");
}

void SvnOps::update(const std::filesystem::path& wc_path,
                    std::optional<std::int64_t> revision) {
    std::vector<std::string> args{"update"};
    if (revision.has_value()) {
        args.push_back("-r");
        args.push_back(std::to_string(*revision));
    }
    args.push_back(escapePath(wc_path));
    requireSuccess(runSvn(args), "svn update");
}

std::int64_t SvnOps::commit(const std::filesystem::path& wc_path,
                            const std::string& commit_message) {
    requireIsvnMarker(commit_message);
    const SvnResult result = runSvn({"commit", escapePath(wc_path), "-m", commit_message});
    requireSuccess(result, "svn commit");
    const std::string marker = "Committed revision ";
    const std::size_t begin = result.stdout_text.find(marker);
    if (begin == std::string::npos) {
        throw std::runtime_error("Unable to parse svn commit revision");
    }
    const std::size_t number_begin = begin + marker.size();
    const std::size_t number_end = result.stdout_text.find_first_not_of("0123456789",
                                                                        number_begin);
    return parseInteger(result.stdout_text.substr(number_begin, number_end - number_begin),
                        "commit revision");
}

void SvnOps::add(const std::filesystem::path& path, bool depth_empty) {
    std::vector<std::string> args{"add"};
    if (depth_empty) { args.push_back("--depth"); args.push_back("empty"); }
    args.push_back(escapePath(path));
    requireSuccess(runSvn(args), "svn add");
}

void SvnOps::remove(const std::filesystem::path& path) {
    requireSuccess(runSvn({"delete", escapePath(path)}), "svn delete");
}

void SvnOps::revert(const std::filesystem::path& path) {
    requireSuccess(runSvn({"revert", escapePath(path)}), "svn revert");
}

std::vector<SvnStatusEntry> SvnOps::status(const std::filesystem::path& wc_path) const {
    const SvnResult result = runSvn({"status", "--xml", escapePath(wc_path)});
    requireSuccess(result, "svn status");
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    std::vector<SvnStatusEntry> entries;
    std::size_t position = 0;
    while ((position = xml.find("<entry", position)) != std::string::npos) {
        const std::size_t entry_end = xml.find('>', position);
        if (entry_end == std::string::npos) break;
        const std::size_t block_end = xml.find("</entry>", entry_end);
        if (block_end == std::string::npos) break;
        SvnStatusEntry entry;
        entry.path = attributeText(xml.substr(position, entry_end - position), "path")
                         .value_or("");
        const std::size_t wc_status = xml.find("<wc-status", entry_end);
        if (wc_status != std::string::npos && wc_status < block_end) {
            const std::size_t status_end = xml.find('>', wc_status);
            if (status_end != std::string::npos) {
                entry.status = statusCodeFromXmlItem(
                    attributeText(xml.substr(wc_status, status_end - wc_status), "item")
                        .value_or(""));
            }
        }
        if (!entry.path.empty()) entries.push_back(entry);
        position = block_end + 8;
    }
    return entries;
}

std::vector<SvnStatusEntry> SvnOps::statusBounded(
    const std::vector<std::filesystem::path>& targets, const SvnQueryLimits& limits,
    SvnQueryBudget& budget) const {
    if (targets.empty()) return {};
    std::vector<SvnStatusEntry> entries;
    std::vector<std::string> args{"status", "--xml", "--depth", "infinity"};
    std::size_t bytes = 0;
    for (const auto& arg : args) bytes += arg.size() + 1;
    auto consume = [&]() {
        const SvnResult result = runSvnBounded(args, limits, budget);
        // A non-zero status can still carry syntactically valid XML for a
        // subset of targets.  That is incomplete evidence, never a valid
        // Checkin plan.
        if (result.exit_code != 0) throw SvnQueryError(SvnQueryFailure::Failed);
        const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
        if (!hasCompleteXmlRoot(xml, "status")) throw SvnQueryError(SvnQueryFailure::Failed);
        std::size_t position = 0;
        while ((position = xml.find("<entry", position)) != std::string::npos) {
            const std::size_t entry_end = xml.find('>', position);
            const std::size_t block_end = xml.find("</entry>", entry_end);
            if (entry_end == std::string::npos || block_end == std::string::npos) {
                throw SvnQueryError(SvnQueryFailure::Failed);
            }
            SvnStatusEntry entry;
            entry.path = attributeText(xml.substr(position, entry_end - position), "path")
                             .value_or("");
            const std::size_t wc_status = xml.find("<wc-status", entry_end);
            if (wc_status != std::string::npos && wc_status < block_end) {
                const std::size_t status_end = xml.find('>', wc_status);
                if (status_end == std::string::npos) throw SvnQueryError(SvnQueryFailure::Failed);
                const auto item = attributeText(xml.substr(wc_status, status_end - wc_status), "item");
                if (!item.has_value()) throw SvnQueryError(SvnQueryFailure::Failed);
                entry.status = statusCodeFromXmlItem(*item);
            }
            if (entry.path.empty()) throw SvnQueryError(SvnQueryFailure::Failed);
            entries.push_back(std::move(entry));
            position = block_end + 8;
        }
    };
    for (const auto& target : targets) {
        const std::string value = escapePath(target);
        if (value.size() + 1 > limits.max_argv_bytes || bytes > limits.max_argv_bytes - value.size() - 1) {
            if (args.size() == 4) throw SvnQueryError(SvnQueryFailure::ArgvLimit);
            consume();
            args = {"status", "--xml", "--depth", "infinity"};
            bytes = 0;
            for (const auto& arg : args) bytes += arg.size() + 1;
            if (value.size() + 1 > limits.max_argv_bytes - bytes) {
                throw SvnQueryError(SvnQueryFailure::ArgvLimit);
            }
        }
        args.push_back(value);
        bytes += value.size() + 1;
    }
    if (args.size() > 4) consume();
    return entries;
}

std::vector<SvnLogEntry> SvnOps::log(const std::string& url_or_path, int limit) const {
    if (limit <= 0) throw std::invalid_argument("SVN log limit must be positive");
    const SvnResult result =
        runSvn({"log", "--xml", "--limit", std::to_string(limit), url_or_path});
    requireSuccess(result, "svn log");
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    std::vector<SvnLogEntry> entries;
    std::size_t position = 0;
    while ((position = xml.find("<logentry", position)) != std::string::npos) {
        const std::size_t open_end = xml.find('>', position);
        const std::size_t block_end = xml.find("</logentry>", position);
        if (open_end == std::string::npos || block_end == std::string::npos) break;
        const std::string open = xml.substr(position, open_end - position);
        const std::string block = xml.substr(open_end + 1, block_end - open_end - 1);
        SvnLogEntry entry;
        entry.revision = parseInteger(attributeText(open, "revision").value_or("0"),
                                      "log revision");
        entry.author = tagText(block, "author").value_or("");
        entry.date = tagText(block, "date").value_or("");
        entry.message = tagText(block, "msg").value_or("");
        entries.push_back(entry);
        position = block_end + 11;
    }
    return entries;
}

std::vector<SvnVerboseLogEntry> SvnOps::logVerbose(const std::string& target_url,
                                                   std::int64_t from_revision,
                                                   std::int64_t to_revision) const {
    if (from_revision < 0 || to_revision < from_revision) {
        throw std::invalid_argument("SVN verbose log requires 0 <= from <= to");
    }
    const std::string range = std::to_string(from_revision) + ":" + std::to_string(to_revision);
    const SvnResult result =
        runSvn({"log", "--xml", "--verbose", "-r", range, target_url});
    requireSuccess(result, "svn log --verbose");
// Strip copy-source metadata. Deletes carry none, so the paths survive.
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    if (xml.find("<log") == std::string::npos) {
        throw std::runtime_error("svn verbose log produced no parseable <log> root");
    }
    std::vector<SvnVerboseLogEntry> entries;
    std::size_t position = 0;
    while ((position = xml.find("<logentry", position)) != std::string::npos) {
        const std::size_t open_end = xml.find('>', position);
        const std::size_t block_end = xml.find("</logentry>", position);
        if (open_end == std::string::npos || block_end == std::string::npos) {
            throw std::runtime_error("svn verbose log XML is truncated");
        }
        const std::string open = xml.substr(position, open_end - position);
        const std::string block = xml.substr(open_end + 1, block_end - open_end - 1);
        SvnVerboseLogEntry entry;
        entry.revision = parseInteger(attributeText(open, "revision").value_or("0"),
                                      "verbose log revision");
        entry.author = tagText(block, "author").value_or("");
        entry.date = tagText(block, "date").value_or("");
        entry.message = tagText(block, "msg").value_or("");
        // Parse the <paths> block: each <path action="X" ...>repo/abs/path</path>.
        const std::size_t paths_begin = block.find("<paths>");
        const std::size_t paths_end = block.find("</paths>");
        if (paths_begin != std::string::npos && paths_end != std::string::npos &&
            paths_end > paths_begin) {
            // Inner content only, so the "<paths>" container tag is not mistaken
            // for a "<path>" element.
            const std::size_t inner = paths_begin + std::string("<paths>").size();
            const std::string paths = block.substr(inner, paths_end - inner);
            std::size_t p = 0;
            while ((p = paths.find("<path", p)) != std::string::npos) {
                const std::size_t tag_end = paths.find('>', p);
                const std::size_t close = paths.find("</path>", p);
                if (tag_end == std::string::npos || close == std::string::npos) {
                    throw std::runtime_error("svn verbose log <path> is truncated");
                }
                const std::string tag = paths.substr(p, tag_end - p);
                SvnChangedPath changed;
                changed.action = attributeText(tag, "action").value_or("");
                changed.path = decodeXml(paths.substr(tag_end + 1, close - tag_end - 1));
                entry.paths.push_back(changed);
                p = close + 7;
            }
        }
        entries.push_back(entry);
        position = block_end + 11;
    }
    return entries;
}

std::vector<SvnVerboseLogEntry> SvnOps::logVerboseBounded(
    const std::string& target_url, std::int64_t from_revision, std::int64_t to_revision,
    const SvnQueryLimits& limits, SvnQueryBudget& budget) const {
    if (from_revision < 0 || to_revision < from_revision) {
        throw std::invalid_argument("SVN verbose log requires 0 <= from <= to");
    }
    const std::string range = std::to_string(from_revision) + ":" + std::to_string(to_revision);
    const SvnResult result =
        runSvnBounded({"log", "--xml", "--verbose", "-r", range, target_url}, limits, budget);
    if (result.exit_code != 0) throw SvnQueryError(SvnQueryFailure::Failed);
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    if (!hasCompleteXmlRoot(xml, "log")) throw SvnQueryError(SvnQueryFailure::Failed);
    std::vector<SvnVerboseLogEntry> entries;
    std::size_t position = 0;
    while ((position = xml.find("<logentry", position)) != std::string::npos) {
        const std::size_t open_end = xml.find('>', position);
        const std::size_t block_end = xml.find("</logentry>", position);
        if (open_end == std::string::npos || block_end == std::string::npos) {
            throw SvnQueryError(SvnQueryFailure::Failed);
        }
        const std::string open = xml.substr(position, open_end - position);
        const std::string block = xml.substr(open_end + 1, block_end - open_end - 1);
        const auto revision = attributeText(open, "revision");
        const auto author = tagText(block, "author");
        const auto message = tagText(block, "msg");
        if (!revision.has_value() || !author.has_value() || !message.has_value()) {
            throw SvnQueryError(SvnQueryFailure::Failed);
        }
        SvnVerboseLogEntry entry;
        entry.revision = parseInteger(*revision, "verbose log revision");
        entry.author = *author;
        entry.date = tagText(block, "date").value_or("");
        entry.message = *message;
        const std::size_t paths_begin = block.find("<paths>");
        const std::size_t paths_end = block.find("</paths>");
        if ((paths_begin == std::string::npos) != (paths_end == std::string::npos) ||
            (paths_begin != std::string::npos && paths_end <= paths_begin)) {
            throw SvnQueryError(SvnQueryFailure::Failed);
        }
        if (paths_begin != std::string::npos) {
            const std::size_t inner = paths_begin + std::string("<paths>").size();
            const std::string paths = block.substr(inner, paths_end - inner);
            std::size_t p = 0;
            while ((p = paths.find("<path", p)) != std::string::npos) {
                const std::size_t tag_end = paths.find('>', p);
                const std::size_t close = paths.find("</path>", p);
                if (tag_end == std::string::npos || close == std::string::npos) {
                    throw SvnQueryError(SvnQueryFailure::Failed);
                }
                const auto action = attributeText(paths.substr(p, tag_end - p), "action");
                if (!action.has_value() || action->empty()) throw SvnQueryError(SvnQueryFailure::Failed);
                entry.paths.push_back(SvnChangedPath{*action,
                    decodeXml(paths.substr(tag_end + 1, close - tag_end - 1))});
                p = close + 7;
            }
        }
        entries.push_back(std::move(entry));
        position = block_end + 11;
    }
    return entries;
}

std::int64_t SvnOps::headRevision(const std::string& url) const {
    const SvnResult result = runSvn({"info", "--show-item", "revision", url});
    requireSuccess(result, "svn info --show-item revision");
    std::string trimmed = result.stdout_text;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                               trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    return parseInteger(trimmed, "head revision");
}

std::int64_t SvnOps::headRevisionBounded(const std::string& url,
                                         const SvnQueryLimits& limits,
                                         SvnQueryBudget& budget) const {
    const SvnResult result =
        runSvnBounded({"info", "--show-item", "revision", url}, limits, budget);
    if (result.exit_code != 0) throw SvnQueryError(SvnQueryFailure::Failed);
    std::string trimmed = result.stdout_text;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' ||
                               trimmed.back() == ' ')) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) throw SvnQueryError(SvnQueryFailure::Failed);
    try {
        return parseInteger(trimmed, "head revision");
    } catch (const std::exception&) {
        throw SvnQueryError(SvnQueryFailure::Failed);
    }
}

std::int64_t SvnOps::commitPaths(const std::vector<std::filesystem::path>& paths,
                                 const std::string& commit_message, bool depth_empty) const {
    requireIsvnMarker(commit_message);
    if (paths.empty()) throw std::invalid_argument("commitPaths requires at least one target");
    std::vector<std::string> args{"commit"};
    if (depth_empty) { args.push_back("--depth"); args.push_back("empty"); }
    for (const auto& path : paths) args.push_back(escapePath(path));
    args.push_back("-m");
    args.push_back(commit_message);
    const SvnResult result = runSvn(args);
    requireSuccess(result, "svn commit <paths>");
    const std::string marker = "Committed revision ";
    const std::size_t begin = result.stdout_text.find(marker);
    if (begin == std::string::npos) {
        throw std::runtime_error("Unable to parse svn commit revision");
    }
    const std::size_t number_begin = begin + marker.size();
    const std::size_t number_end =
        result.stdout_text.find_first_not_of("0123456789", number_begin);
    return parseInteger(result.stdout_text.substr(number_begin, number_end - number_begin),
                        "commit revision");
}

SvnInfo SvnOps::info(const std::filesystem::path& wc_path) const {
    const SvnResult result = runSvn({"info", "--xml", escapePath(wc_path)});
    requireSuccess(result, "svn info");
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    const std::size_t entry_begin = xml.find("<entry");
    const std::size_t entry_open_end = xml.find('>', entry_begin);
    if (entry_begin == std::string::npos || entry_open_end == std::string::npos) {
        throw std::runtime_error("Unable to parse svn info entry");
    }
    SvnInfo info;
    info.revision = parseInteger(
        attributeText(xml.substr(entry_begin, entry_open_end - entry_begin), "revision")
            .value_or("0"),
        "info revision");
    info.url = tagText(xml, "url").value_or("");
    info.relative_url = tagText(xml, "relative-url").value_or("");
    info.repo_root = tagText(xml, "root").value_or("");
    const std::size_t commit_begin = xml.find("<commit");
    if (commit_begin != std::string::npos) {
        const std::size_t commit_end = xml.find("</commit>", commit_begin);
        if (commit_end != std::string::npos) {
            const std::string commit_block =
                xml.substr(commit_begin, commit_end - commit_begin);
            info.last_author = tagText(commit_block, "author").value_or("");
            info.last_date = tagText(commit_block, "date").value_or("");
        }
    }
    return info;
}

SvnInfo SvnOps::infoBounded(const std::filesystem::path& target,
                             const SvnQueryLimits& limits, SvnQueryBudget& budget) const {
    const SvnResult result = runSvnBounded({"info", "--xml", escapePath(target)}, limits, budget);
    if (result.exit_code != 0) throw SvnQueryError(SvnQueryFailure::Failed);
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    if (!hasCompleteXmlRoot(xml, "info")) throw SvnQueryError(SvnQueryFailure::Failed);
    const std::size_t entry_begin = xml.find("<entry");
    const std::size_t entry_open_end = xml.find('>', entry_begin);
    if (entry_begin == std::string::npos || entry_open_end == std::string::npos) {
        throw SvnQueryError(SvnQueryFailure::Failed);
    }
    try {
        SvnInfo info;
        info.revision = parseInteger(
            attributeText(xml.substr(entry_begin, entry_open_end - entry_begin), "revision")
                .value_or("0"),
            "info revision");
        info.url = tagText(xml, "url").value_or("");
        info.relative_url = tagText(xml, "relative-url").value_or("");
        info.repo_root = tagText(xml, "root").value_or("");
        if (info.url.empty() || info.repo_root.empty()) throw SvnQueryError(SvnQueryFailure::Failed);
        const std::size_t commit_begin = xml.find("<commit");
        if (commit_begin != std::string::npos) {
            const std::size_t commit_end = xml.find("</commit>", commit_begin);
            if (commit_end == std::string::npos) throw SvnQueryError(SvnQueryFailure::Failed);
            const std::string commit_block = xml.substr(commit_begin, commit_end - commit_begin);
            info.last_author = tagText(commit_block, "author").value_or("");
            info.last_date = tagText(commit_block, "date").value_or("");
        }
        return info;
    } catch (const SvnQueryError&) {
        throw;
    } catch (const std::exception&) {
        throw SvnQueryError(SvnQueryFailure::Failed);
    }
}

std::vector<std::string> SvnOps::list(const std::string& url, bool recursive) const {
    std::vector<std::string> args{"list"};
    if (recursive) args.push_back("-R");
    args.push_back(url);
    const SvnResult result = runSvn(args);
    requireSuccess(result, "svn list");
    return nonemptyLines(result.stdout_text);
}

SvnImmediateListResult SvnOps::listImmediateBounded(const std::string& url,
                                                     const SvnQueryLimits& limits,
                                                     SvnQueryBudget& budget) const {
    const SvnResult result =
        runSvnBounded({"list", "--xml", "--depth", "immediates", url}, limits, budget);
    if (result.exit_code != 0) {
        if (isStableNotFound(result)) return SvnImmediateListResult{false, {}};
        throw SvnQueryError(SvnQueryFailure::Failed);
    }
    const std::string xml = removeSvnCopySourceNodes(result.stdout_text);
    if (!hasCompleteXmlRoot(xml, "lists")) throw SvnQueryError(SvnQueryFailure::Failed);
    SvnImmediateListResult parsed;
    std::size_t position = 0;
    while ((position = xml.find("<entry", position)) != std::string::npos) {
        const std::size_t entry_end = xml.find('>', position);
        const std::size_t block_end = xml.find("</entry>", entry_end);
        if (entry_end == std::string::npos || block_end == std::string::npos) {
            throw SvnQueryError(SvnQueryFailure::Failed);
        }
        const auto kind = attributeText(xml.substr(position, entry_end - position), "kind");
        const auto name = tagText(xml.substr(entry_end + 1, block_end - entry_end - 1), "name");
        if (!kind.has_value() || !name.has_value() || name->empty() ||
            (*kind != "file" && *kind != "dir")) {
            throw SvnQueryError(SvnQueryFailure::Failed);
        }
        parsed.entries.push_back(SvnListEntry{*name, *kind == "dir"});
        position = block_end + 8;
    }
    return parsed;
}

std::string SvnOps::versionToUrl(const std::string& version_path) const {
    const auto parts = splitVersionPath(version_path);
    std::string url = repo_root_ + "/branches";
    for (const auto& part : parts) url += "/" + part;
    return url;
}

std::optional<std::string> SvnOps::urlToVersion(const std::string& url) const {
    const std::string prefix = repo_root_ + "/branches/";
    if (url.rfind(prefix, 0) != 0) return std::nullopt;
    const std::string version = url.substr(prefix.size());
    try {
        splitVersionPath(version);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return version;
}

SvnResult SvnOps::runTool(const std::string& program,
                          const std::vector<std::string>& args) const {
    return runToolBounded(program, args, std::numeric_limits<std::size_t>::max(),
                          std::numeric_limits<std::size_t>::max(),
                          std::chrono::milliseconds{0}, std::chrono::milliseconds{0});
}

SvnResult SvnOps::runToolBounded(const std::string& program,
                                 const std::vector<std::string>& args,
                                 std::size_t stdout_limit, std::size_t stderr_limit,
                                 std::chrono::milliseconds timeout,
                                 std::chrono::milliseconds termination_grace) const {
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (::pipe(stdout_pipe) != 0) {
        throw std::runtime_error("Unable to create svn subprocess pipes");
    }
    if (::pipe(stderr_pipe) != 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        throw std::runtime_error("Unable to create svn subprocess pipes");
    }
    setCloseOnExec(stdout_pipe[0]);
    setCloseOnExec(stdout_pipe[1]);
    setCloseOnExec(stderr_pipe[0]);
    setCloseOnExec(stderr_pipe[1]);

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        throw std::runtime_error("Unable to fork svn subprocess");
    }
    if (child == 0) {
        // Checkin's bounded query path owns a process group.  Descendants
        // normally inherit it, so a stalled helper cannot keep pipes open after
        // the parent is terminated.  Legacy callers use this same safe group
        // setup but retain their historical unbounded capture/deadline policy.
        ::setpgid(0, 0);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);

        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.emplace_back(program);
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        ::execvp(program.c_str(), argv.data());
        const std::string message =
            std::string("execvp ") + program + " failed: " + std::strerror(errno) + "\n";
        ::write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
    }

    // Races with the child-side setpgid are harmless (EACCES/ESRCH means the
    // child has already exec'd/exited); it is still in its own group in the
    // normal case.
    (void)::setpgid(child, child);

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    setNonblocking(stdout_pipe[0]);
    setNonblocking(stderr_pipe[0]);

    SvnResult result;
    int stdout_fd = stdout_pipe[0];
    int stderr_fd = stderr_pipe[0];
    const auto started = std::chrono::steady_clock::now();
    const bool bounded = timeout.count() > 0;
    const auto deadline = started + timeout;
    bool terminating = false;
    bool force_killed = false;
    auto force_kill_at = deadline;
    auto terminate_group = [&](bool force) {
        const int signal = force ? SIGKILL : SIGTERM;
        if (::kill(-child, signal) != 0 && errno != ESRCH) result.runner_error = true;
        if (!force) {
            terminating = true;
            force_kill_at = std::chrono::steady_clock::now() + termination_grace;
        } else {
            force_killed = true;
        }
    };
    auto trigger_timeout = [&]() {
        if (!terminating) {
            result.timed_out = true;
            terminate_group(false);
        }
    };
    auto trigger_output = [&](bool stdout_stream) {
        if (stdout_stream) result.stdout_limit_exceeded = true;
        else result.stderr_limit_exceeded = true;
        if (!terminating) terminate_group(false);
    };
    while (stdout_fd >= 0 || stderr_fd >= 0) {
        const auto now = std::chrono::steady_clock::now();
        if (bounded && !terminating && now >= deadline) trigger_timeout();
        if (terminating && !force_killed && now >= force_kill_at) terminate_group(true);
        fd_set read_set;
        FD_ZERO(&read_set);
        int max_fd = -1;
        if (stdout_fd >= 0) {
            FD_SET(stdout_fd, &read_set);
            max_fd = std::max(max_fd, stdout_fd);
        }
        if (stderr_fd >= 0) {
            FD_SET(stderr_fd, &read_set);
            max_fd = std::max(max_fd, stderr_fd);
        }
        timeval wait{};
        timeval* wait_ptr = nullptr;
        if (bounded || terminating) {
            const auto next = terminating && !force_killed ? force_kill_at : deadline;
            const auto remaining = next > now ? next - now : std::chrono::steady_clock::duration::zero();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
            wait.tv_sec = static_cast<time_t>(micros / 1000000);
            wait.tv_usec = static_cast<suseconds_t>(micros % 1000000);
            wait_ptr = &wait;
        }
        const int selected = ::select(max_fd + 1, &read_set, nullptr, nullptr, wait_ptr);
        if (selected < 0) {
            if (errno == EINTR) continue;
            result.runner_error = true;
            if (!terminating) terminate_group(false);
            continue;
        }
        if (selected == 0) continue;
        if (stdout_fd >= 0 && FD_ISSET(stdout_fd, &read_set)) {
            char buffer[4096];
            const ssize_t count = ::read(stdout_fd, buffer, sizeof(buffer));
            if (count > 0) {
                const auto bytes = static_cast<std::size_t>(count);
                if (!terminating && (bytes > stdout_limit ||
                                     result.stdout_bytes > stdout_limit - bytes)) {
                    result.stdout_bytes = stdout_limit;
                    trigger_output(true);
                } else if (!terminating) {
                    result.stdout_text.append(buffer, bytes);
                    result.stdout_bytes += bytes;
                }
            } else if (count == 0) {
                ::close(stdout_fd);
                stdout_fd = -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                result.runner_error = true;
                ::close(stdout_fd);
                stdout_fd = -1;
                if (!terminating) terminate_group(false);
            }
        }
        if (stderr_fd >= 0 && FD_ISSET(stderr_fd, &read_set)) {
            char buffer[4096];
            const ssize_t count = ::read(stderr_fd, buffer, sizeof(buffer));
            if (count > 0) {
                const auto bytes = static_cast<std::size_t>(count);
                if (!terminating && (bytes > stderr_limit ||
                                     result.stderr_bytes > stderr_limit - bytes)) {
                    result.stderr_bytes = stderr_limit;
                    trigger_output(false);
                } else if (!terminating) {
                    result.stderr_text.append(buffer, bytes);
                    result.stderr_bytes += bytes;
                }
            } else if (count == 0) {
                ::close(stderr_fd);
                stderr_fd = -1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                result.runner_error = true;
                ::close(stderr_fd);
                stderr_fd = -1;
                if (!terminating) terminate_group(false);
            }
        }
    }

    int status = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited < 0) {
            if (errno == EINTR) continue;
            result.runner_error = true;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (bounded && !terminating && now >= deadline) trigger_timeout();
        if (terminating && !force_killed && now >= force_kill_at) terminate_group(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.terminating_signal = WTERMSIG(status);
        result.exit_code = 128 + result.terminating_signal;
    } else {
        result.exit_code = 1;
    }
    if (result.exit_code == 127 && result.stderr_text.rfind("execvp ", 0) == 0) {
        result.runner_error = true;
    }
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    return result;
}

SvnResult SvnOps::runSvn(const std::vector<std::string>& args) const {
    return runTool("svn", args);
}

SvnResult SvnOps::runSvnBounded(const std::vector<std::string>& args,
                                 const SvnQueryLimits& limits,
                                 SvnQueryBudget& budget) const {
    if (budget.process_count >= limits.max_processes) {
        throw SvnQueryError(SvnQueryFailure::ProcessLimit);
    }
    std::size_t argv_bytes = 4;  // "svn" plus NUL
    for (const auto& arg : args) {
        if (arg.size() >= limits.max_argv_bytes || argv_bytes > limits.max_argv_bytes - arg.size() - 1) {
            throw SvnQueryError(SvnQueryFailure::ArgvLimit);
        }
        argv_bytes += arg.size() + 1;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - budget.started);
    if (elapsed >= limits.total_timeout) throw SvnQueryError(SvnQueryFailure::Timeout);
    const auto remaining = limits.total_timeout - elapsed;
    const auto timeout = std::min(limits.per_process_timeout, remaining);
    if (budget.stdout_bytes >= limits.max_stdout_total || budget.stderr_bytes >= limits.max_stderr_total) {
        throw SvnQueryError(SvnQueryFailure::OutputLimit);
    }
    const std::size_t stdout_limit = std::min(limits.max_stdout_per_process,
                                              limits.max_stdout_total - budget.stdout_bytes);
    const std::size_t stderr_limit = std::min(limits.max_stderr_per_process,
                                              limits.max_stderr_total - budget.stderr_bytes);
    ++budget.process_count;
    SvnResult result = runToolBounded("svn", args, stdout_limit, stderr_limit, timeout,
                                      limits.termination_grace);
    if (result.stdout_limit_exceeded || result.stderr_limit_exceeded) {
        budget.stdout_bytes = limits.max_stdout_total;
        budget.stderr_bytes = limits.max_stderr_total;
        throw SvnQueryError(SvnQueryFailure::OutputLimit);
    }
    budget.stdout_bytes += result.stdout_bytes;
    budget.stderr_bytes += result.stderr_bytes;
    if (result.timed_out) throw SvnQueryError(SvnQueryFailure::Timeout);
    if (result.runner_error) throw SvnQueryError(SvnQueryFailure::Failed);
    return result;
}

SvnResult SvnOps::runSvnmucc(const std::vector<std::string>& args) const {
    return runTool("svnmucc", args);
}

void SvnOps::requireIsvnMarker(const std::string& commit_message) {
    if (commit_message.rfind("[isvn]", 0) != 0) {
        throw std::invalid_argument("SVN write commit message must start with [isvn]");
    }
}

void SvnOps::requireSuccess(const SvnResult& result, const std::string& operation) {
    if (result.exit_code != 0) {
        throw std::runtime_error(operation + " failed: " + firstNonemptyError(result));
    }
}

}  // namespace isvn::version
