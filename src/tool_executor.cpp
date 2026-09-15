#include "tool_executor.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

namespace arn {
namespace {

constexpr std::size_t max_file_bytes = 256 * 1024;
constexpr std::size_t max_listed_entries = 300;

ToolExecution failure(std::string message) {
    return {false, {{"error", std::move(message)}}};
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    if (candidate == root) return true;
    const auto relative = candidate.lexically_relative(root);
    // std::filesystem::path::starts_with is not implemented by every MSVC
    // standard-library version yet. Checking the first path component gives us
    // the same traversal protection without depending on that newer API.
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

bool contains_protected_component(const std::filesystem::path& path) {
    for (const auto& component : path) {
        const auto name = component.string();
        if (name == ".git" || name == ".env" || name == ".env.local") return true;
    }
    return false;
}

std::filesystem::path safe_path(const std::filesystem::path& root, const std::string& relative_path,
                                bool allow_missing_leaf, std::string& error) {
    if (relative_path.empty()) {
        error = "A relative path is required.";
        return {};
    }
    const std::filesystem::path requested(relative_path);
    if (requested.is_absolute() || contains_protected_component(requested)) {
        error = "This path is outside the allowed project area or protected.";
        return {};
    }
    std::error_code ec;
    const auto root_real = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        error = "Could not resolve the project root.";
        return {};
    }
    const auto target = root_real / requested;
    const auto resolved = allow_missing_leaf
        ? std::filesystem::weakly_canonical(target.parent_path(), ec) / target.filename()
        : std::filesystem::weakly_canonical(target, ec);
    if (ec || !is_within(root_real, resolved)) {
        error = "The requested path escapes the project folder.";
        return {};
    }
    return resolved;
}

std::string read_text(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > max_file_bytes) {
        error = "The file is unavailable or larger than 256 KiB.";
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Could not open the file.";
        return {};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

} // namespace

ToolExecutor::ToolExecutor(std::filesystem::path project_root) {
    std::error_code ec;
    project_root_ = std::filesystem::weakly_canonical(std::move(project_root), ec);
    if (ec) project_root_ = std::filesystem::current_path();
}

const std::filesystem::path& ToolExecutor::project_root() const noexcept { return project_root_; }

ToolExecution ToolExecutor::execute(const std::string& name, const nlohmann::json& arguments,
                                    const ConfirmationFn& confirm) const {
    const auto path_text = arguments.value("path", "");
    std::string error;

    if (name == "list_files") {
        const auto directory = safe_path(project_root_, path_text.empty() ? "." : path_text, false, error);
        if (directory.empty()) return failure(error);
        if (!std::filesystem::is_directory(directory)) return failure("The requested path is not a directory.");
        nlohmann::json entries = nlohmann::json::array();
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(directory,
                 std::filesystem::directory_options::skip_permission_denied, ec), end;
             it != end && entries.size() < max_listed_entries; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto relative = it->path().lexically_relative(project_root_);
            if (contains_protected_component(relative)) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            entries.push_back({{"path", relative.generic_string()},
                               {"type", it->is_directory(ec) ? "directory" : "file"}});
        }
        const bool truncated = entries.size() >= max_listed_entries;
        return {true, {{"root", project_root_.string()}, {"entries", std::move(entries)}, {"truncated", truncated}}};
    }

    if (name == "read_file") {
        const auto path = safe_path(project_root_, path_text, false, error);
        if (path.empty()) return failure(error);
        if (!std::filesystem::is_regular_file(path)) return failure("The requested path is not a regular file.");
        const auto content = read_text(path, error);
        if (!error.empty()) return failure(error);
        return {true, {{"path", path.lexically_relative(project_root_).generic_string()}, {"content", content}}};
    }

    if (name == "write_file") {
        if (!arguments.contains("content") || !arguments["content"].is_string()) {
            return failure("write_file requires a string content field.");
        }
        const auto path = safe_path(project_root_, path_text, true, error);
        if (path.empty()) return failure(error);
        const auto content = arguments.at("content").get<std::string>();
        const bool existed = std::filesystem::exists(path);
        const ToolRequest request{name, arguments,
            std::string(existed ? "Replace " : "Create ") + path.lexically_relative(project_root_).generic_string() +
                " (" + std::to_string(content.size()) + " bytes)", true};
        if (!confirm(request)) return failure("The user declined this file change.");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return failure("Could not create the parent directory.");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return failure("Could not write the file.");
        stream << content;
        if (!stream) return failure("Writing the file failed.");
        return {true, {{"path", path.lexically_relative(project_root_).generic_string()}, {"created", !existed}}};
    }

    if (name == "replace_text") {
        const auto path = safe_path(project_root_, path_text, false, error);
        if (path.empty()) return failure(error);
        if (!arguments.contains("old_text") || !arguments["old_text"].is_string() ||
            !arguments.contains("new_text") || !arguments["new_text"].is_string()) {
            return failure("replace_text requires old_text and new_text strings.");
        }
        auto content = read_text(path, error);
        if (!error.empty()) return failure(error);
        const auto old_text = arguments.at("old_text").get<std::string>();
        const auto new_text = arguments.at("new_text").get<std::string>();
        if (old_text.empty()) return failure("old_text cannot be empty.");
        const auto first = content.find(old_text);
        if (first == std::string::npos) return failure("old_text was not found in the file.");
        if (content.find(old_text, first + old_text.size()) != std::string::npos) {
            return failure("old_text appears more than once; use a more specific selection.");
        }
        const ToolRequest request{name, arguments,
            "Replace text in " + path.lexically_relative(project_root_).generic_string(), true};
        if (!confirm(request)) return failure("The user declined this file change.");
        content.replace(first, old_text.size(), new_text);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) return failure("Could not write the file.");
        stream << content;
        return {true, {{"path", path.lexically_relative(project_root_).generic_string()}, {"replacements", 1}}};
    }

    if (name == "delete_file") {
        const auto path = safe_path(project_root_, path_text, false, error);
        if (path.empty()) return failure(error);
        if (!std::filesystem::is_regular_file(path)) return failure("Only regular files can be deleted.");
        const ToolRequest request{name, arguments,
            "Delete " + path.lexically_relative(project_root_).generic_string() + " permanently", true};
        if (!confirm(request)) return failure("The user declined deletion.");
        std::error_code ec;
        if (!std::filesystem::remove(path, ec) || ec) return failure("Could not delete the file.");
        return {true, {{"path", path.lexically_relative(project_root_).generic_string()}, {"deleted", true}}};
    }

    return failure("Unknown tool requested: " + name);
}

} // namespace arn
