#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/platform/desktop/common/testing/event_helpers.h"

#include "src/ui/desktop/hexedit/qhexedit/qhexedit.h"

// Layout divides by the glyph metrics. A runner without installed fonts (the
// Windows offscreen CI job) reports zero-pixel glyphs, which would raise an
// integer divide-by-zero here.
TEST(QHexEditTest, showingAndResizingLaysOutWithoutCrashing)
{
    QHexEdit edit;
    edit.setData(QByteArray(64, '\x0a'));
    edit.resize(300, 200);
    edit.show();
    fastecu::testing::process_events_for(std::chrono::milliseconds(10));

    ASSERT_EQ(edit.data().size(), qsizetype{64});
}
namespace
{
const auto *const environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
