#include "ParameterizedCaseNameCheck.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
namespace fastecu::tidy
{
namespace
{
class InstantiationCallbacks : public clang::PPCallbacks
{
  public:
    explicit InstantiationCallbacks(ParameterizedCaseNameCheck& check) : check_(check)
    {
    }
    void MacroExpands(const clang::Token& token, const clang::MacroDefinition&, clang::SourceRange range,
                      const clang::MacroArgs *arguments) override
    {
        if (!token.getIdentifierInfo() || token.getIdentifierInfo()->getName() != "INSTANTIATE_TEST_SUITE_P" ||
            !arguments)
            return;
        // GoogleTest has two fixed parameters; the value generator and
        // optional name generator share its variadic argument.
        bool named = false;
        if (arguments->getNumMacroArguments() >= 3)
        {
            const auto *tokens = arguments->getUnexpArgument(2);
            unsigned depth = 0;
            for (unsigned index = 0; !tokens[index].is(clang::tok::eof); ++index)
            {
                if (tokens[index].is(clang::tok::l_paren))
                    ++depth;
                else if (tokens[index].is(clang::tok::r_paren) && depth)
                    --depth;
                else if (tokens[index].is(clang::tok::comma) && depth == 0)
                    named = !tokens[index + 1].is(clang::tok::eof);
            }
        }
        if (!named)
            check_.diag(
                range.getBegin(),
                "give parameterized cases an explicit name generator as the fourth INSTANTIATE_TEST_SUITE_P argument");
    }

  private:
    ParameterizedCaseNameCheck& check_;
};
} // namespace
void ParameterizedCaseNameCheck::registerPPCallbacks(const clang::SourceManager&, clang::Preprocessor *preprocessor,
                                                     clang::Preprocessor *)
{
    preprocessor->addPPCallbacks(std::make_unique<InstantiationCallbacks>(*this));
}
} // namespace fastecu::tidy
