
#include "DropZoneWidget.h"

#include "Core/AssetPipeline/AudioImportGate.h"
#include "Core/ThemeManager.h"

#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QPainter>
#include <QUrl>
#include <QFileInfo>
#include <QStyle>

namespace VNX {


DropZoneWidget::DropZoneWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("DropZoneWidget");
    acceptedExtensions_ = AudioImportGate::acceptedExtensions();

    setAcceptDrops(true);
    setMinimumHeight(100);

    setupUI();
}

DropZoneWidget::~DropZoneWidget() = default;

void DropZoneWidget::setupUI()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    emptyLabel_ = new QLabel(emptyText_);
    emptyLabel_->setObjectName(QStringLiteral("dropZoneEmptyLabel"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setWordWrap(true);
    layout->addWidget(emptyLabel_);

    fileListWidget_ = new QListWidget();
    fileListWidget_->setObjectName(QStringLiteral("dropZoneFileList"));
    fileListWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    fileListWidget_->setDragEnabled(true);
    fileListWidget_->setVisible(false);
    layout->addWidget(fileListWidget_);

    fileListWidget_->setContextMenuPolicy(Qt::ActionsContextMenu);
    QAction* removeAction = new QAction(tr("Remove"), fileListWidget_);
    connect(removeAction, &QAction::triggered, [this]() {
        auto selected = fileListWidget_->selectedItems();
        for (auto* item : selected)
        {
            removeFile(item->data(Qt::UserRole).toString());
        }
    });
    fileListWidget_->addAction(removeAction);

    QAction* clearAction = new QAction(tr("Clear All"), fileListWidget_);
    connect(clearAction, &QAction::triggered, this, &DropZoneWidget::clearDroppedFiles);
    fileListWidget_->addAction(clearAction);
}

void DropZoneWidget::clearDroppedFiles()
{
    droppedFiles_.clear();
    updateListWidget();
    updateEmptyState();
    emit filesCleared();
}

void DropZoneWidget::setAcceptedExtensions(const QStringList& extensions)
{
    acceptedExtensions_ = extensions;
}

void DropZoneWidget::setEmptyText(const QString& text)
{
    emptyText_ = text;
    emptyLabel_->setText(text);
}

void DropZoneWidget::setShowList(bool show)
{
    showList_ = show;
    updateEmptyState();
}

void DropZoneWidget::addFile(const QString& filePath)
{
    if (!droppedFiles_.contains(filePath) && isAcceptedFile(filePath))
    {
        droppedFiles_.append(filePath);
        updateListWidget();
        updateEmptyState();
        emit fileAdded(filePath);
    }
}

void DropZoneWidget::removeFile(const QString& filePath)
{
    if (droppedFiles_.removeOne(filePath))
    {
        updateListWidget();
        updateEmptyState();
        emit fileRemoved(filePath);
    }
}

void DropZoneWidget::addFiles(const QStringList& filePaths)
{
    bool anyAdded = false;
    for (const QString& path : filePaths)
    {
        if (!droppedFiles_.contains(path) && isAcceptedFile(path))
        {
            droppedFiles_.append(path);
            anyAdded = true;
            emit fileAdded(path);
        }
    }

    if (anyAdded)
    {
        updateListWidget();
        updateEmptyState();
    }
}

void DropZoneWidget::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
    {
        for (const QUrl& url : event->mimeData()->urls())
        {
            if (isAcceptedFile(url.toLocalFile()))
            {
                event->acceptProposedAction();
                isDragging_ = true;
                update();
                return;
            }
        }
    }
    event->ignore();
}

void DropZoneWidget::dragMoveEvent(QDragMoveEvent* event)
{
    if (isDragging_)
    {
        event->acceptProposedAction();
    }
}

void DropZoneWidget::dragLeaveEvent(QDragLeaveEvent* event)
{
    Q_UNUSED(event)
    isDragging_ = false;
    update();
}

void DropZoneWidget::dropEvent(QDropEvent* event)
{
    isDragging_ = false;

    if (event->mimeData()->hasUrls())
    {
        QStringList newFiles;
        for (const QUrl& url : event->mimeData()->urls())
        {
            QString filePath = url.toLocalFile();
            if (isAcceptedFile(filePath))
            {
                newFiles.append(filePath);
            }
        }

        if (!newFiles.isEmpty())
        {
            addFiles(newFiles);
            event->acceptProposedAction();
        }
    }

    update();
}

void DropZoneWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const auto colors = ThemeManager::instance().colors();
    QColor borderColor = isDragging_ ? colors.info : colors.borderPrimary;
    QPen pen(borderColor, 2, Qt::DashLine);
    painter.setPen(pen);
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 8, 8);

    if (isDragging_)
    {
        QColor fill = colors.info;
        fill.setAlpha(30);
        painter.fillRect(rect(), fill);
    }
}

bool DropZoneWidget::isAcceptedFile(const QString& filePath) const
{
    QFileInfo info(filePath);
    QString ext = info.suffix().toLower();
    return acceptedExtensions_.contains(ext);
}

void DropZoneWidget::updateEmptyState()
{
    bool empty = droppedFiles_.isEmpty();
    emptyLabel_->setVisible(empty);
    fileListWidget_->setVisible(!empty && showList_);
}

void DropZoneWidget::updateListWidget()
{
    fileListWidget_->clear();

    for (const QString& path : droppedFiles_)
    {
        QFileInfo info(path);
        QListWidgetItem* item = new QListWidgetItem();
        item->setText(QString("🎵 %1").arg(info.fileName()));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        fileListWidget_->addItem(item);
    }
}


bool AudioDropTarget::canAcceptAudioDrop(const QMimeData* mimeData) const
{
    return AudioImportGate::mimeHasAudio(mimeData);
}

QStringList AudioDropTarget::extractAudioFiles(const QMimeData* mimeData) const
{
    return AudioImportGate::extractAudioFiles(mimeData);
}

}
