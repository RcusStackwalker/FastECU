// RUN: %check_clang_tidy -std=c++23 %s fastecu-message-concatenation %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <format>
#include <string>
#include <filesystem>
#include "src/backend/ports/result.h"
#include "src/backend/ports/event_sink.h"
void Messages(fastecu::IEventSink& events, std::string name) {
    auto bad = fastecu::Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, "timeout: " + name);
    events.Log(fastecu::LogLevel::kInfo, "selected " + name);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
    auto formatted = fastecu::Fail(fastecu::ErrorKind::kTimeout, std::format("timeout: {}", name));
    const auto path = "directory/" + name;
    auto literal = fastecu::Fail(fastecu::ErrorKind::kTimeout, "timeout");
    events.Log(fastecu::LogLevel::kInfo, std::format("selected {}", name));
    auto nested_path = fastecu::Fail(fastecu::ErrorKind::kTimeout,
        std::filesystem::path("directory/" + name).string());
    auto format_then_concat = fastecu::Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, "prefix " + std::format("{}", name));
}
