#pragma once

// POSIX, and the Windows alternate-screen renderer. While a request runs,
// TerminalCancellationMonitor reads the terminal on its own thread. View keys
// it reads there (PgUp, PgDn, End and the mouse wheel) are kept instead of
// dropped; the thread that renders takes them at its next frame. Nothing is
// rendered from the monitor thread. With plain output (Windows, redirected
// input or output) nothing is kept and this always returns an empty list.

#include "terminal.hpp"

#include <vector>

namespace arn {

[[nodiscard]] std::vector<TerminalEvent> take_deferred_view_events();

} // namespace arn
