#include "model_provider.hpp"

#include <arn/core/provider/openrouter_provider.hpp>

namespace arn {

std::unique_ptr<::arn::core::IModelProvider> make_provider(Provider provider) {
    auto core_provider = ::arn::core::create_provider(provider);
    if (!core_provider)
        return nullptr;
    if (provider == Provider::openrouter) {
        if (auto* or_prov = dynamic_cast<::arn::core::OpenRouterProvider*>(core_provider.get())) {
            auto config = or_prov->config();
            config.http_referer = "https://github.com/arn-org/arn";
            config.app_title = "ARN";
            or_prov->set_config(std::move(config));
        }
    }
    return core_provider;
}

} // namespace arn
