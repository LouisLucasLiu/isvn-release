#include "version.h"

#include <sstream>

#ifndef ISVN_VERSION
#define ISVN_VERSION "0.1.0-dev"
#endif
#ifndef ISVN_GIT_COMMIT
#define ISVN_GIT_COMMIT "unknown"
#endif
#ifndef ISVN_BUILD_TIME
#define ISVN_BUILD_TIME "unknown"
#endif
#ifndef ISVN_BUILD_TYPE
#define ISVN_BUILD_TYPE "unknown"
#endif

namespace isvn::version {
namespace {

std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

const char* appVersion() { return ISVN_VERSION; }
const char* gitCommit() { return ISVN_GIT_COMMIT; }
const char* buildTime() { return ISVN_BUILD_TIME; }
const char* buildType() { return ISVN_BUILD_TYPE; }

std::string plainText(const std::string& program_name) {
    std::ostringstream out;
    out << program_name << " " << appVersion() << "\n"
        << "commit: " << gitCommit() << "\n"
        << "build_time: " << buildTime() << "\n"
        << "build_type: " << buildType() << "\n";
    return out.str();
}

std::string json(const std::string& program_name) {
    std::ostringstream out;
    out << "{\"result\":\"ok\""
        << ",\"program\":\"" << jsonEscape(program_name) << "\""
        << ",\"version\":\"" << jsonEscape(appVersion()) << "\""
        << ",\"commit\":\"" << jsonEscape(gitCommit()) << "\""
        << ",\"build_time\":\"" << jsonEscape(buildTime()) << "\""
        << ",\"build_type\":\"" << jsonEscape(buildType()) << "\"}\n";
    return out.str();
}

}  // namespace isvn::version
