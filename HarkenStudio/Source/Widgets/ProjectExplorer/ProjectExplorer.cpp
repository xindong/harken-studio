
#include "ProjectExplorer.h"
#include "Core/ThemeLayout.h"
#include "Core/FontManager.h"
#include "Widgets/Common/PaneSection.h"
#include "Widgets/Common/PaneSectionStack.h"
#include "ResourcesTreeWidget.h"
#include "Core/AppSettings.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/AssetTypes.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/AssetPipeline/SourceFileSystem.h"
#include "Core/ItemTypeIcons.h"
#include "Core/ProjectManager.h"
#include "Core/ThemeAssetCatalog.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TreeExpansion.h"
#include "Widgets/Shared/AudioAssetLabels.h"
#include "Widgets/Shared/TreeRowPing.h"
#include "Widgets/Shared/TreeRowStyle.h"

#include <QApplication>
#include <QDirIterator>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSplitter>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace VNX {

namespace {

class ResourceTreeItem : public QTreeWidgetItem
{
public:
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const bool folder = data(0, kResourceFolderRole).toBool();
        const bool otherFolder = other.data(0, kResourceFolderRole).toBool();
        return folder != otherFolder ? folder
            : text(0).compare(other.text(0), Qt::CaseInsensitive) < 0;
    }
};

}

ProjectExplorer::ProjectExplorer(QWidget* parent)
    : ProjectTreePanel(PanelId::Explorer, parent)
{
    setObjectName("Explorer");
    initPanel();

    connect(ProjectManager::instance().fileWatcher(), &FileWatcher::sourcesChanged,
            this, &ProjectExplorer::rebuildResourcesTree);

    auto& registry = AssetRegistry::instance();
    connect(&registry, &AssetRegistry::sourceFoldersChanged,
            this, &ProjectExplorer::rebuildResourcesTree);
    connect(&registry, &AssetRegistry::assetRegistered, this, [this](uint32_t) {
        rebuildResourcesTree();
    });
    connect(&registry, &AssetRegistry::assetUnregistered, this, [this](uint32_t) {
        rebuildResourcesTree();
    });
    connect(&registry, &AssetRegistry::assetModified, this, [this](uint32_t sourceId) {
        if (QTreeWidgetItem* row = resourceRows_.value(sourceId, nullptr)) {
            if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId)) {
                if (AssetRegistry::instance().sourceFileExists(sourceId)
                    && row->data(0, kResourcePathRole).toString() == asset->relativePath)
                    updateResourceRow(row, *asset);
                else
                    rebuildResourcesTree();
            }
        } else {
            rebuildResourcesTree();
        }
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this,
            [this](ProjectData*) { rebuildResourcesTree(); });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
            [this]() { rebuildResourcesTree(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        const int extent = ThemeManager::instance().metrics().iconExtent;
        resourcesTree_->setIconSize(QSize(extent, extent));
    });
    connect(&ThemeManager::instance(), &ThemeManager::appearanceChanged, this, [this]() {
        auto& theme = ThemeManager::instance();
        const auto resourceIcon = theme.resolveIcon(QString::fromLatin1(ThemeAssetIds::AudioSource)).icon;
        const auto folderIcon = theme.resolveToggleIcon(
            QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
            QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon;
        const QSignalBlocker blocker(resourcesTree_->model());
        for (auto* row : std::as_const(resourceRows_))
            row->setIcon(0, resourceIcon);
        for (auto* row : std::as_const(resourceFolders_))
            row->setIcon(0, folderIcon);
        resourcesTree_->viewport()->update();
    });
    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged, this,
            [this](PanelId panelId, int size) {
                if (panelId != PanelId::Explorer || !resourcesTree_)
                    return;
                QFont font = FontManager::instance().getFont(FontRole::Body, size);
                resourcesTree_->setFont(font);
            });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this] {
        resourcesTree_->setFont(FontManager::instance().getFont(
            FontRole::Body, AppSettings::instance().panelFontSize(PanelId::Explorer)));
    });
    if (resourcesTree_) {
        QFont font = FontManager::instance().getFont(
            FontRole::Body, AppSettings::instance().panelFontSize(PanelId::Explorer));
        resourcesTree_->setFont(font);
    }

    rebuildResourcesTree();
}

ProjectExplorer::~ProjectExplorer() = default;

void ProjectExplorer::setupPanelChrome()
{
    auto* layout = new QVBoxLayout(this);
    ThemeLayout::margins(layout, ThemeLayout::Space::Panel);
    ThemeLayout::spacing(layout, ThemeLayout::Space::Section);
    layout->addWidget(searchBox_);
    connect(searchBox_, &QLineEdit::textChanged, this, [this]() {
        applyResourcesVisibility();
    });

    auto* sections = new PaneSectionStack(QStringLiteral("Explorer"), this);
    sections->setObjectName(QStringLiteral("projectExplorerContent"));
    sections->splitter()->setObjectName(QStringLiteral("projectExplorerSplitter"));
    layout->addWidget(sections, 1);

    assetsSection_ = sections->addSection(tr("Assets"), QStringLiteral("assets"), 300);
    assetsSection_->setObjectName(QStringLiteral("projectAssetsSection"));
    auto* assetsLayout = new QVBoxLayout(assetsSection_->content());
    assetsLayout->setContentsMargins(0, 0, 0, 0);
    assetsLayout->setSpacing(0);
    assetsLayout->addWidget(treeWidget_, 1);
    treeWidget_->setMinimumSize(0, 0);

    resourcesSection_ = sections->addSection(tr("Resources"), QStringLiteral("sources"), 200);
    resourcesSection_->setObjectName(QStringLiteral("projectResourcesSection"));
    auto* resourcesLayout = new QVBoxLayout(resourcesSection_->content());
    resourcesLayout->setContentsMargins(0, 0, 0, 0);
    resourcesLayout->setSpacing(0);

    auto* resourcesTree = new ResourcesTreeWidget();
    resourcesTree_ = resourcesTree;
    resourcesTree->setRootDropTarget(resourcesSection_->header());
    resourcesTree_->setObjectName("projectResourcesTree");
    resourcesTree_->setHeaderHidden(true);
    resourcesTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    resourcesTree_->setDragEnabled(true);
    resourcesTree_->setDragDropMode(QAbstractItemView::DragDrop);
    resourcesTree_->setAcceptDrops(true);
    resourcesTree_->setAnimated(false);
    const int iconExtent = ThemeManager::instance().metrics().iconExtent;
    resourcesTree_->setIconSize(QSize(iconExtent, iconExtent));
    resourcesTree_->setExpandsOnDoubleClick(false);
    resourcesTree_->setRootIsDecorated(true);
    resourcesTree_->setMinimumSize(0, 0);
    resourcesTree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    if (QScrollBar* scrollBar = resourcesTree_->verticalScrollBar()) {
        connect(scrollBar, &QScrollBar::rangeChanged, scrollBar,
                [scrollBar](int minimum, int maximum) {
                    scrollBar->setEnabled(minimum < maximum);
                });
        scrollBar->setEnabled(scrollBar->minimum() < scrollBar->maximum());
    }
    connect(resourcesTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item)
                    return;
                const uint32_t sourceId = item->data(0, kResourceIdRole).toUInt();
                if (sourceId == 0) {
                    TreeExpansion::toggle(item, QApplication::keyboardModifiers());
                    return;
                }
                emit fileViewerRequested(sourceId);
            });
    resourcesLayout->addWidget(resourcesTree_, 1);

    connect(resourcesSection_, &PaneSection::headerContextMenuRequested,
            resourcesTree, &ResourcesTreeWidget::showRootContextMenu);
    connect(resourcesTree, &ResourcesTreeWidget::contentRevealRequested, this, [this]() {
        resourcesSection_->setExpanded(true);
        for (auto* row = resourcesTree_->currentItem(); row; row = row->parent()) {
            if (row->isHidden()) {
                searchBox_->clear();
                break;
            }
        }
    });
}

void ProjectExplorer::addAction(const QString& packName, const QString& actionName)
{
    Q_UNUSED(packName)
    ProjectItem* created = ProjectManager::instance().createItem(
        nullptr, actionName, ProjectItem::Type::Action,
        deriveFolderPathForBucket(ItemTypeCatalog::kActionsFolderName));
    if (created) selectCreatedItem(created);
}

void ProjectExplorer::prepareItemReveal(QTreeWidgetItem*)
{
    assetsSection_->setExpanded(true);
}

bool ProjectExplorer::pingResource(uint32_t sourceId)
{
    QTreeWidgetItem* row = resourceRows_.value(sourceId, nullptr);
    if (!row) return false;
    if (row->isHidden()) searchBox_->clear();
    resourcesSection_->setExpanded(true);
    TreeRowPing::ping(resourcesTree_, row);
    return true;
}

QIcon ProjectExplorer::decorateItemIcon(const QIcon& icon, const ProjectItem* item) const
{
    return iconWithColorChip(icon, item->color);
}

QIcon ProjectExplorer::iconWithColorChip(const QIcon& base, uint32_t color) const
{
    if (color == 0) return base;

    const int sz = ThemeManager::instance().metrics().iconExtent;
    QPixmap px = base.pixmap(QSize(sz, sz), devicePixelRatioF());
    if (px.isNull()) {
        px = QPixmap(QSize(sz, sz) * devicePixelRatioF());
        px.setDevicePixelRatio(devicePixelRatioF());
        px.fill(ThemeManager::instance().colors().bgPrimary);
    }

    const qreal dpr = px.devicePixelRatio() > 0 ? px.devicePixelRatio() : 1.0;
    const int w = static_cast<int>(px.width() / dpr);
    const int h = static_cast<int>(px.height() / dpr);
    const int chip = qRound(std::min(w, h) * 0.35);
    const auto& metrics = ThemeManager::instance().metrics();
    const int margin = metrics.borderWidth;
    QRectF chipRect(w - chip - margin, h - chip - margin, chip, chip);

    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor outline = ThemeManager::instance().colors().bgPrimary;
    outline.setAlphaF(outline.alphaF() * 160 / 255.0);
    p.setPen(outline);
    p.setBrush(QColor::fromRgba(color));
    p.drawRoundedRect(chipRect, metrics.radiusCompact, metrics.radiusCompact);
    p.end();

    return QIcon(px);
}

void ProjectExplorer::rebuildResourcesTree()
{
    if (!resourcesTree_)
        return;

    QSet<QString> knownFolders;
    QSet<QString> expandedFolders;
    QSet<QString> selectedFolders;
    QSet<uint32_t> selectedResources;
    const auto* current = resourcesTree_->currentItem();
    const uint32_t currentId = current ? current->data(0, kResourceIdRole).toUInt() : 0;
    const QString currentPath = current ? current->data(0, kResourcePathRole).toString() : QString();
    const int scroll = resourcesTree_->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(resourcesTree_); *it; ++it) {
        auto* row = *it;
        const QString path = row->data(0, kResourcePathRole).toString();
        if (row->data(0, kResourceFolderRole).toBool()) {
            knownFolders.insert(path);
            if (row->isExpanded()) expandedFolders.insert(path);
            if (row->isSelected()) selectedFolders.insert(path);
        } else if (row->isSelected()) {
            selectedResources.insert(row->data(0, kResourceIdRole).toUInt());
        }
    }
    const QSignalBlocker blocker(resourcesTree_);
    resourcesTree_->clear();
    resourceRows_.clear();
    resourceFolders_.clear();

    const QString audioRoot = SourceFileSystem::absolutePath({});
    if (!audioRoot.isEmpty() && QDir(audioRoot).exists()) {
        QDirIterator directories(audioRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDirIterator::Subdirectories);
        while (directories.hasNext()) {
            directories.next();
            if (directories.fileInfo().isSymLink())
                continue;
            ensureResourceFolder(QDir(audioRoot).relativeFilePath(directories.filePath()));
        }
    }

    QList<const AssetInfo*> assets = AssetRegistry::instance().getAllAssets();
    std::sort(assets.begin(), assets.end(), [](const AssetInfo* a, const AssetInfo* b) {
        return PathUtils::normalizeToUnix(a->relativePath)
            .compare(PathUtils::normalizeToUnix(b->relativePath), Qt::CaseInsensitive) < 0;
    });

    for (const AssetInfo* asset : assets) {
        if (!asset || !AssetRegistry::instance().sourceFileExists(asset->sourceID))
            continue;
        auto* row = new ResourceTreeItem();
        updateResourceRow(row, *asset);
        if (QTreeWidgetItem* folder = ensureResourceFolder(assetFolderSegments(*asset).join(QLatin1Char('/'))))
            folder->addChild(row);
        else
            resourcesTree_->addTopLevelItem(row);
        resourceRows_.insert(asset->sourceID, row);
        row->setSelected(selectedResources.contains(asset->sourceID));
    }

    for (auto it = resourceFolders_.cbegin(); it != resourceFolders_.cend(); ++it) {
        it.value()->setExpanded(!knownFolders.contains(it.key()) || expandedFolders.contains(it.key()));
        it.value()->setSelected(selectedFolders.contains(it.key()));
    }
    resourcesTree_->sortItems(0, Qt::AscendingOrder);
    if (auto* restored = currentId ? resourceRows_.value(currentId, nullptr)
                                  : resourceFolders_.value(currentPath, nullptr))
        resourcesTree_->setCurrentItem(restored, 0, QItemSelectionModel::NoUpdate);
    applyResourcesVisibility();
    resourcesTree_->verticalScrollBar()->setValue(scroll);
}

void ProjectExplorer::applyResourcesVisibility()
{
    if (!resourcesTree_)
        return;

    const QString searchText = searchBox_ ? searchBox_->text() : QString();
    const bool hasSearch = !searchText.isEmpty();

    std::function<bool(QTreeWidgetItem*, bool)> apply = [&](QTreeWidgetItem* item, bool ancestorMatch) -> bool {
        const bool matched = !hasSearch || ancestorMatch
            || QFileInfo(item->data(0, kResourcePathRole).toString()).fileName().contains(searchText, Qt::CaseInsensitive);
        bool hasVisibleChild = false;
        for (int i = 0; i < item->childCount(); ++i) {
            if (apply(item->child(i), matched))
                hasVisibleChild = true;
        }
        const bool visible = matched || hasVisibleChild;

        item->setHidden(!visible);
        if (visible && hasSearch)
            item->setExpanded(true);
        return visible;
    };

    for (int i = 0; i < resourcesTree_->topLevelItemCount(); ++i)
        apply(resourcesTree_->topLevelItem(i), false);
}

void ProjectExplorer::updateResourceRow(QTreeWidgetItem* row, const AssetInfo& asset)
{
    row->setFlags(row->flags() | Qt::ItemIsEditable);
    row->setText(0, assetDisplayName(asset));
    row->setIcon(0, ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AudioSource)).icon);
    row->setData(0, kResourceIdRole, asset.sourceID);
    row->setData(0, kResourcePathRole, PathUtils::normalizeToUnix(asset.relativePath));
    row->setData(0, kResourceFolderRole, false);
    row->setToolTip(0, assetTooltip(asset));
}

QTreeWidgetItem* ProjectExplorer::ensureResourceFolder(const QString& path)
{
    const QStringList segments = PathUtils::normalizeToUnix(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty())
        return nullptr;

    QTreeWidgetItem* parent = nullptr;
    QString relative;
    for (const QString& segment : segments) {
        relative = relative.isEmpty() ? segment : relative + QLatin1Char('/') + segment;
        QTreeWidgetItem* folder = resourceFolders_.value(relative, nullptr);
        if (!folder) {
            folder = new ResourceTreeItem();
            folder->setFlags(folder->flags() | Qt::ItemIsEditable);
            folder->setText(0, segment);
            folder->setData(0, kResourcePathRole, relative);
            folder->setData(0, kResourceFolderRole, true);
            folder->setToolTip(0, relative);
            folder->setIcon(0, ThemeManager::instance().resolveToggleIcon(
                QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
                QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon);
            if (parent)
                parent->addChild(folder);
            else
                resourcesTree_->addTopLevelItem(folder);
            folder->setExpanded(true);
            resourceFolders_.insert(relative, folder);
        }
        parent = folder;
    }
    return parent;
}

}
