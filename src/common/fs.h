#pragma once

#include <filesystem>
#include <sys/types.h>

namespace isvn::common {

gid_t path_group(const std::filesystem::path& path);
void ensure_directory(const std::filesystem::path& path, mode_t mode, gid_t group);
void ensure_file_mode_group(const std::filesystem::path& path, mode_t mode, gid_t group);
void fsync_directory(const std::filesystem::path& path);

}  // namespace isvn::common
