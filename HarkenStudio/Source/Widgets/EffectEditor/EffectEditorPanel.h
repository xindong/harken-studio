#pragma once

#include <QUuid>
#include <QVariantMap>
#include <QWidget>

#include <array>
#include <memory>

class QToolButton;

namespace VNX {

struct ProjectItem;
class ParameterSlider;

class EffectEditorPanel final : public QWidget
{
    Q_OBJECT

public:
    enum class Presentation { Standalone, Embedded };

    explicit EffectEditorPanel(QWidget* parent = nullptr,
                               Presentation presentation = Presentation::Standalone);
    ~EffectEditorPanel() override;

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
    void detachRequested(const QUuid& ownerGuid, const QUuid& rowGuid);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    struct Selection;
    static std::unique_ptr<Selection> resolveSelection(const ProjectItem* item, const QUuid& rowGuid);
    void installFocusFilterRecursively(QObject* root);
    void refreshEffect();
    void cancelPendingEdits();
    void commitParameter(size_t index, double value);

    int panelId_ = 0;
    bool pinned_ = false;
    QUuid ownerGuid_;
    QUuid rowGuid_;
    QToolButton* detachButton_ = nullptr;
    QWidget* fields_ = nullptr;
    std::array<ParameterSlider*, 2> parameters_{};
    std::unique_ptr<Selection> selection_;
    bool updating_ = false;
};

}
