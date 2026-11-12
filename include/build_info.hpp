#pragma once

#include <string>

namespace arn {

// The line printed by `arn --version`: "arn <version> (<commit>)".
// <commit> is the first 12 hex digits of the source commit, followed by
// "-dirty" for a local build with uncommitted tracked changes, or "unknown"
// when the build had no commit information (see cmake/ArnBuildInfo.cmake).
std::string version_line();

} // namespace arn
