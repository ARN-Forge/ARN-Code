#include "arn/core/provider/model_provider.hpp"
#include "arn/core/provider/gemini_provider.hpp"
#include "arn/core/provider/deepseek_provider.hpp"

namespace arn::core {

std::unique_ptr<IModelProvider> create_provider(ProviderType type) {
    switch (type) {
    case ProviderType::gemini:
        return std::make_unique<GeminiProvider>();
    case ProviderType::deepseek:
        return std::make_unique<DeepSeekProvider>();
    case ProviderType::none:
    case ProviderType::custom:
        return nullptr;
    }
    return nullptr;
}

} // namespace arn::core
