
#include "PackStructureDialog.h"
#include "PackManagerPanel.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ProportionalTreeWidget.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cstring>

namespace VNX {

PackStructureDialog::PackStructureDialog(const QString& path, QWidget* parent)
    : BaseDialog(parent)
{
    setObjectName("packStructureDialog");
    setWindowTitle(tr("Pack Structure"));
    setMinimumSize(520, 380);
    setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(this);

    infoLabel_ = new QLabel();
    infoLabel_->setObjectName("packStructureInfoLabel");
    infoLabel_->setProperty("textRole", QStringLiteral("muted"));
    layout->addWidget(infoLabel_);

    auto* table = new TableWidget(TableOptions::display(), this);
    sectionTree_ = table->createItemView(QStringLiteral("packStructureTree"),
        {tr("Section"), tr("Details"), tr("Count")}, {400, 400, 200});
    sectionTree_->setRootIsDecorated(true);
    layout->addWidget(table, 1);

    setupStandardButtons(QDialogButtonBox::Close);
    layout->addWidget(buttonBox());

    populate(path);
}

bool PackStructureDialog::readPackHeader(QFile& file, PackFileHeader& out)
{
    if (!file.isOpen() || file.size() < 32)
        return false;
    file.seek(0);
    const QByteArray headerData = file.read(32);
    if (headerData.size() < 32)
        return false;
    const char* hdr = headerData.constData();
    std::memcpy(out.magic, hdr, 4);
    std::memcpy(&out.version, hdr + 4, 4);
    std::memcpy(&out.packID, hdr + 8, 4);
    std::memcpy(&out.langID, hdr + 12, 4);
    std::memcpy(&out.sectionCount, hdr + 16, 4);
    std::memcpy(&out.totalSize, hdr + 20, 4);
    std::memcpy(&out.crc32, hdr + 24, 4);
    std::memcpy(&out.flags, hdr + 28, 4);
    return true;
}

void PackStructureDialog::populate(const QString& path)
{
    sectionTree_->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        infoLabel_->setText(tr("Failed to open: %1").arg(path));
        return;
    }

    PackFileHeader hdr;
    if (!readPackHeader(file, hdr)) {
        infoLabel_->setText(tr("File too small for pack header"));
        return;
    }

    const QFileInfo fi(path);
    infoLabel_->setText(tr("Pack: %1 | Version: V%2 | Sections: %3 | Size: %4")
        .arg(fi.fileName()).arg(hdr.version).arg(hdr.sectionCount)
        .arg(PackManagerPanel::humanBytes(fi.size())));

    auto* headerItem = new QTreeWidgetItem(sectionTree_,
        {tr("Header"),
         QString("magic=%1, packID=0x%2").arg(hdr.magic).arg(hdr.packID, 8, 16, QChar('0')),
         tr("32 bytes")});
    headerItem->setExpanded(true);

    new QTreeWidgetItem(headerItem, {tr("Version"), QString::number(hdr.version), ""});
    new QTreeWidgetItem(headerItem, {tr("Flags"), QString("0x%1").arg(hdr.flags, 8, 16, QChar('0')), ""});
    new QTreeWidgetItem(headerItem, {tr("CRC32"), QString("0x%1").arg(hdr.crc32, 8, 16, QChar('0')), ""});

    qint64 offset = 32;
    for (uint32_t s = 0; s < hdr.sectionCount && offset + 8 <= file.size(); ++s) {
        file.seek(offset);
        const QByteArray chunkHdr = file.read(8);
        if (chunkHdr.size() < 8)
            break;

        uint32_t chunkType = 0, chunkSize = 0;
        std::memcpy(&chunkType, chunkHdr.constData(), 4);
        std::memcpy(&chunkSize, chunkHdr.constData() + 4, 4);

        char typeStr[5] = {};
        std::memcpy(typeStr, chunkHdr.constData(), 4);

        auto* secItem = new QTreeWidgetItem(sectionTree_,
            {tr("Section: %1").arg(typeStr),
             PackManagerPanel::humanBytes(chunkSize), ""});

        if (offset + 12 <= file.size()) {
            file.seek(offset + 8);
            const QByteArray countData = file.read(4);
            if (countData.size() >= 4) {
                uint32_t entryCount = 0;
                std::memcpy(&entryCount, countData.constData(), 4);
                secItem->setText(2, tr("%1 entries").arg(entryCount));
            }
        }

        secItem->setExpanded(true);
        offset += 8 + chunkSize;
    }
}

}
