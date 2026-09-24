#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <arn/core/agent/agent_session.hpp>
#include <arn/core/confirmation/confirmation_gate.hpp>
#include <arn/core/confirmation/confirmation_handler.hpp>
#include <arn/core/net/http_client.hpp>
#include <arn/core/net/model_parser.hpp>
#include <arn/core/net/sse_decoder.hpp>
#include <arn/core/provider/deepseek_provider.hpp>
#include <arn/core/provider/gemini_provider.hpp>
#include <arn/core/provider/model_provider.hpp>
#include <arn/core/provider/openrouter_provider.hpp>
#include <arn/core/tool/tool.hpp>
#include <arn/core/tool/tool_registry.hpp>
#include <arn/core/version.hpp>

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Architecture Check Failed: " << message << std::endl;
        std::exit(1);
    }
}

std::string read_file_content(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

void test_core_isolation(const std::filesystem::path& core_dir) {
    check(std::filesystem::exists(core_dir), "arn_core directory must exist at " + core_dir.string());

    // Match any includes of ARN application headers without arn/core prefix
    const std::vector<std::regex> forbidden_include_regexes = {
        std::regex(R"(#include\s*["<]api_client\.hpp[">])"),
        std::regex(R"(#include\s*["<]model_provider\.hpp[">])"),
        std::regex(R"(#include\s*["<]tool_executor\.hpp[">])"),
        std::regex(R"(#include\s*["<]server\.hpp[">])"),
        std::regex(R"(#include\s*["<]terminal\.hpp[">])"),
        std::regex(R"(#include\s*["<]config_manager\.hpp[">])"),
        std::regex(R"(#include\s*["<](?:tools/)?coding_tools\.hpp[">])"),
        std::regex(R"(#include\s*["<](?:tools/)?workspace_sandbox\.hpp[">])"),
        std::regex(R"(#include\s*["<](?:agent/)?coding_prompt\.hpp[">])")
    };

    const std::vector<std::string> forbidden_terms = {
        "safe_path",
        "ToolExecutor",
        "ApiClient",
        "coding assistant",
        "You are ARN",
        ".env.production",
        "read_text",
        "execute_modifying_tool"
    };

    const std::regex namespace_arn_regex(R"(namespace\s+arn\s*\{)");

    std::size_t files_checked = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(core_dir)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".hpp" && ext != ".cpp")
            continue;

        const auto content = read_file_content(entry.path());
        files_checked++;

        for (const auto& re : forbidden_include_regexes) {
            check(!std::regex_search(content, re),
                  entry.path().filename().string() + " references forbidden ARN application header");
        }

        for (const auto& term : forbidden_terms) {
            check(content.find(term) == std::string::npos,
                  entry.path().filename().string() + " contains ARN application term: " + term);
        }

        check(!std::regex_search(content, namespace_arn_regex),
              entry.path().filename().string() + " declares 'namespace arn {' instead of 'namespace arn::core {'");
    }

    check(files_checked >= 15, "Expected to check at least 15 arn_core source/header files.");
    std::cout << "Checked " << files_checked << " arn_core files for strict architectural isolation.\n";
}

void test_non_coding_consumer() {
    // Verify a non-coding application can construct and use AgentSession with custom tools and instructions.
    arn::core::AgentSession session(arn::core::AgentConfig{
        .system_instruction = "You are a customer service assistant.",
        .max_tool_rounds = 5,
        .max_history_entries = 10
    });

    check(session.system_instruction() == "You are a customer service assistant.",
          "System instruction mismatch");
    check(session.tools() == nullptr, "Tools should initially be null");
    check(session.session_entries() == 0, "Initial session entries should be 0");
    check(session.active_provider() == arn::core::ProviderType::none, "Active provider should be none");

    auto registry = std::make_shared<arn::core::ToolRegistry>();
    session.set_tools(registry);
    check(session.tools() != nullptr, "Tools should be set");
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path core_dir;
#ifdef ARN_CORE_DIR
    core_dir = ARN_CORE_DIR;
#else
    if (argc > 1) {
        core_dir = argv[1];
    } else {
        core_dir = std::filesystem::current_path() / "arn_core";
        if (!std::filesystem::exists(core_dir)) {
            core_dir = std::filesystem::current_path().parent_path() / "arn-core";
        }
    }
#endif

    try {
        test_core_isolation(core_dir);
        test_non_coding_consumer();
        std::cout << "All architectural boundary checks passed!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Architecture test threw exception: " << e.what() << '\n';
        return 1;
    }
}
