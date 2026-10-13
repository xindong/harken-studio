#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Core/ThemeManager.h"
#include "Core/QtControlSkin.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/ObjectRefNavigation.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/Inspector/Drawers/ListFieldDrawer.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Shared/RefDomain.h"
#include "Widgets/Shared/RefIdMime.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QContextMenuEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QImage>
#include <QLayout>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>

#include <memory>

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

bool sendDrop(ObjectRefWidget& w, const QList<uint32_t>& ids)
{
    QMimeData mime;
    ProjectItemMime::writeObjectIDs(&mime, ids);
    QDropEvent ev(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::NoModifier);
    // QApplication::notify only routes drop events to the live drag target, so
    // synthesized drops must enter through QWidget::event directly.
    static_cast<QObject&>(w).event(&ev);
    return ev.isAccepted();
}

bool sendAudioAssetDrop(ObjectRefWidget& w, const QList<uint32_t>& ids)
{
    QMimeData mime;
    RefIdMime::write(&mime, RefDomain::AudioAsset, ids);
    QDropEvent ev(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(w).event(&ev);
    return ev.isAccepted();
}

bool sendFileUrlDrop(ObjectRefWidget& w, const QString& path)
{
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDropEvent ev(QPointF(5, 5), Qt::CopyAction, &mime,
                  Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject&>(w).event(&ev);
    return ev.isAccepted();
}

AssetInfo makeRegisteredAsset(uint32_t sourceId, const QString& name)
{
    AssetInfo asset;
    asset.sourceID = sourceId;
    asset.relativePath = name;
    asset.metadata.originalFormat = QStringLiteral("WAV");
    asset.metadata.durationSec = 1.0;
    asset.metadata.sampleRate = 48000;
    EXPECT_TRUE(AssetRegistry::instance().registerAsset(asset));
    return asset;
}

SelectorWindow* selectPickerRow(uint32_t id)
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* selector = qobject_cast<SelectorWindow*>(widget);
        if (!selector || !selector->isVisible())
            continue;
        auto* tree = selector->findChild<QTreeWidget*>();
        if (!tree)
            continue;
        for (QTreeWidgetItemIterator row(tree); *row; ++row) {
            if ((*row)->data(0, kRoleObjectID).toUInt() == id
                && (*row)->data(0, kRoleSelectable).toBool()) {
                tree->setCurrentItem(*row);
                return selector;
            }
        }
    }
    return nullptr;
}

class ObjectRefDropTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ObjectRefDropProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));

        masterBus_ = ProjectManager::instance().getMasterBusItem();
        ASSERT_NE(masterBus_, nullptr);
        ASSERT_NE(masterBus_->objectID, 0u);
        ASSERT_EQ(ProjectManager::instance().findItemByObjectID(masterBus_->objectID),
                  masterBus_);

        ProjectItem* layer = ProjectManager::instance().createItem(
            nullptr, QStringLiteral("DropLayer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer, nullptr);
        sound_ = ProjectManager::instance().createItem(
            layer, QStringLiteral("DropSound"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(sound_->objectID, 0u);
        ASSERT_EQ(ProjectManager::instance().findItemByObjectID(sound_->objectID), sound_);
    }

    void TearDown() override
    {
        SelectorWindow::dismiss();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    ProjectItem* masterBus_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

}

TEST(ProjectItemMimeTest, RoundTripAndSingleIdContract)
{
    ensureApp();

    QMimeData multi;
    ProjectItemMime::writeObjectIDs(&multi, {3u, 0u, 7u});
    EXPECT_TRUE(ProjectItemMime::hasObjectIDs(&multi));
    EXPECT_EQ(ProjectItemMime::readObjectIDs(&multi), (QList<uint32_t>{3u, 7u}));
    EXPECT_EQ(ProjectItemMime::readSingleObjectID(&multi), 0u);

    QMimeData single;
    ProjectItemMime::writeObjectIDs(&single, {42u});
    EXPECT_EQ(ProjectItemMime::readSingleObjectID(&single), 42u);

    QMimeData empty;
    ProjectItemMime::writeObjectIDs(&empty, {0u});
    EXPECT_FALSE(ProjectItemMime::hasObjectIDs(&empty));
    EXPECT_TRUE(ProjectItemMime::readObjectIDs(&empty).isEmpty());
    EXPECT_EQ(ProjectItemMime::readSingleObjectID(&empty), 0u);
    EXPECT_EQ(ProjectItemMime::readSingleObjectID(nullptr), 0u);
}

TEST_F(ObjectRefDropTest, AcceptsMatchingTypeAndAppliesSelection)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_TRUE(w.canAcceptObject(masterBus_->objectID));
    EXPECT_TRUE(sendDrop(w, {masterBus_->objectID}));
    EXPECT_EQ(w.currentObjectID(), masterBus_->objectID);
    EXPECT_FALSE(w.isInherited());
    EXPECT_EQ(emitted, (QList<uint32_t>{masterBus_->objectID}));
}

TEST_F(ObjectRefDropTest, HoverHighlightsOnlyTheBorder)
{
    ObjectRefWidget widget;
    widget.setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    widget.setCurrentObjectID(masterBus_->objectID);
    widget.resize(240, widget.sizeHint().height());
    widget.show();
    QApplication::processEvents();
    QLabel* icon = nullptr;
    for (auto* label : widget.findChildren<QLabel*>())
        if (!label->pixmap().isNull()) icon = label;
    ASSERT_NE(icon, nullptr);
    auto* name = widget.findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(name, nullptr);
    const auto iconBefore = icon->pixmap().toImage();
    const auto textBefore = name->palette().color(QPalette::WindowText);
    QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), widget.mapToGlobal(QPoint(2, 2)));
    QApplication::sendEvent(&widget, &enter);
    EXPECT_EQ(icon->pixmap().toImage(), iconBefore);
    EXPECT_EQ(name->palette().color(QPalette::WindowText), textBefore);
    QImage image(widget.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    widget.render(&image);
    EXPECT_EQ(image.pixelColor(0, widget.height() / 2), ThemeManager::instance().colors().accent);
}

TEST_F(ObjectRefDropTest, InvalidReferencesUseTheErrorBorderAndOptionalEmptyReferencesStayValid)
{
    ObjectRefWidget widget;
    widget.setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    widget.resize(240, widget.sizeHint().height());
    widget.show();
    QApplication::processEvents();
    QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), widget.mapToGlobal(QPoint(2, 2)));
    QApplication::sendEvent(&widget, &enter);
    const auto& colors = ThemeManager::instance().colors();
    const auto border = [&] {
        QImage image(widget.size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget.render(&image);
        return image.pixelColor(0, widget.height() / 2);
    };
    EXPECT_EQ(border(), colors.error);
    EXPECT_TRUE(sendDrop(widget, {masterBus_->objectID}));
    EXPECT_EQ(border(), colors.accent);
    widget.setCurrentObjectID(0);
    EXPECT_EQ(border(), colors.error);
    widget.setRequired(false);
    EXPECT_EQ(border(), colors.accent);
    widget.setUnresolvedReference(QStringLiteral("missing-guid"));
    EXPECT_EQ(border(), colors.error);
    widget.setMixedValue();
    EXPECT_EQ(border(), colors.accent);
    widget.setRequired(true);
    widget.setCurrentObjectID(0);
    widget.setInherited(true);
    EXPECT_EQ(border(), colors.accent);
    widget.setInherited(false);
    widget.setCurrentObjectID(masterBus_->objectID);
    widget.setValidationError(QStringLiteral("Invalid target"));
    EXPECT_EQ(border(), colors.error);
    EXPECT_EQ(widget.toolTip(), QStringLiteral("Invalid target"));
    emit ThemeManager::instance().themeChanged(ThemeManager::instance().snapshot());
    EXPECT_EQ(border(), colors.error);
    widget.setCurrentObjectID(masterBus_->objectID);
    EXPECT_EQ(border(), colors.accent);
}

TEST_F(ObjectRefDropTest, DisabledAndReadOnlyReferencesNeverHighlightOnHover)
{
    QWidget parent;
    ObjectRefWidget widget(&parent);
    widget.setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    widget.setCurrentObjectID(masterBus_->objectID);
    widget.resize(240, widget.sizeHint().height());
    parent.resize(260, 60);
    parent.show();
    QApplication::processEvents();
    const auto border = [&] {
        QImage image(widget.size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        widget.render(&image);
        return image.pixelColor(0, widget.height() / 2);
    };
    const auto enter = [&] {
        QEnterEvent event(QPointF(2, 2), QPointF(2, 2), widget.mapToGlobal(QPoint(2, 2)));
        QApplication::sendEvent(&widget, &event);
    };
    for (bool compact : {false, true}) {
        widget.setCompact(compact);
        for (int mode = 0; mode < 4; ++mode) {
            SCOPED_TRACE(::testing::Message() << "compact=" << compact << " mode=" << mode);
            const auto disable = [&](bool disabled) {
                if (mode == 0) widget.setDisabled(disabled);
                if (mode == 1) parent.setDisabled(disabled);
                if (mode == 2) widget.setReadOnly(disabled);
                if (mode == 3) widget.setSelectorEnabled(!disabled);
            };
            enter();
            EXPECT_EQ(border(), ThemeManager::instance().colors().accent);
            disable(true);
            EXPECT_EQ(border(), ThemeManager::instance().colors().borderPrimary);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&widget, &leave);
            enter();
            EXPECT_EQ(border(), ThemeManager::instance().colors().borderPrimary);
            disable(false);
            EXPECT_EQ(border(), ThemeManager::instance().colors().accent);
        }
    }
}

TEST_F(ObjectRefDropTest, NarrowReferencesShrinkWithTheirParentAndKeepThePickerVisible)
{
    QWidget parent;
    parent.setStyleSheet(QtControlSkin::current().applicationStyleSheet());
    auto* layout = new QHBoxLayout(&parent);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* reference = new ObjectRefWidget(&parent);
    reference->setCompactPickable(true);
    sound_->name = QStringLiteral("A referenced object with a long display name");
    reference->setCurrentObjectID(sound_->objectID);
    layout->addWidget(reference);
    auto* picker = reference->findChild<QToolButton*>(QStringLiteral("refPick"));
    auto* name = reference->findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(picker, nullptr);
    ASSERT_NE(name, nullptr);
    parent.resize(260, reference->sizeHint().height());
    parent.show();
    QApplication::processEvents();
    const int height = reference->height();
    for (bool compact : {false, true}) {
        reference->setCompact(compact);
        for (int width : {260, 80, 64, 40, 32, 260}) {
            SCOPED_TRACE(::testing::Message() << "compact=" << compact << " width=" << width);
            parent.resize(width, parent.height());
            QApplication::processEvents();
            EXPECT_EQ(parent.width(), width);
            EXPECT_EQ(reference->width(), width);
            EXPECT_EQ(reference->height(), height);
            EXPECT_TRUE(picker->isVisible());
            EXPECT_TRUE(reference->contentsRect().contains(picker->geometry()));
            for (auto* label : reference->findChildren<QLabel*>()) {
                if (!label->isVisible() || label->width() == 0) continue;
                EXPECT_TRUE(reference->contentsRect().contains(label->geometry()));
                EXPECT_FALSE(label->geometry().intersects(picker->geometry()));
            }
        }
        EXPECT_TRUE(name->isVisible());
        EXPECT_EQ(name->text(), sound_->name);
    }
}

TEST_F(ObjectRefDropTest, ObjectIconsMatchExplorerAndSelectorInBothLayouts)
{
    sound_->color = 0xff123456u;
    ProjectExplorer explorer;
    ObjectRefWidget reference;
    reference.setCompactPickable(true);
    reference.setCurrentObjectID(masterBus_->objectID);
    reference.resize(320, reference.height());
    reference.show();
    SelectorRequest request;
    request.filterType = ProjectItem::Type::Bus;
    auto* selector = SelectorWindow::open(request, &reference);
    auto* tree = selector->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    auto* icon = qobject_cast<QLabel*>(reference.layout()->itemAt(0)->widget());
    ASSERT_NE(icon, nullptr);
    const int extent = ThemeManager::instance().metrics().objectIconExtent;
    const QSize iconSize(extent, extent);
    EXPECT_EQ(explorer.treeWidget()->iconSize(), iconSize);
    EXPECT_EQ(explorer.sourcesTree()->iconSize(), iconSize);
    EXPECT_EQ(tree->iconSize(), iconSize);
    ASSERT_TRUE(explorer.selectProjectItem(sound_));
    const auto* coloredRow = explorer.treeWidget()->currentItem();
    ASSERT_NE(coloredRow, nullptr);
    EXPECT_EQ(coloredRow->icon(0).actualSize(iconSize), iconSize);

    for (bool compact : {false, true}) {
        reference.setCompact(compact);
        for (bool readOnly : {false, true}) {
            reference.setReadOnly(readOnly);
            QApplication::processEvents();
            EXPECT_EQ(icon->size(), iconSize);
            EXPECT_FALSE(icon->pixmap().isNull());
            EXPECT_EQ(icon->pixmap().deviceIndependentSize(), QSizeF(iconSize));
            EXPECT_DOUBLE_EQ(icon->pixmap().devicePixelRatio(), reference.devicePixelRatioF());
            EXPECT_TRUE(reference.contentsRect().contains(icon->geometry()));
            EXPECT_GE(reference.height(), extent + 4);
            QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), reference.mapToGlobal(QPoint(1, 1)));
            QApplication::sendEvent(&reference, &enter);
            EXPECT_EQ(icon->pixmap().deviceIndependentSize(), QSizeF(iconSize));
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(&reference, &leave);
        }
    }

    icon->setFixedSize(1, 1);
    explorer.treeWidget()->setIconSize(QSize(1, 1));
    explorer.sourcesTree()->setIconSize(QSize(1, 1));
    tree->setIconSize(QSize(1, 1));
    auto& theme = ThemeManager::instance();
    emit theme.themeChanged(theme.snapshot());
    QApplication::processEvents();
    EXPECT_EQ(icon->size(), iconSize);
    EXPECT_EQ(icon->pixmap().deviceIndependentSize(), QSizeF(iconSize));
    EXPECT_EQ(explorer.treeWidget()->iconSize(), iconSize);
    EXPECT_EQ(explorer.sourcesTree()->iconSize(), iconSize);
    EXPECT_EQ(tree->iconSize(), iconSize);
}

TEST_F(ObjectRefDropTest, SameIdDropDoesNotReEmit)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    w.setCurrentObjectID(masterBus_->objectID);
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_TRUE(sendDrop(w, {masterBus_->objectID}));
    EXPECT_TRUE(emitted.isEmpty());
}

TEST_F(ObjectRefDropTest, InheritedSameIdDropOverrides)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    w.setCurrentObjectID(masterBus_->objectID);
    w.setInherited(true);
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_TRUE(sendDrop(w, {masterBus_->objectID}));
    EXPECT_FALSE(w.isInherited());
    EXPECT_EQ(emitted, (QList<uint32_t>{masterBus_->objectID}));
}

TEST_F(ObjectRefDropTest, RejectsTypeMismatch)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_FALSE(w.canAcceptObject(sound_->objectID));
    EXPECT_FALSE(sendDrop(w, {sound_->objectID}));
    EXPECT_EQ(w.currentObjectID(), 0u);
    EXPECT_TRUE(emitted.isEmpty());
}

TEST_F(ObjectRefDropTest, FilterTypesListGatesAcrossTypes)
{
    ObjectRefWidget w;
    w.setFilterTypes({ProjectItem::Type::Sound, ProjectItem::Type::Bus});
    EXPECT_TRUE(w.canAcceptObject(sound_->objectID));
    EXPECT_TRUE(w.canAcceptObject(masterBus_->objectID));

    w.setFilterTypes({ProjectItem::Type::Attenuation});
    EXPECT_FALSE(w.canAcceptObject(sound_->objectID));
    EXPECT_FALSE(w.canAcceptObject(masterBus_->objectID));
}

TEST_F(ObjectRefDropTest, IsSelectablePredicateGates)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);
    const uint32_t blocked = masterBus_->objectID;
    w.setIsSelectable([blocked](uint32_t id) { return id != blocked; });

    EXPECT_FALSE(w.canAcceptObject(masterBus_->objectID));
    EXPECT_FALSE(sendDrop(w, {masterBus_->objectID}));
    EXPECT_EQ(w.currentObjectID(), 0u);
}

TEST_F(ObjectRefDropTest, ReadOnlyOrDisabledSelectorRejects)
{
    ObjectRefWidget w;
    w.setFilterType(ProjectItem::Type::Bus);

    w.setReadOnly(true);
    EXPECT_FALSE(w.canAcceptObject(masterBus_->objectID));
    EXPECT_FALSE(sendDrop(w, {masterBus_->objectID}));

    w.setReadOnly(false);
    w.setSelectorEnabled(false);
    EXPECT_FALSE(w.canAcceptObject(masterBus_->objectID));
    EXPECT_FALSE(sendDrop(w, {masterBus_->objectID}));

    w.setSelectorEnabled(true);
    EXPECT_TRUE(w.canAcceptObject(masterBus_->objectID));
}

TEST_F(ObjectRefDropTest, MultiObjectPayloadRejected)
{
    ObjectRefWidget w;
    w.setFilterTypes({ProjectItem::Type::Sound, ProjectItem::Type::Bus});
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_FALSE(sendDrop(w, {masterBus_->objectID, sound_->objectID}));
    EXPECT_EQ(w.currentObjectID(), 0u);
    EXPECT_TRUE(emitted.isEmpty());
}

TEST_F(ObjectRefDropTest, RejectsOsWavUrlOnProjectObjectAndAudioAsset)
{
    const QString wavPath = QDir(tempDir_.path()).filePath(QStringLiteral("os-drop.wav"));
    {
        QFile file(wavPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("RIFF");
    }
    ObjectRefWidget project;
    project.setFilterType(ProjectItem::Type::Bus);
    QList<uint32_t> projectEmitted;
    QObject::connect(&project, &ObjectRefWidget::objectChanged,
                     [&projectEmitted](uint32_t id) { projectEmitted.append(id); });
    EXPECT_FALSE(sendFileUrlDrop(project, wavPath));
    EXPECT_EQ(project.currentObjectID(), 0u);
    EXPECT_TRUE(projectEmitted.isEmpty());

    ObjectRefWidget audio;
    audio.setRefDomain(RefDomain::AudioAsset);
    QList<uint32_t> audioEmitted;
    QObject::connect(&audio, &ObjectRefWidget::objectChanged,
                     [&audioEmitted](uint32_t id) { audioEmitted.append(id); });
    EXPECT_FALSE(sendFileUrlDrop(audio, wavPath));
    EXPECT_EQ(audio.currentObjectID(), 0u);
    EXPECT_TRUE(audioEmitted.isEmpty());
}

TEST_F(ObjectRefDropTest, AcceptsRegisteredAudioAssetMime)
{
    makeRegisteredAsset(9101, QStringLiteral("kick.wav"));

    ObjectRefWidget w;
    w.setRefDomain(RefDomain::AudioAsset);
    QList<uint32_t> emitted;
    QObject::connect(&w, &ObjectRefWidget::objectChanged,
                     [&emitted](uint32_t id) { emitted.append(id); });

    EXPECT_TRUE(w.canAcceptObject(9101));
    EXPECT_TRUE(sendAudioAssetDrop(w, {9101u}));
    EXPECT_EQ(w.currentObjectID(), 9101u);
    EXPECT_EQ(emitted, (QList<uint32_t>{9101u}));
}

TEST_F(ObjectRefDropTest, EditGuardRejectsDropAndRechecksPickerCompletion)
{
    ObjectRefWidget widget;
    bool editable = true;
    int changes = 0;
    widget.setEditGuard([&] { return editable; });
    QObject::connect(&widget, &ObjectRefWidget::objectChanged, [&] { ++changes; });
    auto* pick = widget.findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    pick->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    editable = false;
    EXPECT_FALSE(widget.canAcceptObject(masterBus_->objectID));
    EXPECT_FALSE(sendDrop(widget, {masterBus_->objectID}));
    selector->accept();
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(widget.currentObjectID(), 0u);
}

TEST_F(ObjectRefDropTest, ReadOnlyCancelsPendingPickerWithoutRevivingItOnUnlock)
{
    ObjectRefWidget widget;
    int changes = 0;
    QObject::connect(&widget, &ObjectRefWidget::objectChanged, [&] { ++changes; });
    auto* pick = widget.findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    pick->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    widget.setReadOnly(true);
    EXPECT_FALSE(selector->isVisible());
    widget.setReadOnly(false);
    selector->accept();
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(widget.currentObjectID(), 0u);
}

TEST_F(ObjectRefDropTest, ReplacingEditContextInvalidatesTheOpenPicker)
{
    ObjectRefWidget widget;
    int changes = 0;
    widget.setEditGuard([] { return true; });
    QObject::connect(&widget, &ObjectRefWidget::objectChanged, [&] { ++changes; });
    auto* pick = widget.findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    pick->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    widget.setEditGuard([] { return true; });
    EXPECT_FALSE(selector->isVisible());
    selector->accept();
    EXPECT_EQ(changes, 0);
}

TEST_F(ObjectRefDropTest, CancellingAnOldWidgetDoesNotCloseAnotherWidgetsPicker)
{
    ObjectRefWidget first;
    ObjectRefWidget second;
    int firstChanges = 0;
    int secondChanges = 0;
    QObject::connect(&first, &ObjectRefWidget::objectChanged, [&] { ++firstChanges; });
    QObject::connect(&second, &ObjectRefWidget::objectChanged, [&] { ++secondChanges; });
    auto* firstPick = first.findChild<QToolButton*>(QStringLiteral("refPick"));
    auto* secondPick = second.findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(firstPick, nullptr);
    ASSERT_NE(secondPick, nullptr);
    firstPick->click();
    secondPick->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    first.cancelPendingSelection();
    EXPECT_TRUE(selector->isVisible());
    selector->accept();
    EXPECT_EQ(firstChanges, 0);
    EXPECT_EQ(secondChanges, 1);
    EXPECT_EQ(second.currentObjectID(), masterBus_->objectID);
}

TEST_F(ObjectRefDropTest, NavigationFollowsTheCurrentOwningWindow)
{
    QWidget firstHost;
    QWidget secondHost;
    ObjectRefNavigation firstNavigation(&firstHost);
    ObjectRefNavigation secondNavigation(&secondHost);
    QWidget floatingContainer(&firstHost, Qt::Tool);
    QWidget nestedContainer(&floatingContainer);
    ObjectRefWidget widget(&nestedContainer);
    widget.setCurrentObjectID(masterBus_->objectID);
    int firstLocated = 0;
    int secondLocated = 0;
    QObject::connect(&firstNavigation, &ObjectRefNavigation::locateRequested,
                     [&] { ++firstLocated; });
    QObject::connect(&secondNavigation, &ObjectRefNavigation::locateRequested,
                     [&] { ++secondLocated; });
    const auto click = [&] {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&widget, &press);
    };
    click();
    EXPECT_EQ(firstLocated, 1);
    EXPECT_EQ(secondLocated, 0);
    floatingContainer.setParent(&secondHost, Qt::Tool);
    click();
    EXPECT_EQ(firstLocated, 1);
    EXPECT_EQ(secondLocated, 1);
}

TEST_F(ObjectRefDropTest, DeferredNavigationDoesNotOpenAReplacedOrDestroyedReference)
{
    QWidget host;
    ObjectRefNavigation navigation(&host);
    auto widget = std::make_unique<ObjectRefWidget>(&host);
    widget->setCurrentObjectID(masterBus_->objectID);
    int navigated = 0;
    QObject::connect(&navigation, &ObjectRefNavigation::navigateRequested,
                     [&] { ++navigated; });
    const auto doubleClick = [&] {
        QMouseEvent event(QEvent::MouseButtonDblClick, QPointF(5, 5), QPointF(5, 5),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget.get(), &event);
    };
    doubleClick();
    widget->setCurrentObjectID(sound_->objectID);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(navigated, 0);
    doubleClick();
    widget.reset();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    EXPECT_EQ(navigated, 0);
}

TEST_F(ObjectRefDropTest, ClearReferenceRechecksTheEditGuardAfterOpeningItsMenu)
{
    ObjectRefWidget widget;
    widget.setCurrentObjectID(masterBus_->objectID);
    widget.setClearSupport(true);
    bool editable = true;
    int changes = 0;
    widget.setEditGuard([&] { return editable; });
    QObject::connect(&widget, &ObjectRefWidget::objectChanged, [&] { ++changes; });
    QTimer::singleShot(0, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        ASSERT_NE(menu, nullptr);
        ASSERT_FALSE(menu->actions().isEmpty());
        editable = false;
        menu->setActiveAction(menu->actions().front());
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &press);
    });
    QContextMenuEvent event(QContextMenuEvent::Other, QPoint(5, 5), QPoint(5, 5));
    QApplication::sendEvent(&widget, &event);
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(widget.currentObjectID(), masterBus_->objectID);
}

TEST_F(ObjectRefDropTest, SendTablePickerRechecksGuardAndCancelsOnReadOnly)
{
    registerBuiltinInspectorCustomizations();
    PropertyDescriptor table;
    table.field = QStringLiteral("sends");
    table.type = PropType::List;
    PropertyDescriptor target;
    target.field = QStringLiteral("targetBusID");
    target.type = PropType::ObjectRef;
    target.refFilter = static_cast<int>(ProjectItem::Type::Bus);
    PropertyDescriptor level;
    level.field = QStringLiteral("sendLevel");
    level.type = PropType::Float;
    level.min = -96.0;
    level.max = 12.0;
    level.def = 0.0;
    table.elementSchema = {target, level};
    ListFieldDrawer drawer;
    std::unique_ptr<QWidget> widget(drawer.build(table, nullptr));
    bool editable = true;
    int changes = 0;
    ListFieldDrawer::setWidgetEditGuard(widget.get(), [&] { return editable; });
    drawer.connectChanged(widget.get(), [&](const QVariant&) { ++changes; });
    drawer.bind(widget.get(), QVariantList{}, sound_);
    auto* add = widget->findChild<QToolButton*>(QStringLiteral("listAdd_sends"));
    ASSERT_NE(add, nullptr);
    add->click();
    auto* selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    editable = false;
    selector->accept();
    EXPECT_EQ(changes, 0);

    editable = true;
    add->click();
    selector = selectPickerRow(masterBus_->objectID);
    ASSERT_NE(selector, nullptr);
    ListFieldDrawer::setWidgetReadOnly(widget.get(), true);
    EXPECT_FALSE(selector->isVisible());
    ListFieldDrawer::setWidgetReadOnly(widget.get(), false);
    selector->accept();
    EXPECT_EQ(changes, 0);
}
