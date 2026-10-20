// `"text " + x` passed directly to fastecu::Fail or IEventSink::Log, following
// transparent temporary/conversion wrappers. Not matched: arbitrary functions,
// locals traced across statements, or path construction nested in another call.
// Directly formatted messages are allowed.
#include "MessageConcatenationCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void MessageConcatenationCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        callExpr(callee(functionDecl(hasAnyName("::fastecu::Fail", "::fastecu::IEventSink::Log"))),
                 hasArgument(1, ignoringParenImpCasts(anyOf(
                                    ignoringImplicit(cxxOperatorCallExpr(
                                        hasOverloadedOperatorName("+"),
                                        hasType(hasUnqualifiedDesugaredType(recordType(
                                            hasDeclaration(cxxRecordDecl(hasName("::std::basic_string")))))))),
                                    cxxMemberCallExpr(callee(cxxConversionDecl()),
                                                      on(ignoringImplicit(cxxOperatorCallExpr(
                                                          hasOverloadedOperatorName("+"),
                                                          hasType(hasUnqualifiedDesugaredType(recordType(hasDeclaration(
                                                              cxxRecordDecl(hasName("::std::basic_string"))))))))))))))
            .bind("message"),
        this);
}

void MessageConcatenationCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("message"))
    {
        diag(node->getBeginLoc(), "construct this diagnostic message with std::format instead of string concatenation");
    }
}
} // namespace fastecu::tidy
