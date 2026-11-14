#include "common/system.h"

#include <pwd.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <stdexcept>

namespace isvn::common {

std::string system_user() {
    if (const passwd* entry = getpwuid(geteuid());
        entry != nullptr && entry->pw_name != nullptr && entry->pw_name[0] != '\0') {
        return entry->pw_name;
    }
    throw std::runtime_error("Unable to determine the current system user");
}

std::string utc_now() {
    const std::time_t time =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_r(&time, &utc) == nullptr) {
        throw std::runtime_error("Unable to convert current time to UTC");
    }
    char buffer[21]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        throw std::runtime_error("Unable to format current UTC time");
    }
    return buffer;
}

}  // namespace isvn::common
