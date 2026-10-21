// RUN: %check_clang_tidy -std=c++23 %s fastecu-redundant-address-dereference %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>
#define ITERATOR_ADDRESS(it) (&*(it))
template<class Container> auto FindAddress(const Container& values, int value) {
    const auto found = std::ranges::find(values, value);
    return found == values.end() ? nullptr : &*found;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
}
void ContiguousAddresses(std::vector<int>& values, const std::vector<int>& constants,
                         std::string& text, const std::string& constant_text,
                         std::array<int, 2>& array, std::span<int> span) {
    auto mutable_iterator = values.begin();
    auto mutable_address = &*mutable_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto constant_iterator = constants.begin();
    auto constant_address = &(*constant_iterator);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto text_iterator = text.begin();
    auto text_address = &*text_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto constant_text_iterator = constant_text.begin();
    auto constant_text_address = &*constant_text_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto array_iterator = array.begin();
    auto array_address = &*array_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto span_iterator = span.begin();
    auto span_address = &*span_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto macro_address = ITERATOR_ADDRESS(mutable_iterator);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto template_address = FindAddress(constants, 42);
    auto replacement = std::to_address(mutable_iterator);
}
