#include "TemplateTypenameCheck.h"

#include "clang/AST/DeclTemplate.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"

namespace fastecu::tidy
{

using namespace clang;
using namespace clang::ast_matchers;

void TemplateTypenameCheck::registerMatchers(MatchFinder *finder)
{
    finder->addMatcher(templateTypeParmDecl(unless(isImplicit())).bind("parameter"), this);
}

void TemplateTypenameCheck::check(const MatchFinder::MatchResult& result)
{
    const auto *parameter = result.Nodes.getNodeAs<TemplateTypeParmDecl>("parameter");
    // A constrained parameter (`template <std::integral T>`) has no keyword.
    if (parameter->wasDeclaredWithTypename() || parameter->hasTypeConstraint())
    {
        return;
    }
    // Compiler-synthesized parameters (implicit deduction guides copy the
    // class template's) have no source location.
    const SourceLocation keyword = parameter->getBeginLoc();
    if (keyword.isInvalid())
    {
        return;
    }
    auto report = diag(keyword, "declare this template type parameter with typename instead of class");
    if (keyword.isMacroID())
    {
        return;
    }
    const auto range = CharSourceRange::getTokenRange(keyword, keyword);
    if (Lexer::getSourceText(range, *result.SourceManager, getLangOpts()) == "class")
    {
        report << FixItHint::CreateReplacement(range, "typename");
    }
}

} // namespace fastecu::tidy
