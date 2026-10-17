// Reports template type parameters declared with `class` and rewrites them to
// `typename`. Query-based custom checks cannot see the keyword, hence this plugin.
#pragma once

#include "clang-tidy/ClangTidyCheck.h"

namespace fastecu::tidy
{

class TemplateTypenameCheck : public clang::tidy::ClangTidyCheck
{
  public:
    using ClangTidyCheck::ClangTidyCheck;
    void registerMatchers(clang::ast_matchers::MatchFinder *finder) override;
    void check(const clang::ast_matchers::MatchFinder::MatchResult& result) override;
};

} // namespace fastecu::tidy
