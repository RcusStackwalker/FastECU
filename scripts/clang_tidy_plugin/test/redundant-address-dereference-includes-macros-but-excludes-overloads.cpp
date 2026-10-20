// RUN: %check_clang_tidy -std=c++23 %s fastecu-redundant-address-dereference %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <array>
#include <expected>
#include <memory>
#include <optional>
#define POINTER_ROUND_TRIP(p) (&*(p))
#define DEREFERENCE(p) (*(p))
#define ADDRESS(v) (&(v))
struct AddressOverload {
    AddressOverload *operator&() { return nullptr; }
};
struct DereferenceOverload {
    int& operator*();
};
template<class T> T *RoundTrip(T *pointer) {
    return &*pointer;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
}
template<class T> auto OverloadedRoundTrip(T& value) { return &*value; }
void PointerExpressions(int *pointer, int value, void (*callback)(),
                        AddressOverload *overloaded_address,
                        DereferenceOverload overloaded_dereference) {
    auto direct = &*pointer;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto parenthesized = &(*pointer);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto nested_parentheses = &((*(pointer)));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    const int *const_pointer = pointer;
    auto qualified = &*const_pointer;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    std::array<int, 2> array{1, 2};
    int *raw_iterator = array.data();
    auto iterator_address = &*raw_iterator;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto template_result = RoundTrip(pointer);
    auto function_address = &*callback;
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto address = std::to_address(pointer);
    auto reversed = *&value;
    auto address_overload = &*overloaded_address;
    auto dereference_overload = &*overloaded_dereference;
    std::optional<int> optional{value};
    std::expected<int, int> expected{value};
    auto optional_address = &*optional;
    auto expected_address = &*expected;
    auto optional_template = OverloadedRoundTrip(optional);
    auto expected_template = OverloadedRoundTrip(expected);
    auto macro_pair = POINTER_ROUND_TRIP(pointer);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto macro_dereference = &DEREFERENCE(pointer);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
    auto macro_address = ADDRESS(*pointer);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-redundant-address-dereference]
}
