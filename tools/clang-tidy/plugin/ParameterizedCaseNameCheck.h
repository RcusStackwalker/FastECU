#pragma once
#include "clang-tidy/ClangTidyCheck.h"
namespace fastecu::tidy
{
class ParameterizedCaseNameCheck : public clang::tidy::ClangTidyCheck
{
  public:
    using ClangTidyCheck::ClangTidyCheck;
    void registerPPCallbacks(const clang::SourceManager& source, clang::Preprocessor *preprocessor,
                             clang::Preprocessor *module_expander) override;
};
} // namespace fastecu::tidy
