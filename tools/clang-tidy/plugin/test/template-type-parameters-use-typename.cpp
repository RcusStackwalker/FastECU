// RUN: %check_clang_tidy -std=c++23 %s fastecu-template-typename %t_test -- \
// RUN:   %load %check_config -- %cxx_flags

#include <concepts>
template <class First,
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: template <typename First,
          typename Second, class... Rest>
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: typename Second, typename... Rest>
struct Pair {};
template <class>
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: template <typename>
void Unnamed() {}
template <class Value = int>
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: template <typename Value = int>
void Defaulted() {}
template <template <class> class Wrapper>
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: template <template <typename> class Wrapper>
void Nested() {}
template <typename Value> void Fine() {}
template <typename... Values> void FinePack() {}
template <std::integral Constrained> void FineConstrained() {}
template <auto Value> void NonType() {}
template <template <typename> class Wrapper> void FineNested() {}
void Implicit() {
    auto generic = [](auto value) { return value; };
    (void)generic(1);
}
void Abbreviated(auto value) { (void)value; }
template <class Guided> struct Wrap { Wrap(Guided) {} };
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-template-typename]
// CHECK-FIXES: template <typename Guided> struct Wrap { Wrap(Guided) {} };
void Deduce() { Wrap wrapped(1); }
