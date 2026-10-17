#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

#include "qhexedit.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

// ********************************************************************** Constructor, destructor

/*
static auto const bars = QStringLiteral(
    " ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁"
    "▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂▂"
    "▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃"
    "▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄"
    "▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅▅"
    "▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆▆"
    "▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇▇"
    "████████████████████████████████");

static inline QChar decode_bars(char ch)
{
    return bars[uchar(ch)];
}
*/

QHexEdit::QHexEdit(QWidget *parent)
    : QAbstractScrollArea(parent), address_area_(true), address_width_(4), ascii_area_(true), bar_area_(true),
      bytes_per_line_(16), hex_chars_in_line_(47), highlighting_(true), overwrite_mode_(true), read_only_(false),
      hex_caps_(false), dynamic_bytes_per_line_(false), edit_area_is_ascii_(false), edit_area_is_bar_(false),
      chunks_(new Chunks(this)), cursor_position_(0), last_event_size_(0), undo_stack_(new UndoStack(chunks_, this))
{
#ifdef Q_OS_WIN32
    setMonospaceFont(QFont("Courier", 10));
#else
    setMonospaceFont(QFont("Monospace", 10));
#endif
    setAddressAreaColor(this->palette().alternateBase().color());
    setHighlightingColor(QColor(0xff, 0xff, 0x99, 0xff));
    setSelectionColor(this->palette().highlight().color());
    setAddressFontColor(QPalette::WindowText);
    setAsciiAreaColor(this->palette().alternateBase().color());
    setAsciiFontColor(QPalette::WindowText);
    setBarAreaColor(this->palette().alternateBase().color());
    setBarFontColor(QPalette::WindowText);

    connect(&cursor_timer_, SIGNAL(timeout()), this, SLOT(updateCursor()));
    connect(verticalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(adjust()));
    connect(horizontalScrollBar(), SIGNAL(valueChanged(int)), this, SLOT(adjust()));
    connect(undo_stack_, SIGNAL(indexChanged(int)), this, SLOT(dataChangedPrivate(int)));

    cursor_timer_.setInterval(500);
    cursor_timer_.start();

    setAddressWidth(4);
    setAddressArea(true);
    setAsciiArea(true);
    setBarArea(true);
    setOverwriteMode(true);
    setHighlighting(true);
    setReadOnly(false);

    init();
}

QHexEdit::~QHexEdit()
{
}

// ********************************************************************** Properties

void QHexEdit::setAddressArea(bool addressArea)
{
    address_area_ = addressArea;
    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

bool QHexEdit::addressArea()
{
    return address_area_;
}

void QHexEdit::setAddressAreaColor(const QColor& color)
{
    address_area_color_ = color;
    viewport()->update();
}

QColor QHexEdit::addressAreaColor()
{
    return address_area_color_;
}

void QHexEdit::setAddressFontColor(const QColor& color)
{
    address_font_color_ = color;
    viewport()->update();
}

QColor QHexEdit::addressFontColor()
{
    return address_font_color_;
}

void QHexEdit::setAsciiAreaColor(const QColor& color)
{
    ascii_area_color_ = color;
    viewport()->update();
}

QColor QHexEdit::asciiAreaColor()
{
    return ascii_area_color_;
}

void QHexEdit::setAsciiFontColor(const QColor& color)
{
    ascii_font_color_ = color;
    viewport()->update();
}

QColor QHexEdit::asciiFontColor()
{
    return ascii_font_color_;
}

void QHexEdit::setBarAreaColor(const QColor& color)
{
    bar_area_color_ = color;
    viewport()->update();
}

QColor QHexEdit::barAreaColor()
{
    return bar_area_color_;
}

void QHexEdit::setBarFontColor(const QColor& color)
{
    bar_font_color_ = color;
    viewport()->update();
}

QColor QHexEdit::barFontColor()
{
    return bar_font_color_;
}

void QHexEdit::setHexFontColor(const QColor& color)
{
    hex_font_color_ = color;
    viewport()->update();
}

QColor QHexEdit::hexFontColor()
{
    return hex_font_color_;
}

void QHexEdit::setAddressOffset(qint64 addressOffset)
{
    address_offset_ = addressOffset;
    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

qint64 QHexEdit::addressOffset()
{
    return address_offset_;
}

void QHexEdit::setAddressWidth(int addressWidth)
{
    address_width_ = addressWidth;
    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

int QHexEdit::addressWidth()
{
    qint64 size = chunks_->size();
    int n = 1;
    if (size > Q_INT64_C(0x100000000))
    {
        n += 8;
        size /= Q_INT64_C(0x100000000);
    }
    if (size > 0x10000)
    {
        n += 4;
        size /= 0x10000;
    }
    if (size > 0x100)
    {
        n += 2;
        size /= 0x100;
    }
    if (size > 0x10)
    {
        n += 1;
    }

    if (n > address_width_)
    {
        return n;
    }
    else
    {
        return address_width_;
    }
}

void QHexEdit::setAsciiArea(bool asciiArea)
{
    if (!asciiArea)
    {
        edit_area_is_ascii_ = false;
    }
    ascii_area_ = asciiArea;
    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

bool QHexEdit::asciiArea()
{
    return ascii_area_;
}

void QHexEdit::setBarArea(bool barArea)
{
    if (!barArea)
    {
        edit_area_is_bar_ = false;
    }
    bar_area_ = barArea;
    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

bool QHexEdit::barArea()
{
    return bar_area_;
}

void QHexEdit::setBytesPerLine(int count)
{
    // Layout divides by the line length.
    bytes_per_line_ = std::max(count, 1);
    hex_chars_in_line_ = bytes_per_line_ * 3 - 1;

    adjust();
    setCursorPosition(cursor_position_);
    viewport()->update();
}

int QHexEdit::bytesPerLine()
{
    return bytes_per_line_;
}

void QHexEdit::setCursorPosition(qint64 position)
{
    // 1. delete old cursor
    blink_ = false;
    viewport()->update(cursor_rect_);

    // 2. Check, if cursor in range?
    if (position > (chunks_->size() * 2 - 1))
    {
        position = chunks_->size() * 2 - (overwrite_mode_ ? 1 : 0);
    }

    if (position < 0)
    {
        position = 0;
    }

    // 3. Calc new position of cursor
    b_pos_current_ = position / 2;
    px_cursor_y_ = static_cast<int>(((position / 2 - b_pos_first_) / bytes_per_line_ + 1) * px_char_height_);
    int x = static_cast<int>(position % static_cast<qint64>(2 * bytes_per_line_));
    if (edit_area_is_bar_)
    {
        px_cursor_x_ = x / 2 * px_char_width_ + px_pos_bar_x_;
        cursor_position_ = position - position % 2;
    }
    if (edit_area_is_ascii_)
    {
        px_cursor_x_ = x / 2 * px_char_width_ + px_pos_ascii_x_;
        cursor_position_ = position - position % 2;
    }
    else
    {
        px_cursor_x_ = (((x / 2) * 3) + (x % 2)) * px_char_width_ + px_pos_hex_x_;
        cursor_position_ = position;
    }

    if (overwrite_mode_)
    {
        cursor_rect_ = QRect(px_cursor_x_ - horizontalScrollBar()->value(), px_cursor_y_ + px_cursor_width_,
                             px_char_width_, px_cursor_width_);
    }
    else
    {
        cursor_rect_ = QRect(px_cursor_x_ - horizontalScrollBar()->value(), px_cursor_y_ - px_char_height_ + 4,
                             px_cursor_width_, px_char_height_);
    }

    // 4. Immediately draw new cursor
    blink_ = true;
    viewport()->update(cursor_rect_);
    emit currentAddressChanged(b_pos_current_);
}

qint64 QHexEdit::cursorPosition(QPoint pos)
{
    // Calc cursor position depending on a graphical position
    qint64 result = -1;
    int posX = pos.x() + horizontalScrollBar()->value();
    int posY = pos.y() - 3;
    if ((posX >= px_pos_hex_x_) && (posX < (px_pos_hex_x_ + (1 + hex_chars_in_line_) * px_char_width_)))
    {
        edit_area_is_ascii_ = false;
        edit_area_is_bar_ = false;
        int x = (posX - px_pos_hex_x_) / px_char_width_;
        x = (x / 3) * 2 + x % 3;
        int y = (posY / px_char_height_) * 2 * bytes_per_line_;
        result = b_pos_first_ * 2 + x + y;
    }
    else if (ascii_area_ && (posX >= px_pos_ascii_x_) &&
             (posX < (px_pos_ascii_x_ + (1 + bytes_per_line_) * px_char_width_)))
    {
        edit_area_is_ascii_ = true;
        int x = 2 * (posX - px_pos_ascii_x_) / px_char_width_;
        int y = (posY / px_char_height_) * 2 * bytes_per_line_;
        result = b_pos_first_ * 2 + x + y;
    }
    else if (bar_area_ && (posX >= px_pos_bar_x_) && (posX < (px_pos_bar_x_ + (1 + bytes_per_line_) * px_char_width_)))
    {
        edit_area_is_bar_ = true;
        int x = 2 * (posX - px_pos_bar_x_) / px_char_width_;
        int y = (posY / px_char_height_) * 2 * bytes_per_line_;
        result = b_pos_first_ * 2 + x + y;
    }
    return result;
}

qint64 QHexEdit::cursorPosition()
{
    return cursor_position_;
}

void QHexEdit::setData(const QByteArray& ba)
{
    data_ = ba;
    b_data_.setData(data_);
    setData(b_data_);
}

QByteArray QHexEdit::data()
{
    return chunks_->data(0, -1);
}

void QHexEdit::setHighlighting(bool highlighting)
{
    highlighting_ = highlighting;
    viewport()->update();
}

bool QHexEdit::highlighting()
{
    return highlighting_;
}

void QHexEdit::setHighlightingColor(const QColor& color)
{
    brush_highlighted_ = QBrush(color);
    pen_highlighted_ = QPen(viewport()->palette().color(QPalette::WindowText));
    viewport()->update();
}

QColor QHexEdit::highlightingColor()
{
    return brush_highlighted_.color();
}

void QHexEdit::setOverwriteMode(bool overwriteMode)
{
    overwrite_mode_ = overwriteMode;
    emit overwriteModeChanged(overwriteMode);
}

bool QHexEdit::overwriteMode()
{
    return overwrite_mode_;
}

void QHexEdit::setSelectionColor(const QColor& color)
{
    brush_selection_ = QBrush(color);
    pen_selection_ = QPen(Qt::white);
    viewport()->update();
}

QColor QHexEdit::selectionColor()
{
    return brush_selection_.color();
}

bool QHexEdit::isReadOnly()
{
    return read_only_;
}

void QHexEdit::setReadOnly(bool readOnly)
{
    read_only_ = readOnly;
}

void QHexEdit::setHexCaps(const bool isCaps)
{
    if (hex_caps_ != isCaps)
    {
        hex_caps_ = isCaps;
        viewport()->update();
    }
}

bool QHexEdit::hexCaps()
{
    return hex_caps_;
}

void QHexEdit::setDynamicBytesPerLine(const bool isDynamic)
{
    dynamic_bytes_per_line_ = isDynamic;
    resizeEvent(nullptr);
}

bool QHexEdit::dynamicBytesPerLine()
{
    return dynamic_bytes_per_line_;
}

// ********************************************************************** Access to data of qhexedit
bool QHexEdit::setData(QIODevice& iODevice)
{
    bool ok = chunks_->setIODevice(iODevice);
    init();
    return ok;
}

QByteArray QHexEdit::dataAt(qint64 pos, qint64 count)
{
    return chunks_->data(pos, count);
}

bool QHexEdit::write(QIODevice& iODevice, qint64 pos, qint64 count)
{
    return chunks_->write(iODevice, pos, count);
}

// ********************************************************************** Char handling
void QHexEdit::insert(qint64 index, char ch)
{
    undo_stack_->insert(index, ch);
    refresh();
}

void QHexEdit::remove(qint64 index, qint64 len)
{
    undo_stack_->removeAt(index, len);
    refresh();
}

void QHexEdit::replace(qint64 index, char ch)
{
    undo_stack_->overwrite(index, ch);
    refresh();
}

// ********************************************************************** ByteArray handling
void QHexEdit::insert(qint64 pos, const QByteArray& ba)
{
    undo_stack_->insert(pos, ba);
    refresh();
}

void QHexEdit::replace(qint64 pos, qint64 len, const QByteArray& ba)
{
    undo_stack_->overwrite(pos, static_cast<int>(len), ba);
    refresh();
}

// ********************************************************************** Utility functions
void QHexEdit::ensureVisible()
{
    if (cursor_position_ < (b_pos_first_ * 2))
    {
        verticalScrollBar()->setValue((int)(cursor_position_ / 2 / bytes_per_line_));
    }
    if (cursor_position_ > ((b_pos_first_ + static_cast<qint64>((rows_shown_ - 1) * bytes_per_line_)) * 2))
    {
        verticalScrollBar()->setValue((int)(cursor_position_ / 2 / bytes_per_line_) - rows_shown_ + 1);
    }
    if (px_cursor_x_ < horizontalScrollBar()->value())
    {
        horizontalScrollBar()->setValue(px_cursor_x_);
    }
    if ((px_cursor_x_ + px_char_width_) > (horizontalScrollBar()->value() + viewport()->width()))
    {
        horizontalScrollBar()->setValue(px_cursor_x_ + px_char_width_ - viewport()->width());
    }
    viewport()->update();
}

qint64 QHexEdit::indexOf(const QByteArray& ba, qint64 from)
{
    qint64 pos = chunks_->indexOf(ba, from);
    if (pos > -1)
    {
        qint64 curPos = pos * 2;
        setCursorPosition(curPos + ba.length() * 2);
        resetSelection(curPos);
        setSelection(curPos + ba.length() * 2);
        ensureVisible();
    }
    return pos;
}

bool QHexEdit::isModified()
{
    return modified_;
}

qint64 QHexEdit::lastIndexOf(const QByteArray& ba, qint64 from)
{
    qint64 pos = chunks_->lastIndexOf(ba, from);
    if (pos > -1)
    {
        qint64 curPos = pos * 2;
        setCursorPosition(curPos - 1);
        resetSelection(curPos);
        setSelection(curPos + ba.length() * 2);
        ensureVisible();
    }
    return pos;
}

void QHexEdit::redo()
{
    undo_stack_->redo();
    if (edit_area_is_ascii_)
    {
        setCursorPosition(chunks_->pos() * 2);
    }
    else if (edit_area_is_bar_)
    {
        setCursorPosition(chunks_->pos() * 3);
    }
    else
    {
        setCursorPosition(chunks_->pos());
    }
    refresh();
}

QString QHexEdit::selectionToReadableString()
{
    QByteArray ba = chunks_->data(getSelectionBegin(), getSelectionEnd() - getSelectionBegin());
    return toReadable(ba);
}

QString QHexEdit::selectedData()
{
    QByteArray ba = chunks_->data(getSelectionBegin(), getSelectionEnd() - getSelectionBegin()).toHex();
    return ba;
}

void QHexEdit::setMonospaceFont(const QFont& font)
{
    QFont theFont(font);
    theFont.setStyleHint(QFont::Monospace);
    QWidget::setFont(theFont);
    QFontMetrics metrics = fontMetrics();
    // A platform with no installed fonts reports zero-pixel glyphs; layout divides by both.
    px_char_width_ = std::max(metrics.horizontalAdvance(QLatin1Char('2')), 1);
    px_char_height_ = std::max(metrics.height(), 1);
    px_gap_adr_ = px_char_width_ / 2;
    px_gap_adr_hex_ = px_char_width_;
    px_gap_hex_ascii_ = 2 * px_char_width_;
    px_gap_ascii_bar_ = 2 * px_char_width_;
    px_cursor_width_ = px_char_height_ / 7;
    px_selection_sub_ = px_char_height_ / 5;
    viewport()->update();
}

QString QHexEdit::toReadableString()
{
    QByteArray ba = chunks_->data();
    return toReadable(ba);
}

void QHexEdit::undo()
{
    undo_stack_->undo();
    if (edit_area_is_ascii_)
    {
        setCursorPosition(chunks_->pos() * 2);
    }
    else if (edit_area_is_bar_)
    {
        setCursorPosition(chunks_->pos() * 3);
    }
    else
    {
        setCursorPosition(chunks_->pos());
    }
    refresh();
}

// ********************************************************************** Handle events
namespace
{
// Move and select key of each QHexEdit::Motion, in its order.
constexpr std::array<std::pair<QKeySequence::StandardKey, QKeySequence::StandardKey>, 10> kMotionKeys{{
    {QKeySequence::MoveToNextChar, QKeySequence::SelectNextChar},
    {QKeySequence::MoveToPreviousChar, QKeySequence::SelectPreviousChar},
    {QKeySequence::MoveToEndOfLine, QKeySequence::SelectEndOfLine},
    {QKeySequence::MoveToStartOfLine, QKeySequence::SelectStartOfLine},
    {QKeySequence::MoveToPreviousLine, QKeySequence::SelectPreviousLine},
    {QKeySequence::MoveToNextLine, QKeySequence::SelectNextLine},
    {QKeySequence::MoveToNextPage, QKeySequence::SelectNextPage},
    {QKeySequence::MoveToPreviousPage, QKeySequence::SelectPreviousPage},
    {QKeySequence::MoveToEndOfDocument, QKeySequence::SelectEndOfDocument},
    {QKeySequence::MoveToStartOfDocument, QKeySequence::SelectStartOfDocument},
}};

bool isTypingModifier(Qt::KeyboardModifiers modifiers)
{
    return modifiers == Qt::NoModifier || modifiers == Qt::KeypadModifier || modifiers == Qt::ShiftModifier ||
           modifiers == (Qt::AltModifier | Qt::ControlModifier) || modifiers == Qt::GroupSwitchModifier;
}
} // namespace

void QHexEdit::keyPressEvent(QKeyEvent *event)
{
    moveOrSelect(event);
    if (!read_only_)
    {
        edit(event);
    }
    if (event->matches(QKeySequence::Copy))
    {
        copySelection();
    }

    // Switch between insert/overwrite mode
    if ((event->key() == Qt::Key_Insert) && (event->modifiers() == Qt::NoModifier))
    {
        setOverwriteMode(!overwriteMode());
        setCursorPosition(cursor_position_);
    }
    switchEditArea(event);
    refresh();
}

// The cursor position, in nibbles, that a motion key moves to. `page` is
// the nibbles in one page.
qint64 QHexEdit::motionTarget(Motion motion, qint64 page)
{
    const qint64 line = 2 * static_cast<qint64>(bytes_per_line_);
    const qint64 charStep = 1 + (edit_area_is_ascii_ ? 1 : 0) + (edit_area_is_bar_ ? 2 : 0);
    switch (motion)
    {
    case Motion::kNextChar:
        return cursor_position_ + charStep;
    case Motion::kPreviousChar:
        return cursor_position_ - charStep;
    case Motion::kEndOfLine:
        return cursor_position_ - cursor_position_ % line + line - 1;
    case Motion::kStartOfLine:
        return cursor_position_ - cursor_position_ % line;
    case Motion::kPreviousLine:
        return cursor_position_ - line;
    case Motion::kNextLine:
        return cursor_position_ + line;
    case Motion::kNextPage:
        return cursor_position_ + page;
    case Motion::kPreviousPage:
        return cursor_position_ - page;
    case Motion::kEndOfDocument:
        return chunks_->size() * 2;
    case Motion::kStartOfDocument:
    default:
        return 0;
    }
}

void QHexEdit::moveOrSelect(QKeyEvent *event)
{
    const qint64 line = 2 * static_cast<qint64>(bytes_per_line_);
    for (std::size_t i = 0; i < kMotionKeys.size(); ++i)
    {
        const auto motion = static_cast<Motion>(i);
        if (event->matches(kMotionKeys[i].first))
        {
            const qint64 pos = motionTarget(motion, (rows_shown_ - 1) * line);
            setCursorPosition(pos);
            // A character step anchors the selection where it aimed, before
            // the cursor is clamped.
            resetSelection(motion == Motion::kNextChar || motion == Motion::kPreviousChar ? pos : cursor_position_);
        }
        if (event->matches(kMotionKeys[i].second))
        {
            const qint64 pos = motionTarget(motion, (viewport()->height() / px_char_height_ - 1) * line);
            setCursorPosition(pos);
            setSelection(pos);
        }
    }
    if (event->matches(QKeySequence::SelectAll))
    {
        resetSelection(0);
        setSelection(2 * chunks_->size() + 1);
    }
}

void QHexEdit::edit(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Cut))
    {
        copySelection();
        eraseSelection();
        setCursorPosition(2 * getSelectionBegin());
        resetSelection(2 * getSelectionBegin());
    }
    else if (event->matches(QKeySequence::Paste))
    {
        pasteClipboard();
    }
    else if (event->matches(QKeySequence::Delete))
    {
        deleteForward();
    }
    else if ((event->key() == Qt::Key_Backspace) && (event->modifiers() == Qt::NoModifier))
    {
        deleteBackward();
    }
    else if (event->matches(QKeySequence::Undo))
    {
        undo();
    }
    else if (event->matches(QKeySequence::Redo))
    {
        redo();
    }
    else if (isTypingModifier(QApplication::keyboardModifiers()))
    {
        type(event->text());
    }
}

// Copies the selection as hex, 16 bytes per line.
void QHexEdit::copySelection()
{
    QByteArray ba = chunks_->data(getSelectionBegin(), getSelectionEnd() - getSelectionBegin()).toHex();
    for (qint64 idx = 32; idx < ba.size(); idx += 33)
    {
        ba.insert(idx, "\n");
    }
    QApplication::clipboard()->setText(ba);
}

// Zeroes the selected bytes in overwrite mode and removes them in insert mode.
void QHexEdit::eraseSelection()
{
    const qint64 begin = getSelectionBegin();
    const qint64 len = getSelectionEnd() - begin;
    if (overwrite_mode_)
    {
        replace(begin, len, QByteArray(len, char(0)));
    }
    else
    {
        remove(begin, len);
    }
}

void QHexEdit::pasteClipboard()
{
    QByteArray ba = QByteArray::fromHex(QApplication::clipboard()->text().toLatin1());
    if (overwrite_mode_)
    {
        ba = ba.left(std::min<qint64>(ba.size(), (chunks_->size() - b_pos_current_)));
        replace(b_pos_current_, ba.size(), ba);
    }
    else
    {
        insert(b_pos_current_, ba);
    }
    setCursorPosition(cursor_position_ + 2 * ba.size());
    resetSelection(getSelectionBegin());
}

// Delete: erases the selection, or else the byte at the cursor.
void QHexEdit::deleteForward()
{
    if (getSelectionBegin() != getSelectionEnd())
    {
        b_pos_current_ = getSelectionBegin();
        eraseSelection();
    }
    else if (overwrite_mode_)
    {
        replace(b_pos_current_, char(0));
    }
    else
    {
        remove(b_pos_current_, 1);
    }
    setCursorPosition(2 * b_pos_current_);
    resetSelection(2 * b_pos_current_);
}

// Backspace: erases the selection, or else the byte before the cursor.
void QHexEdit::deleteBackward()
{
    if (getSelectionBegin() != getSelectionEnd())
    {
        b_pos_current_ = getSelectionBegin();
        setCursorPosition(2 * b_pos_current_);
        eraseSelection();
        resetSelection(2 * b_pos_current_);
        return;
    }
    const bool behindLastByte = (cursor_position_ / 2) == chunks_->size();
    b_pos_current_ -= 1;
    if (overwrite_mode_)
    {
        replace(b_pos_current_, char(0));
    }
    else
    {
        remove(b_pos_current_, 1);
    }
    if (!behindLastByte)
    {
        b_pos_current_ -= 1;
    }
    setCursorPosition(2 * b_pos_current_);
    resetSelection(2 * b_pos_current_);
}

// Hex digits edit the nibble under the cursor in the hex area; printable
// characters edit the byte when both the ascii and bar areas are active.
bool QHexEdit::acceptsKey(int key) const
{
    const bool hexArea = !edit_area_is_ascii_ && !edit_area_is_bar_;
    const bool hexDigit = (key >= '0' && key <= '9') || (key >= 'a' && key <= 'f');
    return (hexDigit && hexArea) || (key >= ' ' && edit_area_is_ascii_ && edit_area_is_bar_);
}

// The byte under the cursor with the typed hex digit in the cursor's nibble.
char QHexEdit::withTypedNibble(int key)
{
    QByteArray hexValue = chunks_->data(b_pos_current_, 1).toHex();
    hexValue[cursor_position_ % 2] = static_cast<char>(key);
    return QByteArray::fromHex(hexValue)[0];
}

void QHexEdit::type(const QString& text)
{
    int key = 0;
    if (!text.isEmpty())
    {
        key = edit_area_is_ascii_ || edit_area_is_bar_ ? static_cast<uchar>(text.at(0).toLatin1())
                                                       : static_cast<uchar>(text.at(0).toLower().toLatin1());
    }
    if (!acceptsKey(key))
    {
        return;
    }
    if (getSelectionBegin() != getSelectionEnd())
    {
        eraseSelection();
        if (!overwrite_mode_)
        {
            b_pos_current_ = getSelectionBegin();
        }
        setCursorPosition(2 * b_pos_current_);
        resetSelection(2 * b_pos_current_);
    }

    // If insert mode, then insert a byte
    if (!overwrite_mode_ && (cursor_position_ % 2) == 0)
    {
        insert(b_pos_current_, char(0));
    }
    if (chunks_->size() == 0)
    {
        return;
    }
    replace(b_pos_current_, !edit_area_is_ascii_ || !edit_area_is_bar_ ? withTypedNibble(key) : static_cast<char>(key));
    setCursorPosition(cursor_position_ + (edit_area_is_ascii_ ? 2 : edit_area_is_bar_ ? 3 : 1));
    resetSelection(cursor_position_);
}

// Tab cycles hex -> bar and ascii -> bar; Backtab cycles bar -> hex and
// ascii -> hex. Each step re-places the cursor in the area it passes.
void QHexEdit::switchEditArea(QKeyEvent *event)
{
    const auto switchTo = [this](bool ascii, bool bar)
    {
        edit_area_is_ascii_ = ascii;
        edit_area_is_bar_ = bar;
        setCursorPosition(cursor_position_);
    };
    if (event->key() == Qt::Key_Tab)
    {
        if (!edit_area_is_ascii_ && !edit_area_is_bar_)
        {
            switchTo(true, false);
        }
        if (edit_area_is_ascii_ && !edit_area_is_bar_)
        {
            switchTo(false, true);
        }
    }
    if (event->key() == Qt::Key_Backtab)
    {
        if (!edit_area_is_ascii_ && edit_area_is_bar_)
        {
            switchTo(true, false);
        }
        if (edit_area_is_ascii_ && !edit_area_is_bar_)
        {
            switchTo(false, false);
        }
    }
}

void QHexEdit::mouseMoveEvent(QMouseEvent *event)
{
    blink_ = false;
    viewport()->update();
    qint64 actPos = cursorPosition(event->pos());
    if (actPos >= 0)
    {
        setCursorPosition(actPos);
        setSelection(actPos);
    }
}

void QHexEdit::mousePressEvent(QMouseEvent *event)
{
    blink_ = false;
    viewport()->update();
    qint64 cPos = cursorPosition(event->pos());
    if (cPos >= 0)
    {
        if (event->button() != Qt::RightButton)
        {
            resetSelection(cPos);
        }
        setCursorPosition(cPos);
    }
}

namespace
{
int printable(int ch)
{
    return ch < ' ' || ch > '~' ? '.' : ch;
}
} // namespace

void QHexEdit::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    const int pxOfsX = horizontalScrollBar()->value();

    if (event->rect() != cursor_rect_)
    {
        paintAreas(painter, event->rect(), pxOfsX);
        painter.setPen(viewport()->palette().color(QPalette::WindowText));
        if (address_area_)
        {
            paintAddresses(painter, pxOfsX);
        }
        painter.setBackgroundMode(Qt::TransparentMode);
        for (int row = 0, pxPosY = px_char_height_; row <= rows_shown_; row++, pxPosY += px_char_height_)
        {
            const qint64 bPosLine = static_cast<qint64>(row) * bytes_per_line_;
            for (int colIdx = 0; ((bPosLine + colIdx) < data_shown_.size() && (colIdx < bytes_per_line_)); colIdx++)
            {
                paintByte(painter, bPosLine + colIdx, colIdx, pxPosY, pxOfsX);
            }
        }
        painter.setBackgroundMode(Qt::TransparentMode);
        painter.setPen(viewport()->palette().color(QPalette::WindowText));
    }
    paintCursor(painter, pxOfsX);

    // emit event, if size has changed
    if (last_event_size_ != chunks_->size())
    {
        last_event_size_ = chunks_->size();
        emit currentSizeChanged(last_event_size_);
    }
}

// The background, the address area and the separators before the ascii and
// bar areas.
void QHexEdit::paintAreas(QPainter& painter, const QRect& rect, int pxOfsX)
{
    painter.fillRect(rect, viewport()->palette().color(QPalette::Base));
    if (address_area_)
    {
        painter.fillRect(QRect(-pxOfsX, rect.top(), px_pos_hex_x_ - px_gap_adr_hex_ / 2, height()),
                         address_area_color_);
    }
    const auto separator = [&](int linePos)
    {
        painter.setPen(Qt::gray);
        painter.drawLine(linePos - pxOfsX, rect.top(), linePos - pxOfsX, height());
    };
    if (ascii_area_)
    {
        separator(px_pos_ascii_x_ - (px_gap_hex_ascii_ / 2));
    }
    if (bar_area_)
    {
        separator(px_pos_bar_x_ - (px_gap_ascii_bar_ / 2));
    }
}

void QHexEdit::paintAddresses(QPainter& painter, int pxOfsX)
{
    painter.setPen(QPen(address_font_color_));
    for (int row = 0, pxPosY = px_char_height_; (static_cast<qsizetype>(row * bytes_per_line_)) < data_shown_.size();
         row++, pxPosY += px_char_height_)
    {
        const QString address = QString("%1").arg(
            b_pos_first_ + static_cast<qint64>(row) * bytes_per_line_ + address_offset_, addr_digits_, 16, QChar('0'));
        painter.drawText(px_pos_adr_x_ - pxOfsX, pxPosY, hexCaps() ? address.toUpper() : address);
    }
}

// One shown byte in the hex, ascii and bar areas; `index` counts from the
// first shown byte.
void QHexEdit::paintByte(QPainter& painter, qint64 index, int colIdx, int pxPosY, int pxOfsX)
{
    const QColor base = viewport()->palette().color(QPalette::Base);
    QColor c = base;
    painter.setPen(QPen(hex_font_color_));
    const qint64 posBa = b_pos_first_ + index;
    if ((getSelectionBegin() <= posBa) && (getSelectionEnd() > posBa))
    {
        c = brush_selection_.color();
        painter.setPen(pen_selection_);
    }
    else if (highlighting_ && marked_shown_.at(static_cast<int>(index)))
    {
        c = brush_highlighted_.color();
        painter.setPen(pen_highlighted_);
    }

    // render hex value
    const int top = pxPosY - px_char_height_ + px_selection_sub_;
    const int pxPosX = px_pos_hex_x_ - pxOfsX + colIdx * 3 * px_char_width_;
    painter.fillRect(colIdx == 0 ? QRect(pxPosX, top, 2 * px_char_width_, px_char_height_)
                                 : QRect(pxPosX - px_char_width_, top, 3 * px_char_width_, px_char_height_),
                     c);
    const QByteArray hex = hex_data_shown_.mid(index * 2, 2);
    painter.drawText(pxPosX, pxPosY, hexCaps() ? hex.toUpper() : hex);

    const auto byte = static_cast<uchar>(data_shown_.at(index));
    // render ascii value
    if (ascii_area_)
    {
        if (c == base)
        {
            c = ascii_area_color_;
        }
        const int x = px_pos_ascii_x_ - pxOfsX + colIdx * px_char_width_;
        painter.fillRect(QRect(x, top, px_char_width_, px_char_height_), c);
        painter.setPen(QPen(ascii_font_color_));
        painter.drawText(x, pxPosY, QChar(printable(byte)));
    }
    // render bar value
    if (bar_area_)
    {
        if (c == base)
        {
            c = bar_area_color_;
        }
        const int x = px_pos_bar_x_ - pxOfsX + colIdx * px_char_width_;
        painter.fillRect(QRect(x, top, px_char_width_, px_char_height_), c);
        const QFont prevFont = painter.font();
        painter.setFont(QFont("fastecu_bars_128"));
        painter.setPen(QPen(bar_font_color_));
        painter.drawText(x, pxPosY, QChar(byte / 2 + 0x30));
        painter.setFont(prevFont);
    }
}

void QHexEdit::paintCursor(QPainter& painter, int pxOfsX)
{
    // _cursorPosition counts in 2, _bPosFirst counts in 1
    const int hexPositionInShowData = static_cast<int>(cursor_position_ - 2 * b_pos_first_);

    // due to scrolling the cursor can go out of the currently displayed data
    if ((hexPositionInShowData < 0) || (hexPositionInShowData >= hex_data_shown_.size()))
    {
        return;
    }
    if (read_only_)
    {
        QColor color = viewport()->palette().dark().color();
        painter.fillRect(QRect(px_cursor_x_ - pxOfsX, px_cursor_y_ - px_char_height_ + px_selection_sub_,
                               px_char_width_, px_char_height_),
                         color);
    }
    else if (blink_ && hasFocus())
    {
        painter.fillRect(cursor_rect_, this->palette().color(QPalette::WindowText));
    }
    if (edit_area_is_ascii_ || edit_area_is_bar_)
    {
        // every 2 hex there is 1 ascii or bar
        const int ch = static_cast<uchar>(data_shown_.at(hexPositionInShowData / 2));
        painter.drawText(px_cursor_x_ - pxOfsX, px_cursor_y_, QChar(printable(ch)));
    }
    else
    {
        const QByteArray hex = hex_data_shown_.mid(hexPositionInShowData, 1);
        painter.drawText(px_cursor_x_ - pxOfsX, px_cursor_y_, hexCaps() ? hex.toUpper() : hex);
    }
}

void QHexEdit::resizeEvent(QResizeEvent *)
{
    if (dynamic_bytes_per_line_)
    {
        int pxFixGaps = 0;
        if (address_area_)
        {
            pxFixGaps = addressWidth() * px_char_width_ + px_gap_adr_;
        }
        pxFixGaps += px_gap_adr_hex_;
        if (ascii_area_)
        {
            pxFixGaps += px_gap_hex_ascii_;
        }
        if (bar_area_)
        {
            pxFixGaps += px_gap_ascii_bar_;
        }

        // +1 because the last hex value do not have space. so it is effective one char more
        int charWidth = (viewport()->width() - pxFixGaps) / px_char_width_ + 1;

        // 2 hex alfa-digits 1 space 1 ascii per byte = 4; if ascii is disabled then 3
        // to prevent devision by zero use the min value 1
        if (ascii_area_ && bar_area_)
        {
            setBytesPerLine(std::max(charWidth / 6, 1));
        }
        else if (ascii_area_ || bar_area_)
        {
            setBytesPerLine(std::max(charWidth / 4, 1));
        }
        else
        {
            setBytesPerLine(std::max(charWidth / 3, 1));
        }
    }
    adjust();
}

bool QHexEdit::focusNextPrevChild(bool next)
{
    if (address_area_)
    {
        if ((next && edit_area_is_ascii_) || (!next && !edit_area_is_ascii_) || (next && edit_area_is_bar_) ||
            (!next && !edit_area_is_bar_))
        {
            return QWidget::focusNextPrevChild(next);
        }
        else
        {
            return false;
        }
    }
    else
    {
        return QWidget::focusNextPrevChild(next);
    }
}

// ********************************************************************** Handle selections
void QHexEdit::resetSelection()
{
    b_selection_begin_ = b_selection_init_;
    b_selection_end_ = b_selection_init_;
}

void QHexEdit::resetSelection(qint64 pos)
{
    pos = pos / 2;
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > chunks_->size())
    {
        pos = chunks_->size();
    }

    b_selection_init_ = pos;
    b_selection_begin_ = pos;
    b_selection_end_ = pos;
}

void QHexEdit::setSelection(qint64 pos)
{
    pos = pos / 2;
    if (pos < 0)
    {
        pos = 0;
    }
    if (pos > chunks_->size())
    {
        pos = chunks_->size();
    }

    if (pos >= b_selection_init_)
    {
        b_selection_end_ = pos;
        b_selection_begin_ = b_selection_init_;
    }
    else
    {
        b_selection_begin_ = pos;
        b_selection_end_ = b_selection_init_;
    }
}

qint64 QHexEdit::getSelectionBegin()
{
    return b_selection_begin_;
}

qint64 QHexEdit::getSelectionEnd()
{
    return b_selection_end_;
}

// ********************************************************************** Private utility functions
void QHexEdit::init()
{
    undo_stack_->clear();
    setAddressOffset(0);
    resetSelection(0);
    setCursorPosition(0);
    verticalScrollBar()->setValue(0);
    modified_ = false;
}

void QHexEdit::adjust()
{
    // recalc Graphics
    if (address_area_)
    {
        addr_digits_ = addressWidth();
        px_pos_hex_x_ = px_gap_adr_ + addr_digits_ * px_char_width_ + px_gap_adr_hex_;
    }
    else
    {
        px_pos_hex_x_ = px_gap_adr_hex_;
    }
    px_pos_adr_x_ = px_gap_adr_;
    px_pos_ascii_x_ = px_pos_hex_x_ + hex_chars_in_line_ * px_char_width_ + px_gap_hex_ascii_;
    px_pos_bar_x_ = px_pos_ascii_x_ + bytes_per_line_ * px_char_width_ + px_gap_ascii_bar_;

    // set horizontalScrollBar()
    int pxWidth = 0; //_pxPosAsciiX;
    if (ascii_area_)
    {
        pxWidth = px_pos_ascii_x_;
        pxWidth += bytes_per_line_ * px_char_width_;
    }
    else if (bar_area_)
    {
        pxWidth = px_pos_bar_x_;
        pxWidth += bytes_per_line_ * px_char_width_;
    }
    horizontalScrollBar()->setRange(0, pxWidth - viewport()->width());
    horizontalScrollBar()->setPageStep(viewport()->width());

    // set verticalScrollbar()
    rows_shown_ = ((viewport()->height() - 4) / px_char_height_);
    int lineCount = (int)(chunks_->size() / (qint64)bytes_per_line_) + 1;
    verticalScrollBar()->setRange(0, lineCount - rows_shown_);
    verticalScrollBar()->setPageStep(rows_shown_);

    int value = verticalScrollBar()->value();
    b_pos_first_ = (qint64)value * bytes_per_line_;
    b_pos_last_ = b_pos_first_ + (qint64)(rows_shown_ * bytes_per_line_) - 1;
    if (b_pos_last_ >= chunks_->size())
    {
        b_pos_last_ = chunks_->size() - 1;
    }
    readBuffers();
    setCursorPosition(cursor_position_);
}

void QHexEdit::dataChangedPrivate(int)
{
    modified_ = undo_stack_->index() != 0;
    adjust();
    emit dataChanged();
}

void QHexEdit::refresh()
{
    ensureVisible();
    readBuffers();
}

void QHexEdit::readBuffers()
{
    data_shown_ = chunks_->data(b_pos_first_, b_pos_last_ - b_pos_first_ + bytes_per_line_ + 1, &marked_shown_);
    hex_data_shown_ = QByteArray(data_shown_.toHex());
}

QString QHexEdit::toReadable(const QByteArray& ba)
{
    QString result;

    for (int i = 0; i < ba.size(); i += 16)
    {
        QString addrStr = QString("%1").arg(address_offset_ + i, addressWidth(), 16, QChar('0'));
        QString hexStr;
        QString ascStr;
        for (int j = 0; j < 16; j++)
        {
            if ((i + j) < ba.size())
            {
                hexStr.append(" ").append(ba.mid(i + j, 1).toHex());
                char ch = ba[i + j];
                if ((ch < 0x20) || (ch > 0x7e))
                {
                    ch = '.';
                }
                ascStr.append(QChar(ch));
            }
        }
        result += addrStr + " " + QString("%1").arg(hexStr, -48) + "  " + QString("%1").arg(ascStr, -17) + "\n";
    }
    return result;
}

void QHexEdit::updateCursor()
{
    blink_ = !blink_;
    viewport()->update(cursor_rect_);
}
