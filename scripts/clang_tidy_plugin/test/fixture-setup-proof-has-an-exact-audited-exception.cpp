// RUN: rm -rf %t.dir
// RUN: mkdir -p %t.dir/usage
// RUN: cp %S/Inputs/fixture-setup-proof-has-an-exact-audited-exception.yaml %t.dir/policy.yaml
// RUN: env FASTECU_TIDY_EXCEPTION_USAGE_DIR=%t.dir/usage %check_clang_tidy -std=c++23 %s fastecu-result-access %t.dir/fixture_test -- \
// RUN:   %load --config="{CheckOptions: {fastecu.ExceptionsFile: %t.dir/policy.yaml}}" -- %cxx_flags
// RUN: diff %t.dir/fixture_test.orig %t.dir/fixture_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
class SharedSetup : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_THAT(result, fastecu::testing::IsOk()); }
    const fastecu::Result<int> result = ReadValue();
};
TEST_F(SharedSetup, Allowed) { EXPECT_EQ(*result, 1); }
TEST_F(SharedSetup, Other) { EXPECT_EQ(*result, 1); }
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]

// RUN: cat %t.dir/usage/*.json | FileCheck %s --check-prefix=USAGE --match-full-lines --implicit-check-not='{{.}}'
// USAGE: [{"check":"fastecu-result-access","file":"fixture_test.cpp","symbol":"SharedSetup.Allowed"}]
