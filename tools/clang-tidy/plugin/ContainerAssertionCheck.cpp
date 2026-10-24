// Boolean container predicates (the std algorithms and empty()) as the direct
// operand of a GoogleTest assertion, with the same operand rules as
// fastecu-result-assertion.
#include "ContainerAssertionCheck.h"
#include "TestAssertionSupport.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

ContainerAssertionCheck::ContainerAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context), uniform_bytes_enabled_(context->isCheckEnabled("fastecu-uniform-bytes"))
{
}

void ContainerAssertionCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(cxxForRangeStmt().bind("element_loop"), this);
    const auto empty_call =
        cxxMemberCallExpr(callee(cxxMethodDecl(hasName("empty"), parameterCountIs(0))), hasType(booleanType()))
            .bind("empty_assertion");
    finder->addMatcher(
        cxxConstructExpr(hasType(recordType(hasDeclaration(cxxRecordDecl(hasName("::testing::AssertionResult"))))),
                         anyOf(isExpandedFromMacro("EXPECT_TRUE"), isExpandedFromMacro("EXPECT_FALSE"),
                               isExpandedFromMacro("ASSERT_TRUE"), isExpandedFromMacro("ASSERT_FALSE")),
                         hasArgument(0, ignoringImplicit(ignoringParenImpCasts(anyOf(
                                            empty_call, unaryOperator(hasOperatorName("!"),
                                                                      hasUnaryOperand(ignoringImplicit(
                                                                          ignoringParenImpCasts(empty_call))))))))),
        this);
    finder->addMatcher(
        cxxConstructExpr(
            hasType(recordType(hasDeclaration(cxxRecordDecl(hasName("::testing::AssertionResult"))))),
            anyOf(isExpandedFromMacro("EXPECT_TRUE"), isExpandedFromMacro("EXPECT_FALSE"),
                  isExpandedFromMacro("ASSERT_TRUE"), isExpandedFromMacro("ASSERT_FALSE")),
            hasArgument(
                0,
                ignoringImplicit(ignoringParenImpCasts(anyOf(
                    expr(anyOf(callExpr(callee(functionDecl(
                                   hasAnyName("::std::all_of", "::std::any_of", "::std::none_of", "::std::equal")))),
                               cxxOperatorCallExpr(
                                   hasOverloadedOperatorName("()"),
                                   hasArgument(0, ignoringParenImpCasts(declRefExpr(to(varDecl(hasAnyName(
                                                      "::std::ranges::all_of", "::std::ranges::any_of",
                                                      "::std::ranges::none_of", "::std::ranges::equal")))))))))
                        .bind("assertion"),
                    unaryOperator(
                        hasOperatorName("!"),
                        hasUnaryOperand(ignoringImplicit(ignoringParenImpCasts(
                            expr(anyOf(callExpr(callee(functionDecl(hasAnyName("::std::all_of", "::std::any_of",
                                                                               "::std::none_of", "::std::equal")))),
                                       cxxOperatorCallExpr(
                                           hasOverloadedOperatorName("()"),
                                           hasArgument(0, ignoringParenImpCasts(declRefExpr(to(varDecl(hasAnyName(
                                                              "::std::ranges::all_of", "::std::ranges::any_of",
                                                              "::std::ranges::none_of", "::std::ranges::equal")))))))))
                                .bind("assertion")))))))))),
        this);
}

void ContainerAssertionCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *loop = result.Nodes.getNodeAs<Stmt>("element_loop");
        loop && IsElementAssertionLoop(*loop, *result.Context))
        diag(loop->getBeginLoc(), "assert every element with a container matcher instead of an assertion loop");
    if (const auto *empty = result.Nodes.getNodeAs<Stmt>("empty_assertion"))
        diag(empty->getBeginLoc(),
             "assert emptiness with IsEmpty() or Not(IsEmpty()) so a failure prints the contents");
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("assertion"))
    {
        if (uniform_bytes_enabled_)
            if (const auto *expression = llvm::dyn_cast<Expr>(node);
                expression && IsUniformBytePredicate(expression, *result.Context))
                return;
        diag(node->getBeginLoc(),
             "assert container properties with Google Mock matchers instead of reducing the predicate to a boolean");
    }
}
} // namespace fastecu::tidy
