#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/RefDomainAdapter.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Shared/AudioAssetLabels.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/RefIdMime.h"
#include "Widgets/Shared/TreeRowStyle.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDropEvent>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTemporaryDir>

#include <functional>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

AssetInfo makeAsset(uint32_t sourceId, const QString& relativePath)
{
    AssetInfo info;
    info.sourceID = sourceId;
    info.relativePath = relativePath;
    info.metadata.originalFormat = QStringLiteral("WAV");
    info.metadata.durationSec = 1.25;
    info.metadata.sampleRate = 48000;
    info.metadata.channels = 2;
    info.absolutePath = QDir(PathUtils::getAudioSourcesDir(AssetRegistry::instance().projectRoot()))
                            .filePath(relativePath);
    QDir().mkpath(QFileInfo(info.absolutePath).absolutePath());
    QFile file(info.absolutePath);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("audio");
    return info;
}

bool sendDrop(ObjectRefWidget& w, RefDomain domain, const QList<uint32_t>& ids)
{
    QMimeData mime;
    RefIdMime::write(&mime, domain, ids);
    QDropEvent ev(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(w).event(&ev);
    return ev.isAccepted();
}

bool sendProjectItemDrop(ObjectRefWidget& w, const QList<uint32_t>& ids)
{
    QMimeData mime;
    ProjectItemMime::writeObjectIDs(&mime, ids);
    QDropEvent ev(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(w).event(&ev);
    return ev.isAccepted();
}

QTreeWidgetItem* findRowIn(QTreeWidgetItem* row, const QString& label)
{
    if (row->text(0) == label) return row;
    for (int i = 0; i < row->childCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(row->child(i), label))
            return hit;
    }
    return nullptr;
}

QTreeWidgetItem* findRow(QTreeWidget* tree, const QString& label)
{
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        if (QTreeWidgetItem* hit = findRowIn(tree->topLevelItem(i), label))
            return hit;
    }
    return nullptr;
}

int countLeaves(QTreeWidgetItem* row)
{
    int n = 0;
    if (!row->data(0, TreeRowStyle::kBucketRole).toBool())
        ++n;
    for (int i = 0; i < row->childCount(); ++i)
        n += countLeaves(row->child(i));
    return n;
}

int countLeaves(QTreeWidget* tree)
{
    int n = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
        n += countLeaves(tree->topLevelItem(i));
    return n;
}

class AudioAssetRefTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        AssetRegistry::instance().clear();
        ASSERT_TRUE(temp_.isValid());
        AssetRegistry::instance().setProjectRoot(temp_.path());
    }

    void TearDown() override
    {
        AssetRegistry::instance().clear();
    }
    QTemporaryDir temp_;
};

}

TEST_F(AudioAssetRefTest, WidgetShowsAssetNameAndTooltip)
{
    const AssetInfo asset = makeAsset(11, QStringLiteral("fx/whoosh.wav"));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));

    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setCurrentObjectID(11);

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), assetDisplayName(asset));
    EXPECT_EQ(w.toolTip(), assetTooltip(asset));
}

TEST_F(AudioAssetRefTest, MissingAssetKeepsIdentityUntilReplaced)
{
    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setRequired(false);
    w.setCurrentObjectID(99);

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->text().isEmpty());
    EXPECT_FALSE(w.toolTip().isEmpty());
    EXPECT_TRUE(w.property("inputInvalid").toBool());
    EXPECT_EQ(w.currentObjectID(), 99u);
    EXPECT_FALSE(w.canAcceptObject(99));

    const auto asset = makeAsset(11, QStringLiteral("fx/replacement.wav"));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    w.setCurrentObjectID(11);
    EXPECT_EQ(w.currentObjectID(), 11u);
    EXPECT_EQ(name->text(), assetDisplayName(asset));
    EXPECT_EQ(w.toolTip(), assetTooltip(asset));
    EXPECT_FALSE(w.property("inputInvalid").toBool());
    EXPECT_TRUE(w.canAcceptObject(11));
}

TEST_F(AudioAssetRefTest, UnresolvedReferenceStillDisplays)
{
    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setUnresolvedReference(QStringLiteral("xxx"));

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(w.toolTip().contains(QStringLiteral("xxx")));
}

TEST_F(AudioAssetRefTest, AcceptsAudioMimeAndRejectsProjectItemMime)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));

    ObjectRefWidget audio;
    audio.setRefDomain(RefDomain::AudioAsset);
    QList<uint32_t> audioEmitted;
    QObject::connect(&audio, &ObjectRefWidget::objectChanged,
                     [&](uint32_t id) { audioEmitted.append(id); });

    EXPECT_TRUE(audio.canAcceptObject(11));
    EXPECT_TRUE(sendDrop(audio, RefDomain::AudioAsset, {11u}));
    EXPECT_EQ(audio.currentObjectID(), 11u);
    EXPECT_EQ(audioEmitted, (QList<uint32_t>{11u}));

    audioEmitted.clear();
    EXPECT_FALSE(sendProjectItemDrop(audio, {11u}));
    EXPECT_EQ(audio.currentObjectID(), 11u);
    EXPECT_TRUE(audioEmitted.isEmpty());

    ObjectRefWidget project;
    QList<uint32_t> projectEmitted;
    QObject::connect(&project, &ObjectRefWidget::objectChanged,
                     [&](uint32_t id) { projectEmitted.append(id); });
    EXPECT_FALSE(sendDrop(project, RefDomain::AudioAsset, {11u}));
    EXPECT_EQ(project.currentObjectID(), 0u);
    EXPECT_TRUE(projectEmitted.isEmpty());
}

TEST_F(AudioAssetRefTest, AssetModifiedRefreshesDisplay)
{
    AssetInfo asset = makeAsset(11, QStringLiteral("kick.wav"));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));

    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setCurrentObjectID(11);

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(w.toolTip().contains(QStringLiteral("1.25s")));

    AssetInfo* stored = AssetRegistry::instance().getAsset(11);
    ASSERT_NE(stored, nullptr);
    stored->relativePath = QStringLiteral("fx/snare.wav");
    AudioMetadata meta = stored->metadata;
    meta.durationSec = 9.5;
    ASSERT_TRUE(AssetRegistry::instance().updateSourceFile(
        11, meta, stored->absolutePath, QStringLiteral("new-hash"), 1));

    const AssetInfo* updated = AssetRegistry::instance().getAsset(11);
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(name->text(), QStringLiteral("snare.wav"));
    EXPECT_EQ(w.toolTip(), assetTooltip(*updated));
    EXPECT_TRUE(w.toolTip().contains(QStringLiteral("9.50s")));
}

TEST_F(AudioAssetRefTest, OrphanedAssetKeepsNameAndStatusLine)
{
    AssetInfo asset = makeAsset(14, QStringLiteral("fx/gone.wav"));
    asset.isOrphaned = true;
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));

    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setCurrentObjectID(14);

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->text(), assetDisplayName(asset));
    EXPECT_EQ(w.toolTip(), assetTooltip(asset));
}

TEST_F(AudioAssetRefTest, PopulateBuildsFolderHierarchyAndRoles)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(12, QStringLiteral("fx/whoosh.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(13, QStringLiteral("fx/impacts/hit.wav"))));

    QTreeWidget tree;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    refDomainAdapter(RefDomain::AudioAsset).populate(&tree, req);

    EXPECT_EQ(countLeaves(&tree), 3);

    QTreeWidgetItem* kick = findRow(&tree, QStringLiteral("kick.wav"));
    QTreeWidgetItem* fx = findRow(&tree, QStringLiteral("fx"));
    QTreeWidgetItem* whoosh = findRow(&tree, QStringLiteral("whoosh.wav"));
    QTreeWidgetItem* impacts = findRow(&tree, QStringLiteral("impacts"));
    QTreeWidgetItem* hit = findRow(&tree, QStringLiteral("hit.wav"));
    ASSERT_NE(kick, nullptr);
    ASSERT_NE(fx, nullptr);
    ASSERT_NE(whoosh, nullptr);
    ASSERT_NE(impacts, nullptr);
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(kick->parent(), nullptr);
    EXPECT_EQ(whoosh->parent(), fx);
    EXPECT_EQ(impacts->parent(), fx);
    EXPECT_EQ(hit->parent(), impacts);

    EXPECT_TRUE(fx->data(0, TreeRowStyle::kBucketRole).toBool());
    EXPECT_TRUE(impacts->data(0, TreeRowStyle::kBucketRole).toBool());
    EXPECT_FALSE(fx->flags() & Qt::ItemIsSelectable);
    EXPECT_FALSE(impacts->flags() & Qt::ItemIsSelectable);

    EXPECT_EQ(kick->data(0, kRoleObjectID).toUInt(), 11u);
    EXPECT_EQ(whoosh->data(0, kRoleObjectID).toUInt(), 12u);
    EXPECT_EQ(hit->data(0, kRoleObjectID).toUInt(), 13u);
    EXPECT_TRUE(kick->data(0, kRoleSelectable).toBool());
    EXPECT_TRUE(whoosh->data(0, kRoleSelectable).toBool());
    EXPECT_TRUE(hit->data(0, kRoleSelectable).toBool());
}

TEST_F(AudioAssetRefTest, PopulateHonorsIsSelectablePredicate)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(12, QStringLiteral("fx/whoosh.wav"))));

    QTreeWidget tree;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    req.isSelectable = [](uint32_t id) { return id == 12u; };
    refDomainAdapter(RefDomain::AudioAsset).populate(&tree, req);

    QTreeWidgetItem* kick = findRow(&tree, QStringLiteral("kick.wav"));
    QTreeWidgetItem* whoosh = findRow(&tree, QStringLiteral("whoosh.wav"));
    ASSERT_NE(kick, nullptr);
    ASSERT_NE(whoosh, nullptr);
    EXPECT_FALSE(kick->data(0, kRoleSelectable).toBool());
    EXPECT_FALSE(kick->flags() & Qt::ItemIsSelectable);
    EXPECT_FALSE(kick->flags() & Qt::ItemIsEnabled);
    EXPECT_TRUE(whoosh->data(0, kRoleSelectable).toBool());
    EXPECT_TRUE(whoosh->flags() & Qt::ItemIsSelectable);
}

TEST_F(AudioAssetRefTest, PopulateEmptyRegistryYieldsEmptyTree)
{
    QTreeWidget tree;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    refDomainAdapter(RefDomain::AudioAsset).populate(&tree, req);
    EXPECT_EQ(tree.topLevelItemCount(), 0);
}

TEST_F(AudioAssetRefTest, PopulateSortsCaseInsensitiveByPath)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(31, QStringLiteral("fx/Zoo.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(32, QStringLiteral("fx/alpha.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(33, QStringLiteral("fx/Beta.wav"))));

    QTreeWidget tree;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    refDomainAdapter(RefDomain::AudioAsset).populate(&tree, req);

    QTreeWidgetItem* fx = findRow(&tree, QStringLiteral("fx"));
    ASSERT_NE(fx, nullptr);
    ASSERT_EQ(fx->childCount(), 3);
    EXPECT_EQ(fx->child(0)->text(0), QStringLiteral("alpha.wav"));
    EXPECT_EQ(fx->child(1)->text(0), QStringLiteral("Beta.wav"));
    EXPECT_EQ(fx->child(2)->text(0), QStringLiteral("Zoo.wav"));
}

TEST_F(AudioAssetRefTest, PopulateKeepsAssetFolderNamesThatCollideWithBuckets)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(41, QStringLiteral("Actions/beep.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(42, QStringLiteral("Busses/hum.wav"))));

    QTreeWidget tree;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    refDomainAdapter(RefDomain::AudioAsset).populate(&tree, req);

    QTreeWidgetItem* actions = findRow(&tree, QStringLiteral("Actions"));
    ASSERT_NE(actions, nullptr);
    EXPECT_EQ(actions->text(0), QStringLiteral("Actions"));
    EXPECT_TRUE(actions->data(0, TreeRowStyle::kBucketRole).toBool());
    EXPECT_EQ(actions->parent(), nullptr);
    QTreeWidgetItem* beep = findRow(&tree, QStringLiteral("beep.wav"));
    ASSERT_NE(beep, nullptr);
    EXPECT_EQ(beep->parent(), actions);

    QTreeWidgetItem* busses = findRow(&tree, QStringLiteral("Busses"));
    ASSERT_NE(busses, nullptr);
    EXPECT_EQ(busses->text(0), QStringLiteral("Busses"));
    EXPECT_TRUE(busses->data(0, TreeRowStyle::kBucketRole).toBool());
    EXPECT_EQ(busses->parent(), nullptr);
    QTreeWidgetItem* hum = findRow(&tree, QStringLiteral("hum.wav"));
    ASSERT_NE(hum, nullptr);
    EXPECT_EQ(hum->parent(), busses);
}

TEST_F(AudioAssetRefTest, UnregisterReportsMissingUntilReferenceIsCleared)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));

    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    w.setCurrentObjectID(11);

    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_FALSE(name->text().isEmpty());

    AssetRegistry::instance().unregisterAsset(11);

    EXPECT_FALSE(name->text().isEmpty());
    EXPECT_NE(name->text(), QStringLiteral("kick.wav"));
    EXPECT_FALSE(w.toolTip().isEmpty());
    EXPECT_TRUE(w.property("inputInvalid").toBool());
    EXPECT_EQ(w.currentObjectID(), 11u);
    EXPECT_FALSE(w.canAcceptObject(11));

    w.setRequired(false);
    w.setCurrentObjectID(0);
    EXPECT_TRUE(name->text().isEmpty());
    EXPECT_TRUE(w.toolTip().isEmpty());
    EXPECT_FALSE(w.property("inputInvalid").toBool());
}

TEST_F(AudioAssetRefTest, SetRefDomainClearsIdentity)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));

    ObjectRefWidget w;
    w.setCurrentObjectID(11);
    w.setRefDomain(RefDomain::AudioAsset);

    EXPECT_EQ(w.currentObjectID(), 0u);
    auto* name = w.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(name->text().isEmpty());
}

TEST_F(AudioAssetRefTest, SelectorWindowJoinsAudioDomain)
{
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(12, QStringLiteral("fx/whoosh.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(13, QStringLiteral("fx/impacts/hit.wav"))));
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(
        makeAsset(11, QStringLiteral("kick.wav"))));

    uint32_t selected = 0;
    SelectorRequest req;
    req.domain = RefDomain::AudioAsset;
    req.currentObjectID = 13;
    req.onSelected = [&](uint32_t id) { selected = id; };
    SelectorWindow::open(req, nullptr);
    QCoreApplication::processEvents();

    SelectorWindow* selector = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* candidate = qobject_cast<SelectorWindow*>(widget);
        if (candidate && candidate->isVisible()) {
            selector = candidate;
            break;
        }
    }
    ASSERT_NE(selector, nullptr);

    auto* tree = selector->findChild<QTreeWidget*>(QStringLiteral("SelectorTreeWidget"));
    ASSERT_NE(tree, nullptr);
    ASSERT_NE(tree->currentItem(), nullptr);
    EXPECT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), 13u);

    auto* search = selector->findChild<QLineEdit*>();
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("whoo"));

    QTreeWidgetItem* whoosh = findRow(tree, QStringLiteral("whoosh.wav"));
    QTreeWidgetItem* kick = findRow(tree, QStringLiteral("kick.wav"));
    QTreeWidgetItem* fx = findRow(tree, QStringLiteral("fx"));
    ASSERT_NE(whoosh, nullptr);
    ASSERT_NE(kick, nullptr);
    ASSERT_NE(fx, nullptr);
    EXPECT_FALSE(whoosh->isHidden());
    EXPECT_TRUE(kick->isHidden());
    EXPECT_FALSE(fx->isHidden());

    tree->setCurrentItem(whoosh);
    emit tree->itemActivated(whoosh, 0);
    QCoreApplication::processEvents();
    EXPECT_EQ(selected, 12u);

    SelectorWindow::dismiss();
    QCoreApplication::processEvents();
}
