// Match the direct operand of GoogleTest's AssertionResult construction.
// Following only transparent wrappers and logical negation excludes unrelated
// nested calls/aggregates while retaining direct assertions inside callbacks.
// Boolean assertions on any std::expected or std::optional are diagnosed; the
// any-error rules apply only to std::expected<T, fastecu::Error>. The Result
// API suite (src/backend/ports/result_test.cpp) is excluded.
#include "ResultAssertionCheck.h"

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/ParentMapContext.h"

namespace fastecu::tidy
{
using namespace clang;
using namespace clang::ast_matchers;

ResultAssertionCheck::ResultAssertionCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}

void ResultAssertionCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(
        cxxConstructExpr(hasType(recordType(hasDeclaration(cxxRecordDecl(hasName("::testing::AssertionResult"))))),
                         anyOf(isExpandedFromMacro("EXPECT_TRUE"), isExpandedFromMacro("EXPECT_FALSE"),
                               isExpandedFromMacro("ASSERT_TRUE"), isExpandedFromMacro("ASSERT_FALSE")))
            .bind("boolean_construct"),
        this);
    finder->addMatcher(cxxMemberCallExpr(callee(cxxConversionDecl())).bind("conversion"), this);
    finder->addMatcher(cxxMemberCallExpr(callee(cxxMethodDecl(hasName("error")))).bind("error_access"), this);
    finder->addMatcher(cxxOperatorCallExpr().bind("matcher_call"), this);
    finder->addMatcher(
        cxxConstructExpr(
            hasType(recordType(hasDeclaration(cxxRecordDecl(hasName("::testing::AssertionResult"))))),
            anyOf(isExpandedFromMacro("EXPECT_TRUE"), isExpandedFromMacro("EXPECT_FALSE"),
                  isExpandedFromMacro("ASSERT_TRUE"), isExpandedFromMacro("ASSERT_FALSE")),
            hasArgument(
                0, ignoringImplicit(ignoringParenImpCasts(anyOf(
                       cxxMemberCallExpr(callee(cxxMethodDecl(hasName("has_value")))).bind("assertion"),
                       unaryOperator(
                           hasOperatorName("!"),
                           hasUnaryOperand(ignoringImplicit(ignoringParenImpCasts(
                               cxxMemberCallExpr(callee(cxxMethodDecl(hasName("has_value")))).bind("assertion"))))))))),
            unless(isExpansionInFileMatching("(^|.*[/\\\\])src[/\\\\]backend[/\\\\]ports[/\\\\]result_test[.]cpp$"))),
        this);
}

namespace
{
// The library may declare has_value() and operator bool in an implementation
// base class, so look through the implicit derived-to-base conversion.
QualType ObjectType(const CXXMemberCallExpr& call)
{
    const auto type = call.getImplicitObjectArgument()->IgnoreImpCasts()->getType();
    return type->isPointerType() ? type->getPointeeType() : type;
}

bool IsBooleanAssertedType(QualType type)
{
    return IsExpectedType(type) || IsOptionalType(type);
}

bool IsErrorAssertion(const CXXMemberCallExpr& access, ASTContext& context)
{
    if (!IsResultType(access.getImplicitObjectArgument()->getType()))
        return false;
    if (!AssertionMacro(access.getBeginLoc(), context.getSourceManager(), context.getLangOpts()).empty())
        return true;
    // Appended messages are outside the macro; follow their stream's left edge.
    auto current = DynTypedNode::create(access);
    for (unsigned depth = 0; depth < 32; ++depth)
    {
        const auto parents = context.getParents(current);
        if (parents.size() != 1)
            break;
        if (const auto *stream = parents[0].get<CXXOperatorCallExpr>(); stream && stream->getOperator() == OO_LessLess)
            if (!AssertionMacro(stream->getBeginLoc(), context.getSourceManager(), context.getLangOpts()).empty())
                return true;
        if (!parents[0].get<Expr>())
            break;
        current = parents[0];
    }
    return false;
}
} // namespace

bool ResultAssertionCheck::InResultApi(const Stmt& statement) const
{
    const auto& source = ast_->getSourceManager();
    return llvm::StringRef(source.getFilename(source.getExpansionLoc(statement.getBeginLoc())))
        .ends_with("src/backend/ports/result_test.cpp");
}

void ResultAssertionCheck::DiagnoseBoolean(const Stmt& statement, QualType type)
{
    if (InResultApi(statement))
        return;
    if (IsResultType(type))
        diag(statement.getBeginLoc(),
             "assert Result/Status with IsOk(), IsErr(), or IsErrWith() rather than a bare boolean assertion");
    else if (IsExpectedType(type))
        diag(statement.getBeginLoc(),
             "assert std::expected with IsOk() or IsErrThat() rather than a bare boolean assertion");
    else
        diag(statement.getBeginLoc(),
             "assert std::optional with Optional() or Eq(std::nullopt) rather than a bare boolean assertion");
}

void ResultAssertionCheck::DiagnoseError(const Expr& expression)
{
    if (InResultApi(expression) || !IsTestSource(expression.getBeginLoc(), ast_->getSourceManager()))
        return;
    if (!EnclosingFunction(expression, *ast_))
        return;
    diag(expression.getBeginLoc(),
         "assert Result errors with IsErr() or IsErrWith(); an any-error assertion requires a NOLINT that says why");
}

void ResultAssertionCheck::CheckConversion(const CXXMemberCallExpr& conversion)
{
    const auto macro = AssertionMacro(conversion.getBeginLoc(), ast_->getSourceManager(), ast_->getLangOpts());
    const bool boolean_assertion =
        macro == "ASSERT_TRUE" || macro == "ASSERT_FALSE" || macro == "EXPECT_TRUE" || macro == "EXPECT_FALSE";
    const auto type = ObjectType(conversion);
    if (boolean_assertion && IsBooleanAssertedType(type))
        DiagnoseBoolean(conversion, type);
}

void ResultAssertionCheck::check(const MatchFinder::MatchResult& result)
{
    ast_ = result.Context;
    if (const auto *construct = result.Nodes.getNodeAs<CXXConstructExpr>("boolean_construct");
        construct && construct->getNumArgs())
    {
        const auto *actual = StripExpression(construct->getArg(0));
        if (IsBooleanAssertedType(actual->getType()))
            DiagnoseBoolean(*actual, actual->getType());
    }
    if (const auto *conversion = result.Nodes.getNodeAs<CXXMemberCallExpr>("conversion"))
        CheckConversion(*conversion);
    if (const auto *access = result.Nodes.getNodeAs<CXXMemberCallExpr>("error_access");
        access && IsErrorAssertion(*access, *ast_))
        DiagnoseError(*access);
    if (const auto *call = result.Nodes.getNodeAs<CXXOperatorCallExpr>("matcher_call"))
    {
        const auto assertion = GetMatcherAssertion(*call, *ast_);
        if (assertion && IsResultType(assertion->actual->getType()) && IsAnyErrorMatcher(assertion->matcher))
            DiagnoseError(*assertion->actual);
    }
    if (const auto *node = result.Nodes.getNodeAs<CXXMemberCallExpr>("assertion");
        node && IsBooleanAssertedType(ObjectType(*node)))
        DiagnoseBoolean(*node, ObjectType(*node));
}
} // namespace fastecu::tidy
