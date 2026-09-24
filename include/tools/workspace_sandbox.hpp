#pragma once

#include <filesystem>
#include <string>

namespace arn {

constexpr std::size_t max_file_bytes = 256 * 1024;
constexpr std::size_t max_listed_entries = 300;

[[nodiscard]] bool is_within(const std::filesystem::path& root,
                             const std::filesystem::path& candidate);

[[nodiscard]] bool contains_protected_component(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path safe_path(const std::filesystem::path& root,
                                              const std::string& path_str,
                                              bool allow_missing_leaf,
                                              std::string& error);

[[nodiscard]] std::string read_text(const std::filesystem::path& path, std::string& error);

} // namespace arn
