#include "common/fs.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <chrono>

namespace isvn::common {
namespace {

void ensureGroup(const std::filesystem::path& path, gid_t group) {
    for (int attempt = 0; attempt != 50; ++attempt) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0) {
            throw std::runtime_error("Unable to stat path: " + path.string());
        }
        if (status.st_gid == group) return;
        if (::chown(path.c_str(), static_cast<uid_t>(-1), group) == 0) return;
        if (errno != EPERM && errno != EACCES) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Unable to set group for " + path.string() + ": " +
                             std::strerror(errno));
}

void ensureMode(const std::filesystem::path& path, mode_t mode) {
    for (int attempt = 0; attempt != 50; ++attempt) {
        struct stat status {};
        if (::stat(path.c_str(), &status) != 0) {
            throw std::runtime_error("Unable to stat path: " + path.string());
        }
        if ((status.st_mode & 07777) == mode) return;
        if (::chmod(path.c_str(), mode) == 0) return;
        if (errno != EPERM && errno != EACCES) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Unable to set permissions for " + path.string() + ": " +
                             std::strerror(errno));
}

}  // namespace

gid_t path_group(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        throw std::runtime_error("Unable to read group for " + path.string());
    }
    return status.st_gid;
}

void ensure_directory(const std::filesystem::path& path, mode_t mode, gid_t group) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        throw std::runtime_error("Unable to create directory " + path.string() + ": " +
                                 error.message());
    }
    ensureGroup(path, group);
    ensureMode(path, mode);
}

void ensure_file_mode_group(const std::filesystem::path& path, mode_t mode, gid_t group) {
    ensureGroup(path, group);
    ensureMode(path, mode);
}

void fsync_directory(const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("Unable to open directory for fsync: " + path.string());
    }
    const int result = ::fsync(descriptor);
    const int saved_errno = errno;
    ::close(descriptor);
    if (result != 0) {
        throw std::runtime_error("Unable to fsync directory " + path.string() + ": " +
                                 std::strerror(saved_errno));
    }
}

}  // namespace isvn::common
