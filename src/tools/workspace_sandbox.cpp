#include "tools/workspace_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace arn {

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

} // namespace arn
