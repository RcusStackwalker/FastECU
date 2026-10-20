#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace fastecu::tidy
{
class LinearSearchLoopCheck : public clang::tidy::ClangTidyCheck
{
  public:
    using ClangTidyCheck::ClangTidyCheck;
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
};
} // namespace fastecu::tidy
