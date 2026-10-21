#include "SingleUseResultCheck.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
namespace fastecu::tidy
{
namespace
{
class References : public clang::RecursiveASTVisitor<References>
{
  public:
    const clang::VarDecl *variable;
    unsigned count = 0;
    explicit References(const clang::VarDecl *selected) : variable(selected)
    {
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr *reference)
    {
        if (reference->getDecl() == variable)
            ++count;
        return true;
    }
};
class Assertion : public clang::RecursiveASTVisitor<Assertion>
{
  public:
    clang::ASTContext& context;
    std::optional<MatcherAssertion> found;
    explicit Assertion(clang::ASTContext& selected) : context(selected)
    {
    }
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *call)
    {
        if (auto candidate = GetMatcherAssertion(*call, context))
            found = candidate;
        return true;
    }
};
bool IsPureStatusAssertion(const clang::CompoundStmt& selected, const clang::VarDecl *variable,
                           clang::ASTContext& context)
{
    const auto *block = &selected;
    const auto macro =
        AssertionMacro(block->body_back()->getBeginLoc(), context.getSourceManager(), context.getLangOpts());
    if (macro != "ASSERT_THAT" && macro != "EXPECT_THAT")
        return false;
    Assertion assertion(context);
    assertion.TraverseStmt(const_cast<clang::Stmt *>(block->body_back()));
    if (!assertion.found)
        return false;
    const auto *actual = llvm::dyn_cast_or_null<clang::DeclRefExpr>(StripExpression(assertion.found->actual));
    const auto *matcher = llvm::dyn_cast_or_null<clang::CallExpr>(StripExpression(assertion.found->matcher));
    if (!actual || actual->getDecl() != variable || !matcher || !matcher->getDirectCallee() ||
        matcher->getDirectCallee()->getQualifiedNameAsString() != "fastecu::testing::IsOk" || matcher->getNumArgs())
        return false;
    return true;
}
} // namespace
SingleUseResultCheck::SingleUseResultCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}
void SingleUseResultCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(compoundStmt().bind("block"), this);
}
void SingleUseResultCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    const auto *block = result.Nodes.getNodeAs<clang::CompoundStmt>("block");
    if (!block || block->size() < 2)
        return;
    const auto *function = EnclosingFunction(*block, *result.Context);
    if (!function || !IsTestFunction(*function, *result.SourceManager))
        return;
    // Only the final adjacent pair preserves destruction timing without having
    // to infer cleanup effects in arbitrary subsequent statements.
    const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(*(block->body_end() - 2));
    if (!declaration || !declaration->isSingleDecl())
        return;
    const auto *variable = llvm::dyn_cast<clang::VarDecl>(declaration->getSingleDecl());
    if (!variable || !variable->hasInit() || !IsResultType(variable->getType()))
        return;
    const auto *specialization = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
        variable->getType().getCanonicalType()->getAsCXXRecordDecl());
    if (!specialization || !specialization->getTemplateArgs()[0].getAsType()->isVoidType())
        return;
    References references(variable);
    references.TraverseStmt(function->getBody());
    if (references.count != 1)
        return;
    if (!IsPureStatusAssertion(*block, variable, *result.Context))
        return;
    diag(variable->getLocation(), "inline this single-use Status operation into its adjacent assertion");
}
} // namespace fastecu::tidy
