
#pragma once

#include <QWidget>
#include <QLabel>
#include <QListWidget>
#include <QStringList>
#include <QMimeData>

namespace VNX {

class DropZoneWidget : public QWidget
{
    Q_OBJECT

public:
    explicit DropZoneWidget(QWidget* parent = nullptr);
    ~DropZoneWidget() override;

    QStringList droppedFiles() const { return droppedFiles_; }
    void clearDroppedFiles();

    void setAcceptedExtensions(const QStringList& extensions);
    QStringList acceptedExtensions() const { return acceptedExtensions_; }

    void setEmptyText(const QString& text);
    void setShowList(bool show);
    bool showList() const { return showList_; }

signals:
    void fileAdded(const QString& filePath);
    void fileRemoved(const QString& filePath);
    void filesCleared();

public slots:
    void addFile(const QString& filePath);
    void removeFile(const QString& filePath);
    void addFiles(const QStringList& filePaths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void setupUI();
    bool isAcceptedFile(const QString& filePath) const;
    void updateEmptyState();
    void updateListWidget();

private:
    QLabel* emptyLabel_;
    QListWidget* fileListWidget_;
    QStringList droppedFiles_;
    QStringList acceptedExtensions_;
    QString emptyText_;
    bool showList_ = true;
    bool isDragging_ = false;
};

class AudioDropTarget
{
public:
    virtual ~AudioDropTarget() = default;

    bool canAcceptAudioDrop(const QMimeData* mimeData) const;
    QStringList extractAudioFiles(const QMimeData* mimeData) const;
};

}
