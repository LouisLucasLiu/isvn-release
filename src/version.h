#pragma once

#include <string>

// Build/deployment identity. Values are injected at CMake configure time
// (commit/build time/type); missing git metadata falls back to "unknown".
namespace isvn::version {

const char* appVersion();   // e.g. "0.1.0-dev"
const char* gitCommit();    // short hash, or "unknown"
const char* buildTime();    // ISO-8601 UTC, or "unknown"
const char* buildType();    // "Release" / "Debug" / "unknown"

// Multi-line human-readable identity block for the given program name.
std::string plainText(const std::string& program_name);

// JSON identity object in the project's CLI JSON style.
std::string json(const std::string& program_name);

}  // namespace isvn::version
