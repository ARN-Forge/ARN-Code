#pragma once

#include <string_view>

#define ARN_CORE_VERSION_MAJOR 0
#define ARN_CORE_VERSION_MINOR 1
#define ARN_CORE_VERSION_PATCH 0
#define ARN_CORE_VERSION_STRING "0.1.0"

namespace arn::core {

inline constexpr int version_major = ARN_CORE_VERSION_MAJOR;
inline constexpr int version_minor = ARN_CORE_VERSION_MINOR;
inline constexpr int version_patch = ARN_CORE_VERSION_PATCH;

inline constexpr std::string_view version_string = ARN_CORE_VERSION_STRING;

[[nodiscard]] constexpr std::string_view version() noexcept {
    return version_string;
}

} // namespace arn::core
