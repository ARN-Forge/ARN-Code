#pragma once

// Windows only. How ARN presents itself is decided once, when the terminal
// session starts:
//
//   standard input and output are a console     the alternate-screen renderer
//   (that accepts VT output)                     built on arn::tui
//   standard input or output is redirected       plain append-only output
//
// ARN_TERMINAL_RENDERER (compared without regard to case) no longer selects
// anything:
//
//   unset, empty or alternate   accepted
//   native                      no longer supported (the previous renderer
//                               was removed): ARN shows a warning
//   anything else               not supported: ARN shows a warning
//
// The variable never overrides redirected input or output.

#include <string>

namespace arn {

struct WindowsRendererSetting {
    bool recognized{true}; // False for any value other than unset, empty or alternate.
    bool removed_native{}; // The value is native, the renderer that was removed.
    std::string value;     // The value as set (UTF-8, at most 64 characters), for the warning.
};

[[nodiscard]] WindowsRendererSetting windows_renderer_setting();

} // namespace arn
