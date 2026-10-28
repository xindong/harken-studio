
#include "Hierarchy.h"
#include "Core/ItemTypeCatalog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QChildEvent>
#include <QHash>
#include <QSet>
#include <functional>
#include <vector>

namespace VNX {

Hierarchy::Hierarchy(QWidget* parent)
    : ProjectTreePanel(PanelId::Hierarchy, parent)
{
    setObjectName("Hierarchy");
    initPanel();

    setFocusPolicy(Qt::StrongFocus);
    installFocusFilterRecursively(this);
}

Hierarchy::~Hierarchy() = default;

void Hierarchy::installFocusFilterRecursively(QObject* root)
{
    if (!root) return;
    root->installEventFilter(this);
    for (QObject* child : root->children())
        installFocusFilterRecursively(child);
}

void Hierarchy::focusInEvent(QFocusEvent* event)
{
    emit panelFocused(panelId_);
    QWidget::focusInEvent(event);
}

bool Hierarchy::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type t = event->type();
    if (t == QEvent::FocusIn) {
        emit panelFocused(panelId_);
    } else if (t == QEvent::ChildAdded) {
        if (auto* ce = static_cast<QChildEvent*>(event)) {
            if (QObject* added = ce->child())
                installFocusFilterRecursively(added);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void Hierarchy::setupPanelChrome()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    filterBar_ = new QWidget();
    QHBoxLayout* filterLayout = new QHBoxLayout(filterBar_);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(4);

    filterCombo_ = new QComboBox();
    filterCombo_->setObjectName("projectFilterCombo");
    filterCombo_->addItem(tr("All"), static_cast<int>(ItemTypeCatalog::Bucket::None));
    for (ItemTypeCatalog::Bucket bucket : ItemTypeCatalog::allBuckets()) {
        const QString folder = ItemTypeCatalog::folderNameOf(bucket);
        filterCombo_->addItem(ItemTypeCatalog::bucketDisplayName(folder),
                              static_cast<int>(bucket));
    }
    connect(filterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Hierarchy::onFilterChanged);
    auto* filterLabel = new QLabel(tr("Filter:"));
    filterLabel->setProperty("textRole", QStringLiteral("formLabel"));
    filterLayout->addWidget(filterLabel);
    filterLayout->addWidget(filterCombo_, 1);

    layout->addWidget(filterBar_);
    layout->addWidget(searchBox_);
    layout->addWidget(treeWidget_);
}

void Hierarchy::setFilter(ItemTypeCatalog::Bucket bucket)
{
    const int index = filterCombo_->findData(static_cast<int>(bucket));
    filterCombo_->setCurrentIndex(index >= 0 ? index : 0);
}

ItemTypeCatalog::Bucket Hierarchy::currentFilter() const
{
    return filterCombo_
        ? static_cast<ItemTypeCatalog::Bucket>(filterCombo_->currentData().toInt())
        : ItemTypeCatalog::Bucket::None;
}

void Hierarchy::onFilterChanged(int )
{
    applyVisibility();
}

namespace {
const QSet<ProjectItem::Type>& visibleTypesForBucket(ItemTypeCatalog::Bucket bucket)
{
    static const QHash<ItemTypeCatalog::Bucket, QSet<ProjectItem::Type>> cache = [] {
        QHash<ItemTypeCatalog::Bucket, QSet<ProjectItem::Type>> out;
        for (ItemTypeCatalog::Bucket b : ItemTypeCatalog::allBuckets()) {
            QSet<ProjectItem::Type> types;
            std::vector<ProjectItem::Type> pending;
            const auto seed = [&](ProjectItem::Type t) {
                if (types.contains(t)) return;
                types.insert(t);
                pending.push_back(t);
            };
            for (ProjectItem::Type t : ItemTypeCatalog::allTypes()) {
                if (ItemTypeCatalog::bucketOf(t) == b) seed(t);
            }
            for (ProjectItem::Type t : ItemTypeCatalog::allowedTopLevelTypesForBucket(
                     ItemTypeCatalog::folderNameOf(b))) {
                seed(t);
            }
            while (!pending.empty()) {
                const ProjectItem::Type t = pending.back();
                pending.pop_back();
                for (ProjectItem::Type child : ItemTypeCatalog::allowedChildTypesOf(t))
                    seed(child);
            }
            out.insert(b, types);
        }
        return out;
    }();
    static const QSet<ProjectItem::Type> none;
    const auto it = cache.constFind(bucket);
    return it != cache.cend() ? it.value() : none;
}
}

void Hierarchy::prepareItemReveal(QTreeWidgetItem* item)
{
    for (auto* row = item; row; row = row->parent()) {
        if (row->isHidden()) {
            setFilter(ItemTypeCatalog::Bucket::None);
            break;
        }
    }
}

void Hierarchy::applyVisibility()
{
    updateSearchLabels();
    const ItemTypeCatalog::Bucket filter = currentFilter();
    const bool filterAll = (filter == ItemTypeCatalog::Bucket::None);
    const QString filterFolder = ItemTypeCatalog::folderNameOf(filter);
    const QSet<ProjectItem::Type>& filterTypes = visibleTypesForBucket(filter);
    const QString searchText = searchBox_ ? searchBox_->text() : QString();
    const bool hasSearch = !searchText.isEmpty();

    const auto isTargetCategory = [&](const QString& folderName) -> bool {
        return filterAll || folderName == filterFolder;
    };

    QHash<QTreeWidgetItem*, QString> categoryRootName;
    for (auto it = categoryFolders_.cbegin(); it != categoryFolders_.cend(); ++it) {
        categoryRootName.insert(it.value(), it.key());
    }

    std::function<bool(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) -> bool {
        ProjectItem* projItem = itemMap_.value(item, nullptr);
        const bool physicalFolder = folderMap_.contains(item);
        const auto catIt = categoryRootName.constFind(item);
        const bool isCategoryRoot = (catIt != categoryRootName.cend());
        const bool isScaffold = (!projItem && !physicalFolder && !isCategoryRoot);

        bool hasVisibleChild = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (apply(item->child(i))) hasVisibleChild = true;
        }

        if (isCategoryRoot) {
            const bool anchored = isTargetCategory(catIt.value());
            const bool visible = anchored || hasVisibleChild;
            item->setHidden(!visible);
            return visible;
        }

        bool filterPass;
        if (filterAll) {
            filterPass = true;
        } else if (isScaffold) {
            filterPass = hasVisibleChild;
        } else if (physicalFolder) {
            QTreeWidgetItem* categoryRoot = item;
            while (categoryRoot->parent()) categoryRoot = categoryRoot->parent();
            filterPass = isTargetCategory(categoryRootName.value(categoryRoot))
                       || hasVisibleChild;
        } else {
            filterPass = filterTypes.contains(projItem->type) || hasVisibleChild;
        }

        bool searchPass;
        if (!hasSearch) {
            searchPass = true;
        } else if (isScaffold) {
            searchPass = hasVisibleChild;
        } else {
            searchPass = item->text(0).contains(searchText, Qt::CaseInsensitive)
                       || hasVisibleChild;
        }

        const bool visible = filterPass && searchPass;
        item->setHidden(!visible);
        if (visible && hasSearch) {
            item->setExpanded(true);
        }
        return visible;
    };

    for (int i = 0; i < treeWidget_->topLevelItemCount(); ++i) {
        apply(treeWidget_->topLevelItem(i));
    }
}

}
