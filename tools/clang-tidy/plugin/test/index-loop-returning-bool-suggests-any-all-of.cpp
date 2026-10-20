// RUN: %check_clang_tidy -std=c++23 %s fastecu-any-all-index-loop %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <array>
#include <cstddef>
#include <vector>
struct Block { int start; int len; };
bool MatchesExpected(const std::vector<Block>& blocks, const std::array<Block, 2>& expected) {
    for (std::size_t i = 0; i < blocks.size(); ++i) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-any-all-index-loop]
        if (blocks[i].start != expected[i].start) {
            return false;
        }
    }
    return true;
}
bool HasFlag(const std::vector<int>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-any-all-index-loop]
        if (values[i] == 3) {
            return true;
        }
    }
    return false;
}
bool Unrelated(const std::vector<int>& values) {
    for (const int value : values) {
        if (value == 3) {
            return true;
        }
    }
    return false;
}
int Attempt(int value) { return value; }
bool Retry() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (Attempt(attempt) == 2) {
            return true;
        }
    }
    return false;
}
int FindIndex(const std::vector<int>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == 3) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
bool TwoStatements(const std::vector<int>& values, int& seen) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        ++seen;
        if (values[i] == 3) {
            return true;
        }
    }
    return false;
}
