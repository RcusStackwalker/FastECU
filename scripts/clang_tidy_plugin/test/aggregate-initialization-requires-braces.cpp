// RUN: %check_clang_tidy -std=c++23 %s fastecu-aggregate-parens %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <vector>
struct Aggregate { int value; int other = 0; };
using Alias = Aggregate;
template <typename T> struct Box { T value; };
struct Constructed { explicit Constructed(int) {} };
struct Reference { const int& value; };
struct Base { int value; };
struct Derived : Base { int other; };
union Choice { int value; double other; };
#define INITIALIZE(type, value) type(value)
template <typename T> T Build(int value) {
    return T(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
}
void Examples(int value, double fractional) {
    Aggregate direct(value, 2);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto temporary = Aggregate(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto alias = Alias(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto deduced = Box(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto specialized = Box<int>(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto *allocated = new Aggregate(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    int array[2](value, 2);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    Choice choice(value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    Derived derived(Base{value}, 2);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    Aggregate narrowing(fractional);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    Reference reference(42);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto macro = INITIALIZE(Aggregate, value);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-aggregate-parens]
    auto from_template = Build<Aggregate>(value);
    auto constructed_template = Build<Constructed>(value);
    Aggregate braced{value, 2};
    auto braced_temporary = Aggregate{value};
    Aggregate copied(braced);
    auto copied_temporary = Aggregate(braced);
    Constructed constructor(value);
    std::vector<int> count_and_value(3, value);
    auto constructed = Constructed(value);
    auto scalar = int(value);
    auto zeroed = Aggregate();
    auto *zeroed_allocation = new Aggregate();
    int expression = (value + 2);
}
