// Index loops that return a bool from a subscripted test: the condition must read
// an array through the loop counter, the body must be one `if` returning a bool
// literal, and the counter must advance with ++. Range-for loops are left to
// readability-use-anyofallof. Diagnostic only: confirm the bound is the array
// the condition reads.
#include "AnyAllIndexLoopCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void AnyAllIndexLoopCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        stmt(forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl().bind("index")))),
                     hasIncrement(unaryOperator(hasOperatorName("++"),
                                                hasUnaryOperand(declRefExpr(to(varDecl(equalsBoundNode("index"))))))),
                     hasBody(compoundStmt(
                         statementCountIs(1),
                         has(ifStmt(unless(hasElse(stmt())),
                                    hasCondition(hasDescendant(expr(anyOf(
                                        arraySubscriptExpr(hasIndex(
                                            ignoringParenImpCasts(declRefExpr(to(varDecl(equalsBoundNode("index"))))))),
                                        cxxOperatorCallExpr(hasOverloadedOperatorName("[]"),
                                                            hasArgument(1, ignoringParenImpCasts(declRefExpr(to(varDecl(
                                                                               equalsBoundNode("index"))))))))))),
                                    hasThen(compoundStmt(
                                        statementCountIs(1),
                                        has(returnStmt(hasReturnValue(ignoringParenImpCasts(cxxBoolLiteral()))))))))))))
            .bind("loop"),
        this);
}

void AnyAllIndexLoopCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("loop"))
    {
        diag(node->getBeginLoc(), "Index loop returning a bool from a subscripted search; consider "
                                  "std::ranges::any_of, std::ranges::all_of, or std::ranges::none_of");
    }
}
} // namespace fastecu::tidy
