// RUN: %check_clang_tidy -std=c++23 %s fastecu-linear-search-loop %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <array>
#include <expected>
#include <string_view>
struct Entry { std::string_view name; int kind; };
constexpr std::array<Entry, 2> kEntries{{{"stock", 1}, {"custom", 2}}};
struct Device { const char *name; };
const Device kDevices[] = {{"stock"}, {"custom"}, {nullptr}};
std::expected<const Entry *, int> FindVariant(std::string_view name, int kind) {
    for (const Entry& entry : kEntries) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (entry.name == name) {
            if (entry.kind != kind) { return std::unexpected(1); }
            return &entry;
        }
    }
    return std::unexpected(2);
}
const Entry *FindCompound(std::string_view name, int kind) {
    for (const Entry& entry : kEntries) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (entry.name == name && entry.kind == kind) { return &entry; }
    }
    return nullptr;
}
int FindIndex(std::string_view name) {
    for (int index = 0; kDevices[index].name != nullptr; ++index) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (name == kDevices[index].name) { return index; }
    }
    return -1;
}
const Entry *FindIndexedAddress(std::string_view name) {
    for (unsigned index = 0; index < kEntries.size(); ++index) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-linear-search-loop]
        if (kEntries[index].name == name) { return &kEntries[index]; }
    }
    return nullptr;
}
const Entry *ReturnUnrelatedIndex(std::string_view name, const Entry& other) {
    for (unsigned index = 0; index < kEntries.size(); ++index) {
        if (kEntries[index].name == name) { return &other; }
    }
    return nullptr;
}
const Entry *ReturnUnrelatedCompound(std::string_view name, int kind, const Entry& other) {
    for (const Entry& entry : kEntries) {
        if (other.name == name && entry.kind) { return &entry; }
    }
    return nullptr;
}
int RetryCounter(int target) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt == target) { return attempt; }
    }
    return -1;
}
std::expected<const Entry *, int> ReturnOtherWrapped(std::string_view name, const Entry& other) {
    for (const Entry& entry : kEntries) {
        if (entry.name == name) { return &other; }
    }
    return std::unexpected(1);
}
struct ValidationFailure { const Entry *entry; int error; };
ValidationFailure StatefulValidation(std::string_view name) {
    bool seen = false;
    for (const Entry& entry : kEntries) {
        if (entry.name == name && seen) { return ValidationFailure{&entry, 1}; }
        seen = true;
    }
    return {nullptr, 0};
}
void NestedLambda(std::string_view name) {
    for (const Entry& entry : kEntries) {
        if (entry.name == name) { auto callback = [&] { return &entry; }; }
        auto callback = [&]() -> const Entry * {
            if (entry.name == name) { return &entry; }
            return nullptr;
        };
    }
}
