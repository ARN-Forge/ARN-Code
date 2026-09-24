#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <arn/core/provider/model_provider.hpp>

namespace arn {

using Provider = ::arn::core::ProviderType;
using ApiResult = ::arn::core::ApiResult;
using ProviderStreamCallback = ::arn::core::TextStreamCallback;
using IModelProvider = ::arn::core::IModelProvider;
using ModelProvider = ::arn::core::IModelProvider;

[[nodiscard]] std::unique_ptr<::arn::core::IModelProvider> make_provider(Provider provider);

[[nodiscard]] inline Provider provider_from_name(std::string_view name) {
    return ::arn::core::provider_type_from_name(name);
}

[[nodiscard]] inline std::string provider_name(Provider provider) {
    return std::string(::arn::core::provider_type_name(provider));
}

} // namespace arn
