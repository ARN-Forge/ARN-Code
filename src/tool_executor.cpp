#include "tool_executor.hpp"

#include <algorithm>
#include <cctype>
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
    if (candidate == root)
        return true;
    const auto relative = candidate.lexically_relative(root);
    // std::filesystem::path::starts_with is not implemented by every MSVC
    // standard-library version yet. Checking the first path component gives us
    // the same traversal protection without depending on that newer API.
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

bool contains_protected_component(const std::filesystem::path& path) {
    for (const auto& component : path) {
        auto name = component.string();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!name.empty() && (name.back() == '.' || name.back() == ' '))
            name.pop_back();
        if (name == ".git" || name == ".ssh" || name == ".aws" || name == ".codex" ||
            name == ".agents" || name == ".env" || name.starts_with(".env.") ||
            name == "credentials" || name == "credentials.json" || name == "id_rsa" ||
            name == "id_ed25519" || name.ends_with(".pem") || name.ends_with(".key") ||
            name.find(':') != std::string::npos)
            return true;
    }
    return false;
}

std::filesystem::path safe_path(const std::filesystem::path& root, const std::string& path_str,
                                bool /*allow_missing_leaf*/, std::string& error) {
    if (path_str.empty()) {
        error = "A path is required.";
        return {};
    }
    std::filesystem::path requested(path_str);
    std::error_code ec;
    const auto root_real = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        error = "Could not resolve the project root.";
        return {};
    }

    // Determine target path (support relative paths and project-contained absolute paths)
    std::filesystem::path target;
    if (requested.is_absolute()) {
        target = requested;
    } else {
        target = root_real / requested;
    }

    if (contains_protected_component(target.lexically_relative(root_real))) {
        error = "Protected file or directory.";
        return {};
    }
    // Resolve the entire target, including an existing leaf symlink.
    const auto resolved = std::filesystem::weakly_canonical(target, ec);

    if (ec || !is_within(root_real, resolved)) {
        error = "The requested path escapes the project folder.";
        return {};
    }

    // Check protected components on the relative path inside project
    const auto relative = resolved.lexically_relative(root_real);
    if (contains_protected_component(relative)) {
        error = "This path is inside a protected folder or file (.git, .env).";
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

ToolExecutor::ToolExecutor(std::filesystem::path project_root, std::function<void()> on_change)
    : on_change_(std::move(on_change)) {
    std::error_code ec;
    project_root_ = std::filesystem::weakly_canonical(std::move(project_root), ec);
    if (ec)
        project_root_ = std::filesystem::current_path();
}

const std::filesystem::path& ToolExecutor::project_root() const noexcept {
    return project_root_;
}

ToolExecution ToolExecutor::execute(const std::string& name, const nlohmann::json& arguments,
                                    const ConfirmationFn& confirm) const {
    const auto path_text = arguments.value("path", "");
    std::string error;

    if (name == "list_files") {
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
        return {true,
                {{"root", project_root_.string()},
                 {"entries", std::move(entries)},
                 {"truncated", truncated}}};
    }

    if (name == "read_file") {
        const auto path = safe_path(project_root_, path_text, false, error);
        if (path.empty())
            return failure(error);
        if (!std::filesystem::is_regular_file(path))
            return failure("The requested path is not a regular file.");
        const auto content = read_text(path, error);
        if (!error.empty())
            return failure(error);
        return {true,
                {{"path", path.lexically_relative(project_root_).generic_string()},
                 {"content", content}}};
    }

    if (name == "write_file" || name == "replace_text" || name == "delete_file") {
        const auto path = safe_path(project_root_, path_text, true, error);
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
        preview["path"] = path.lexically_relative(project_root_).generic_string();
        preview["before"] = before;
        preview["after"] = after;
        const ToolRequest request{name, preview, name + " " + preview["path"].get<std::string>(),
                                  true};
        if (!confirm || !confirm(request))
            return failure("File change declined, cancelled or timed out.");
        // The filesystem may have changed while the user was reviewing the proposal.
        const auto checked = safe_path(project_root_, path_text, true, error);
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
        if (on_change_)
            on_change_();
        return {
            true,
            {{"path", preview["path"]}, {"deleted", name == "delete_file"}, {"created", !existed}}};
    }
    return failure("Unknown tool requested: " + name);
}
} // namespace arn
