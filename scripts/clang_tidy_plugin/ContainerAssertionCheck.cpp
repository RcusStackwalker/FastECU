// Boolean container predicates as the direct operand of a GoogleTest assertion,
// with the same operand rules as fastecu-result-assertion.
#include "ContainerAssertionCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

ContainerAssertionCheck::ContainerAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}

void ContainerAssertionCheck::registerMatchers(MatchFinder *finder)
{
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
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("assertion"))
    {
        diag(node->getBeginLoc(),
             "assert container properties with Google Mock matchers instead of reducing the predicate to a boolean");
    }
}
} // namespace fastecu::tidy
