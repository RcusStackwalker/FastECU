// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <stdexcept>
#include "src/backend/ports/result.h"
fastecu::Result<int> ReadValue();
void Log();

int ThrowOfAPlainValue() {
    auto result = ReadValue();
    if (!result.has_value()) throw 1;
    return *result;
}
int ThrowOfAConstructedException() {
    auto result = ReadValue();
    if (!result.has_value()) throw std::runtime_error("no value");
    return *result;
}
int ThrowInsideBraces() {
    auto result = ReadValue();
    if (!result.has_value()) { throw std::runtime_error("no value"); }
    return *result;
}
int GuardThatDoesNotTerminate() {
    auto result = ReadValue();
    if (!result.has_value()) Log();
    return *result;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
