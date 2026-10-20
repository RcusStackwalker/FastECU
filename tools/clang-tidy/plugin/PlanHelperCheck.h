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
    void storeOptions(clang::tidy::ClangTidyOptions::OptionMap& options) override;
    void onEndOfTranslationUnit() override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

  private:
    ExceptionPolicy policy_;
    clang::ASTContext *ast_ = nullptr;
};
} // namespace fastecu::tidy
