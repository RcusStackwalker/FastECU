#include "TemplateTypenameCheck.h"

#include "clang-tidy/ClangTidyModule.h"

namespace fastecu::tidy
{

class FastecuModule : public clang::tidy::ClangTidyModule
{
  public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& factories) override
    {
        factories.registerCheck<TemplateTypenameCheck>("fastecu-template-typename");
    }
};

} // namespace fastecu::tidy

static clang::tidy::ClangTidyModuleRegistry::Add<fastecu::tidy::FastecuModule>
    registration("fastecu-module", "Repository conventions that need the AST.");
