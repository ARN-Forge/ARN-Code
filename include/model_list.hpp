#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arn::detail {
inline std::vector<std::string> parse_model_list(std::string_view body, bool gemini) {
    // Own the JSON document for the entire loop. A reference returned by at()
    // on a temporary parse result can dangle on supported compilers.
    const auto document = nlohmann::json::parse(body);
    std::vector<std::string> models;
    for (const auto& model : document.at(gemini ? "models" : "data")) {
        if (gemini) {
            const auto methods = model.value("supportedGenerationMethods", std::vector<std::string>{});
            const auto actions = model.value("supportedActions", std::vector<std::string>{});
            if (std::ranges::find(methods, "generateContent") == methods.end()
                && std::ranges::find(actions, "generateContent") == actions.end()) continue;
        }
        auto name = model.at(gemini ? "name" : "id").get<std::string>();
        if (gemini && name.starts_with("models/")) name.erase(0, 7);
        models.push_back(std::move(name));
    }
    std::ranges::sort(models);
    return models;
}
}
