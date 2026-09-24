#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <arn/core/confirmation/confirmation_request.hpp>
#include <arn/core/tool/tool.hpp>
#include <arn/core/tool/tool_registry.hpp>

namespace arn {

using ToolRequest = ::arn::core::ConfirmationRequest;
using ToolExecution = ::arn::core::ToolResult;

class ListFilesTool final : public ::arn::core::ITool {
public:
    explicit ListFilesTool(std::filesystem::path project_root);
    [[nodiscard]] const ::arn::core::ToolDefinition& definition() const noexcept override;
    [[nodiscard]] bool requires_confirmation() const noexcept override { return false; }
    [[nodiscard]] ::arn::core::ToolResult execute(const nlohmann::json& arguments,
                                                  const ::arn::core::ToolContext& context) override;

private:
    std::filesystem::path project_root_;
};

class ReadFileTool final : public ::arn::core::ITool {
public:
    explicit ReadFileTool(std::filesystem::path project_root);
    [[nodiscard]] const ::arn::core::ToolDefinition& definition() const noexcept override;
    [[nodiscard]] bool requires_confirmation() const noexcept override { return false; }
    [[nodiscard]] ::arn::core::ToolResult execute(const nlohmann::json& arguments,
                                                  const ::arn::core::ToolContext& context) override;

private:
    std::filesystem::path project_root_;
};

class WriteFileTool final : public ::arn::core::ITool {
public:
    explicit WriteFileTool(std::filesystem::path project_root, std::function<void()> on_change = {});
    [[nodiscard]] const ::arn::core::ToolDefinition& definition() const noexcept override;
    [[nodiscard]] bool requires_confirmation() const noexcept override { return true; }
    [[nodiscard]] ::arn::core::ToolResult execute(const nlohmann::json& arguments,
                                                  const ::arn::core::ToolContext& context) override;

private:
    std::filesystem::path project_root_;
    std::function<void()> on_change_;
};

class ReplaceTextTool final : public ::arn::core::ITool {
public:
    explicit ReplaceTextTool(std::filesystem::path project_root, std::function<void()> on_change = {});
    [[nodiscard]] const ::arn::core::ToolDefinition& definition() const noexcept override;
    [[nodiscard]] bool requires_confirmation() const noexcept override { return true; }
    [[nodiscard]] ::arn::core::ToolResult execute(const nlohmann::json& arguments,
                                                  const ::arn::core::ToolContext& context) override;

private:
    std::filesystem::path project_root_;
    std::function<void()> on_change_;
};

class DeleteFileTool final : public ::arn::core::ITool {
public:
    explicit DeleteFileTool(std::filesystem::path project_root, std::function<void()> on_change = {});
    [[nodiscard]] const ::arn::core::ToolDefinition& definition() const noexcept override;
    [[nodiscard]] bool requires_confirmation() const noexcept override { return true; }
    [[nodiscard]] ::arn::core::ToolResult execute(const nlohmann::json& arguments,
                                                  const ::arn::core::ToolContext& context) override;

private:
    std::filesystem::path project_root_;
    std::function<void()> on_change_;
};

void register_coding_tools(::arn::core::ToolRegistry& registry,
                           const std::filesystem::path& project_root,
                           std::function<void()> on_change = {});

[[nodiscard]] std::shared_ptr<::arn::core::ToolRegistry>
create_coding_tool_registry(const std::filesystem::path& project_root,
                            std::function<void()> on_change = {});

// Composition helper for ARN coding tools in a workspace
class ToolExecutor {
public:
    using ConfirmationFn = ::arn::core::ConfirmationFn;

    explicit ToolExecutor(std::filesystem::path project_root = std::filesystem::current_path(),
                          std::function<void()> on_change = {});

    [[nodiscard]] ToolExecution execute(const std::string& name, const nlohmann::json& arguments,
                                        const ConfirmationFn& confirm) const;
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept;
    [[nodiscard]] const ::arn::core::ToolRegistry& registry() const noexcept;
    [[nodiscard]] std::shared_ptr<const ::arn::core::ToolRegistry> registry_ptr() const noexcept;

private:
    std::filesystem::path project_root_;
    std::function<void()> on_change_;
    std::shared_ptr<::arn::core::ToolRegistry> registry_;
};

} // namespace arn
