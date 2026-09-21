#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace arn {

struct ToolRequest {
    std::string name;
    nlohmann::json arguments;
    std::string summary;
    bool changes_files{};
};

struct ToolExecution {
    bool ok{};
    nlohmann::json result;
};

class ToolExecutor {
  public:
    using ConfirmationFn = std::function<bool(const ToolRequest&)>;

    // The project root is the folder in which `arn` was launched.
    explicit ToolExecutor(std::filesystem::path project_root = std::filesystem::current_path(),
                          std::function<void()> on_change = {});

    [[nodiscard]] ToolExecution execute(const std::string& name, const nlohmann::json& arguments,
                                        const ConfirmationFn& confirm) const;
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept;

  private:
    std::filesystem::path project_root_;
    std::function<void()> on_change_;
};

} // namespace arn
