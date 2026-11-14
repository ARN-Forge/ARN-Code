#include "build_info.hpp"

// Generated on every build; kept out of main.cpp so a new commit recompiles
// only this file.
#include "arn_build_info.hpp"

#ifndef ARN_VERSION
#define ARN_VERSION "dev"
#endif

namespace arn {

std::string version_line() {
    return std::string("arn ") + ARN_VERSION + " (" + ARN_BUILD_COMMIT + ")";
}

} // namespace arn
