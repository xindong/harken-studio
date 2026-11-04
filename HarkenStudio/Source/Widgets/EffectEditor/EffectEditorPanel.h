#pragma once

#include <QUuid>
#include <QVariantMap>
#include <QWidget>

namespace VNX {

struct ProjectItem;

class EffectEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit EffectEditorPanel(QWidget* parent = nullptr);

    int panelId() const { return panelId_; }
    void setPanelId(int id) { panelId_ = id; }
    bool isPinned() const { return pinned_; }
    void setPinned(bool pinned);
    QUuid ownerGuid() const { return ownerGuid_; }
    QUuid rowGuid() const { return rowGuid_; }
    ProjectItem* boundItem() const;
    bool hasEffect() const;
    static bool canOpen(const ProjectItem* item, const QUuid& rowGuid);
    void bindEffect(const QUuid& ownerGuid, const QUuid& rowGuid = {});
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
    void refreshEffect();

    int panelId_ = 0;
    bool pinned_ = false;
    QUuid ownerGuid_;
    QUuid rowGuid_;
};

}
