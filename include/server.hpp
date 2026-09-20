#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace arn {

// Run ARN in headless server mode, reading JSONL commands from stdin
// and writing JSONL events to stdout. Diagnostic output goes to stderr.
// Blocks until EOF or a fatal error.
int run_server();

} // namespace arn
