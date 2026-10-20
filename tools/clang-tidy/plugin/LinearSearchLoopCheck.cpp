// Report element-address searches (including implicit Result construction and
// conjunctions) and indexed searches returning the matching index/address.
// Returns must be direct children of the matched branch: nested callbacks are
// not loop exits. Indexed equality must access an array using the loop counter.
// Diagnostic only: sentinel bounds and validation order need manual review.
#include "LinearSearchLoopCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void LinearSearchLoopCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        stmt(anyOf(
                 cxxForRangeStmt(
                     hasLoopVariable(varDecl().bind("element")),
                     hasBody(compoundStmt(has(ifStmt(
                         hasCondition(expr(anyOf(
                             expr(anyOf(binaryOperator(hasOperatorName("==")),
                                        cxxOperatorCallExpr(hasOverloadedOperatorName("=="))),
                                  hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element")))))),
                             binaryOperator(
                                 hasOperatorName("&&"),
                                 hasDescendant(
                                     expr(anyOf(binaryOperator(hasOperatorName("==")),
                                                cxxOperatorCallExpr(hasOverloadedOperatorName("=="))),
                                          hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element"))))))))))),
                         hasThen(compoundStmt(has(returnStmt(hasReturnValue(expr(anyOf(
                             unaryOperator(hasOperatorName("&"), hasUnaryOperand(ignoringParenImpCasts(declRefExpr(
                                                                     to(varDecl(equalsBoundNode("element"))))))),
                             expr(anyOf(cxxConstructExpr(
                                            hasDeclaration(
                                                cxxConstructorDecl(ofClass(cxxRecordDecl(hasName("::std::expected"))))),
                                            hasDescendant(unaryOperator(
                                                hasOperatorName("&"), hasUnaryOperand(ignoringParenImpCasts(declRefExpr(
                                                                          to(varDecl(equalsBoundNode("element"))))))))),
                                        hasDescendant(cxxConstructExpr(
                                            hasDeclaration(
                                                cxxConstructorDecl(ofClass(cxxRecordDecl(hasName("::std::expected"))))),
                                            hasDescendant(
                                                unaryOperator(hasOperatorName("&"),
                                                              hasUnaryOperand(ignoringParenImpCasts(declRefExpr(
                                                                  to(varDecl(equalsBoundNode("element"))))))))))),
                                  unless(hasDescendant(lambdaExpr()))))))))))))))),
                 forStmt(
                     hasLoopInit(declStmt(hasSingleDecl(varDecl().bind("index")))),
                     hasIncrement(unaryOperator(hasOperatorName("++"),
                                                hasUnaryOperand(declRefExpr(to(varDecl(equalsBoundNode("index"))))))),
                     hasBody(compoundStmt(has(ifStmt(
                         hasCondition(expr(
                             anyOf(binaryOperator(hasOperatorName("==")),
                                   cxxOperatorCallExpr(hasOverloadedOperatorName("=="))),
                             hasDescendant(expr(anyOf(
                                 arraySubscriptExpr(hasIndex(
                                     ignoringParenImpCasts(declRefExpr(to(varDecl(equalsBoundNode("index"))))))),
                                 cxxOperatorCallExpr(hasOverloadedOperatorName("[]"),
                                                     hasArgument(1, ignoringParenImpCasts(declRefExpr(
                                                                        to(varDecl(equalsBoundNode("index")))))))))))),
                         hasThen(compoundStmt(has(returnStmt(hasReturnValue(ignoringParenImpCasts(expr(anyOf(
                             declRefExpr(to(varDecl(equalsBoundNode("index")))),
                             unaryOperator(hasOperatorName("&"),
                                           hasUnaryOperand(ignoringParenImpCasts(expr(anyOf(
                                               arraySubscriptExpr(hasIndex(ignoringParenImpCasts(
                                                   declRefExpr(to(varDecl(equalsBoundNode("index"))))))),
                                               cxxOperatorCallExpr(
                                                   hasOverloadedOperatorName("[]"),
                                                   hasArgument(1, ignoringParenImpCasts(declRefExpr(to(varDecl(
                                                                      equalsBoundNode("index")))))))))))))))))))))))))))
            .bind("loop"),
        this);
}

void LinearSearchLoopCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("loop"))
    {
        diag(node->getBeginLoc(), "Linear search loop returning a matching element or index; consider "
                                  "std::ranges::find or std::ranges::find_if");
    }
}
} // namespace fastecu::tidy
