#include "model_list.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto check = [](std::string_view body, bool gemini, const std::vector<std::string>& expected) {
            if (arn::detail::parse_model_list(body, gemini) != expected)
                throw std::runtime_error("Model list differs from expected result");
        };
        for (int i = 0; i < 100; ++i) {
            check(R"({"models":[{"name":"models/gemini-test","supportedGenerationMethods":["generateContent"]}]})",
                  true, {"gemini-test"});
            check(R"({"models":[{"name":"models/z","supportedActions":["generateContent"]},{"name":"models/embed","supportedGenerationMethods":["embedContent"]},{"name":"models/a","supportedGenerationMethods":["generateContent"]},{"name":"models/unknown"}]})",
                  true, {"a", "z"});
            check(R"({"data":[{"id":"deepseek-reasoner"},{"id":"deepseek-chat"}]})",
                  false, {"deepseek-chat", "deepseek-reasoner"});
            check(R"({"models":[]})", true, {});
        }
        bool rejected = false;
        try { arn::detail::parse_model_list("not json", true); }
        catch (const nlohmann::json::exception&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Invalid JSON was accepted");
        std::cout << "Model list regression tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
