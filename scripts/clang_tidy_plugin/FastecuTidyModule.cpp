#include "RedundantAddressDereferenceCheck.h"
#include "ResultAssertionCheck.h"
#include "ContainerAssertionCheck.h"
#include "MessageConcatenationCheck.h"
#include "LinearSearchLoopCheck.h"
#include "AnyAllIndexLoopCheck.h"
#include "CountIfLoopCheck.h"
#include "AccumulateLoopCheck.h"
#include "TransformCollectLoopCheck.h"
#include "TemplateTypenameCheck.h"

#include "clang-tidy/ClangTidyModule.h"

namespace fastecu::tidy
{

class FastecuModule : public clang::tidy::ClangTidyModule
{
  public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& factories) override
    {
        factories.registerCheck<RedundantAddressDereferenceCheck>("fastecu-redundant-address-dereference");
        factories.registerCheck<ResultAssertionCheck>("fastecu-result-assertion");
        factories.registerCheck<ContainerAssertionCheck>("fastecu-container-assertion");
        factories.registerCheck<MessageConcatenationCheck>("fastecu-message-concatenation");
        factories.registerCheck<LinearSearchLoopCheck>("fastecu-linear-search-loop");
        factories.registerCheck<AnyAllIndexLoopCheck>("fastecu-any-all-index-loop");
        factories.registerCheck<CountIfLoopCheck>("fastecu-count-if-loop");
        factories.registerCheck<AccumulateLoopCheck>("fastecu-accumulate-loop");
        factories.registerCheck<TransformCollectLoopCheck>("fastecu-transform-collect-loop");
        factories.registerCheck<TemplateTypenameCheck>("fastecu-template-typename");
    }
};

} // namespace fastecu::tidy

static clang::tidy::ClangTidyModuleRegistry::Add<fastecu::tidy::FastecuModule>
    g_registration("fastecu-module", "Repository conventions that need the AST.");
