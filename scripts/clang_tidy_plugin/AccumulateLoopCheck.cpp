// Summing loops: a single `+=` into a variable declared outside the loop, whose
// right-hand side reads the loop variable. Strided loops (`offset += 4`) are not
// matched because the counter must advance with ++. Diagnostic only.
#include "AccumulateLoopCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void AccumulateLoopCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        stmt(anyOf(cxxForRangeStmt(
                       hasLoopVariable(varDecl().bind("element")),
                       hasBody(compoundStmt(
                           statementCountIs(1),
                           has(stmt(anyOf(
                               binaryOperator(hasOperatorName("+="), hasLHS(declRefExpr()),
                                              hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element")))))),
                               cxxOperatorCallExpr(
                                   hasOverloadedOperatorName("+="), hasArgument(0, declRefExpr()),
                                   hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element")))))))))))),
                   forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl().bind("index")))),
                           hasIncrement(unaryOperator(hasOperatorName("++"), hasUnaryOperand(declRefExpr(to(
                                                                                 varDecl(equalsBoundNode("index"))))))),
                           hasBody(compoundStmt(
                               statementCountIs(1),
                               has(stmt(anyOf(
                                   binaryOperator(hasOperatorName("+="), hasLHS(declRefExpr()),
                                                  hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("index")))))),
                                   cxxOperatorCallExpr(
                                       hasOverloadedOperatorName("+="), hasArgument(0, declRefExpr()),
                                       hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("index"))))))))))))))
            .bind("loop"),
        this);
}

void AccumulateLoopCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("loop"))
    {
        diag(node->getBeginLoc(), "Summing loop; consider std::ranges::fold_left (or std::accumulate) over the range");
    }
}
} // namespace fastecu::tidy
