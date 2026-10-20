#include "AssertionLoopTraceCheck.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include <set>
namespace fastecu::tidy
{
namespace
{
const clang::Stmt *LoopBody(const clang::Stmt *statement)
{
    if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(statement))
        return loop->getBody();
    if (const auto *loop = llvm::dyn_cast<clang::CXXForRangeStmt>(statement))
        return loop->getBody();
    if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(statement))
        return loop->getBody();
    if (const auto *loop = llvm::dyn_cast<clang::DoStmt>(statement))
        return loop->getBody();
    return nullptr;
}
bool IsTrace(const clang::Stmt *statement)
{
    const auto *declaration = llvm::dyn_cast<clang::DeclStmt>(statement);
    if (!declaration)
        return false;
    for (const auto *item : declaration->decls())
        if (const auto *variable = llvm::dyn_cast<clang::VarDecl>(item))
            if (const auto *record = variable->getType()->getAsCXXRecordDecl();
                record && record->getQualifiedNameAsString() == "testing::ScopedTrace")
                return true;
    return false;
}
class TraceAnalysis
{
  public:
    explicit TraceAnalysis(clang::ASTContext& context) : context_(context)
    {
    }
    bool Missing(const clang::Stmt *statement, bool active = false, bool accept_trace = true)
    {
        if (!statement || llvm::isa<clang::LambdaExpr>(statement))
            return false;
        if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(statement))
            return MissingInBlock(*block, active, accept_trace);
        const auto macro =
            AssertionMacro(statement->getBeginLoc(), context_.getSourceManager(), context_.getLangOpts());
        if (macro == "SCOPED_TRACE")
            return false;
        if (!macro.empty())
            return !active;
        if (const auto *body = LoopBody(statement))
            return Missing(body, active, false);
        if (const auto *call = llvm::dyn_cast<clang::CXXOperatorCallExpr>(statement);
            call && MissingInLambda(*call, active, accept_trace))
            return true;
        for (const auto *child : statement->children())
            if (Missing(child, active, accept_trace))
                return true;
        return false;
    }

  private:
    bool MissingInBlock(const clang::CompoundStmt& block, bool active, bool accept_trace)
    {
        for (const auto *child : block.body())
        {
            if (accept_trace && IsTrace(child))
                active = true;
            if (Missing(child, active, accept_trace))
                return true;
        }
        return false;
    }
    bool MissingInLambda(const clang::CXXOperatorCallExpr& call, bool active, bool accept_trace)
    {
        if (call.getOperator() != clang::OO_Call)
            return false;
        const auto *method = llvm::dyn_cast_or_null<clang::CXXMethodDecl>(call.getDirectCallee());
        if (!method || !method->getParent()->isLambda() || !method->hasBody() || !lambdas_.insert(method).second)
            return false;
        const bool missing = Missing(method->getBody(), active, accept_trace);
        lambdas_.erase(method);
        return missing;
    }
    clang::ASTContext& context_;
    std::set<const clang::CXXMethodDecl *> lambdas_;
};
} // namespace
AssertionLoopTraceCheck::AssertionLoopTraceCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context), policy_(*context)
{
}
void AssertionLoopTraceCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(stmt(anyOf(forStmt(), cxxForRangeStmt(), whileStmt(), doStmt())).bind("loop"), this);
}
void AssertionLoopTraceCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    ast_ = result.Context;
    const auto *loop = result.Nodes.getNodeAs<clang::Stmt>("loop");
    const auto *function = EnclosingFunction(*loop, *result.Context);
    if (!function || !IsTestFunction(*function, *result.SourceManager))
        return;
    // Ignore loops generated inside assertion macros themselves.
    if (!AssertionMacro(loop->getBeginLoc(), *result.SourceManager, result.Context->getLangOpts()).empty())
        return;
    if (IsElementAssertionLoop(*loop, *result.Context))
        return;
    TraceAnalysis analysis(*result.Context);
    if (analysis.Missing(LoopBody(loop)) && !policy_.Allows("fastecu-assertion-loop-trace", *loop, *result.Context))
        diag(loop->getBeginLoc(),
             "identify each assertion-loop iteration with an iteration-local SCOPED_TRACE or named parameterized case");
}
void AssertionLoopTraceCheck::storeOptions(clang::tidy::ClangTidyOptions::OptionMap& options)
{
    policy_.StoreOptions(options);
}
void AssertionLoopTraceCheck::onEndOfTranslationUnit()
{
    if (!ast_)
        return;
    const auto error = policy_.ConfigurationError().empty() ? policy_.FlushUsage() : policy_.ConfigurationError();
    if (!error.empty())
        diag(ast_->getSourceManager().getLocForStartOfFile(ast_->getSourceManager().getMainFileID()), "%0",
             clang::DiagnosticIDs::Error)
            << error;
}
} // namespace fastecu::tidy
