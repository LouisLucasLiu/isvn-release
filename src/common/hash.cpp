#include "common/hash.h"

#include <blake3.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace isvn::common {
namespace {

std::string finalizeHex(blake3_hasher& hasher) {
    std::array<std::uint8_t, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}
    ~FileDescriptor() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return descriptor_; }

private:
    int descriptor_;
};

}  // namespace

std::string blake3_hex(std::string_view content) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, content.data(), content.size());
    return finalizeHex(hasher);
}

std::string blake3_file_hex(const std::filesystem::path& path) {
    FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (file.get() < 0) {
        throw std::runtime_error("Unable to open file for BLAKE3: " +
                                 std::string(std::strerror(errno)));
    }
    return blake3_fd_hex(file.get());
}

std::string blake3_fd_hex(int fd) {
    if (fd < 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
        throw std::runtime_error("Unable to seek file for BLAKE3: " +
                                 std::string(std::strerror(errno)));
    }
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<char, 1024 * 1024> buffer{};
    while (true) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Unable to read file for BLAKE3: " +
                                     std::string(std::strerror(errno)));
        }
        blake3_hasher_update(&hasher, buffer.data(), static_cast<std::size_t>(count));
    }
    return finalizeHex(hasher);
}

}  // namespace isvn::common
