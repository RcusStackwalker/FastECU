#pragma once
#include "TestAssertionSupport.h"
#include "clang-tidy/ClangTidyCheck.h"
namespace fastecu::tidy
{
class AssertionLoopTraceCheck : public clang::tidy::ClangTidyCheck
{
  public:
    AssertionLoopTraceCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context);
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
    void storeOptions(clang::tidy::ClangTidyOptions::OptionMap& options) override;
    void onEndOfTranslationUnit() override;

  private:
    ExceptionPolicy policy_;
    clang::ASTContext *ast_ = nullptr;
};
} // namespace fastecu::tidy
