// RUN: rm -rf %t.dir
// RUN: mkdir -p %t.dir/usage
// RUN: cp %S/Inputs/result-assertion-exception-is-exact-and-reports-usage.yaml %t.dir/policy.yaml
// RUN: env FASTECU_TIDY_EXCEPTION_USAGE_DIR=%t.dir/usage %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t.dir/fixture_test -- \
// RUN:   %load --config="{CheckOptions: {fastecu.ExceptionsFile: %t.dir/policy.yaml}}" -- %cxx_flags
// RUN: diff %t.dir/fixture_test.orig %t.dir/fixture_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(Exceptions, Allowed) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk()));
}
TEST(Exceptions, Other) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk()));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
}

// RUN: cat %t.dir/usage/*.json | FileCheck %s --check-prefix=USAGE --match-full-lines --implicit-check-not='{{.}}'
// USAGE: [{"check":"fastecu-result-assertion","file":"fixture_test.cpp","symbol":"Exceptions.Allowed"}]
