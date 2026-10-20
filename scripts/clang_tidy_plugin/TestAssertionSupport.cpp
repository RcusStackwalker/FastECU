#include "TestAssertionSupport.h"

#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLTraits.h"

#include <cstdlib>
#include <cstdint>
#include "llvm/ADT/StringExtras.h"
#include <set>
#include <tuple>
#include <vector>

namespace fastecu::tidy
{
struct ExceptionRecord
{
    std::string check;
    std::string file;
    std::string symbol;
    std::string rationale;
};
struct ExceptionDocument
{
    int version = 0;
    std::vector<ExceptionRecord> exceptions;
};
} // namespace fastecu::tidy

LLVM_YAML_IS_SEQUENCE_VECTOR(fastecu::tidy::ExceptionRecord)
namespace llvm::yaml
{
template <> struct MappingTraits<fastecu::tidy::ExceptionRecord>
{
    static void mapping(IO& io, fastecu::tidy::ExceptionRecord& record)
    {
        io.mapRequired("check", record.check);
        io.mapRequired("file", record.file);
        io.mapRequired("symbol", record.symbol);
        io.mapRequired("rationale", record.rationale);
    }
};
template <> struct MappingTraits<fastecu::tidy::ExceptionDocument>
{
    static void mapping(IO& io, fastecu::tidy::ExceptionDocument& document)
    {
        io.mapRequired("version", document.version);
        io.mapRequired("exceptions", document.exceptions);
    }
};
} // namespace llvm::yaml

namespace fastecu::tidy
{
namespace
{
const clang::ClassTemplateSpecializationDecl *ResultDeclaration(clang::QualType type)
{
    type = type.getNonReferenceType().getCanonicalType();
    const auto *record = type->getAsCXXRecordDecl();
    const auto *specialization = llvm::dyn_cast_or_null<clang::ClassTemplateSpecializationDecl>(record);
    if (!specialization || specialization->getName() != "expected")
        return nullptr;
    const clang::DeclContext *scope = specialization->getDeclContext();
    while (const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(scope))
    {
        if (!space->isInline())
            break;
        scope = space->getParent();
    }
    const auto *space = llvm::dyn_cast<clang::NamespaceDecl>(scope);
    if (!space || !space->isStdNamespace())
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

std::string AbsolutePath(llvm::StringRef path, llvm::StringRef directory)
{
    llvm::SmallString<256> absolute(path);
    llvm::sys::path::make_absolute(directory, absolute);
    llvm::sys::path::remove_dots(absolute, true);
    return llvm::sys::path::convert_to_slash(absolute);
}

std::string FunctionIdentity(const clang::FunctionDecl& function, clang::ASTContext& context)
{
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        method && method->getNameAsString() == "TestBody")
    {
        auto location = context.getSourceManager().getExpansionLoc(method->getParent()->getBeginLoc());
        auto token = clang::Lexer::findNextToken(location, context.getSourceManager(), context.getLangOpts());
        if (token && token->is(clang::tok::l_paren))
        {
            token =
                clang::Lexer::findNextToken(token->getLocation(), context.getSourceManager(), context.getLangOpts());
            if (token)
            {
                const auto suite = clang::Lexer::getSpelling(*token, context.getSourceManager(), context.getLangOpts());
                token = clang::Lexer::findNextToken(token->getLocation(), context.getSourceManager(),
                                                    context.getLangOpts());
                if (token)
                    token = clang::Lexer::findNextToken(token->getLocation(), context.getSourceManager(),
                                                        context.getLangOpts());
                if (token)
                    return suite + "." +
                           clang::Lexer::getSpelling(*token, context.getSourceManager(), context.getLangOpts());
            }
        }
    }
    std::string identity = function.getQualifiedNameAsString() + "(";
    for (unsigned index = 0; index < function.getNumParams(); ++index)
    {
        if (index)
            identity += ", ";
        identity += function.getParamDecl(index)->getOriginalType().getAsString();
    }
    return identity + ")";
}
} // namespace

bool IsResultType(clang::QualType type)
{
    return ResultDeclaration(type) != nullptr;
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

struct ExceptionPolicy::Impl
{
    clang::tidy::ClangTidyContext& context;
    std::string option;
    std::string root;
    std::string error;
    bool loaded = false;
    ExceptionDocument document;
    std::set<std::tuple<std::string, std::string, std::string>> used;

    explicit Impl(clang::tidy::ClangTidyContext& selected) : context(selected)
    {
        const auto& options = context.getOptions().CheckOptions;
        const auto found = options.find("fastecu.ExceptionsFile");
        option = found == options.end() ? ".clang-tidy-exceptions.yaml" : found->second.Value;
    }

    void Load()
    {
        if (loaded)
            return;
        loaded = true;
        const auto path = AbsolutePath(option, context.getCurrentBuildDirectory());
        root = llvm::sys::path::parent_path(path).str();
        const auto buffer = llvm::MemoryBuffer::getFile(path);
        if (!buffer)
        {
            error = "cannot read central exception policy: " + path;
            return;
        }
        llvm::yaml::Input input((*buffer)->getBuffer());
        input >> document;
        if (input.error() || document.version != 1)
            error = "invalid central exception policy: " + path;
    }
};

ExceptionPolicy::ExceptionPolicy(clang::tidy::ClangTidyContext& context) : impl_(std::make_unique<Impl>(context))
{
}
ExceptionPolicy::~ExceptionPolicy() = default;

bool ExceptionPolicy::Allows(llvm::StringRef check, const clang::Stmt& statement, clang::ASTContext& context)
{
    impl_->Load();
    if (!impl_->error.empty())
        return false;
    const auto *function = EnclosingFunction(statement, context);
    if (!function)
        return false;
    const auto& source = context.getSourceManager();
    const auto absolute = AbsolutePath(source.getFilename(source.getExpansionLoc(statement.getBeginLoc())),
                                       impl_->context.getCurrentBuildDirectory());
    const auto prefix = impl_->root + "/";
    if (!llvm::StringRef(absolute).starts_with(prefix))
        return false;
    const auto file = absolute.substr(prefix.size());
    const auto symbol = FunctionIdentity(*function, context);
    for (const auto& record : impl_->document.exceptions)
    {
        if (record.check == check && record.file == file && record.symbol == symbol && !record.rationale.empty())
        {
            impl_->used.emplace(record.check, file, symbol);
            return true;
        }
    }
    return false;
}

const std::string& ExceptionPolicy::ConfigurationError() const
{
    return impl_->error;
}

void ExceptionPolicy::StoreOptions(clang::tidy::ClangTidyOptions::OptionMap& options) const
{
    options["fastecu.ExceptionsFile"] = clang::tidy::ClangTidyOptions::ClangTidyValue(llvm::StringRef(impl_->option));
}

std::string ExceptionPolicy::FlushUsage()
{
    const char *directory = std::getenv("FASTECU_TIDY_EXCEPTION_USAGE_DIR");
    if (!directory || impl_->used.empty())
        return {};
    llvm::SmallString<256> path(directory);
    llvm::sys::path::append(path, "usage-%%%%%%.json");
    int descriptor = -1;
    llvm::SmallString<256> output;
    if (const auto error = llvm::sys::fs::createUniqueFile(path, descriptor, output))
        return error.message();
    llvm::json::Array records;
    for (const auto& [check, file, symbol] : impl_->used)
        records.push_back(llvm::json::Object{{"check", check}, {"file", file}, {"symbol", symbol}});
    llvm::raw_fd_ostream stream(descriptor, true);
    stream << llvm::formatv("{0}", llvm::json::Value(std::move(records)));
    stream.flush();
    return stream.has_error() ? "cannot write central exception usage" : std::string{};
}
} // namespace fastecu::tidy
