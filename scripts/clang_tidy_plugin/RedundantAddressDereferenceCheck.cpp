// The outer address operator must be built-in. Dereference is either built-in
// or belongs to an allowlisted contiguous iterator in libc++, libstdc++, or
// MSVC STL. Pointer-based wrappers must wrap a raw pointer. Match the operand's
// actual type, not an inherited operator's declaring class. Other overloads
// (optional/expected, smart pointers, proxies, reverse/node iterators, user
// operators) stay excluded. Dependent templates wait for resolved instantiations.
// Unlike Sonar cpp:S3491, macro pairs are included and *& is
// outside this check's scope. The query provides diagnostics, not fix-its.
// Fix: std::to_address(p) for object pointers/iterators, keeping any end() guard. For
// function pointers, remove the pair; std::to_address does not accept them.
#include "RedundantAddressDereferenceCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

void RedundantAddressDereferenceCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        unaryOperator(
            hasOperatorName("&"), unless(isTypeDependent()),
            hasUnaryOperand(ignoringParenImpCasts(anyOf(
                unaryOperator(hasOperatorName("*")),
                cxxOperatorCallExpr(
                    hasOverloadedOperatorName("*"), callee(cxxMethodDecl()),
                    hasArgument(0, ignoringParenImpCasts(expr(hasType(hasUnqualifiedDesugaredType(
                                       recordType(hasDeclaration(classTemplateSpecializationDecl(anyOf(
                                           allOf(isInStdNamespace(),
                                                 anyOf(allOf(matchesName("(^|::)__wrap_iter$"),
                                                             hasTemplateArgument(0, refersToType(pointerType()))),
                                                       matchesName("(^|::)(__bounded_iter|__static_bounded_iter|_"
                                                                   "Vector_(const_)?iterator|_String_(const_)?iterator|"
                                                                   "_Array_(const_)?iterator|_Span_iterator)$"))),
                                           allOf(matchesName("^::__gnu_cxx::(__[0-9]+::)?__normal_iterator$"),
                                                 hasTemplateArgument(0, refersToType(pointerType())))))))))))))))))
            .bind("address"),
        this);
}

void RedundantAddressDereferenceCheck::check(const MatchFinder::MatchResult& result)
{
    if (const auto *node = result.Nodes.getNodeAs<Stmt>("address"))
    {
        diag(node->getBeginLoc(),
             "remove redundant &*; use std::to_address for object pointers and contiguous iterators");
    }
}
} // namespace fastecu::tidy
