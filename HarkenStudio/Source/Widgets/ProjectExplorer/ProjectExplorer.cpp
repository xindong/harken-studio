
#include "ProjectExplorer.h"
#include "ExplorerSection.h"
#include "SourcesTreeWidget.h"
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
#include "Widgets/Shared/AudioAssetLabels.h"
#include "Widgets/Shared/TreeRowPing.h"
#include "Widgets/Shared/TreeRowStyle.h"

#include <QDirIterator>
#include <QFrame>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace VNX {

namespace {

class SourceTreeItem : public QTreeWidgetItem
{
public:
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const bool folder = data(0, kSourceFolderRole).toBool();
        const bool otherFolder = other.data(0, kSourceFolderRole).toBool();
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
            this, &ProjectExplorer::rebuildSourcesTree);

    auto& registry = AssetRegistry::instance();
    connect(&registry, &AssetRegistry::sourceFoldersChanged,
            this, &ProjectExplorer::rebuildSourcesTree);
    connect(&registry, &AssetRegistry::assetRegistered, this, [this](uint32_t) {
        rebuildSourcesTree();
    });
    connect(&registry, &AssetRegistry::assetUnregistered, this, [this](uint32_t) {
        rebuildSourcesTree();
    });
    connect(&registry, &AssetRegistry::assetModified, this, [this](uint32_t sourceId) {
        if (QTreeWidgetItem* row = sourceRows_.value(sourceId, nullptr)) {
            if (const AssetInfo* asset = AssetRegistry::instance().getAsset(sourceId)) {
                if (AssetRegistry::instance().sourceFileExists(sourceId)
                    && row->data(0, kSourcePathRole).toString() == asset->relativePath)
                    updateSourceRow(row, *asset);
                else
                    rebuildSourcesTree();
            }
        } else {
            rebuildSourcesTree();
        }
    });
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this,
            [this](ProjectData*) { rebuildSourcesTree(); });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this,
            [this]() { rebuildSourcesTree(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        const int extent = ThemeManager::instance().metrics().objectIconExtent;
        sourcesTree_->setIconSize(QSize(extent, extent));
        rebuildSourcesTree();
    });
    connect(&AppSettings::instance(), &AppSettings::fontSizeChanged, this,
            [this](PanelId panelId, int size) {
                if (panelId != PanelId::Explorer || !sourcesTree_)
                    return;
                QFont font = sourcesTree_->font();
                font.setPointSize(size);
                sourcesTree_->setFont(font);
            });
    if (sourcesTree_) {
        QFont font = sourcesTree_->font();
        font.setPointSize(AppSettings::instance().panelFontSize(PanelId::Explorer));
        sourcesTree_->setFont(font);
    }

    rebuildSourcesTree();
}

ProjectExplorer::~ProjectExplorer() = default;

void ProjectExplorer::setupPanelChrome()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(searchBox_);
    connect(searchBox_, &QLineEdit::textChanged, this, [this]() {
        applySourcesVisibility();
    });

    auto* content = new QFrame(this);
    content->setObjectName(QStringLiteral("projectExplorerContent"));
    content->setProperty("explorerRole", "content");
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    layout->addWidget(content, 1);

    sectionsSplitter_ = new QSplitter(Qt::Vertical, content);
    sectionsSplitter_->setObjectName(QStringLiteral("projectExplorerSplitter"));
    sectionsSplitter_->setProperty("splitterRole", QStringLiteral("border"));
    sectionsSplitter_->setHandleWidth(1);
    sectionsSplitter_->setChildrenCollapsible(false);
    assetsSection_ = new ExplorerSection(tr("Assets"), sectionsSplitter_);
    assetsSection_->setObjectName(QStringLiteral("projectAssetsSection"));
    auto* assetsLayout = new QVBoxLayout(assetsSection_->content());
    assetsLayout->setContentsMargins(0, 0, 0, 0);
    assetsLayout->setSpacing(0);
    assetsLayout->addWidget(treeWidget_, 1);
    treeWidget_->setMinimumSize(0, 0);

    sourcesSection_ = new ExplorerSection(tr("Sources"), sectionsSplitter_);
    sourcesSection_->setObjectName(QStringLiteral("projectSourcesSection"));
    auto* sourcesLayout = new QVBoxLayout(sourcesSection_->content());
    sourcesLayout->setContentsMargins(0, 0, 0, 0);
    sourcesLayout->setSpacing(0);

    auto* sourcesTree = new SourcesTreeWidget();
    sourcesTree_ = sourcesTree;
    sourcesTree->setRootDropTarget(sourcesSection_->header());
    sourcesTree_->setObjectName("projectSourcesTree");
    sourcesTree_->setHeaderHidden(true);
    sourcesTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    sourcesTree_->setDragEnabled(true);
    sourcesTree_->setDragDropMode(QAbstractItemView::DragDrop);
    sourcesTree_->setAcceptDrops(true);
    sourcesTree_->setAnimated(false);
    const int iconExtent = ThemeManager::instance().metrics().objectIconExtent;
    sourcesTree_->setIconSize(QSize(iconExtent, iconExtent));
    sourcesTree_->setExpandsOnDoubleClick(false);
    sourcesTree_->setRootIsDecorated(true);
    sourcesTree_->setMinimumSize(0, 0);
    sourcesTree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    if (QScrollBar* scrollBar = sourcesTree_->verticalScrollBar()) {
        connect(scrollBar, &QScrollBar::rangeChanged, scrollBar,
                [scrollBar](int minimum, int maximum) {
                    scrollBar->setEnabled(minimum < maximum);
                });
        scrollBar->setEnabled(scrollBar->minimum() < scrollBar->maximum());
    }
    connect(sourcesTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                if (!item)
                    return;
                const uint32_t sourceId = item->data(0, kSourceIdRole).toUInt();
                if (sourceId == 0) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                emit sourceEditorRequested(sourceId);
            });
    sourcesLayout->addWidget(sourcesTree_, 1);

    connect(sourcesSection_, &ExplorerSection::headerContextMenuRequested,
            sourcesTree, &SourcesTreeWidget::showRootContextMenu);
    connect(sourcesTree, &SourcesTreeWidget::contentRevealRequested, this, [this]() {
        sourcesSection_->setExpanded(true);
        for (auto* row = sourcesTree_->currentItem(); row; row = row->parent()) {
            if (row->isHidden()) {
                searchBox_->clear();
                break;
            }
        }
    });

    contentLayout->addWidget(sectionsSplitter_, 1);
    collapsedSpace_ = new QWidget(content);
    collapsedSpace_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    contentLayout->addWidget(collapsedSpace_, 1);

    QSettings settings;
    const auto sizes = settings.value(QStringLiteral("Explorer/sectionSizes")).toList();
    if (sizes.size() == 2 && sizes[0].toInt() > 0 && sizes[1].toInt() > 0)
        expandedSizes_ = {sizes[0].toInt(), sizes[1].toInt()};
    assetsSection_->setExpanded(settings.value(QStringLiteral("Explorer/assetsExpanded"), true).toBool());
    sourcesSection_->setExpanded(settings.value(QStringLiteral("Explorer/sourcesExpanded"), true).toBool());
    connect(assetsSection_, &ExplorerSection::expandedChanged, this, [this]() {
        updateSectionLayout();
        saveSectionLayout();
    });
    connect(sourcesSection_, &ExplorerSection::expandedChanged, this, [this]() {
        updateSectionLayout();
        saveSectionLayout();
    });
    connect(sectionsSplitter_, &QSplitter::splitterMoved, this, [this]() {
        if (assetsSection_->isExpanded() && sourcesSection_->isExpanded()) {
            expandedSizes_ = sectionsSplitter_->sizes();
            saveSectionLayout();
        }
    });
    updateSectionLayout();
}

void ProjectExplorer::updateSectionLayout()
{
    const bool assets = assetsSection_->isExpanded();
    const bool sources = sourcesSection_->isExpanded();
    collapsedSpace_->setVisible(!assets && !sources);
    layout()->activate();
    sectionsSplitter_->parentWidget()->layout()->activate();
    sectionsSplitter_->refresh();
    sectionsSplitter_->handle(1)->setEnabled(assets && sources);
    if (assets && sources)
        sectionsSplitter_->setSizes(expandedSizes_);
    else if (assets || sources)
        sectionsSplitter_->setSizes({assets ? 10000 : assetsSection_->minimumHeight(),
                                     sources ? 10000 : sourcesSection_->minimumHeight()});
}

void ProjectExplorer::saveSectionLayout()
{
    QSettings settings;
    settings.setValue(QStringLiteral("Explorer/assetsExpanded"), assetsSection_->isExpanded());
    settings.setValue(QStringLiteral("Explorer/sourcesExpanded"), sourcesSection_->isExpanded());
    settings.setValue(QStringLiteral("Explorer/sectionSizes"),
        QVariantList{expandedSizes_[0], expandedSizes_[1]});
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

bool ProjectExplorer::pingSource(uint32_t sourceId)
{
    QTreeWidgetItem* row = sourceRows_.value(sourceId, nullptr);
    if (!row) return false;
    if (row->isHidden()) searchBox_->clear();
    sourcesSection_->setExpanded(true);
    TreeRowPing::ping(sourcesTree_, row);
    return true;
}

QIcon ProjectExplorer::decorateItemIcon(const QIcon& icon, const ProjectItem* item) const
{
    return iconWithColorChip(icon, item->color);
}

QIcon ProjectExplorer::iconWithColorChip(const QIcon& base, uint32_t color) const
{
    if (color == 0) return base;

    const int sz = ThemeManager::instance().metrics().objectIconExtent;
    QPixmap px = base.pixmap(QSize(sz, sz), devicePixelRatioF());
    if (px.isNull()) {
        px = QPixmap(QSize(sz, sz) * devicePixelRatioF());
        px.setDevicePixelRatio(devicePixelRatioF());
        px.fill(ThemeManager::instance().colors().bgPrimary);
    }

    const qreal dpr = px.devicePixelRatio() > 0 ? px.devicePixelRatio() : 1.0;
    const int w = static_cast<int>(px.width() / dpr);
    const int h = static_cast<int>(px.height() / dpr);
    const int chip = 7;
    const int margin = 1;
    QRectF chipRect(w - chip - margin, h - chip - margin, chip, chip);

    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor outline = ThemeManager::instance().colors().bgPrimary;
    outline.setAlpha(160);
    p.setPen(outline);
    p.setBrush(QColor::fromRgba(color));
    p.drawRoundedRect(chipRect, 1.5, 1.5);
    p.end();

    return QIcon(px);
}

void ProjectExplorer::rebuildSourcesTree()
{
    if (!sourcesTree_)
        return;

    QSet<QString> knownFolders;
    QSet<QString> expandedFolders;
    QSet<QString> selectedFolders;
    QSet<uint32_t> selectedSources;
    const auto* current = sourcesTree_->currentItem();
    const uint32_t currentId = current ? current->data(0, kSourceIdRole).toUInt() : 0;
    const QString currentPath = current ? current->data(0, kSourcePathRole).toString() : QString();
    const int scroll = sourcesTree_->verticalScrollBar()->value();
    for (QTreeWidgetItemIterator it(sourcesTree_); *it; ++it) {
        auto* row = *it;
        const QString path = row->data(0, kSourcePathRole).toString();
        if (row->data(0, kSourceFolderRole).toBool()) {
            knownFolders.insert(path);
            if (row->isExpanded()) expandedFolders.insert(path);
            if (row->isSelected()) selectedFolders.insert(path);
        } else if (row->isSelected()) {
            selectedSources.insert(row->data(0, kSourceIdRole).toUInt());
        }
    }
    const QSignalBlocker blocker(sourcesTree_);
    sourcesTree_->clear();
    sourceRows_.clear();
    sourceFolders_.clear();

    const QString audioRoot = SourceFileSystem::absolutePath({});
    if (!audioRoot.isEmpty() && QDir(audioRoot).exists()) {
        QDirIterator directories(audioRoot, QDir::Dirs | QDir::NoDotAndDotDot,
                                 QDirIterator::Subdirectories);
        while (directories.hasNext()) {
            directories.next();
            if (directories.fileInfo().isSymLink())
                continue;
            ensureSourcesFolder(QDir(audioRoot).relativeFilePath(directories.filePath()));
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
        auto* row = new SourceTreeItem();
        updateSourceRow(row, *asset);
        if (QTreeWidgetItem* folder = ensureSourcesFolder(assetFolderSegments(*asset).join(QLatin1Char('/'))))
            folder->addChild(row);
        else
            sourcesTree_->addTopLevelItem(row);
        sourceRows_.insert(asset->sourceID, row);
        row->setSelected(selectedSources.contains(asset->sourceID));
    }

    for (auto it = sourceFolders_.cbegin(); it != sourceFolders_.cend(); ++it) {
        it.value()->setExpanded(!knownFolders.contains(it.key()) || expandedFolders.contains(it.key()));
        it.value()->setSelected(selectedFolders.contains(it.key()));
    }
    sourcesTree_->sortItems(0, Qt::AscendingOrder);
    if (auto* restored = currentId ? sourceRows_.value(currentId, nullptr)
                                  : sourceFolders_.value(currentPath, nullptr))
        sourcesTree_->setCurrentItem(restored, 0, QItemSelectionModel::NoUpdate);
    applySourcesVisibility();
    sourcesTree_->verticalScrollBar()->setValue(scroll);
}

void ProjectExplorer::applySourcesVisibility()
{
    if (!sourcesTree_)
        return;

    const QString searchText = searchBox_ ? searchBox_->text() : QString();
    const bool hasSearch = !searchText.isEmpty();

    std::function<bool(QTreeWidgetItem*, bool)> apply = [&](QTreeWidgetItem* item, bool ancestorMatch) -> bool {
        const bool matched = !hasSearch || ancestorMatch || item->text(0).contains(searchText, Qt::CaseInsensitive);
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

    for (int i = 0; i < sourcesTree_->topLevelItemCount(); ++i)
        apply(sourcesTree_->topLevelItem(i), false);
}

void ProjectExplorer::updateSourceRow(QTreeWidgetItem* row, const AssetInfo& asset)
{
    row->setFlags(row->flags() | Qt::ItemIsEditable);
    row->setText(0, assetDisplayName(asset));
    row->setIcon(0, ThemeManager::instance().resolveIcon(
        QString::fromLatin1(ThemeAssetIds::AudioSource)).icon);
    row->setData(0, kSourceIdRole, asset.sourceID);
    row->setData(0, kSourcePathRole, PathUtils::normalizeToUnix(asset.relativePath));
    row->setData(0, kSourceFolderRole, false);
    row->setToolTip(0, assetTooltip(asset));
}

QTreeWidgetItem* ProjectExplorer::ensureSourcesFolder(const QString& path)
{
    const QStringList segments = PathUtils::normalizeToUnix(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (segments.isEmpty())
        return nullptr;

    QTreeWidgetItem* parent = nullptr;
    QString relative;
    for (const QString& segment : segments) {
        relative = relative.isEmpty() ? segment : relative + QLatin1Char('/') + segment;
        QTreeWidgetItem* folder = sourceFolders_.value(relative, nullptr);
        if (!folder) {
            folder = new SourceTreeItem();
            folder->setFlags(folder->flags() | Qt::ItemIsEditable);
            folder->setText(0, segment);
            folder->setData(0, kSourcePathRole, relative);
            folder->setData(0, kSourceFolderRole, true);
            folder->setToolTip(0, relative);
            folder->setIcon(0, ThemeManager::instance().resolveToggleIcon(
                QString::fromLatin1(ThemeAssetIds::TreeFolderOpen),
                QString::fromLatin1(ThemeAssetIds::TreeFolderClosed)).icon);
            if (parent)
                parent->addChild(folder);
            else
                sourcesTree_->addTopLevelItem(folder);
            folder->setExpanded(true);
            sourceFolders_.insert(relative, folder);
        }
        parent = folder;
    }
    return parent;
}

}
