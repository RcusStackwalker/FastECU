#pragma once

#include "clang-tidy/ClangTidyCheck.h"
#include "TestAssertionSupport.h"

namespace fastecu::tidy
{
class ResultAssertionCheck : public clang::tidy::ClangTidyCheck
{
  public:
    ResultAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context);
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;

  private:
    void DiagnoseBoolean(const clang::Stmt& statement, clang::QualType type);
    void DiagnoseError(const clang::Expr& expression);
    void CheckConversion(const clang::CXXMemberCallExpr& conversion);
    bool InResultApi(const clang::Stmt& statement) const;
    clang::ASTContext *ast_ = nullptr;
};
} // namespace fastecu::tidy
