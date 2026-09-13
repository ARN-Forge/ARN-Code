#pragma once

#include <string>
#include <vector>

namespace arn {

enum class Provider { none, deepseek, gemini };

struct ApiResult {
    bool ok{};
    std::string message;
    std::vector<std::string> models;
};

class ApiClient {
public:
    [[nodiscard]] ApiResult list_models(Provider provider, const std::string& api_key) const;
    [[nodiscard]] ApiResult submit_prompt(Provider provider, const std::string& api_key,
                                          const std::string& model, const std::string& prompt) const;
};

[[nodiscard]] std::string provider_name(Provider provider);

} // namespace arn
