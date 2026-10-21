#include "PlanHelperCheck.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include <set>
namespace fastecu::tidy
{
namespace
{
bool IsFixtureMemberInitializer(const clang::FunctionDecl& function, clang::ASTContext& context)
{
    using namespace clang::ast_matchers;
    const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
    if (!method || !method->getParent()->isLambda())
        return false;
    const auto *constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(method->getParent()->getDeclContext());
    if (!constructor ||
        match(cxxRecordDecl(isDerivedFrom("::testing::Test")), *constructor->getParent(), context).empty())
        return false;
    for (const auto *initializer : constructor->inits())
    {
        if (!initializer->isMemberInitializer())
            continue;
        const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(StripExpression(initializer->getInit()));
        if (call && call->getDirectCallee() == method)
            return true;
    }
    return false;
}

class ReturnDependency : public clang::RecursiveASTVisitor<ReturnDependency>
{
  public:
    bool fallible = false;
    std::set<const clang::VarDecl *> visited;
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitExpr(clang::Expr *expression)
    {
        fallible |= IsResultPlanType(expression->getType());
        return true;
    }
    bool VisitDeclRefExpr(clang::DeclRefExpr *reference)
    {
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
            variable && variable->hasInit() && visited.insert(variable).second)
            TraverseStmt(const_cast<clang::Expr *>(variable->getInit()));
        return true;
    }
};
class PlanBody : public clang::RecursiveASTVisitor<PlanBody>
{
  public:
    bool fallible_return = false;
    bool fallible_call = false;
    bool aborts = false;
    clang::ASTContext& context;
    explicit PlanBody(clang::ASTContext& selected) : context(selected)
    {
    }
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitReturnStmt(clang::ReturnStmt *statement)
    {
        ReturnDependency dependency;
        dependency.TraverseStmt(statement->getRetValue());
        fallible_return |= dependency.fallible;
        return true;
    }
    bool VisitCallExpr(clang::CallExpr *call)
    {
        fallible_call |= IsResultPlanType(call->getType());
        const auto macro = AssertionMacro(call->getBeginLoc(), context.getSourceManager(), context.getLangOpts());
        aborts |= !macro.empty();
        return true;
    }
    bool VisitCXXThrowExpr(clang::CXXThrowExpr *)
    {
        aborts = true;
        return true;
    }
};
} // namespace
PlanHelperCheck::PlanHelperCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}
void PlanHelperCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(functionDecl(isDefinition(), unless(isImplicit())).bind("function"), this);
}
void PlanHelperCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    const auto *function = result.Nodes.getNodeAs<clang::FunctionDecl>("function");
    if (!function || !IsTestFunction(*function, *result.SourceManager))
        return;
    const auto type = function->getReturnType().getNonReferenceType().getCanonicalType();
    const auto *record = type->getAsCXXRecordDecl();
    const bool raw = record && record->getQualifiedNameAsString() == "fastecu::flash::FlashPlan";
    if (!raw && !IsResultPlanType(type))
        return;
    if (raw && IsFixtureMemberInitializer(*function, *result.Context))
        return;
    PlanBody body(*result.Context);
    body.TraverseStmt(function->getBody());
    if ((raw && body.fallible_return) || (!raw && body.fallible_call && body.aborts))
        diag(function->getLocation(), "propagate fallible plan construction as Result<FlashPlan>; assert in the "
                                      "caller instead of unwrapping or throwing in a helper");
}
} // namespace fastecu::tidy
