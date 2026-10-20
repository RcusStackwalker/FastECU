#include "ResultAssertionCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void ResultAssertionCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        cxxConstructExpr(
            hasType(recordType(hasDeclaration(cxxRecordDecl(hasName("::testing::AssertionResult"))))),
            anyOf(isExpandedFromMacro("EXPECT_TRUE"), isExpandedFromMacro("EXPECT_FALSE"),
                  isExpandedFromMacro("ASSERT_TRUE"), isExpandedFromMacro("ASSERT_FALSE")),
            hasArgument(
                0, ignoringImplicit(ignoringParenImpCasts(anyOf(
                       cxxMemberCallExpr(
                           callee(cxxMethodDecl(
                               hasName("has_value"),
                               ofClass(classTemplateSpecializationDecl(
                                   hasName("::std::expected"),
                                   hasTemplateArgument(1, refersToType(recordType(hasDeclaration(
                                                              cxxRecordDecl(hasName("::fastecu::Error")))))))))))
                           .bind("assertion"),
                       unaryOperator(
                           hasOperatorName("!"),
                           hasUnaryOperand(ignoringImplicit(ignoringParenImpCasts(
                               cxxMemberCallExpr(
                                   callee(cxxMethodDecl(
                                       hasName("has_value"),
                                       ofClass(classTemplateSpecializationDecl(
                                           hasName("::std::expected"),
                                           hasTemplateArgument(1, refersToType(recordType(hasDeclaration(cxxRecordDecl(
                                                                      hasName("::fastecu::Error")))))))))))
                                   .bind("assertion"))))))))),
            unless(isExpansionInFileMatching("(^|.*[/\\\\])src[/\\\\]backend[/\\\\]ports[/\\\\]result_test[.]cpp$"))),
        this);
}

void ResultAssertionCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("assertion"))
        diag(node->getBeginLoc(),
             "assert Result/Status with IsOk(), IsErr(), or IsErrWith() rather than a bare boolean assertion");
}
} // namespace fastecu::tidy
