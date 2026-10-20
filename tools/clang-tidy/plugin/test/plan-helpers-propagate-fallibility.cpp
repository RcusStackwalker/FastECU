// RUN: %check_clang_tidy -std=c++23 %s fastecu-plan-helper %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <stdexcept>
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::Result;
using fastecu::flash::FlashPlan;
Result<FlashPlan> Build();
FlashPlan HiddenFailure() {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-plan-helper]
    auto result = Build();
    EXPECT_THAT(result, fastecu::testing::IsOk());
    return std::move(*result);
}
FlashPlan ThrowingFailure() {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-plan-helper]
    auto result = Build();
    if (!result.has_value()) throw std::runtime_error("failed");
    return std::move(*result);
}
Result<FlashPlan> Propagate() { return Build(); }
FlashPlan Forward(FlashPlan plan) { return plan; }
