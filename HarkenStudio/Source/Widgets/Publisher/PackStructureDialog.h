
#pragma once

#include "../../Core/BaseDialog.h"

#include <QString>
#include <cstdint>

class QFile;
class QLabel;
class QTreeWidget;

namespace VNX {

struct PackFileHeader {
    char     magic[5] = {};
    uint32_t version = 0;
    uint32_t packID = 0;
    uint32_t langID = 0;
    uint32_t sectionCount = 0;
    uint32_t totalSize = 0;
    uint32_t crc32 = 0;
    uint32_t flags = 0;
};

class PackStructureDialog : public BaseDialog
{
    Q_OBJECT

public:
    explicit PackStructureDialog(const QString& path, QWidget* parent = nullptr);

    static bool readPackHeader(QFile& file, PackFileHeader& out);

private:
    void populate(const QString& path);

    QLabel*      infoLabel_ = nullptr;
    QTreeWidget* sectionTree_ = nullptr;
};

}
