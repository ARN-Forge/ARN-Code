#include "tools/coding_tools.hpp"
#include "tools/workspace_sandbox.hpp"

#include <fstream>
#include <system_error>
#include <utility>

namespace arn {
namespace {

::arn::core::ToolResult failure(std::string message) {
    return ::arn::core::ToolResult::failure(std::move(message));
}

::arn::core::ToolResult execute_modifying_tool(
    const std::string& name,
    const std::filesystem::path& project_root,
    const std::function<void()>& on_change,
    const nlohmann::json& arguments,
    const ::arn::core::ToolContext& context) {
    const auto path_text = arguments.value("path", "");
    std::string error;
    const auto path = safe_path(project_root, path_text, true, error);
    if (path.empty())
        return failure(error);
    const bool existed = std::filesystem::exists(path);
    if (existed && !std::filesystem::is_regular_file(path))
        return failure("Only regular files can be changed.");
    if (!existed && name != "write_file")
        return failure("File does not exist.");
    const auto before = existed ? read_text(path, error) : std::string{};
    if (!error.empty())
        return failure(error);
    std::string after;
    if (name == "write_file") {
        if (!arguments.contains("content") || !arguments["content"].is_string())
            return failure("String content is required.");
        after = arguments.at("content").get<std::string>();
    } else if (name == "replace_text") {
        const auto old_text = arguments.value("old_text", "");
        const auto new_text = arguments.value("new_text", "");
        const auto first = before.find(old_text);
        if (old_text.empty() || first == std::string::npos ||
            before.find(old_text, first + old_text.size()) != std::string::npos)
            return failure("old_text must match exactly once.");
        after = before;
        after.replace(first, old_text.size(), new_text);
    }
    if (after.size() > max_file_bytes)
        return failure("Result exceeds 256 KiB.");
    auto preview = arguments;
    preview["path"] = path.lexically_relative(project_root).generic_string();
    preview["before"] = before;
    preview["after"] = after;
    const ToolRequest request{name, preview, name + " " + preview["path"].get<std::string>(),
                              true, true, preview};
    if (!context.confirm || !context.confirm(request))
        return failure("File change declined, cancelled or timed out.");
    // The filesystem may have changed while the user was reviewing the proposal.
    const auto checked = safe_path(project_root, path_text, true, error);
    if (checked.empty() || checked != path)
        return failure("Target changed during confirmation.");
    if (std::filesystem::exists(path) != existed)
        return failure("File appeared or disappeared during confirmation.");
    if (existed && (read_text(path, error) != before || !error.empty()))
        return failure("File changed during confirmation.");
    std::error_code ec;
    if (name == "delete_file") {
        if (!std::filesystem::remove(path, ec) || ec)
            return failure("Could not delete file.");
    } else {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return failure("Could not create parent directory.");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            return failure("Could not open file for writing.");
        stream << after;
        stream.close();
        if (!stream)
            return failure("Writing file failed.");
    }
    if (on_change)
        on_change();
    return ::arn::core::ToolResult::success(
        {{"path", preview["path"]}, {"deleted", name == "delete_file"}, {"created", !existed}});
}

} // namespace

// ListFilesTool
ListFilesTool::ListFilesTool(std::filesystem::path project_root)
    : project_root_(std::move(project_root)) {}

const ::arn::core::ToolDefinition& ListFilesTool::definition() const noexcept {
    static const ::arn::core::ToolDefinition def{
        "list_files",
        "List files and folders in the project. Never inspect .git or environment files.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "A relative path inside the current project."}}}
            }}
        }
    };
    return def;
}

::arn::core::ToolResult ListFilesTool::execute(const nlohmann::json& arguments,
                                              const ::arn::core::ToolContext&) {
    const auto path_text = arguments.value("path", "");
    std::string error;
    const auto directory =
        safe_path(project_root_, path_text.empty() ? "." : path_text, false, error);
    if (directory.empty())
        return failure(error);
    if (!std::filesystem::is_directory(directory))
        return failure("The requested path is not a directory.");
    nlohmann::json entries = nlohmann::json::array();
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(directory, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end && entries.size() < max_listed_entries; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto relative = it->path().lexically_relative(project_root_);
        std::string path_error;
        if (safe_path(project_root_, relative.generic_string(), false, path_error).empty()) {
            it.disable_recursion_pending();
            continue;
        }
        entries.push_back({{"path", relative.generic_string()},
                           {"type", it->is_directory(ec) ? "directory" : "file"}});
    }
    const bool truncated = entries.size() >= max_listed_entries;
    return ::arn::core::ToolResult::success(
        {{"root", project_root_.string()},
         {"entries", std::move(entries)},
         {"truncated", truncated}});
}

// ReadFileTool
ReadFileTool::ReadFileTool(std::filesystem::path project_root)
    : project_root_(std::move(project_root)) {}

const ::arn::core::ToolDefinition& ReadFileTool::definition() const noexcept {
    static const ::arn::core::ToolDefinition def{
        "read_file",
        "Read a small UTF-8 text file in the project before changing it.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "A relative path inside the current project."}}}
            }},
            {"required", {"path"}}
        }
    };
    return def;
}

::arn::core::ToolResult ReadFileTool::execute(const nlohmann::json& arguments,
                                             const ::arn::core::ToolContext&) {
    const auto path_text = arguments.value("path", "");
    std::string error;
    const auto path = safe_path(project_root_, path_text, false, error);
    if (path.empty())
        return failure(error);
    if (!std::filesystem::is_regular_file(path))
        return failure("The requested path is not a regular file.");
    const auto content = read_text(path, error);
    if (!error.empty())
        return failure(error);
    return ::arn::core::ToolResult::success(
        {{"path", path.lexically_relative(project_root_).generic_string()},
         {"content", content}});
}

// WriteFileTool
WriteFileTool::WriteFileTool(std::filesystem::path project_root, std::function<void()> on_change)
    : project_root_(std::move(project_root)), on_change_(std::move(on_change)) {}

const ::arn::core::ToolDefinition& WriteFileTool::definition() const noexcept {
    static const ::arn::core::ToolDefinition def{
        "write_file",
        "Create or replace a UTF-8 text file. The user will be asked before the change.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "A relative path inside the current project."}}},
                {"content", {{"type", "string"}}}
            }},
            {"required", {"path", "content"}}
        }
    };
    return def;
}

::arn::core::ToolResult WriteFileTool::execute(const nlohmann::json& arguments,
                                              const ::arn::core::ToolContext& context) {
    return execute_modifying_tool("write_file", project_root_, on_change_, arguments, context);
}

// ReplaceTextTool
ReplaceTextTool::ReplaceTextTool(std::filesystem::path project_root, std::function<void()> on_change)
    : project_root_(std::move(project_root)), on_change_(std::move(on_change)) {}

const ::arn::core::ToolDefinition& ReplaceTextTool::definition() const noexcept {
    static const ::arn::core::ToolDefinition def{
        "replace_text",
        "Replace one unique exact text selection in an existing file. The user will be asked before the change.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "A relative path inside the current project."}}},
                {"old_text", {{"type", "string"}}},
                {"new_text", {{"type", "string"}}}
            }},
            {"required", {"path", "old_text", "new_text"}}
        }
    };
    return def;
}

::arn::core::ToolResult ReplaceTextTool::execute(const nlohmann::json& arguments,
                                                const ::arn::core::ToolContext& context) {
    return execute_modifying_tool("replace_text", project_root_, on_change_, arguments, context);
}

// DeleteFileTool
DeleteFileTool::DeleteFileTool(std::filesystem::path project_root, std::function<void()> on_change)
    : project_root_(std::move(project_root)), on_change_(std::move(on_change)) {}

const ::arn::core::ToolDefinition& DeleteFileTool::definition() const noexcept {
    static const ::arn::core::ToolDefinition def{
        "delete_file",
        "Permanently delete one regular file. Use only when the user explicitly asked to delete it; confirmation is required.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {{"type", "string"}, {"description", "A relative path inside the current project."}}}
            }},
            {"required", {"path"}}
        }
    };
    return def;
}

::arn::core::ToolResult DeleteFileTool::execute(const nlohmann::json& arguments,
                                                const ::arn::core::ToolContext& context) {
    return execute_modifying_tool("delete_file", project_root_, on_change_, arguments, context);
}

void register_coding_tools(::arn::core::ToolRegistry& registry,
                           const std::filesystem::path& project_root,
                           std::function<void()> on_change) {
    registry.register_tool(std::make_shared<ListFilesTool>(project_root));
    registry.register_tool(std::make_shared<ReadFileTool>(project_root));
    registry.register_tool(std::make_shared<WriteFileTool>(project_root, on_change));
    registry.register_tool(std::make_shared<ReplaceTextTool>(project_root, on_change));
    registry.register_tool(std::make_shared<DeleteFileTool>(project_root, std::move(on_change)));
}

std::shared_ptr<::arn::core::ToolRegistry>
create_coding_tool_registry(const std::filesystem::path& project_root,
                            std::function<void()> on_change) {
    auto registry = std::make_shared<::arn::core::ToolRegistry>();
    register_coding_tools(*registry, project_root, std::move(on_change));
    return registry;
}

// ToolExecutor implementation
ToolExecutor::ToolExecutor(std::filesystem::path project_root, std::function<void()> on_change)
    : on_change_(std::move(on_change)),
      registry_(std::make_shared<::arn::core::ToolRegistry>()) {
    std::error_code ec;
    project_root_ = std::filesystem::weakly_canonical(std::move(project_root), ec);
    if (ec)
        project_root_ = std::filesystem::current_path();

    register_coding_tools(*registry_, project_root_, on_change_);
}

const std::filesystem::path& ToolExecutor::project_root() const noexcept {
    return project_root_;
}

const ::arn::core::ToolRegistry& ToolExecutor::registry() const noexcept {
    return *registry_;
}

std::shared_ptr<const ::arn::core::ToolRegistry> ToolExecutor::registry_ptr() const noexcept {
    return registry_;
}

ToolExecution ToolExecutor::execute(const std::string& name, const nlohmann::json& arguments,
                                    const ConfirmationFn& confirm) const {
    const ::arn::core::ToolContext context{
        .cancel_requested = nullptr,
        .confirm = confirm
    };
    return registry_->execute(name, arguments, context);
}

} // namespace arn
