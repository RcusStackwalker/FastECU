// Counting loops: a single `if` whose body only increments a counter declared
// outside the loop. Diagnostic only: the counter must not be read inside the loop.
#include "CountIfLoopCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void CountIfLoopCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        stmt(anyOf(cxxForRangeStmt(hasBody(compoundStmt(
                       statementCountIs(1), has(ifStmt(hasThen(compoundStmt(
                                                statementCountIs(1), has(unaryOperator(hasOperatorName("++")))))))))),
                   forStmt(hasBody(compoundStmt(
                       statementCountIs(1), has(ifStmt(hasThen(compoundStmt(
                                                statementCountIs(1), has(unaryOperator(hasOperatorName("++"))))))))))))
            .bind("loop"),
        this);
}

void CountIfLoopCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("loop"))
    {
        diag(node->getBeginLoc(), "Counting loop with a conditional increment; consider std::ranges::count_if");
    }
}
} // namespace fastecu::tidy
