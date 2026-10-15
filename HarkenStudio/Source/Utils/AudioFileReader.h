#pragma once

#include <QFile>
#include <dr_wav/dr_wav.h>
#include <limits>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace VNX {

class AudioFileReader final : public QFile
{
public:
    using QFile::QFile;

    bool openReadOnly()
    {
#ifdef Q_OS_WIN
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(fileName().utf16()),
            GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (descriptor == -1) {
            CloseHandle(handle);
            return false;
        }
        if (QFile::open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
            return true;
        _close(descriptor);
        return false;
#else
        return QFile::open(QIODevice::ReadOnly);
#endif
    }

    bool initWav(drwav& wav, drwav_chunk_proc onChunk = nullptr, void* chunkData = nullptr)
    {
        return openReadOnly()
            && drwav_init_ex(&wav, readWav, seekWav, tellWav, onChunk, this, chunkData, 0, nullptr);
    }

private:
    static size_t readWav(void* userData, void* output, size_t bytes)
    {
        auto* file = static_cast<AudioFileReader*>(userData);
        const qint64 read = file->read(static_cast<char*>(output), static_cast<qint64>(bytes));
        return read > 0 ? static_cast<size_t>(read) : 0;
    }

    static drwav_bool32 seekWav(void* userData, int offset, drwav_seek_origin origin)
    {
        auto* file = static_cast<AudioFileReader*>(userData);
        const qint64 base = origin == DRWAV_SEEK_CUR ? file->pos()
            : (origin == DRWAV_SEEK_END ? file->size() : 0);
        if (base < 0 || offset > std::numeric_limits<qint64>::max() - base || offset < -base)
            return DRWAV_FALSE;
        return file->seek(base + offset) ? DRWAV_TRUE : DRWAV_FALSE;
    }

    static drwav_bool32 tellWav(void* userData, drwav_int64* cursor)
    {
        *cursor = static_cast<AudioFileReader*>(userData)->pos();
        return *cursor >= 0 ? DRWAV_TRUE : DRWAV_FALSE;
    }
};

}
