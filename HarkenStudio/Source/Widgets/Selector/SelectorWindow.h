
#pragma once

#include "Core/BaseDialog.h"
#include "Core/ProjectTypes.h"
#include "Widgets/Shared/RefDomain.h"

#include <Qt>
#include <QPointer>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <vector>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace VNX {

constexpr int kRoleObjectID   = Qt::UserRole;
constexpr int kRoleSelectable = Qt::UserRole + 2;
constexpr int kRoleSearchPath = Qt::UserRole + 3;

struct SelectorRequest
{
    QString title;
    RefDomain domain = RefDomain::ProjectObject;
    ProjectItem::Type filterType = ProjectItem::Type::Bus;
    std::vector<ProjectItem::Type> filterTypes;
    QStringList bucketFolders;
    QUuid directChildParentGuid;
    uint32_t currentObjectID = 0;
    std::function<bool(uint32_t)> isSelectable;
    std::function<void(uint32_t)> onSelected;
    std::function<Qt::CheckState(uint32_t)> checkState;
    std::function<bool(uint32_t)> canToggle;
    std::function<void(uint32_t, bool)> onToggled;
};

class SelectorWindow : public BaseDialog
{
    Q_OBJECT

public:
    static SelectorWindow* open(const SelectorRequest& request, QWidget* anchor = nullptr);
    static void dismiss(QWidget* anchor = nullptr);
    static void refresh(QWidget* anchor = nullptr);

    void accept() override;

protected:
    bool event(QEvent* e) override;

private:
    SelectorWindow();
    void applyRequest(const SelectorRequest& request);
    void rebuildTree(bool preserveState = false);
    void refreshSwitches();
    void applySearchFilter(const QString& text);
    void notifyItem(QTreeWidgetItem* item);

    SelectorRequest request_;
    QPointer<QWidget> anchor_;
    QMetaObject::Connection anchorDestroyed_;
    bool refreshPending_ = false;
    uint32_t lastNotifiedID_ = 0;
    QLineEdit* searchBox_ = nullptr;
    QTreeWidget* tree_ = nullptr;

    static SelectorWindow* s_instance;
};

}
