# ADR 0009: Use std::string_view instead of const char*

## Status

Proposed

## Context

We have options of using const char * and std::string_view for non-owning strings
to interface with std and pugixml libraries.
Since std::string_view offers more uniform support similar to other standard collections, we'll use it.
We are not using const std::string& because their constexpr constructor is unreliable.

## Decision

Use std::string_view instead of const char * whenever possible.
Pass std::string_view by value.
Don't pass const std::string& unless necessary.
Don't persist std::string_view copies beyond function call.

## Consequences

- NULL-safety
- Easier iteration
- Standard algorithms support

## Follow-up

- Find and convert non-compliant cases
