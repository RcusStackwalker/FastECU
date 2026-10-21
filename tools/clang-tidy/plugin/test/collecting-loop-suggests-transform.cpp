// RUN: %check_clang_tidy -std=c++23 %s fastecu-transform-collect-loop %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
std::vector<std::uint8_t> Indices(int count) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < count; ++i) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-transform-collect-loop]
        out.push_back(static_cast<std::uint8_t>(i));
    }
    return out;
}
std::vector<int> Doubled(const std::vector<int>& values) {
    std::vector<int> out;
    for (const int value : values) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-transform-collect-loop]
        out.emplace_back(value * 2);
    }
    return out;
}
std::vector<std::string> Labels(const std::vector<int>& values) {
    std::vector<std::string> out;
    for (const int value : values) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-transform-collect-loop]
        out.push_back(std::to_string(value));
    }
    return out;
}
void Constants(std::vector<int>& calls) {
    for (int i = 0; i < 3; ++i) {
        calls.push_back(7);
    }
}
void Pairs(std::vector<int>& calls, const std::vector<int>& values) {
    for (const int value : values) {
        calls.push_back(value);
        calls.push_back(value);
    }
}
