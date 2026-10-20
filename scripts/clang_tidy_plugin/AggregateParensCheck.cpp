#include "AggregateParensCheck.h"

#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
void AggregateParensCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang;
    using namespace clang::ast_matchers;
    // LLVM 23 has the AST node but no named matcher for it.
    const internal::VariadicDynCastAllOfMatcher<Stmt, CXXParenListInitExpr> paren_list_init;
    finder->addMatcher(paren_list_init(unless(isExpansionInSystemHeader())).bind("initialization"), this);
}

void AggregateParensCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    const auto *initialization = result.Nodes.getNodeAs<clang::CXXParenListInitExpr>("initialization");
    // Braces change narrowing and reference-lifetime rules, so do not offer
    // a mechanical replacement even when both delimiters are ordinary tokens.
    diag(result.SourceManager->getExpansionLoc(initialization->getBeginLoc()),
         "initialize aggregates with braces instead of parentheses");
}
} // namespace fastecu::tidy
