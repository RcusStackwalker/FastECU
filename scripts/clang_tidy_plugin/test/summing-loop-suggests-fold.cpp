// RUN: %check_clang_tidy -std=c++23 %s fastecu-accumulate-loop %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
std::uint32_t SumBytes(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const std::uint8_t byte : bytes) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-accumulate-loop]
        sum += byte;
    }
    return sum;
}
std::uint32_t SumBlocks(const std::vector<std::uint32_t>& lengths) {
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-accumulate-loop]
        total += lengths[i];
    }
    return total;
}
std::string Joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-accumulate-loop]
        out += part;
    }
    return out;
}
std::uint32_t SumWords(const std::vector<std::uint8_t>& rom, std::size_t size) {
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
        sum += rom[offset];
    }
    return sum;
}
std::uint32_t SumConstant(int count) {
    std::uint32_t sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += 1;
    }
    return sum;
}
