#include "UniformBytesCheck.h"
#include "clang/ASTMatchers/ASTMatchers.h"
namespace fastecu::tidy
{
namespace
{
const clang::CallExpr *DirectCall(const clang::Expr *expression)
{
    return llvm::dyn_cast_or_null<clang::CallExpr>(StripExpression(expression));
}
bool IsEachConstant(const clang::Expr *expression, clang::ASTContext& context)
{
    const auto *each = DirectCall(expression);
    if (!each || !each->getDirectCallee() || each->getDirectCallee()->getQualifiedNameAsString() != "testing::Each" ||
        each->getNumArgs() != 1)
        return false;
    const auto *equal = DirectCall(each->getArg(0));
    return equal && equal->getDirectCallee() && equal->getDirectCallee()->getQualifiedNameAsString() == "testing::Eq" &&
           equal->getNumArgs() == 1 && StripExpression(equal->getArg(0))->isIntegerConstantExpr(context);
}
bool IsFilledVector(const clang::Expr *expression, clang::ASTContext& context)
{
    expression = StripExpression(expression);
    const auto *construct = llvm::dyn_cast_or_null<clang::CXXConstructExpr>(expression);
    if (!construct || !IsByteRange(construct->getType()) || construct->getNumArgs() < 2)
        return false;
    const auto *constructor = construct->getConstructor();
    return constructor->getNumParams() >= 2 && constructor->getParamDecl(0)->getType()->isIntegerType() &&
           StripExpression(construct->getArg(1))->isIntegerConstantExpr(context);
}
const clang::Expr *FilledVectorActual(const clang::CallExpr& call, clang::ASTContext& context)
{
    const auto *callee = call.getDirectCallee();
    if (!callee || callee->getQualifiedNameAsString() != "testing::internal::EqHelper::Compare" ||
        call.getNumArgs() != 4)
        return nullptr;
    for (unsigned side = 2; side <= 3; ++side)
        if (IsByteRange(call.getArg(side)->getType()) && IsFilledVector(call.getArg(5 - side), context))
            return call.getArg(side);
    return nullptr;
}
} // namespace
UniformBytesCheck::UniformBytesCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}
void UniformBytesCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(callExpr(unless(isExpansionInSystemHeader())).bind("call"), this);
}
void UniformBytesCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    const auto *call = result.Nodes.getNodeAs<clang::CallExpr>("call");
    const auto *function = EnclosingFunction(*call, *result.Context);
    if (!function || !IsTestFunction(*function, *result.SourceManager))
        return;
    const clang::Expr *candidate = nullptr;
    bool preserve_size = false;
    if (const auto *operation = llvm::dyn_cast<clang::CXXOperatorCallExpr>(call))
    {
        if (const auto assertion = GetMatcherAssertion(*operation, *result.Context);
            assertion && IsByteRange(assertion->actual->getType()) &&
            IsEachConstant(assertion->matcher, *result.Context))
            candidate = assertion->actual;
    }
    if (const auto *actual = FilledVectorActual(*call, *result.Context))
    {
        candidate = actual;
        preserve_size = true;
    }
    if (IsUniformBytePredicate(call, *result.Context) &&
        !AssertionMacro(call->getBeginLoc(), *result.SourceManager, result.Context->getLangOpts()).empty())
        candidate = call;
    if (candidate)
        diag(candidate->getBeginLoc(),
             preserve_size ? "assert uniform bytes with BytesFilledWith(value) and retain a separate size assertion"
                           : "assert uniform bytes with test_bytes::BytesFilledWith(value)");
}
} // namespace fastecu::tidy
