#include "commands.h"
#include <QUndoCommand>

// Helper class to store single byte commands
class CharCommand : public QUndoCommand
{
  public:
    enum CCmd
    {
        kInsert,
        kRemoveAt,
        kOverwrite
    };

    CharCommand(Chunks *chunks, CCmd cmd, qint64 charPos, char newChar, QUndoCommand *parent = 0);

    void undo();
    void redo();
    bool mergeWith(const QUndoCommand *command);
    int id() const
    {
        return 1234;
    }

  private:
    Chunks *chunks_;
    qint64 char_pos_;
    bool was_changed_;
    char new_char_;
    char old_char_;
    CCmd cmd_;
};

CharCommand::CharCommand(Chunks *chunks, CCmd cmd, qint64 charPos, char newChar, QUndoCommand *parent)
    : QUndoCommand(parent), chunks_(chunks), char_pos_(charPos), was_changed_(false), new_char_(newChar),
      old_char_('\0'), cmd_(cmd)
{
}

bool CharCommand::mergeWith(const QUndoCommand *command)
{
    const CharCommand *nextCommand = static_cast<const CharCommand *>(command);
    bool result = false;

    if (cmd_ != CharCommand::kRemoveAt)
    {
        if (nextCommand->cmd_ == kOverwrite)
        {
            if (nextCommand->char_pos_ == char_pos_)
            {
                new_char_ = nextCommand->new_char_;
                result = true;
            }
        }
    }
    return result;
}

void CharCommand::undo()
{
    switch (cmd_)
    {
    case kInsert:
        chunks_->removeAt(char_pos_);
        break;
    case kOverwrite:
        chunks_->overwrite(char_pos_, old_char_);
        chunks_->setDataChanged(char_pos_, was_changed_);
        break;
    case kRemoveAt:
        chunks_->insert(char_pos_, old_char_);
        chunks_->setDataChanged(char_pos_, was_changed_);
        break;
    }
}

void CharCommand::redo()
{
    switch (cmd_)
    {
    case kInsert:
        chunks_->insert(char_pos_, new_char_);
        break;
    case kOverwrite:
        old_char_ = (*chunks_)[char_pos_];
        was_changed_ = chunks_->dataChanged(char_pos_);
        chunks_->overwrite(char_pos_, new_char_);
        break;
    case kRemoveAt:
        old_char_ = (*chunks_)[char_pos_];
        was_changed_ = chunks_->dataChanged(char_pos_);
        chunks_->removeAt(char_pos_);
        break;
    }
}

UndoStack::UndoStack(Chunks *chunks, QObject *parent) : QUndoStack(parent)
{
    chunks_ = chunks;
    parent_ = parent;
    this->setUndoLimit(1000);
}

void UndoStack::insert(qint64 pos, char c)
{
    if ((pos >= 0) && (pos <= chunks_->size()))
    {
        QUndoCommand *cc = new CharCommand(chunks_, CharCommand::kInsert, pos, c);
        this->push(cc);
    }
}

void UndoStack::insert(qint64 pos, const QByteArray& ba)
{
    if ((pos >= 0) && (pos <= chunks_->size()))
    {
        QString txt = QString(tr("Inserting %1 bytes")).arg(ba.size());
        beginMacro(txt);
        for (int idx = 0; idx < ba.size(); idx++)
        {
            QUndoCommand *cc = new CharCommand(chunks_, CharCommand::kInsert, pos + idx, ba.at(idx));
            this->push(cc);
        }
        endMacro();
    }
}

void UndoStack::removeAt(qint64 pos, qint64 len)
{
    if ((pos >= 0) && (pos < chunks_->size()))
    {
        if (len == 1)
        {
            QUndoCommand *cc = new CharCommand(chunks_, CharCommand::kRemoveAt, pos, char(0));
            this->push(cc);
        }
        else
        {
            QString txt = QString(tr("Delete %1 chars")).arg(len);
            beginMacro(txt);
            for (qint64 cnt = 0; cnt < len; cnt++)
            {
                QUndoCommand *cc = new CharCommand(chunks_, CharCommand::kRemoveAt, pos, char(0));
                push(cc);
            }
            endMacro();
        }
    }
}

void UndoStack::overwrite(qint64 pos, char c)
{
    if ((pos >= 0) && (pos < chunks_->size()))
    {
        QUndoCommand *cc = new CharCommand(chunks_, CharCommand::kOverwrite, pos, c);
        this->push(cc);
    }
}

void UndoStack::overwrite(qint64 pos, int len, const QByteArray& ba)
{
    if ((pos >= 0) && (pos < chunks_->size()))
    {
        QString txt = QString(tr("Overwrite %1 chars")).arg(len);
        beginMacro(txt);
        removeAt(pos, len);
        insert(pos, ba);
        endMacro();
    }
}
