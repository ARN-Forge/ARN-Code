#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "arn/core/provider/model_provider.hpp"
#include "arn/core/tool/tool_registry.hpp"

namespace arn::core::detail {

constexpr int max_tool_rounds = 12;
constexpr std::size_t max_history_entries = 40;

std::string error_message(const nlohmann::json& body);

ApiResult parse_error(int status, const std::string& provider, const std::string& response_body);

void trim_history(nlohmann::json& history, std::size_t keep_from);

std::string lower_ascii(std::string value);

// Gemini payload & streaming helpers
nlohmann::json serialize_gemini_tools(const ToolRegistry& tools);

nlohmann::json build_gemini_payload(const std::string& system_instruction,
                                    const nlohmann::json& contents,
                                    const ToolRegistry& tools);

void parse_gemini_stream_chunk(std::string_view event,
                               std::string& text_accumulator,
                               nlohmann::json& model_response_parts,
                               nlohmann::json& function_calls,
                               const TextStreamCallback& on_text);

// DeepSeek payload & streaming helpers
nlohmann::json serialize_deepseek_tools(const ToolRegistry& tools);

nlohmann::json build_deepseek_payload(const std::string& model,
                                      const nlohmann::json& messages,
                                      const ToolRegistry& tools);

void parse_deepseek_stream_chunk(std::string_view event,
                                 std::string& text_accumulator,
                                 nlohmann::json& tool_calls,
                                 const TextStreamCallback& on_text);

} // namespace arn::core::detail
