#pragma once

#include "arn/core/net/model_parser.hpp"
#include <nlohmann/json.hpp>

namespace arn::detail {

inline std::vector<std::string> parse_model_list(std::string_view body, bool gemini) {
    return arn::core::net::parse_model_list(body, gemini);
}

} // namespace arn::detail
