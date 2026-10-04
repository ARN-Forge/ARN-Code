// Smoke test that drives the real Kiro ACP startup path. Used to reproduce
// the "bad allocation" reported by `/acp kiro` on real installations.
//
// Honours the ARN_KIRO_BIN environment variable; defaults to "kiro-cli" on PATH.
// Prints diagnostic information to stderr and exits non-zero on failure.

#include "acp/acp_client.hpp"
#include "acp/acp_session.hpp"
#include "acp/kiro_adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const auto availability = arn::acp::probe_kiro_cli();
    std::fprintf(stderr, "probe: status=%d detail='%s' path='%s'\n",
                 static_cast<int>(availability.status),
                 availability.detail.c_str(),
                 availability.resolved_path.c_str());
    std::fflush(stderr);
    if (availability.status == arn::acp::KiroStatus::not_installed) {
        std::fprintf(stderr, "SKIP: kiro-cli not installed\n");
        return 77; // ctest skip
    }
    if (availability.status == arn::acp::KiroStatus::not_authenticated) {
        std::fprintf(stderr, "SKIP: kiro-cli not authenticated\n");
        return 77; // ctest skip
    }

    arn::acp::AcpBackendInfo info;
    info.kind = arn::acp::AcpBackendKind::kiro;
    info.label = "Kiro via " + arn::acp::KiroAdapter::agent_label();
    info.binary_path = availability.resolved_path;

    arn::acp::AcpSession session;
    const auto error = session.activate(info);
    if (!error.empty()) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "OK: /acp kiro backend activated; "
                 "models advertised=%zu\n",
                 session.available_models().size());
    std::fflush(stderr);

    std::string set_err;
    if (!session.available_models().empty()) {
        if (!session.set_model(session.available_models().front(), set_err)) {
            std::fprintf(stderr, "WARN: set_model failed: %s\n", set_err.c_str());
        } else {
            std::fprintf(stderr, "OK: set_model selected '%s'\n",
                         session.available_models().front().c_str());
        }
        std::fflush(stderr);
    }

    session.deactivate();
    std::fprintf(stderr, "OK: deactivated cleanly\n");
    std::fflush(stderr);
    return 0;
}
