
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QUuid>
#include <functional>
#include <vector>

#include "Core/Components/VSelection.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/ProjectTypes.h"
#include "Core/VariableSemantics.h"

class QMenu;
class QAction;

namespace VNX {

class ProjectItemMenuController : public QObject
{
    Q_OBJECT

public:
    using CreateParentPolicy = ItemTypeCatalog::CreateParentPolicy;

    struct Callbacks;
    using PostCreateHook = std::function<void(ProjectItem* created, const Callbacks& cb)>;

    struct ItemCreationSpec {
        ProjectItem::Type  type;
        const char*        defaultNameKey;
        CreateParentPolicy parentPolicy;
        ProjectItem::Type  requiredParent;
        bool               selectAfter;
        bool               separatorAfter;
        PostCreateHook     postCreate;
        bool               comingSoon = false;
    };

    enum class ContextKind {
        None,
        Object,
        Folder,
        BucketRoot,
        Empty
    };

    struct MenuContext {
        ContextKind         kind = ContextKind::None;
        QList<QUuid>        selectionGuids;
        QUuid               contextItemGuid;
        PhysicalFolder*     contextFolder = nullptr;
        QList<PhysicalFolder*> selectionFolders;
        QString             bucketFolderName;
        QString             folderPath;
        bool                useSelectionForCreation = true;
    };

    using MenuPredicate = std::function<bool(const MenuContext&)>;
    using SubmenuBuilder = std::function<void(QMenu* sub, const MenuContext&)>;

    struct StandardItemSpec {
        QString               label;
        QString               shortcutHintId;
        MenuPredicate         canShow;
        MenuPredicate         canEnable;
        std::function<void()> handler;
        SubmenuBuilder        buildSubmenu;
        bool                  separatorAfter = false;
    };

    using GetSelectedFn       = std::function<ProjectItem*()>;
    using GetSelectionFn      = std::function<QList<ProjectItem*>()>;
    using SelectResultFn      = std::function<void(ProjectItem*)>;
    using FolderPathFn        = std::function<QString(const QString& bucketFolderName)>;
    using CurrentBucketNameFn = std::function<QString()>;

    struct Callbacks {
        GetSelectedFn       getSelected;
        GetSelectionFn      getSelection;
        SelectResultFn      selectResult;
        FolderPathFn        folderPath;
        CurrentBucketNameFn currentBucketName;
        std::function<ProjectItem*(ProjectItem::Type)> creationParent;
        std::function<void(PhysicalFolder*)> selectFolderResult;
    };

    explicit ProjectItemMenuController(Callbacks cb, QObject* parent = nullptr);

    void buildNewChildMenu(QMenu* parentMenu);
    void buildCreationMenu(QMenu* menu, const std::vector<ProjectItem::Type>& types);
    static void addVariableKindActions(QMenu* menu, const std::function<void(VariableKind)>& create);

    void buildStandardItems(QMenu* parentMenu, std::vector<StandardItemSpec> items);

    void setMenuContext(MenuContext context);
    void updateActionVisibility(ProjectItem* contextItem,
                                const QString& bucketFolderName,
                                const QString& folderPath = {});

    void clearMenuContext();

    bool hasMenuContext() const { return hasMenuContext_; }
    const MenuContext& menuContext() const { return menuContext_; }

    ProjectItem* createFromSpec(const ItemCreationSpec& spec,
                              VariableKind variableKind = VariableKind::Smooth);

    ProjectItem* createByType(ProjectItem::Type type);
    PhysicalFolder* createFolder();

    ProjectItem* createContainerWithMode(SelectionMode mode);

    static const std::vector<ItemCreationSpec>& specs();

    static ProjectItem* resolveDefaultParentForType(ProjectItem::Type type,
                                                    ProjectItem* selected);

    static QString typeDisplayName(ProjectItem::Type type);

    QMenu* newChildMenu() const { return newChildMenu_; }

private:
    void refreshShortcutHints(const QString& id = {});
    void applyMenuContext();
    void refreshStandardSubmenus();

    ProjectItem* resolveParent(const ItemCreationSpec& spec) const;
    bool canCreateFromSpec(const ItemCreationSpec& spec) const;

    Callbacks cb_;

    QMenu* newChildMenu_ = nullptr;
    QMenu* standardMenu_ = nullptr;

    std::vector<QAction*> newChildActions_;
    QAction* newFolderAction_ = nullptr;

    std::vector<StandardItemSpec> standardSpecs_;
    std::vector<QAction*>         standardActions_;
    std::vector<QMenu*>           standardSubmenus_;

    MenuContext menuContext_;
    bool hasMenuContext_ = false;
};

}
