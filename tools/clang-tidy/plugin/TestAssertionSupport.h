#pragma once

#include "clang/AST/ASTContext.h"
#include "clang/AST/ExprCXX.h"
#include "clang-tidy/ClangTidyDiagnosticConsumer.h"

#include <memory>
#include <optional>
#include <string>

namespace fastecu::tidy
{
enum class ResultState
{
    kUnknown,
    kSuccess,
    kError
};

struct MatcherAssertion
{
    const clang::Expr *actual;
    const clang::Expr *matcher;
    bool fatal;
};

bool IsUniformBytePredicate(const clang::Expr *expression, clang::ASTContext& context);
bool IsByteRange(clang::QualType type);
bool IsElementAssertionLoop(const clang::Stmt& statement, clang::ASTContext& context);
bool IsResultType(clang::QualType type);
bool IsResultPlanType(clang::QualType type);
bool IsTestSource(clang::SourceLocation location, const clang::SourceManager& source);
bool IsTestFunction(const clang::FunctionDecl& function, const clang::SourceManager& source);
const clang::Expr *StripExpression(const clang::Expr *expression);
std::string AssertionMacro(clang::SourceLocation location, const clang::SourceManager& source,
                           const clang::LangOptions& language);
std::optional<MatcherAssertion> GetMatcherAssertion(const clang::CXXOperatorCallExpr& call, clang::ASTContext& context);
ResultState MatcherState(const clang::Expr *matcher);
bool IsAnyErrorMatcher(const clang::Expr *matcher);
std::string ResultKey(const clang::Expr *expression);
const clang::FunctionDecl *EnclosingFunction(const clang::Stmt& statement, clang::ASTContext& context);

class ExceptionPolicy
{
  public:
    explicit ExceptionPolicy(clang::tidy::ClangTidyContext& context);
    ~ExceptionPolicy();
    bool Allows(llvm::StringRef check, const clang::Stmt& statement, clang::ASTContext& context);
    const std::string& ConfigurationError() const;
    std::string FlushUsage();
    void StoreOptions(clang::tidy::ClangTidyOptions::OptionMap& options) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fastecu::tidy
