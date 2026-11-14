#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace isvn::common {

std::string blake3_hex(std::string_view content);
std::string blake3_file_hex(const std::filesystem::path& path);
// Hash an already-open descriptor.  The caller owns the descriptor; this
// helper only seeks it to the beginning and never reopens a path.
std::string blake3_fd_hex(int fd);

}  // namespace isvn::common
