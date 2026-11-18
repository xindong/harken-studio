#pragma once

#include "Core/ProjectTypes.h"

#include <QUuid>
#include <QVariantMap>
#include <QWidget>

#include <cstdint>

class QFocusEvent;

namespace HarkenEditor {
class AuditionManager;
}

namespace VNX {

class SourceEditorWidget;

class SourceEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    static QString typeId();
    static QString dockObjectPrefix();

    explicit SourceEditorPanel(QWidget* parent = nullptr);

    static SourceEditorPanel* panelContaining(QWidget* widget);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    SourceEditorWidget* editorWidget() const { return editor_; }
    bool isPreviewPlayheadActive() const;
    QString previewObjectName() const;
    uint32_t sourceId() const;
    QUuid soundGuid() const;
    QUuid entryId() const;
    void bindSource(uint32_t sourceId, const QUuid& soundGuid = {}, const QUuid& entryId = {});
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
    SourceEditorWidget* editor_ = nullptr;
};

}
