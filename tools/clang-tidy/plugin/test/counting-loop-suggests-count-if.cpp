// RUN: %check_clang_tidy -std=c++23 %s fastecu-count-if-loop %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <cstddef>
#include <vector>
int CountLarge(const std::vector<int>& values) {
    int large = 0;
    for (const int value : values) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-count-if-loop]
        if (value > 3) {
            ++large;
        }
    }
    return large;
}
int CountIndexed(const std::vector<int>& values) {
    int hits = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-count-if-loop]
        if (values[i] == 3) {
            hits++;
        }
    }
    return hits;
}
int CountAll(const std::vector<int>& values) {
    int total = 0;
    for (const int value : values) {
        ++total;
    }
    return total;
}
int CountTwoStatements(const std::vector<int>& values, int& seen) {
    int hits = 0;
    for (const int value : values) {
        ++seen;
        if (value == 3) {
            ++hits;
        }
    }
    return hits;
}
