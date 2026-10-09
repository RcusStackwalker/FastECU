#include "chunks.h"
#include <limits>

namespace
{
constexpr int kNormal = 0;
constexpr int kBufferSize = 0x10000;
constexpr int kChunkSize = 0x1000;
constexpr quint64 kReadChunkMask = Q_UINT64_C(0xfffffffffffff000);
} // namespace

// ***************************************** Constructors and file settings

Chunks::Chunks(QObject *parent) : QObject(parent)
{
    QBuffer *buf = new QBuffer(this);
    setIODevice(*buf);
}

Chunks::Chunks(QIODevice& ioDevice, QObject *parent) : QObject(parent)
{
    setIODevice(ioDevice);
}

bool Chunks::setIODevice(QIODevice& ioDevice)
{
    _ioDevice = &ioDevice;
    bool ok = _ioDevice->open(QIODevice::ReadOnly);
    if (ok) // Try to open IODevice
    {
        _size = _ioDevice->size();
        _ioDevice->close();
    }
    else // Fallback is an empty buffer
    {
        QBuffer *buf = new QBuffer(this);
        _ioDevice = buf;
        _size = 0;
    }
    _chunks.clear();
    _pos = 0;
    return ok;
}

// ***************************************** Getting data out of Chunks

QByteArray Chunks::data(qint64 pos, qint64 maxSize, QByteArray *highlighted)
{
    qint64 ioDelta = 0;
    int chunkIdx = 0;

    Chunk chunk;
    QByteArray buffer;

    // Do some checks and some arrangements
    if (highlighted)
    {
        highlighted->clear();
    }

    if (pos >= _size)
    {
        return buffer;
    }

    if (maxSize < 0)
    {
        maxSize = _size;
    }
    else if ((pos + maxSize) > _size)
    {
        maxSize = _size - pos;
    }

    _ioDevice->open(QIODevice::ReadOnly);

    while (maxSize > 0)
    {
        chunk.abs_pos = std::numeric_limits<qint64>::max();
        bool chunksLoopOngoing = true;
        while ((chunkIdx < _chunks.count()) && chunksLoopOngoing)
        {
            // In this section, we track changes before our required data and
            // we take the editdet data, if availible. ioDelta is a difference
            // counter to justify the read pointer to the original data, if
            // data in between was deleted or inserted.

            chunk = _chunks[chunkIdx];
            if (chunk.abs_pos > pos)
            {
                chunksLoopOngoing = false;
            }
            else
            {
                chunkIdx += 1;
                qint64 count;
                qint64 chunkOfs = pos - chunk.abs_pos;
                if (maxSize > ((qint64)chunk.data.size() - chunkOfs))
                {
                    count = (qint64)chunk.data.size() - chunkOfs;
                    ioDelta += kChunkSize - chunk.data.size();
                }
                else
                {
                    count = maxSize;
                }
                if (count > 0)
                {
                    buffer += chunk.data.mid(chunkOfs, (int)count);
                    maxSize -= count;
                    pos += count;
                    if (highlighted)
                    {
                        *highlighted += chunk.data_changed.mid(chunkOfs, (int)count);
                    }
                }
            }
        }

        if ((maxSize > 0) && (pos < chunk.abs_pos))
        {
            // In this section, we read data from the original source. This only will
            // happen, whe no copied data is available

            qint64 byteCount;
            QByteArray readBuffer;
            if ((chunk.abs_pos - pos) > maxSize)
            {
                byteCount = maxSize;
            }
            else
            {
                byteCount = chunk.abs_pos - pos;
            }

            maxSize -= byteCount;
            _ioDevice->seek(pos + ioDelta);
            readBuffer = _ioDevice->read(byteCount);
            buffer += readBuffer;
            if (highlighted)
            {
                *highlighted += QByteArray(readBuffer.size(), kNormal);
            }
            pos += readBuffer.size();
        }
    }
    _ioDevice->close();
    return buffer;
}

bool Chunks::write(QIODevice& iODevice, qint64 pos, qint64 count)
{
    if (count == -1)
    {
        count = _size;
    }
    bool ok = iODevice.open(QIODevice::WriteOnly);
    if (ok)
    {
        for (qint64 idx = pos; idx < count; idx += kBufferSize)
        {
            QByteArray ba = data(idx, kBufferSize);
            iODevice.write(ba);
        }
        iODevice.close();
    }
    return ok;
}

// ***************************************** Set and get highlighting infos

void Chunks::setDataChanged(qint64 pos, bool dataChanged)
{
    if ((pos < 0) || (pos >= _size))
    {
        return;
    }
    int chunkIdx = getChunkIndex(pos);
    qint64 posInBa = pos - _chunks[chunkIdx].abs_pos;
    _chunks[chunkIdx].data_changed[(int)posInBa] = char(dataChanged);
}

bool Chunks::dataChanged(qint64 pos)
{
    QByteArray highlighted;
    data(pos, 1, &highlighted);
    return bool(highlighted.at(0));
}

// ***************************************** Search API

qint64 Chunks::indexOf(const QByteArray& ba, qint64 from)
{
    qint64 result = -1;
    QByteArray buffer;

    for (qint64 pos = from; (pos < _size) && (result < 0); pos += kBufferSize)
    {
        buffer = data(pos, kBufferSize + ba.size() - 1);
        qsizetype findPos = buffer.indexOf(ba);
        if (findPos >= 0)
        {
            result = pos + (qint64)findPos;
        }
    }
    return result;
}

qint64 Chunks::lastIndexOf(const QByteArray& ba, qint64 from)
{
    qint64 result = -1;
    QByteArray buffer;

    for (qint64 pos = from; (pos > 0) && (result < 0); pos -= kBufferSize)
    {
        qint64 sPos = pos - kBufferSize - (qint64)ba.size() + 1;
        if (sPos < 0)
        {
            sPos = 0;
        }
        buffer = data(sPos, pos - sPos);
        qsizetype findPos = buffer.lastIndexOf(ba);
        if (findPos >= 0)
        {
            result = sPos + (qint64)findPos;
        }
    }
    return result;
}

// ***************************************** Char manipulations

bool Chunks::insert(qint64 pos, char b)
{
    if ((pos < 0) || (pos > _size))
    {
        return false;
    }
    int chunkIdx;
    if (pos == _size)
    {
        chunkIdx = getChunkIndex(pos - 1);
    }
    else
    {
        chunkIdx = getChunkIndex(pos);
    }
    qint64 posInBa = pos - _chunks[chunkIdx].abs_pos;
    _chunks[chunkIdx].data.insert(posInBa, b);
    _chunks[chunkIdx].data_changed.insert(posInBa, char(1));
    for (int idx = chunkIdx + 1; idx < _chunks.size(); idx++)
    {
        _chunks[idx].abs_pos += 1;
    }
    _size += 1;
    _pos = pos;
    return true;
}

bool Chunks::overwrite(qint64 pos, char b)
{
    if ((pos < 0) || (pos >= _size))
    {
        return false;
    }
    int chunkIdx = getChunkIndex(pos);
    qint64 posInBa = pos - _chunks[chunkIdx].abs_pos;
    _chunks[chunkIdx].data[(int)posInBa] = b;
    _chunks[chunkIdx].data_changed[(int)posInBa] = char(1);
    _pos = pos;
    return true;
}

bool Chunks::removeAt(qint64 pos)
{
    if ((pos < 0) || (pos >= _size))
    {
        return false;
    }
    int chunkIdx = getChunkIndex(pos);
    qint64 posInBa = pos - _chunks[chunkIdx].abs_pos;
    _chunks[chunkIdx].data.remove(posInBa, 1);
    _chunks[chunkIdx].data_changed.remove(posInBa, 1);
    for (int idx = chunkIdx + 1; idx < _chunks.size(); idx++)
    {
        _chunks[idx].abs_pos -= 1;
    }
    _size -= 1;
    _pos = pos;
    return true;
}

// ***************************************** Utility functions

char Chunks::operator[](qint64 pos)
{
    return data(pos, 1).at(0);
}

qint64 Chunks::pos()
{
    return _pos;
}

qint64 Chunks::size()
{
    return _size;
}

int Chunks::getChunkIndex(qint64 absPos)
{
    // This routine checks, if there is already a copied chunk available. If os, it
    // returns a reference to it. If there is no copied chunk available, original
    // data will be copied into a new chunk.

    int foundIdx = -1;
    int insertIdx = 0;
    qint64 ioDelta = 0;

    for (int idx = 0; idx < _chunks.size(); idx++)
    {
        Chunk chunk = _chunks[idx];
        if ((absPos >= chunk.abs_pos) && (absPos < (chunk.abs_pos + chunk.data.size())))
        {
            foundIdx = idx;
            break;
        }
        if (absPos < chunk.abs_pos)
        {
            insertIdx = idx;
            break;
        }
        ioDelta += chunk.data.size() - kChunkSize;
        insertIdx = idx + 1;
    }

    if (foundIdx == -1)
    {
        Chunk newChunk;
        qint64 readAbsPos = absPos - ioDelta;
        qint64 readPos = static_cast<qint64>(static_cast<quint64>(readAbsPos) & kReadChunkMask);
        _ioDevice->open(QIODevice::ReadOnly);
        _ioDevice->seek(readPos);
        newChunk.data = _ioDevice->read(kChunkSize);
        _ioDevice->close();
        newChunk.abs_pos = absPos - (readAbsPos - readPos);
        newChunk.data_changed = QByteArray(newChunk.data.size(), char(0));
        _chunks.insert(insertIdx, newChunk);
        foundIdx = insertIdx;
    }
    return foundIdx;
}

#ifdef MODUL_TEST
int Chunks::chunkSize()
{
    return _chunks.size();
}

#endif
