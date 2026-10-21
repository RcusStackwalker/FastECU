#include "ResultAccessCheck.h"

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchers.h"

#include <functional>
#include <map>
#include <set>

namespace fastecu::tidy
{
namespace
{
using Facts = std::map<std::string, ResultState>;

void Kill(Facts& facts, const std::string& key)
{
    if (key.empty())
        return;
    if (key == "*")
    {
        facts.clear();
        return;
    }
    std::erase_if(facts, [&](const auto& fact) { return fact.first == key || fact.first.starts_with(key + "."); });
}

Facts Intersection(const Facts& left, const Facts& right)
{
    Facts result;
    for (const auto& [key, state] : left)
        if (const auto found = right.find(key); found != right.end() && found->second == state)
            result.emplace(key, state);
    return result;
}

const clang::Expr *ConditionObject(const clang::Expr *condition)
{
    if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(condition))
    {
        const auto *method = call->getMethodDecl();
        if (!method)
            return nullptr;
        const auto name = method->getNameAsString();
        if (name != "has_value" && name != "operator bool")
            return nullptr;
        return call->getImplicitObjectArgument();
    }
    return condition;
}

void InvalidateChanges(const clang::Stmt *statement, Facts& facts);

void ApplyCondition(const clang::Expr *condition, bool truth, Facts& facts)
{
    condition = StripExpression(condition);
    if (!condition)
        return;
    if (const auto *negation = llvm::dyn_cast<clang::UnaryOperator>(condition);
        negation && negation->getOpcode() == clang::UO_LNot)
    {
        ApplyCondition(negation->getSubExpr(), !truth, facts);
        return;
    }
    if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(condition))
    {
        if ((binary->getOpcode() == clang::BO_LAnd && truth) || (binary->getOpcode() == clang::BO_LOr && !truth))
        {
            ApplyCondition(binary->getLHS(), truth, facts);
            // The RHS executes after the LHS proof. Do not resurrect that
            // proof after a subsequent operand may have changed its object.
            InvalidateChanges(binary->getRHS(), facts);
            ApplyCondition(binary->getRHS(), truth, facts);
        }
        return;
    }
    const auto *object = ConditionObject(condition);
    if (object && IsResultType(object->getType()))
        if (const auto key = ResultKey(object); !key.empty())
            facts[key] = truth ? ResultState::kSuccess : ResultState::kError;
}

std::optional<bool> ConditionTruth(const clang::Expr *condition, const Facts& facts)
{
    condition = StripExpression(condition);
    if (!condition)
        return std::nullopt;
    if (const auto *literal = llvm::dyn_cast<clang::CXXBoolLiteralExpr>(condition))
        return literal->getValue();
    if (const auto *negation = llvm::dyn_cast<clang::UnaryOperator>(condition);
        negation && negation->getOpcode() == clang::UO_LNot)
    {
        if (const auto truth = ConditionTruth(negation->getSubExpr(), facts))
            return !*truth;
        return std::nullopt;
    }
    const clang::Expr *object = condition;
    if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(condition))
    {
        if (!call->getMethodDecl() || (call->getMethodDecl()->getNameAsString() != "has_value" &&
                                       call->getMethodDecl()->getNameAsString() != "operator bool"))
            return std::nullopt;
        object = call->getImplicitObjectArgument();
    }
    if (IsResultType(object->getType()))
        if (const auto found = facts.find(ResultKey(object)); found != facts.end())
            return found->second == ResultState::kSuccess;
    return std::nullopt;
}

class AssertionFinder : public clang::RecursiveASTVisitor<AssertionFinder>
{
  public:
    explicit AssertionFinder(clang::ASTContext& context) : context_(context)
    {
    }
    std::optional<MatcherAssertion> assertion;
    const clang::Expr *boolean = nullptr;
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *call)
    {
        if (const auto candidate = GetMatcherAssertion(*call, context_))
            assertion = candidate;
        return true;
    }
    bool VisitCXXConstructExpr(clang::CXXConstructExpr *construct)
    {
        if (construct->getConstructor()->getParent()->getQualifiedNameAsString() == "testing::AssertionResult" &&
            construct->getNumArgs() == 1)
        {
            Facts proof;
            ApplyCondition(construct->getArg(0), true, proof);
            if (!proof.empty())
                boolean = construct->getArg(0);
        }
        return true;
    }

  private:
    clang::ASTContext& context_;
};

const clang::Expr *MutationObject(const clang::CallExpr& call)
{
    if (const auto *member = llvm::dyn_cast<clang::CXXMemberCallExpr>(&call))
        return member->getImplicitObjectArgument();
    if (const auto *operation = llvm::dyn_cast<clang::CXXOperatorCallExpr>(&call); operation && operation->getNumArgs())
        return operation->getArg(0);
    return nullptr;
}
bool IsResultAccessor(const clang::Expr *object, const clang::CXXMethodDecl& method)
{
    if (!object || !IsResultType(object->getType()))
        return false;
    const auto name = method.getNameAsString();
    return name == "operator*" || name == "operator->" || name == "has_value" || name == "error" || name == "value" ||
           name == "operator bool";
}
std::string ArgumentMutationKey(clang::QualType type, const clang::Expr *argument)
{
    if (!type->isReferenceType() && !type->isPointerType())
        return {};
    if (type->getPointeeType().isConstQualified())
        return {};
    if (type->isPointerType() && IsResultType(type->getPointeeType()))
    {
        const auto *address = llvm::dyn_cast_or_null<clang::UnaryOperator>(StripExpression(argument));
        return address && address->getOpcode() == clang::UO_AddrOf ? ResultKey(address->getSubExpr()) : "*";
    }
    return ResultKey(argument);
}
std::set<std::string> IndirectMutations(const clang::CallExpr& call)
{
    std::set<std::string> keys;
    auto type = call.getCallee()->getType();
    if (type->isPointerType())
        type = type->getPointeeType();
    if (const auto *prototype = type->getAs<clang::FunctionProtoType>())
        for (unsigned index = 0; index < prototype->getNumParams() && index < call.getNumArgs(); ++index)
            keys.insert(ArgumentMutationKey(prototype->getParamType(index), call.getArg(index)));
    keys.erase("");
    return keys;
}

std::set<std::string> OwnMutations(const clang::CallExpr& call)
{
    std::set<std::string> keys;
    const auto *function = call.getDirectCallee();
    if (!function)
        return IndirectMutations(call);
    if (const auto *method = llvm::dyn_cast<clang::CXXMethodDecl>(function); method && !method->isConst())
    {
        const auto *object = MutationObject(call);
        if (object && !IsResultAccessor(object, *method))
            keys.insert(ResultKey(object));
    }
    const unsigned offset =
        llvm::isa<clang::CXXOperatorCallExpr>(call) && llvm::isa<clang::CXXMethodDecl>(function) ? 1U : 0U;
    for (unsigned index = 0; index < function->getNumParams() && index + offset < call.getNumArgs(); ++index)
        keys.insert(ArgumentMutationKey(function->getParamDecl(index)->getType(), call.getArg(index + offset)));
    keys.erase("");
    return keys;
}

class MutationFinder : public clang::RecursiveASTVisitor<MutationFinder>
{
  public:
    std::set<std::string> keys;
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitBinaryOperator(clang::BinaryOperator *operation)
    {
        if (operation->isAssignmentOp())
            keys.insert(ResultKey(operation->getLHS()));
        return true;
    }
    bool VisitUnaryOperator(clang::UnaryOperator *operation)
    {
        if (operation->isIncrementDecrementOp())
            keys.insert(ResultKey(operation->getSubExpr()));
        return true;
    }
    bool VisitCallExpr(clang::CallExpr *call)
    {
        const auto changed = OwnMutations(*call);
        keys.insert(changed.begin(), changed.end());
        return true;
    }
};

void InvalidateChanges(const clang::Stmt *statement, Facts& facts)
{
    MutationFinder mutations;
    mutations.TraverseStmt(const_cast<clang::Stmt *>(statement));
    for (const auto& key : mutations.keys)
        Kill(facts, key);
}

bool HasUnsequencedOperands(const clang::BinaryOperator& operation)
{
    return !operation.isAssignmentOp() && operation.getOpcode() != clang::BO_Comma &&
           operation.getOpcode() != clang::BO_Shl && operation.getOpcode() != clang::BO_Shr;
}

// Structured proof deliberately stops at non-local control flow. A jump can
// bypass a fatal assertion, and handlers can join paths that never reached it.
class NonlocalFlowFinder : public clang::RecursiveASTVisitor<NonlocalFlowFinder>
{
  public:
    bool found = false;
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool VisitGotoStmt(clang::GotoStmt *statement)
    {
        found |= !statement->getBeginLoc().isMacroID();
        return true;
    }
    bool VisitIndirectGotoStmt(clang::IndirectGotoStmt *statement)
    {
        found |= !statement->getBeginLoc().isMacroID();
        return true;
    }
    bool VisitCXXTryStmt(clang::CXXTryStmt *statement)
    {
        found |= !statement->getBeginLoc().isMacroID();
        return true;
    }
};

class ContinueFinder : public clang::RecursiveASTVisitor<ContinueFinder>
{
  public:
    bool found = false;
    bool VisitContinueStmt(clang::ContinueStmt *)
    {
        found = true;
        return true;
    }
    bool TraverseLambdaExpr(clang::LambdaExpr *)
    {
        return true;
    }
    bool TraverseForStmt(clang::ForStmt *)
    {
        return true;
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *)
    {
        return true;
    }
    bool TraverseWhileStmt(clang::WhileStmt *)
    {
        return true;
    }
    bool TraverseDoStmt(clang::DoStmt *)
    {
        return true;
    }
};

Facts MergeContinues(const clang::Stmt *body, Facts fallthrough, Facts entry)
{
    ContinueFinder continues;
    continues.TraverseStmt(const_cast<clang::Stmt *>(body));
    if (!continues.found)
        return fallthrough;
    // Every continue reaches the back edge, including paths that bypass
    // body-local fatal assertions. Retain only mutation-free entry proof.
    InvalidateChanges(body, entry);
    return Intersection(fallthrough, entry);
}

struct Flow
{
    Facts facts;
    bool exits = false;
};

class FunctionAnalysis
{
  public:
    using Report = std::function<void(const clang::Expr&, ResultState)>;
    FunctionAnalysis(clang::ASTContext& context, Report report) : context_(context), report_(std::move(report))
    {
    }

    void AnalyzeFunction(const clang::Stmt *body)
    {
        NonlocalFlowFinder flow;
        flow.TraverseStmt(const_cast<clang::Stmt *>(body));
        if (flow.found)
        {
            Facts unknown;
            Scan(body, unknown);
            return;
        }
        Analyze(body, {});
    }

    Flow Analyze(const clang::Stmt *statement, Facts facts)
    {
        if (!statement)
            return {std::move(facts), false};
        const auto macro =
            AssertionMacro(statement->getBeginLoc(), context_.getSourceManager(), context_.getLangOpts());
        if (macro == "ASSERT_THAT" || macro == "EXPECT_THAT" || macro == "ASSERT_TRUE" || macro == "ASSERT_FALSE")
            if (auto assertion = AnalyzeAssertion(statement, macro, facts))
                return *assertion;
        if (const auto *block = llvm::dyn_cast<clang::CompoundStmt>(statement))
            return AnalyzeBlock(*block, std::move(facts));
        if (const auto *selection = llvm::dyn_cast<clang::SwitchStmt>(statement))
            return AnalyzeSelection(*selection, std::move(facts));
        if (const auto *branch = llvm::dyn_cast<clang::IfStmt>(statement))
            return AnalyzeBranch(*branch, std::move(facts));
        if (const auto *loop = llvm::dyn_cast<clang::ForStmt>(statement))
        {
            facts = Analyze(loop->getInit(), std::move(facts)).facts;
            return AnalyzeLoop(loop->getBody(), loop->getCond(), loop->getInc(), std::move(facts));
        }
        if (const auto *loop = llvm::dyn_cast<clang::CXXForRangeStmt>(statement))
        {
            Scan(loop->getRangeStmt(), facts);
            return AnalyzeLoop(loop->getBody(), nullptr, nullptr, std::move(facts));
        }
        if (const auto *loop = llvm::dyn_cast<clang::WhileStmt>(statement))
            return AnalyzeLoop(loop->getBody(), loop->getCond(), nullptr, std::move(facts));
        if (const auto *loop = llvm::dyn_cast<clang::DoStmt>(statement))
            return AnalyzeLoop(loop->getBody(), loop->getCond(), nullptr, std::move(facts), false);
        Scan(statement, facts);
        return {std::move(facts),
                llvm::isa<clang::ReturnStmt, clang::BreakStmt, clang::ContinueStmt, clang::CXXThrowExpr>(statement)};
    }

  private:
    clang::ASTContext& context_;
    Report report_;

    void Require(const clang::Expr& object, const clang::Expr& access, ResultState needed, const Facts& facts)
    {
        if (!IsResultType(object.getType()))
            return;
        const auto key = ResultKey(&object);
        if (key.empty())
            return; // Indexes and aliases without a stable identity remain review responsibilities.
        const auto found = facts.find(key);
        if (found == facts.end() || found->second != needed)
            report_(access, needed);
    }

    void Scan(const clang::Stmt *statement, Facts& facts)
    {
        if (!statement || llvm::isa<clang::LambdaExpr>(statement))
            return;
        if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(statement); binary && binary->isLogicalOp())
        {
            Scan(binary->getLHS(), facts);
            auto right = facts;
            ApplyCondition(binary->getLHS(), binary->getOpcode() == clang::BO_LAnd, right);
            Scan(binary->getRHS(), right);
            facts = Intersection(facts, right);
            return;
        }
        if (const auto *choice = llvm::dyn_cast<clang::ConditionalOperator>(statement))
        {
            Scan(choice->getCond(), facts);
            auto yes = facts;
            auto no = facts;
            ApplyCondition(choice->getCond(), true, yes);
            ApplyCondition(choice->getCond(), false, no);
            Scan(choice->getTrueExpr(), yes);
            Scan(choice->getFalseExpr(), no);
            facts = Intersection(yes, no);
            return;
        }
        if (ScanCall(statement, facts))
            return;
        if (const auto *binary = llvm::dyn_cast<clang::BinaryOperator>(statement);
            binary && HasUnsequencedOperands(*binary))
        {
            MutationFinder mutations;
            mutations.TraverseStmt(const_cast<clang::Stmt *>(statement));
            for (const auto& key : mutations.keys)
                Kill(facts, key);
        }
        for (const auto *child : statement->children())
            Scan(child, facts);
        ObserveAccess(statement, facts);
        InvalidateMutation(statement, facts);
    }

    bool ScanCall(const clang::Stmt *statement, Facts& facts)
    {
        const auto *call = llvm::dyn_cast<clang::CallExpr>(statement);
        const auto *operation = llvm::dyn_cast<clang::CXXOperatorCallExpr>(statement);
        if (!call || (operation && operation->getOperator() != clang::OO_Call))
            return false;
        // C++17 sequences the callee/object before the arguments, whose order
        // remains unspecified. An argument mutation invalidates sibling proof.
        Scan(call->getCallee(), facts);
        const unsigned first = operation ? 1U : 0U;
        if (first)
            Scan(call->getArg(0), facts);
        MutationFinder mutations;
        for (unsigned index = first; index < call->getNumArgs(); ++index)
            mutations.TraverseStmt(const_cast<clang::Expr *>(call->getArg(index)));
        for (const auto& key : mutations.keys)
            Kill(facts, key);
        for (unsigned index = first; index < call->getNumArgs(); ++index)
            Scan(call->getArg(index), facts);
        ObserveAccess(statement, facts);
        InvalidateMutation(statement, facts);
        return true;
    }

    void ObserveAccess(const clang::Stmt *statement, const Facts& facts)
    {
        if (const auto *operation = llvm::dyn_cast<clang::CXXOperatorCallExpr>(statement);
            operation && operation->getNumArgs())
            if (operation->getOperator() == clang::OO_Star || operation->getOperator() == clang::OO_Arrow)
                Require(*operation->getArg(0), *operation, ResultState::kSuccess, facts);
        if (const auto *call = llvm::dyn_cast<clang::CXXMemberCallExpr>(statement))
            if (const auto *method = call->getMethodDecl(); method && method->getNameAsString() == "error")
                Require(*call->getImplicitObjectArgument(), *call, ResultState::kError, facts);
    }
    void InvalidateMutation(const clang::Stmt *statement, Facts& facts)
    {
        if (const auto *operation = llvm::dyn_cast<clang::BinaryOperator>(statement);
            operation && operation->isAssignmentOp())
            Kill(facts, ResultKey(operation->getLHS()));
        if (const auto *call = llvm::dyn_cast<clang::CallExpr>(statement))
            for (const auto& key : OwnMutations(*call))
                Kill(facts, key);
    }

    std::optional<Flow> AnalyzeAssertion(const clang::Stmt *statement, const std::string& macro, Facts& facts)
    {
        AssertionFinder finder(context_);
        finder.TraverseStmt(const_cast<clang::Stmt *>(statement));
        if (finder.assertion)
        {
            Scan(finder.assertion->actual, facts);
            Scan(finder.assertion->matcher, facts);
            const auto key = ResultKey(finder.assertion->actual);
            const auto state = MatcherState(finder.assertion->matcher);
            if (finder.assertion->fatal && IsResultType(finder.assertion->actual->getType()) && !key.empty() &&
                state != ResultState::kUnknown)
                facts[key] = state;
            return Flow{facts, false};
        }
        if (finder.boolean && (macro == "ASSERT_TRUE" || macro == "ASSERT_FALSE"))
        {
            Scan(finder.boolean, facts);
            ApplyCondition(finder.boolean, macro == "ASSERT_TRUE", facts);
            return Flow{facts, false};
        }
        return std::nullopt;
    }

    Flow AnalyzeBlock(const clang::CompoundStmt& block, Facts facts)
    {
        Flow flow{std::move(facts), false};
        for (const auto *child : block.body())
        {
            flow = Analyze(child, std::move(flow.facts));
            if (flow.exits)
                break;
        }
        return flow;
    }

    Flow AnalyzeSelection(const clang::SwitchStmt& selection, Facts facts)
    {
        Scan(selection.getInit(), facts);
        Scan(selection.getConditionVariableDeclStmt(), facts);
        Scan(selection.getCond(), facts);
        MutationFinder mutations;
        mutations.TraverseStmt(const_cast<clang::Stmt *>(selection.getBody()));
        for (const auto& key : mutations.keys)
            Kill(facts, key);
        auto body_facts = facts;
        Scan(selection.getBody(), body_facts);
        return {std::move(facts), false};
    }

    Flow AnalyzeBranch(const clang::IfStmt& branch, Facts facts)
    {
        facts = Analyze(branch.getInit(), std::move(facts)).facts;
        Scan(branch.getConditionVariableDeclStmt(), facts);
        Scan(branch.getCond(), facts);
        if (const auto truth = ConditionTruth(branch.getCond(), facts))
            return Analyze(*truth ? branch.getThen() : branch.getElse(), std::move(facts));
        auto yes = facts;
        auto no = facts;
        ApplyCondition(branch.getCond(), true, yes);
        ApplyCondition(branch.getCond(), false, no);
        auto taken = Analyze(branch.getThen(), std::move(yes));
        auto other = Analyze(branch.getElse(), std::move(no));
        if (taken.exits)
            return other.exits ? Flow{{}, true} : other;
        if (other.exits)
            return taken;
        return {Intersection(taken.facts, other.facts), false};
    }

    Flow AnalyzeLoop(const clang::Stmt *body, const clang::Expr *condition, const clang::Expr *increment, Facts facts,
                     bool condition_first = true)
    {
        MutationFinder mutations;
        mutations.TraverseStmt(const_cast<clang::Stmt *>(body));
        mutations.TraverseStmt(const_cast<clang::Expr *>(increment));
        mutations.TraverseStmt(const_cast<clang::Expr *>(condition));
        for (const auto& key : mutations.keys)
            Kill(facts, key);
        if (condition_first)
            Scan(condition, facts);
        auto iteration = facts;
        if (condition_first)
            ApplyCondition(condition, true, iteration);
        const auto entry = iteration;
        auto flow = Analyze(body, std::move(iteration));
        flow.facts = MergeContinues(body, std::move(flow.facts), entry);
        Scan(increment, flow.facts);
        if (!condition_first)
            Scan(condition, flow.facts);
        return {std::move(facts), false};
    }
};
} // namespace

ResultAccessCheck::ResultAccessCheck(llvm::StringRef name, clang::tidy::ClangTidyContext *context)
    : ClangTidyCheck(name, context)
{
}

void ResultAccessCheck::registerMatchers(clang::ast_matchers::MatchFinder *finder)
{
    using namespace clang::ast_matchers;
    finder->addMatcher(functionDecl(isDefinition(), unless(isImplicit())).bind("function"), this);
}

void ResultAccessCheck::check(const clang::ast_matchers::MatchFinder::MatchResult& result)
{
    const auto *function = result.Nodes.getNodeAs<clang::FunctionDecl>("function");
    if (!function || !IsTestFunction(*function, *result.SourceManager))
        return;
    if (llvm::StringRef(
            result.SourceManager->getFilename(result.SourceManager->getExpansionLoc(function->getBeginLoc())))
            .ends_with("src/backend/ports/result_test.cpp"))
        return;
    FunctionAnalysis analysis(
        *result.Context,
        [&](const clang::Expr& access, ResultState state)
        {
            diag(access.getExprLoc(),
                 state == ResultState::kSuccess
                     ? "prove Result success with ASSERT_THAT(IsOk()) or a terminating guard before accessing its value"
                     : "prove Result failure with ASSERT_THAT(IsErr()) or a terminating guard before accessing its "
                       "error");
        });
    analysis.AnalyzeFunction(function->getBody());
}

} // namespace fastecu::tidy
