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
    : ClangTidyCheck(name, context), policy_(*context)
{
}
void SingleUseResultCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(compoundStmt().bind("block"), this);
}
void SingleUseResultCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    ast_ = result.Context;
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
    if (!policy_.Allows("fastecu-single-use-result", *declaration, *result.Context))
        diag(variable->getLocation(), "inline this single-use Status operation into its adjacent assertion");
}
void SingleUseResultCheck::storeOptions(clang::tidy::ClangTidyOptions::OptionMap& options)
{
    policy_.StoreOptions(options);
}
void SingleUseResultCheck::onEndOfTranslationUnit()
{
    if (!ast_)
        return;
    const auto error = policy_.ConfigurationError().empty() ? policy_.FlushUsage() : policy_.ConfigurationError();
    if (!error.empty())
        diag(ast_->getSourceManager().getLocForStartOfFile(ast_->getSourceManager().getMainFileID()), "%0",
             clang::DiagnosticIDs::Error)
            << error;
}
} // namespace fastecu::tidy
