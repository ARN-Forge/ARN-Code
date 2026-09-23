#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace arn::core::net {

[[nodiscard]] std::vector<std::string> parse_model_list(std::string_view body, bool gemini);

} // namespace arn::core::net
