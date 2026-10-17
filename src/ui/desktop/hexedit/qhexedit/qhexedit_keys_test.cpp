#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QKeySequence>
#include <string>
#include <variant>
#include <vector>

#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/ui/desktop/hexedit/qhexedit/qhexedit.h"

// Keyboard behavior of QHexEdit: each step sends one key and records the
// cursor (in nibbles), the selected bytes, the data and the overwrite mode.
namespace
{
using ::testing::ElementsAre;
using Key = std::variant<QKeySequence::StandardKey, Qt::Key, QString>;

class QHexEditKeysTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        edit_.setData(QByteArray::fromHex("00112233445566778899aabbccddeeff"
                                          "0123456789abcdef0123456789abcdef"));
        edit_.resize(600, 300);
    }

    void press(const Key& key)
    {
        QKeyCombination combination(Qt::Key_unknown);
        QString text;
        if (const auto *standard = std::get_if<QKeySequence::StandardKey>(&key))
        {
            combination = QKeySequence::keyBindings(*standard).first()[0];
        }
        else if (const auto *plain = std::get_if<Qt::Key>(&key))
        {
            combination = QKeyCombination(*plain);
        }
        else
        {
            text = std::get<QString>(key);
            combination = QKeyCombination(Qt::Key_A);
        }
        QKeyEvent event(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(), text);
        QApplication::sendEvent(&edit_, &event);
    }

    std::vector<std::string> trace(const std::vector<Key>& keys)
    {
        std::vector<std::string> steps;
        for (const auto& key : keys)
        {
            press(key);
            steps.push_back(std::to_string(edit_.cursorPosition()) + " [" + edit_.selectedData().toStdString() + "] " +
                            edit_.data().toHex().toStdString() + "/" + std::to_string(edit_.data().size()) +
                            (edit_.overwriteMode() ? " ovr" : " ins"));
        }
        return steps;
    }

    QHexEdit edit_;
};

using SK = QKeySequence::StandardKey;

TEST_F(QHexEditKeysTest, movesTheCursor)
{
    EXPECT_THAT(trace({SK::MoveToNextChar, SK::MoveToNextLine, SK::MoveToEndOfLine, SK::MoveToStartOfLine,
                       SK::MoveToPreviousLine, SK::MoveToPreviousChar, SK::MoveToNextPage, SK::MoveToPreviousPage,
                       SK::MoveToEndOfDocument, SK::MoveToStartOfDocument}),
                ElementsAre("1 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "33 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "32 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr"));
}

TEST_F(QHexEditKeysTest, extendsTheSelection)
{
    EXPECT_THAT(trace({SK::SelectNextChar, SK::SelectNextChar, SK::SelectNextLine, SK::SelectPreviousChar,
                       SK::SelectEndOfLine, SK::SelectStartOfLine, SK::SelectPreviousLine, SK::SelectNextPage,
                       SK::SelectPreviousPage, SK::SelectEndOfDocument, SK::SelectStartOfDocument, SK::SelectAll}),
                ElementsAre("1 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "34 [00112233445566778899aabbccddeeff01] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "33 [00112233445566778899aabbccddeeff] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [00112233445566778899aabbccddeeff0123456789abcdef0123456789abcd] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "32 [00112233445566778899aabbccddeeff] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "63 [00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef] "
                            "00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr"));
}

TEST_F(QHexEditKeysTest, typesHexNibblesInOverwriteMode)
{
    EXPECT_THAT(trace({QString("a"), QString("B"), QString("g"), QString("7"), SK::Undo, SK::Undo, SK::Redo}),
                ElementsAre("1 [] a0112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [] ab112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [] ab112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "3 [] ab712233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "1 [] ab112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] ab112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr"));
}

TEST_F(QHexEditKeysTest, typesHexNibblesInInsertMode)
{
    EXPECT_THAT(trace({Qt::Key_Insert, QString("c"), QString("d"), SK::SelectNextChar, SK::SelectNextChar, QString("e"),
                       Qt::Key_Insert}),
                ElementsAre("0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ins",
                            "1 [] c000112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ins",
                            "2 [] cd00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ins",
                            "3 [] cd00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ins",
                            "4 [00] cd00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ins",
                            "3 [] cde0112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ins",
                            "3 [] cde0112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/33 ovr"));
}

TEST_F(QHexEditKeysTest, deletesAndBackspacesInBothModes)
{
    EXPECT_THAT(trace({SK::MoveToNextChar, SK::MoveToNextChar, SK::Delete, Qt::Key_Backspace, SK::SelectNextChar,
                       SK::SelectNextChar, SK::SelectNextChar, SK::SelectNextChar, SK::Delete, Qt::Key_Insert,
                       SK::MoveToNextChar, SK::MoveToNextChar, SK::Delete, Qt::Key_Backspace, SK::SelectNextChar,
                       SK::SelectNextChar, Qt::Key_Backspace, SK::MoveToEndOfDocument, Qt::Key_Backspace}),
                ElementsAre("1 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "0 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "1 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "2 [00] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "3 [00] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "4 [0000] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "4 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "4 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ins",
                            "5 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ins",
                            "6 [] 00002233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ins",
                            "6 [] 000022445566778899aabbccddeeff0123456789abcdef0123456789abcdef/31 ins",
                            "4 [] 0000445566778899aabbccddeeff0123456789abcdef0123456789abcdef/30 ins",
                            "5 [] 0000445566778899aabbccddeeff0123456789abcdef0123456789abcdef/30 ins",
                            "6 [44] 0000445566778899aabbccddeeff0123456789abcdef0123456789abcdef/30 ins",
                            "4 [] 00005566778899aabbccddeeff0123456789abcdef0123456789abcdef/29 ins",
                            "58 [] 00005566778899aabbccddeeff0123456789abcdef0123456789abcdef/29 ins",
                            "56 [] 00005566778899aabbccddeeff0123456789abcdef0123456789abcd/28 ins"));
}

TEST_F(QHexEditKeysTest, cutsCopiesAndPastesThroughTheClipboard)
{
    std::vector<std::string> steps = trace({SK::SelectNextChar, SK::SelectNextChar, SK::Copy});
    steps.push_back(QApplication::clipboard()->text().toStdString());
    for (const auto& step : trace({SK::MoveToEndOfLine, SK::Paste, SK::SelectNextChar, SK::SelectNextChar, SK::Cut,
                                   Qt::Key_Insert, SK::Paste, SK::SelectNextChar, SK::SelectNextChar, SK::Cut}))
    {
        steps.push_back(step);
    }
    EXPECT_THAT(
        steps,
        ElementsAre("1 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr", "00",
                    "31 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "33 [] 00112233445566778899aabbccddee000123456789abcdef0123456789abcdef/32 ovr",
                    "34 [778899aabbccddee0001] 00112233445566778899aabbccddee000123456789abcdef0123456789abcdef/32 ovr",
                    "35 [778899aabbccddee0001] 00112233445566778899aabbccddee000123456789abcdef0123456789abcdef/32 ovr",
                    "14 [] 001122334455660000000000000000000023456789abcdef0123456789abcdef/32 ovr",
                    "14 [] 001122334455660000000000000000000023456789abcdef0123456789abcdef/32 ins",
                    "34 [] 00112233445566778899aabbccddee00010000000000000000000023456789abcdef0123456789abcdef/42 ins",
                    "35 [33445566778899aabbccddee0001] "
                    "00112233445566778899aabbccddee00010000000000000000000023456789abcdef0123456789abcdef/42 ins",
                    "36 [33445566778899aabbccddee000100] "
                    "00112233445566778899aabbccddee00010000000000000000000023456789abcdef0123456789abcdef/42 ins",
                    "6 [] 00112200000000000000000023456789abcdef0123456789abcdef/27 ins"));
}

// Tab and Backtab cycle the edit area; the cursor step shows which is active.
TEST_F(QHexEditKeysTest, tabSwitchesTheEditArea)
{
    EXPECT_THAT(trace({Qt::Key_Tab, SK::MoveToNextChar, Qt::Key_Tab, SK::MoveToNextChar, QString("Z"), Qt::Key_Backtab,
                       SK::MoveToNextChar, Qt::Key_Tab, Qt::Key_Backtab, SK::MoveToNextChar}),
                ElementsAre("0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "3 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "3 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "6 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "6 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "6 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "7 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "6 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "6 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                            "7 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr"));
}

TEST_F(QHexEditKeysTest, readOnlyIgnoresEditsButStillCopies)
{
    edit_.setReadOnly(true);
    EXPECT_THAT(
        trace({QString("a"), SK::Delete, Qt::Key_Backspace, SK::SelectNextChar, SK::SelectNextChar, SK::Cut, SK::Copy}),
        ElementsAre("0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "0 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "1 [] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr",
                    "2 [00] 00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef/32 ovr"));
    EXPECT_EQ(QApplication::clipboard()->text(), QString("00"));
}

TEST_F(QHexEditKeysTest, copyWrapsHexAtSixteenBytesPerLine)
{
    trace({SK::SelectAll, SK::Copy});
    EXPECT_EQ(QApplication::clipboard()->text(),
              QString("00112233445566778899aabbccddeeff\n0123456789abcdef0123456789abcdef"));
}

const auto *const kEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));

} // namespace
