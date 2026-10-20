#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "TestAssertionSupport.h"

namespace fastecu::tidy
{
class ResultAssertionCheck : public clang::tidy::ClangTidyCheck
{
  public:
    ResultAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context);
    void storeOptions(clang::tidy::ClangTidyOptions::OptionMap& options) override;
    void onEndOfTranslationUnit() override;
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

  private:
    void DiagnoseBoolean(const clang::Stmt& statement);
    void DiagnoseError(const clang::Expr& expression);
    void CheckConversion(const clang::CXXMemberCallExpr& conversion);
    bool InResultApi(const clang::Stmt& statement) const;
    ExceptionPolicy policy_;
    clang::ASTContext *ast_ = nullptr;
};
} // namespace fastecu::tidy
