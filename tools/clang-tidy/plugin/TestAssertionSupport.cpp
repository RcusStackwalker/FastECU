#include "TestAssertionSupport.h"

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <cstdint>
#include "llvm/ADT/StringExtras.h"
#include <vector>

namespace fastecu::tidy
{
namespace
{
const clang::ClassTemplateSpecializationDecl *StdSpecialization(clang::QualType type, llvm::StringRef name)
{
    type = type.getNonReferenceType().getCanonicalType();
    const auto *record = type->getAsCXXRecordDecl();
    const auto *specialization = llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
    if (!specialization || specialization->getName() != name)
        return nullptr;
    const clang::DeclContext *scope = specialization->getDeclContext();
    while (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(scope))
    {
        if (!space->isInline())
            break;
        scope = space->getParent();
    }
    const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
    return space && space->isStdNamespace() ? specialization : nullptr;
}

const clang::ClassTemplateSpecializationDecl *ResultDeclaration(clang::QualType type)
{
    const auto *specialization = StdSpecialization(type, "expected");
    if (!specialization)
        return nullptr;
    const auto& arguments = specialization->getTemplateArgs();
    if (arguments.size() != 2 || arguments[1].getKind() != clang::TemplateArgument::Type)
        return nullptr;
    const auto *error = arguments[1].getAsType().getCanonicalType()->getAsCXXRecordDecl();
    return error && error->getQualifiedNameAsString() == "fastecu::Error" ? specialization : nullptr;
}

const clang::CallExpr *MatcherCall(const clang::Expr *expression)
{
    expression = StripExpression(expression);
    if (const auto *reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression))
    {
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl()))
            return MatcherCall(variable->getInit());
    }
    return llvm::dyn_cast_or_null<clang::CallExpr>(expression);
}

std::string CallName(const clang::CallExpr *call)
{
    const auto *callee = call ? call->getDirectCallee() : nullptr;
    return callee ? callee->getQualifiedNameAsString() : std::string{};
}

class FormatterFinder : public clang::RecursiveASTVisitor<FormatterFinder>
{
  public:
    const clang::Expr *matcher = nullptr;
    bool VisitCallExpr(clang::CallExpr *call)
    {
        if (const auto *callee = call->getDirectCallee();
            callee && callee->getNameAsString() == "MakePredicateFormatterFromMatcher" && call->getNumArgs() == 1)
            matcher = call->getArg(0);
        return true;
    }
};
} // namespace

bool IsResultType(clang::QualType type)
{
    return ResultDeclaration(type) != nullptr;
}

bool IsExpectedType(clang::QualType type)
{
    return StdSpecialization(type, "expected") != nullptr;
}

bool IsOptionalType(clang::QualType type)
{
    return StdSpecialization(type, "optional") != nullptr;
}

bool IsByteRange(clang::QualType type)
{
    const auto *record = type.getNonReferenceType().getCanonicalType()->getAsCXXRecordDecl();
    const auto *specialization = llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
    if (!specialization)
        return false;
    const auto name = specialization->getSpecializedTemplate()->getQualifiedNameAsString();
    if (name != "std::vector" && name != "std::span" && name != "std::array")
        return false;
    const auto& arguments = specialization->getTemplateArgs();
    return arguments.size() && arguments[0].getKind() == clang::TemplateArgument::Type &&
           arguments[0].getAsType().getCanonicalType().getUnqualifiedType()->isSpecificBuiltinType(
               clang::BuiltinType::UChar);
}

namespace
{
using RangePredicate = std::pair<const clang::Expr *, const clang::Expr *>;
std::optional<RangePredicate> ClassicAllOf(const clang::CallExpr& call)
{
    if (call.getNumArgs() != 3)
        return std::nullopt;
    const auto *first = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(StripExpression(call.getArg(0)));
    const auto *last = llvm::dyn_cast_or_null<clang::CXXMemberCallExpr>(StripExpression(call.getArg(1)));
    if (!first || !last || !first->getMethodDecl() || !last->getMethodDecl())
        return std::nullopt;
    const auto begin = first->getMethodDecl()->getNameAsString();
    const auto end = last->getMethodDecl()->getNameAsString();
    if ((begin != "begin" && begin != "cbegin") || (end != "end" && end != "cend"))
        return std::nullopt;
    const auto *range = first->getImplicitObjectArgument();
    if (ResultKey(range).empty() || ResultKey(range) != ResultKey(last->getImplicitObjectArgument()))
        return std::nullopt;
    return RangePredicate{range, call.getArg(2)};
}
std::optional<RangePredicate> AllOfRange(const clang::CallExpr& call)
{
    if (!call.getDirectCallee())
        return std::nullopt;
    if (call.getDirectCallee()->getQualifiedNameAsString() == "std::all_of")
        return ClassicAllOf(call);
    const auto *operation = llvm::dyn_cast<clang::CXXOperatorCallExpr>(&call);
    if (!operation || operation->getNumArgs() < 3)
        return std::nullopt;
    const auto *object = llvm::dyn_cast_or_null<clang::DeclRefExpr>(StripExpression(operation->getArg(0)));
    if (!object || object->getDecl()->getQualifiedNameAsString() != "std::ranges::all_of")
        return std::nullopt;
    if (operation->getNumArgs() > 3 && !llvm::isa<clang::CXXDefaultArgExpr>(operation->getArg(3)))
        return std::nullopt;
    return RangePredicate{operation->getArg(1), operation->getArg(2)};
}
const clang::BinaryOperator *LambdaEquality(const clang::LambdaExpr& lambda)
{
    if (lambda.capture_size() || lambda.getCallOperator()->getNumParams() != 1)
        return nullptr;
    const auto *body = llvm::dyn_cast<clang::CompoundStmt>(lambda.getBody());
    if (!body || body->size() != 1)
        return nullptr;
    const auto *returned = llvm::dyn_cast<clang::ReturnStmt>(body->body_front());
    const auto *comparison =
        returned ? llvm::dyn_cast_or_null<clang::BinaryOperator>(StripExpression(returned->getRetValue())) : nullptr;
    return comparison && comparison->getOpcode() == clang::BO_EQ ? comparison : nullptr;
}
bool ComparesParameterToConstant(const clang::LambdaExpr& lambda, const clang::BinaryOperator& comparison,
                                 clang::ASTContext& context)
{
    for (unsigned side = 0; side < 2; ++side)
    {
        const auto *actual = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
            StripExpression(side ? comparison.getRHS() : comparison.getLHS()));
        const auto *expected = side ? comparison.getLHS() : comparison.getRHS();
        if (actual && actual->getDecl() == lambda.getCallOperator()->getParamDecl(0) &&
            expected->isIntegerConstantExpr(context))
            return true;
    }
    return false;
}
} // namespace
bool IsUniformBytePredicate(const clang::Expr *expression, clang::ASTContext& context)
{
    const auto *call = llvm::dyn_cast_or_null<clang::CallExpr>(StripExpression(expression));
    if (!call)
        return false;
    const auto pair = AllOfRange(*call);
    if (!pair || !IsByteRange(pair->first->getType()))
        return false;
    const auto *lambda = llvm::dyn_cast_or_null<clang::LambdaExpr>(StripExpression(pair->second));
    if (!lambda)
        return false;
    const auto *comparison = LambdaEquality(*lambda);
    return comparison && ComparesParameterToConstant(*lambda, *comparison, context);
}

bool IsElementAssertionLoop(const clang::Stmt& statement, clang::ASTContext& context)
{
    const auto *loop = llvm::dyn_cast<clang::CXXForRangeStmt>(&statement);
    if (!loop)
        return false;
    const clang::Stmt *body = loop->getBody();
    if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(body))
    {
        if (block->size() != 1)
            return false;
        body = block->body_front();
    }
    const auto macro = AssertionMacro(body->getBeginLoc(), context.getSourceManager(), context.getLangOpts());
    if (macro != "EXPECT_EQ" && macro != "ASSERT_EQ")
        return false;
    class Comparison : public clang::RecursiveASTVisitor<Comparison>
    {
      public:
        const clang::VarDecl *variable;
        clang::ASTContext& context;
        bool found = false;
        Comparison(const clang::VarDecl *selected, clang::ASTContext& ast) : variable(selected), context(ast)
        {
        }
        bool VisitCallExpr(clang::CallExpr *call)
        {
            const auto *callee = call->getDirectCallee();
            if (!callee || callee->getQualifiedNameAsString() != "testing::internal::EqHelper::Compare" ||
                call->getNumArgs() != 4)
                return true;
            for (unsigned index = 2; index <= 3; ++index)
            {
                const auto *actual = llvm::dyn_cast_or_null<clang::DeclRefExpr>(StripExpression(call->getArg(index)));
                const auto *expected = StripExpression(call->getArg(5 - index));
                if (actual && actual->getDecl() == variable && !expected->HasSideEffects(context) &&
                    expected->isIntegerConstantExpr(context))
                    found = true;
            }
            return true;
        }
    } comparison(loop->getLoopVariable(), context);
    comparison.TraverseStmt(const_cast<clang::Stmt *>(body));
    return comparison.found;
}

bool IsResultPlanType(clang::QualType type)
{
    const auto *result = ResultDeclaration(type);
    if (!result)
        return false;
    const auto *value = result->getTemplateArgs()[0].getAsType().getCanonicalType()->getAsCXXRecordDecl();
    return value && value->getQualifiedNameAsString() == "fastecu::flash::FlashPlan";
}

bool IsTestFunction(const clang::FunctionDecl& function, const clang::SourceManager& source)
{
    return IsTestSource(function.getBeginLoc(), source);
}

bool IsTestSource(clang::SourceLocation location, const clang::SourceManager& source)
{
    const auto path = llvm::sys::path::convert_to_slash(source.getFilename(source.getExpansionLoc(location)));
    return llvm::StringRef(path).ends_with("_test.cpp") || llvm::StringRef(path).ends_with("_test.cc") ||
           path.find("/testing/") != std::string::npos || path.find("/tests/") != std::string::npos;
}

const clang::Expr *StripExpression(const clang::Expr *expression)
{
    return expression ? expression->IgnoreImplicit()->IgnoreParenImpCasts() : nullptr;
}

std::string AssertionMacro(clang::SourceLocation location, const clang::SourceManager& source,
                           const clang::LangOptions& language)
{
    while (location.isMacroID())
    {
        const auto name = clang::Lexer::getImmediateMacroName(location, source, language);
        const bool assertion = (name.starts_with("ASSERT_") && !name.starts_with("ASSERT_PRED")) ||
                               (name.starts_with("EXPECT_") && !name.starts_with("EXPECT_PRED"));
        if (assertion || name == "SCOPED_TRACE")
            return name.str();
        location = source.getImmediateMacroCallerLoc(location);
    }
    return {};
}

std::optional<MatcherAssertion> GetMatcherAssertion(const clang::CXXOperatorCallExpr& call, clang::ASTContext& context)
{
    const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call.getDirectCallee());
    if (!method || method->getParent()->getName() != "PredicateFormatterFromMatcher" || call.getNumArgs() != 3)
        return std::nullopt;
    const auto name = AssertionMacro(call.getBeginLoc(), context.getSourceManager(), context.getLangOpts());
    if (name != "ASSERT_THAT" && name != "EXPECT_THAT")
        return std::nullopt;
    FormatterFinder finder;
    finder.TraverseStmt(const_cast<clang::Expr *>(call.getArg(0)));
    if (!finder.matcher)
        return std::nullopt;
    return MatcherAssertion{call.getArg(2), finder.matcher, name == "ASSERT_THAT"};
}

ResultState MatcherState(const clang::Expr *matcher)
{
    const auto *call = MatcherCall(matcher);
    const auto name = CallName(call);
    if (name == "fastecu::testing::IsOk" || name == "fastecu::testing::IsOkAnd")
        return ResultState::kSuccess;
    if (name == "fastecu::testing::IsErr" || name == "fastecu::testing::IsErrWith")
        return ResultState::kError;
    if (name == "testing::Not" && call->getNumArgs() == 1 &&
        CallName(MatcherCall(call->getArg(0))) == "fastecu::testing::IsOk")
        return ResultState::kError;
    return ResultState::kUnknown;
}

bool IsAnyErrorMatcher(const clang::Expr *matcher)
{
    const auto *call = MatcherCall(matcher);
    return CallName(call) == "testing::Not" && call->getNumArgs() == 1 &&
           CallName(MatcherCall(call->getArg(0))) == "fastecu::testing::IsOk";
}

std::string ResultKey(const clang::Expr *expression)
{
    expression = StripExpression(expression);
    if (const auto *reference = llvm::dyn_cast_or_null<clang::DeclRefExpr>(expression))
    {
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(reference->getDecl());
            variable && variable->getType()->isReferenceType() && variable->hasInit())
            return ResultKey(variable->getInit());
        return llvm::utostr(reinterpret_cast<std::uintptr_t>(reference->getDecl()->getCanonicalDecl()));
    }
    if (const auto *address = llvm::dyn_cast_or_null<clang::UnaryOperator>(expression);
        address && address->getOpcode() == clang::UO_AddrOf)
        return ResultKey(address->getSubExpr());
    if (const auto *member = llvm::dyn_cast_or_null<clang::MemberExpr>(expression))
    {
        const auto base = ResultKey(member->getBase());
        if (!base.empty())
            return base + "." +
                   llvm::utostr(reinterpret_cast<std::uintptr_t>(member->getMemberDecl()->getCanonicalDecl()));
    }
    if (llvm::isa_and_nonnull<clang::CXXThisExpr>(expression))
        return "this";
    return {};
}

const clang::FunctionDecl *EnclosingFunction(const clang::Stmt& statement, clang::ASTContext& context)
{
    auto node = clang::DynTypedNode::create(statement);
    for (unsigned depth = 0; depth < 128; ++depth)
    {
        const auto parents = context.getParents(node);
        if (parents.size() != 1)
            return nullptr;
        if (const auto *function = parents[0].get<clang::FunctionDecl>())
            return function;
        node = parents[0];
    }
    return nullptr;
}
} // namespace fastecu::tidy
