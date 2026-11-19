#pragma once

#include "Core/ProjectTypes.h"

#include <QVariantMap>
#include <QWidget>

#include <cstdint>

class QFocusEvent;

namespace HarkenEditor {
class AuditionManager;
}

namespace VNX {

class FileViewerWidget;

class FileViewerPanel final : public QWidget
{
    Q_OBJECT

public:
    static QString typeId();
    static QString dockObjectPrefix();

    explicit FileViewerPanel(QWidget* parent = nullptr);

    static FileViewerPanel* panelContaining(QWidget* widget);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    FileViewerWidget* viewerWidget() const { return viewer_; }
    bool isPreviewPlayheadActive() const;
    QString previewObjectName() const;
    uint32_t sourceId() const;
    void bindSource(uint32_t sourceId);
    void setAuditionManager(HarkenEditor::AuditionManager* audition);
    bool handlePlayStopShortcut();
    void clearEditor();
    QVariantMap snapshotState() const;
    void restoreState(const QVariantMap& state);

signals:
    void panelFocused(int panelId);
    void pinToggled(bool pinned);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    void installFocusFilterRecursively(QObject* root);

    int panelId_ = 0;
    bool pinned_ = false;
    FileViewerWidget* viewer_ = nullptr;
};

}
