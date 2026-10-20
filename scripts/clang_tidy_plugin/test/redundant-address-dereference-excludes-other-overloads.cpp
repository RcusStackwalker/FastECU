// RUN: %check_clang_tidy -std=c++23 %s fastecu-redundant-address-dereference %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <iterator>
#include <list>
#include <memory>
#include <vector>
struct AddressOverload { AddressOverload *operator&(); };
struct Proxy { Proxy *operator&(); };
struct ProxyIterator { Proxy operator*(); };
struct CustomIterator { int& operator*(); int *operator->(); };
namespace unrelated {
template<class T> struct _Vector_iterator { T& operator*(); T *operator->(); };
namespace __gnu_cxx {
template<class T> struct __normal_iterator { int& operator*(); int *operator->(); };
}
}
struct DerivedIterator : std::vector<int>::iterator { int *operator->() const; };
void OtherAddresses(std::vector<int>& values, std::list<int>& nodes,
                    std::vector<AddressOverload>& overloaded,
                    std::unique_ptr<int>& unique, std::shared_ptr<int>& shared,
                    ProxyIterator proxy, CustomIterator custom,
                    unrelated::_Vector_iterator<int> lookalike,
                    unrelated::__gnu_cxx::__normal_iterator<int*> nested_lookalike,
                    DerivedIterator derived) {
    auto reverse = values.rbegin();
    auto reverse_address = &*reverse;
    auto node = nodes.begin();
    auto node_address = &*node;
    auto address_iterator = overloaded.begin();
    auto overloaded_address = &*address_iterator;
    auto unique_address = &*unique;
    auto shared_address = &*shared;
    auto proxy_address = &*proxy;
    auto custom_address = &*custom;
    auto lookalike_address = &*lookalike;
    auto nested_lookalike_address = &*nested_lookalike;
    auto derived_address = &*derived;
}
