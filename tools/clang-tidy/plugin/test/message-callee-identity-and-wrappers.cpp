// RUN: %check_clang_tidy -std=c++23 %s fastecu-message-concatenation %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <string>
#include <string_view>
#include <format>
#include "src/backend/ports/result.h"
#include "src/backend/ports/event_sink.h"
namespace unrelated {
void Fail(fastecu::ErrorKind, std::string) {}
struct IEventSink { void Log(fastecu::LogLevel, std::string_view) {} };
}
struct DerivedSink : fastecu::IEventSink {
    void Log(fastecu::LogLevel, std::string_view) override {}
};
template<class T> void TemplateMessage(T name) {
    auto error = fastecu::Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, "template " + name);
}
void Messages(fastecu::IEventSink& events, DerivedSink& derived, std::string name) {
    auto parentheses = fastecu::Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, (("prefix " + name)));
    auto chain = fastecu::Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, "prefix " + name + " suffix");
    using fastecu::Fail;
    auto imported = Fail(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::ErrorKind::kTimeout, name + " suffix");
    events.Log(fastecu::LogLevel::kInfo, (("prefix " + name)));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
    events.Log(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::LogLevel::kInfo, "prefix " + name + " suffix");
    events.Log(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::LogLevel::kInfo, std::format("{}", name) + " suffix");
    derived.fastecu::IEventSink::Log(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-message-concatenation]
        fastecu::LogLevel::kInfo, "prefix " + name);
    derived.Log(fastecu::LogLevel::kInfo, "prefix " + name);
    unrelated::Fail(fastecu::ErrorKind::kTimeout, "prefix " + name);
    unrelated::IEventSink other;
    other.Log(fastecu::LogLevel::kInfo, "prefix " + name);
    const auto local = "prefix " + name;
    events.Log(fastecu::LogLevel::kInfo, local);
    TemplateMessage(name);
}
