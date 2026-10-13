#pragma once

#include <QByteArray>
#include <QString>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <cstring>
#include <vector>

namespace HarkenHubTest {

struct TestEntry
{
    QByteArray name;
    QByteArray data;
    uint16_t versionMadeby = 0;
    uint32_t externalFa = 0;
};

inline bool writeArchive(const QString& path, const std::vector<TestEntry>& entries)
{
    void* writer = mz_zip_writer_create();
    if (!writer)
        return false;
    if (mz_zip_writer_open_file(writer, path.toUtf8().constData(), 0, 0) != MZ_OK) {
        mz_zip_writer_delete(&writer);
        return false;
    }

    bool ok = true;
    for (const auto& entry : entries) {
        mz_zip_file info;
        std::memset(&info, 0, sizeof(info));
        info.filename = entry.name.constData();
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.version_madeby = entry.versionMadeby;
        info.external_fa = entry.externalFa;
        info.flag = MZ_ZIP_FLAG_UTF8;
        if (mz_zip_writer_add_buffer(writer,
                                     const_cast<char*>(entry.data.constData()),
                                     entry.data.size(), &info) != MZ_OK) {
            ok = false;
            break;
        }
    }
    if (mz_zip_writer_close(writer) != MZ_OK)
        ok = false;
    mz_zip_writer_delete(&writer);
    return ok;
}

}
