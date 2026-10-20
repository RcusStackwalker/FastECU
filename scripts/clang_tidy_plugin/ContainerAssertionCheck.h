#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace fastecu::tidy
{
class ContainerAssertionCheck : public clang::tidy::ClangTidyCheck
{
  public:
    ContainerAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context);
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
};
} // namespace fastecu::tidy
