// RUN: %check_clang_tidy -std=c++23 %s fastecu-linear-search-loop %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <array>
#include <string_view>
struct CatalogEntry { std::string_view protocol; int id; };
constexpr std::array<CatalogEntry, 2> kCatalog{{{"stock", 1}, {"custom", 2}}};
const CatalogEntry *FindCatalog(std::string_view protocol) {
    for (const CatalogEntry& entry : kCatalog) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (entry.protocol == protocol) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *FindId(int id) {
    for (const CatalogEntry& entry : kCatalog) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *FindUnrelated(std::string_view protocol, const CatalogEntry& other) {
    for (const CatalogEntry& entry : kCatalog) {
        if (other.protocol == protocol) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *ReturnOther(std::string_view protocol, const CatalogEntry& other) {
    for (const CatalogEntry& entry : kCatalog) {
        if (entry.protocol == protocol) {
            return &other;
        }
    }
    return nullptr;
}
