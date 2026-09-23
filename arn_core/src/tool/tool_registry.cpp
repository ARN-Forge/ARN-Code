#include "arn/core/tool/tool_registry.hpp"

namespace arn::core {

bool ToolRegistry::register_tool(std::shared_ptr<ITool> tool) {
    if (!tool)
        return false;
    const auto& name = tool->definition().name;
    if (name.empty() || tools_.contains(name))
        return false;
    tools_.emplace(name, std::move(tool));
    return true;
}

bool ToolRegistry::unregister_tool(std::string_view name) {
    const auto it = tools_.find(name);
    if (it == tools_.end())
        return false;
    tools_.erase(it);
    return true;
}

std::shared_ptr<ITool> ToolRegistry::find_tool(std::string_view name) const {
    const auto it = tools_.find(name);
    return it != tools_.end() ? it->second : nullptr;
}

bool ToolRegistry::has_tool(std::string_view name) const {
    return tools_.contains(name);
}

std::vector<ToolDefinition> ToolRegistry::definitions() const {
    std::vector<ToolDefinition> defs;
    defs.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        defs.push_back(tool->definition());
    }
    return defs;
}

nlohmann::json ToolRegistry::definitions_json() const {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& [name, tool] : tools_) {
        const auto& def = tool->definition();
        array.push_back({
            {"name", def.name},
            {"description", def.description},
            {"parameters", def.parameter_schema}
        });
    }
    return array;
}

ToolResult ToolRegistry::execute(const std::string& name, const nlohmann::json& arguments,
                                 const ToolContext& context) const {
    const auto tool = find_tool(name);
    if (!tool)
        return ToolResult::failure("Unknown tool requested: " + name);
    return tool->execute(arguments, context);
}

std::size_t ToolRegistry::size() const noexcept {
    return tools_.size();
}

bool ToolRegistry::empty() const noexcept {
    return tools_.empty();
}

void ToolRegistry::clear() {
    tools_.clear();
}

} // namespace arn::core
