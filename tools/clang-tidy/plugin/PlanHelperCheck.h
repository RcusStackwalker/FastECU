#pragma once
#include "TestAssertionSupport.h"
#include "clang-tidy/ClangTidyCheck.h"
namespace fastecu::tidy
{
class PlanHelperCheck : public clang::tidy::ClangTidyCheck
{
  public:
    PlanHelperCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context);
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

  private:
};
} // namespace fastecu::tidy
