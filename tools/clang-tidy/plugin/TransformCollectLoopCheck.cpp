// Collecting loops: a single push_back or emplace_back whose argument reads the
// loop variable, so each element maps to one appended value. Constant pushes are
// not reported. Diagnostic only: the container must not be read in the loop.
#include "TransformCollectLoopCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void TransformCollectLoopCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        stmt(anyOf(cxxForRangeStmt(
                       hasLoopVariable(varDecl().bind("element")),
                       hasBody(compoundStmt(
                           statementCountIs(1),
                           has(expr(anyOf(
                               cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("push_back", "emplace_back"))),
                                                 hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element")))))),
                               exprWithCleanups(has(cxxMemberCallExpr(
                                   callee(cxxMethodDecl(hasAnyName("push_back", "emplace_back"))),
                                   hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("element")))))))))))))),
                   forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl().bind("index")))),
                           hasIncrement(unaryOperator(hasOperatorName("++"), hasUnaryOperand(declRefExpr(to(
                                                                                 varDecl(equalsBoundNode("index"))))))),
                           hasBody(compoundStmt(
                               statementCountIs(1),
                               has(expr(anyOf(
                                   cxxMemberCallExpr(callee(cxxMethodDecl(hasAnyName("push_back", "emplace_back"))),
                                                     hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("index")))))),
                                   exprWithCleanups(has(cxxMemberCallExpr(
                                       callee(cxxMethodDecl(hasAnyName("push_back", "emplace_back"))),
                                       hasDescendant(declRefExpr(to(varDecl(equalsBoundNode("index"))))))))))))))))
            .bind("loop"),
        this);
}

void TransformCollectLoopCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("loop"))
    {
        diag(node->getBeginLoc(), "Loop appending one value per element; consider std::ranges::transform into "
                                  "std::back_inserter or std::views::transform with std::ranges::to");
    }
}
} // namespace fastecu::tidy
