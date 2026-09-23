#pragma once

#include "arn/core/tool/tool.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace arn::core {

class ToolRegistry {
public:
    ToolRegistry() = default;

    // Registers a tool. Returns true if registered, false if tool is null or name is duplicate.
    bool register_tool(std::shared_ptr<ITool> tool);

    // Unregisters a tool by name. Returns true if found and removed.
    bool unregister_tool(std::string_view name);

    // Finds a tool by name. Returns nullptr if not found.
    [[nodiscard]] std::shared_ptr<ITool> find_tool(std::string_view name) const;

    // Checks if tool exists.
    [[nodiscard]] bool has_tool(std::string_view name) const;

    // Returns all tool definitions.
    [[nodiscard]] std::vector<ToolDefinition> definitions() const;

    // Returns tool definitions as a JSON array suitable for LLM APIs.
    [[nodiscard]] nlohmann::json definitions_json() const;

    // Executes a tool by name.
    [[nodiscard]] ToolResult execute(const std::string& name, const nlohmann::json& arguments,
                                     const ToolContext& context = {}) const;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    void clear();

private:
    std::map<std::string, std::shared_ptr<ITool>, std::less<>> tools_;
};

} // namespace arn::core
