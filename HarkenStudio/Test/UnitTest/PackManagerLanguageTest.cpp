#include "Core/AppSettings.h"
#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VSound.h"
#include "Core/PackBuildPlan.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableGroupHeader.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/PackManager/PackManagerPanel.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollBar>
#include <QScopeGuard>
#include <QAbstractButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>

#include <algorithm>
#include <memory>
#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"

namespace {

class PackManagerLanguageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("PackManagerLanguages");
        config.location = temp_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
        ASSERT_TRUE(pm.createProject(config));
        pm.setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
        sound_ = pm.createItem(nullptr, QStringLiteral("Voice"), VNX::ProjectItem::Type::Sound);
        action_ = pm.createItem(nullptr, QStringLiteral("PlayVoice"), VNX::ProjectItem::Type::Action);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(action_, nullptr);
        sound_->comp<VNX::VSound>()->distinguishLanguage = true;
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::Play;
        step.targetObjectID = sound_->objectID;
        action_->comp<VNX::VAction>()->steps = {step};
        previousDevMode_ = VNX::AppSettings::instance().developerMode();
        VNX::AppSettings::instance().setDeveloperMode(true);
        ASSERT_TRUE(pm.setPackBuildOutputDir(QStringLiteral("../Generated")));
        panel_ = std::make_unique<VNX::PackManagerPanel>();
    }

    void TearDown() override
    {
        panel_.reset();
        VNX::AppSettings::instance().setDeveloperMode(previousDevMode_);
        VNX::ProjectManager::instance().closeProject();
    }

    QTreeWidgetItem* actionRow(const VNX::ProjectItem* action = nullptr,
                               const std::optional<QString>& language = std::nullopt) const
    {
        if (!action) action = action_;
        auto* tree = panel_->findChild<QTreeWidget*>();
        if (!tree) return nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 4).toUInt() == action->objectID
                && (!language || (*it)->data(0, Qt::UserRole + 6).toString() == *language)) return *it;
        return nullptr;
    }

    QString baseName() const
    {
        return action_->name;
    }

    void writeArtifact(const QString& language, const QString& packName = {},
                       const std::vector<harkenstudio::ObjectId>& members = {})
    {
        const QString artifactName = packName.isEmpty() ? baseName() : packName;
        QString relative = QStringLiteral("Generated/Windows/");
        if (!language.isEmpty()) relative += language + QLatin1Char('/');
        ASSERT_TRUE(QDir().mkpath(temp_.filePath(relative)));
        QFile file(temp_.filePath(relative + artifactName + QStringLiteral(".bytes")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("pack"), 4);
        file.close();
        const QString path = VNX::PackBuildPlanning::packFileName(
            language, artifactName, QStringLiteral(".bytes"));
        QJsonArray actions;
        for (const auto member : members.empty()
                ? std::vector<harkenstudio::ObjectId>{action_->runtimeId()} : members)
            actions.append(QJsonObject{
                {QStringLiteral("actionStableId"), VNX::objectIdString(member)},
                {QStringLiteral("ownerPackStableId"), QStringLiteral("1")}});
        const QJsonObject manifest{
            {QStringLiteral("packs"), QJsonArray{QJsonObject{
                {QStringLiteral("packStableId"), QStringLiteral("1")},
                {QStringLiteral("path"), path}}}},
            {QStringLiteral("gameplayCatalog"), QJsonObject{
                {QStringLiteral("actions"), actions}}}};
        QFile manifestFile(temp_.filePath(relative + HarkenEditor::Bridge::packManifestFileName()));
        ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(manifest).toJson();
        ASSERT_EQ(manifestFile.write(bytes), bytes.size());
    }

    void setLocalized(bool enabled)
    {
        sound_->comp<VNX::VSound>()->distinguishLanguage = enabled;
        emit VNX::ProjectManager::instance().itemPropertyChanged(
            sound_, QStringLiteral("distinguishLanguage"), nullptr);
    }

    void resizePanel(int width)
    {
        panel_->resize(width, 480);
        panel_->show();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        EXPECT_EQ(panel_->width(), width);
    }

    void expectNaturalWidth(VNX::SegmentedControl* control)
    {
        SCOPED_TRACE(control->objectName().toStdString());
        EXPECT_TRUE(VNX::TestSupport::spinUntil([control]() {
            for (int i = 0; auto* button = control->button(i); ++i)
                if (!button->isVisible() || button->width() < button->sizeHint().width()) return false;
            return true;
        }));
        int width = 0;
        for (int i = 0; auto* button = control->button(i); ++i) {
            EXPECT_TRUE(button->isVisible());
            EXPECT_GE(button->width(), button->sizeHint().width()) << button->text().toStdString();
            width += button->sizeHint().width();
        }
        EXPECT_NEAR(control->width(), width, 1);
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* action_ = nullptr;
    bool previousDevMode_ = false;
    std::unique_ptr<VNX::PackManagerPanel> panel_;
};

TEST_F(PackManagerLanguageTest, GroupCaptionsMatchSharedHeadersAcrossResizeAndColumnMovement)
{
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto* tree = panel_->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    for (int width : {1000, 520, 760}) {
        resizePanel(width);
        tree->header()->moveSection(tree->header()->visualIndex(0), width == 520 ? 2 : 0);
        tree->setColumnHidden(1, width == 520);
        QApplication::processEvents();
        QImage image(tree->viewport()->size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        tree->viewport()->render(&image);
        QStringList captions;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            auto* row = *it;
            if (!row->data(0, VNX::TableGroupRole).toBool()) {
                const QRect rect = tree->visualRect(tree->indexFromItem(row));
                EXPECT_EQ(rect.left(), tree->header()->sectionViewportPosition(0));
                EXPECT_EQ(rect.width(), tree->header()->sectionSize(0));
                QImage border(std::min(image.width(), tree->header()->length()), 1, image.format());
                border.fill(VNX::ThemeManager::instance().colors().borderPrimary);
                EXPECT_EQ(image.copy(0, rect.bottom(), border.width(), 1), border);
                continue;
            }
            captions.append(row->text(0));
            SCOPED_TRACE(row->text(0).toStdString());
            const QRect rect = tree->visualItemRect(row);
            ASSERT_FALSE(rect.isEmpty());
            VNX::TableGroupHeader reference(row->text(0), nullptr, &VNX::ThemeManager::instance(),
                {row->data(0, Qt::CheckStateRole).isValid(), row->data(0, VNX::TableGroupCollapsibleRole).toBool()});
            reference.setStyleSheet(panel_->styleSheet());
            reference.setCheckState(row->checkState(0));
            reference.setCheckEnabled(row->flags().testFlag(Qt::ItemIsUserCheckable));
            reference.setExpanded(row->isExpanded());
            reference.resize(image.width(), reference.height());
            EXPECT_EQ(rect.height(), reference.height());
            EXPECT_EQ(image.copy(0, rect.top(), image.width(), rect.height()),
                reference.grab().toImage().convertToFormat(image.format()));
            EXPECT_FALSE(row->flags().testFlag(Qt::ItemIsSelectable));
        }
        EXPECT_EQ(captions, (QStringList{"General", "English", "Japanese"}));
        ASSERT_NE(actionRow(), nullptr);
        EXPECT_FALSE(actionRow()->data(0, VNX::TableGroupRole).toBool());
    }
}

TEST_F(PackManagerLanguageTest, SectionSelectionKeepsPlatformScopeAndUndoAcrossFilteringAndRebuilds)
{
    auto& pm = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerProjectActions(actions, &pm, nullptr, {});
    const auto cleanup = qScopeGuard([&] { actions.clearAllForTesting(); });
    pm.setProjectPlatformsInternal(HarkenTest::platforms({"Windows", "Linux"}));
    VNX::PlatformContextService::instance().setSelected({HarkenTest::PlatformIds::Windows},
        HarkenTest::PlatformIds::Windows, false);
    auto* other = pm.createItem(nullptr, "Other", VNX::ProjectItem::Type::Action);
    ASSERT_NE(other, nullptr);
    other->comp<VNX::VAction>()->steps = action_->comp<VNX::VAction>()->steps;
    panel_->refreshPackList();
    auto* tree = panel_->findChild<VNX::ProportionalTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    auto* search = panel_->findChild<QLineEdit*>("packSearchEdit");
    ASSERT_NE(search, nullptr);
    resizePanel(760);
    auto* content = actionRow(other)->parent();
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(content->checkState(0), Qt::Checked);
    content->setExpanded(false);
    search->setText(action_->name);
    EXPECT_TRUE(actionRow(other)->isHidden());
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    ASSERT_NE(stack, nullptr);
    const int undoIndex = stack->index();
    emit tree->groupCheckStateRequested(content, Qt::Unchecked);
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&] { return stack->index() == undoIndex + 1; }));
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString linuxId = HarkenTest::platformIdString("Linux");
    EXPECT_FALSE(pm.currentProject()->includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_TRUE(pm.currentProject()->includesActionPack(action_->runtimeId(), linuxId, "English"));
    EXPECT_TRUE(pm.currentProject()->includesActionPack(action_->runtimeId(), windows, "Japanese"));
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("Japanese"))->checkState(0), Qt::Checked);
    const auto targeting = pm.getActionPackTargeting();
    const auto target = std::find_if(targeting.cbegin(), targeting.cend(),
        [&](const auto& entry) { return entry.stableId == other->runtimeId(); });
    ASSERT_NE(target, targeting.cend());
    EXPECT_FALSE(target->includesPlatform(windows));
    EXPECT_TRUE(target->includesPlatform(linuxId));
    content = actionRow(other)->parent();
    EXPECT_FALSE(content->isExpanded());
    EXPECT_EQ(content->checkState(0), Qt::Unchecked);
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Linux},
        HarkenTest::PlatformIds::Windows, true);
    EXPECT_EQ(actionRow(other)->parent()->checkState(0), Qt::PartiallyChecked);
    stack->undo();
    content = actionRow(other)->parent();
    EXPECT_FALSE(content->isExpanded());
    EXPECT_EQ(content->checkState(0), Qt::Checked);
    EXPECT_EQ(actionRow(other)->checkState(0), Qt::Checked);
    search->clear();
    EXPECT_FALSE(actionRow(other)->isHidden());
    auto* init = tree->topLevelItem(0);
    EXPECT_EQ(init->text(0), QStringLiteral("Init"));
    EXPECT_FALSE(init->data(0, Qt::CheckStateRole).isValid());
}

TEST_F(PackManagerLanguageTest, CheckGesturesPreservePlatformScopeAndUndoAcrossRebuilds)
{
    auto& pm = VNX::ProjectManager::instance();
    auto& actions = VNX::ActionManager::instance();
    actions.clearAllForTesting();
    VNX::registerProjectActions(actions, &pm, nullptr, {});
    const auto cleanup = qScopeGuard([&] { actions.clearAllForTesting(); });
    pm.setProjectPlatformsInternal(HarkenTest::platforms({"Windows", "Linux"}));
    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    auto* other = pm.createItem(nullptr, "Other", VNX::ProjectItem::Type::Action);
    ASSERT_NE(other, nullptr);
    other->comp<VNX::VAction>()->steps = action_->comp<VNX::VAction>()->steps;
    panel_->refreshPackList();
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    resizePanel(760);
    auto* tree = panel_->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    tree->header()->moveSection(tree->header()->visualIndex(0), 2);
    QApplication::processEvents();
    const auto checkPoint = [&](QTreeWidgetItem* item) {
        QStyleOptionViewItem option;
        option.initFrom(tree);
        option.widget = tree;
        option.rect = tree->visualRect(tree->indexFromItem(item));
        option.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay;
        option.checkState = item->checkState(0);
        return tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, tree).center();
    };
    const auto mouse = [&](QEvent::Type type, const QPoint& point, Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, tree->viewport()->mapToGlobal(point),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(tree->viewport(), &event);
    };
    const auto clickCheck = [&](QTreeWidgetItem* item) {
        const QPoint point = checkPoint(item);
        mouse(QEvent::MouseButtonPress, point, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, point, Qt::NoButton);
        QApplication::processEvents();
    };
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString linuxId = HarkenTest::platformIdString("Linux");
    const auto included = [&](const QString& platform) {
        const auto targeting = pm.getActionPackTargeting();
        return std::none_of(targeting.begin(), targeting.end(), [&](const auto& entry) {
            return entry.stableId == other->runtimeId() && !entry.includesPlatform(platform);
        });
    };
    const QStringList exportPlatforms = panel_->generationPlatforms();
    auto* stack = VNX::UndoRedoManager::instance().undoStack();
    ASSERT_NE(stack, nullptr);
    const int undoIndex = stack->index();
    clickCheck(actionRow(other));
    EXPECT_FALSE(included(windows));
    EXPECT_TRUE(included(linuxId));
    context.setAll(true);
    ASSERT_EQ(actionRow(other)->checkState(0), Qt::PartiallyChecked);
    clickCheck(actionRow(other));
    EXPECT_TRUE(included(windows));
    EXPECT_TRUE(included(linuxId));
    stack->undo();
    EXPECT_EQ(actionRow(other)->checkState(0), Qt::PartiallyChecked);
    tree->setCurrentItem(actionRow(other));
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(tree, &space);
    QApplication::processEvents();
    EXPECT_TRUE(included(windows));
    EXPECT_TRUE(included(linuxId));
    while (stack->index() > undoIndex) stack->undo();

    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const QPoint start = checkPoint(actionRow());
    const QPoint end = checkPoint(actionRow(other));
    const QPoint section = tree->visualItemRect(actionRow(other)->parent()).center();
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton);
    QApplication::processEvents();
    mouse(QEvent::MouseMove, end, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, section, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_FALSE(pm.currentProject()->includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_TRUE(pm.currentProject()->includesActionPack(action_->runtimeId(), linuxId, "English"));
    EXPECT_FALSE(included(windows));
    EXPECT_TRUE(included(linuxId));
    EXPECT_TRUE(actionRow()->data(0, Qt::CheckStateRole).isValid());
    while (stack->index() > undoIndex) stack->undo();
    mouse(QEvent::MouseMove, end, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_EQ(stack->index(), undoIndex);
    EXPECT_TRUE(pm.currentProject()->includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_TRUE(included(windows));
    EXPECT_EQ(panel_->generationPlatforms(), exportPlatforms);
    EXPECT_FALSE(tree->topLevelItem(0)->data(0, Qt::CheckStateRole).isValid());
}

TEST_F(PackManagerLanguageTest, ContinuousColumnResizeIsStableWithDeveloperColumnsHiddenOrReordered)
{
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto* tree = panel_->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    resizePanel(1123);
    auto* header = tree->header();
    auto* viewport = header->viewport();
    for (bool developer : {true, false}) {
        SCOPED_TRACE(developer);
        VNX::AppSettings::instance().setDeveloperMode(developer);
        if (!developer) header->moveSection(header->visualIndex(4), 2);
        QApplication::processEvents();
        const int column = 1;
        const int neighbor = header->logicalIndex(header->visualIndex(column) + 1);
        QList<int> initial;
        QList<int> positions;
        for (int i = 0; i < tree->columnCount(); ++i) {
            initial.append(tree->columnWidth(i));
            positions.append(header->sectionViewportPosition(i));
        }
        const int distance = std::min(20,
            initial[neighbor] - VNX::ThemeManager::instance().metrics().table.minimumColumnWidth);
        ASSERT_GT(distance, 0);
        const QPoint edge(positions[column] + initial[column] - 1, header->height() / 2);
        QMouseEvent press(QEvent::MouseButtonPress, edge, viewport->mapToGlobal(edge),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        for (int step = 0; step <= distance * 4; ++step) {
            const int phase = step % (distance * 2);
            const int offset = phase <= distance ? phase : distance * 2 - phase;
            const QPoint point = edge + QPoint(offset, 0);
            QMouseEvent move(QEvent::MouseMove, point, viewport->mapToGlobal(point),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &move);
            QApplication::processEvents();
            for (int i = 0; i < tree->columnCount(); ++i) {
                if (tree->isColumnHidden(i)) continue;
                const int expected = initial[i] + (i == column ? offset : i == neighbor ? -offset : 0);
                ASSERT_EQ(tree->columnWidth(i), expected) << "step " << step << ", column " << i;
                EXPECT_EQ(header->sectionViewportPosition(i), positions[i] + (i == neighbor ? offset : 0));
            }
            EXPECT_EQ(header->length(), tree->viewport()->width());
            EXPECT_FALSE(tree->horizontalScrollBar()->isVisible());
        }
        QMouseEvent release(QEvent::MouseButtonRelease, edge, viewport->mapToGlobal(edge),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &release);
    }
}

TEST_F(PackManagerLanguageTest, ExportPlatformSelectorKeepsContentWidthsAcrossResizeAndRefresh)
{
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto& manager = VNX::ProjectManager::instance();
    auto platforms = manager.currentProject()->platforms;
    for (int stage = 0; stage < 3; ++stage) {
        SCOPED_TRACE(stage);
        if (stage == 1) platforms[0].name = QStringLiteral("Windows_Desktop_Quality");
        if (stage == 2) {
            platforms.append({QUuid::createUuid(), QStringLiteral("Android_Low"), VNX::BasePlatform::Android});
            platforms.append({QUuid::createUuid(), QStringLiteral("Android_High"), VNX::BasePlatform::Android});
        }
        manager.setProjectPlatformsInternal(platforms);
        manager.setPackExportSelectedPlatformsInternal(manager.currentProject()->platformIds());
        for (const int width : {1000, 1800, 700}) {
            SCOPED_TRACE(width);
            resizePanel(width);
            auto* selector = panel_->findChild<VNX::SegmentedControl*>("packPlatformChips");
            ASSERT_NE(selector, nullptr);
            expectNaturalWidth(selector);
            auto* exportButton = panel_->findChild<QAbstractButton*>("projectPackGenerateAllBtn");
            ASSERT_NE(exportButton, nullptr);
            EXPECT_LT(selector->mapTo(panel_.get(), QPoint(selector->width(), 0)).x(),
                      exportButton->mapTo(panel_.get(), QPoint()).x());
        }
    }
}

TEST_F(PackManagerLanguageTest, ExportPlatformSelectorCompressesAndRecoversWithoutChangingSelection)
{
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto& manager = VNX::ProjectManager::instance();
    QVector<VNX::ProjectPlatform> platforms;
    for (int i = 0; i < 9; ++i)
        platforms.append({QUuid::createUuid(), QStringLiteral("Android_Quality_%1_LongName").arg(i), VNX::BasePlatform::Android});
    manager.setProjectPlatformsInternal(platforms);
    const auto selected = manager.currentProject()->platformIds();
    manager.setPackExportSelectedPlatformsInternal(selected);
    for (const int width : {2800, 420, 2800, 420, 2800}) {
        SCOPED_TRACE(width);
        resizePanel(width);
        auto* control = panel_->findChild<VNX::SegmentedControl*>("packPlatformChips");
        ASSERT_NE(control, nullptr);
        EXPECT_TRUE(panel_->rect().contains(QRect(control->mapTo(panel_.get(), QPoint()), control->size())));
        if (width > 420) {
            expectNaturalWidth(control);
        } else {
            ASSERT_LT(control->width(), control->sizeHint().width());
            for (int i = 0; i < selected.size(); ++i) {
                auto* button = control->button(i);
                ASSERT_NE(button, nullptr);
                EXPECT_TRUE(button->isVisible());
                EXPECT_TRUE(button->isChecked());
                EXPECT_EQ(button->property("segmentValue").toString(), selected[i]);
                EXPECT_TRUE(control->rect().contains(button->geometry()));
                EXPECT_NEAR(button->width(), double(control->width()) * button->sizeHint().width() / control->sizeHint().width(), 1);
            }
        }
        EXPECT_EQ(panel_->generationPlatforms(), selected);
    }
}

TEST_F(PackManagerLanguageTest, LanguageChangesRefreshActualVariantPaths)
{
    writeArtifact(QStringLiteral("English"));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(nullptr, QStringLiteral("English")), nullptr);
    ASSERT_NE(actionRow(nullptr, QStringLiteral("Japanese")), nullptr);
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("English"))->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("Japanese"))->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow(nullptr, QStringLiteral("Japanese"))->text(2).isEmpty());
    EXPECT_TRUE(actionRow(nullptr, QStringLiteral("Japanese"))->text(3).isEmpty());
    auto& context = VNX::LanguageContextService::instance();
    context.setLanguage(QStringLiteral("Japanese"));
    EXPECT_NE(actionRow(nullptr, QStringLiteral("English")), nullptr);
    EXPECT_NE(actionRow(nullptr, QStringLiteral("Japanese")), nullptr);

    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("Japanese")), nullptr);
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));
    pm.setProjectLanguagesInternal({QStringLiteral("Japanese"), QStringLiteral("English")});
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("English"))->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("Japanese"))->text(1), QStringLiteral("No"));
}

TEST_F(PackManagerLanguageTest, ToggleRefreshesBetweenFlatAndLocalizedArtifacts)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English")});
    writeArtifact({});
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("No"));

    setLocalized(false);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    setLocalized(true);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("No"));
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));
}

TEST_F(PackManagerLanguageTest, ReservedActionNameCannotReuseSystemPack)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.renameItem(action_, QStringLiteral("Init")));
    setLocalized(false);
    writeArtifact({});
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow()->toolTip(4).contains(QStringLiteral("reserved")));
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());

    ASSERT_TRUE(pm.renameItem(action_, QStringLiteral("init")));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());
}

TEST_F(PackManagerLanguageTest, PackFilesRequirePublishedActionOwnership)
{
    auto& pm = VNX::ProjectManager::instance();
    setLocalized(false);
    writeArtifact({});
    const QString originalName = action_->name;
    auto* other = pm.createItem(nullptr, originalName.toLower(), VNX::ProjectItem::Type::Action);
    ASSERT_NE(other, nullptr);
    other->comp<VNX::VAction>()->steps = action_->comp<VNX::VAction>()->steps;
    const QString originalPath = temp_.filePath(QStringLiteral("Generated/Windows/")
                                               + originalName + QStringLiteral(".bytes"));
    const QString otherPath = temp_.filePath(QStringLiteral("Generated/Windows/")
                                            + other->name + QStringLiteral(".bytes"));
    if (!QFile::exists(otherPath))
        ASSERT_TRUE(QFile::copy(originalPath, otherPath));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    ASSERT_NE(actionRow(other), nullptr);
    EXPECT_EQ(actionRow(other)->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow(other)->data(0, Qt::UserRole + 5).toString().isEmpty());

    ASSERT_TRUE(pm.deleteItem(action_));
    ASSERT_TRUE(pm.renameItem(other, originalName));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(other), nullptr);
    EXPECT_EQ(actionRow(other)->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow(other)->data(0, Qt::UserRole + 5).toString().isEmpty());
}

TEST_F(PackManagerLanguageTest, MissingLanguagesCannotReuseAnOldFlatArtifact)
{
    writeArtifact({});
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("Korean")});
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());

    setLocalized(false);
    EXPECT_TRUE(QFile::exists(actionRow()->data(0, Qt::UserRole + 5).toString()));
}

TEST_F(PackManagerLanguageTest, ActionTargetsAndDependenciesRefreshTheirLanguageDimensions)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    writeArtifact({});
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("No"));

    auto* effect = pm.createItem(nullptr, QStringLiteral("Effect"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(effect, nullptr);
    action_->comp<VNX::VAction>()->steps.front().targetObjectID = effect->objectID;
    emit pm.itemPropertyChanged(action_, QStringLiteral("steps"), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    auto* voice = pm.createItem(nullptr, QStringLiteral("LocalizedVoice"), VNX::ProjectItem::Type::Action);
    auto* trigger = pm.createItem(nullptr, QStringLiteral("Trigger"), VNX::ProjectItem::Type::Action);
    ASSERT_NE(voice, nullptr);
    ASSERT_NE(trigger, nullptr);
    VNX::VActionStep playVoice;
    playVoice.type = VNX::VActionStep::Type::Play;
    playVoice.targetObjectID = sound_->objectID;
    voice->comp<VNX::VAction>()->steps = {playVoice};
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = action_->objectID;
    trigger->comp<VNX::VAction>()->steps = {post};
    trigger->comp<VNX::VAction>()->steps.front().targetObjectID = voice->objectID;
    writeArtifact(QStringLiteral("English"), trigger->name, {trigger->runtimeId()});
    panel_->refreshPackList();
    ASSERT_NE(actionRow()->parent(), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));
    EXPECT_EQ(actionRow(voice)->text(6), QStringLiteral("English/LocalizedVoice.bytes"));
    ASSERT_NE(actionRow(trigger), nullptr);
    EXPECT_EQ(actionRow(trigger)->text(6), QStringLiteral("English/Trigger.bytes"));
    EXPECT_EQ(actionRow(trigger)->text(1), QStringLiteral("Yes"));

    setLocalized(false);
    EXPECT_EQ(actionRow(voice)->text(6), QStringLiteral("LocalizedVoice.bytes"));
    EXPECT_EQ(actionRow(trigger)->text(6), QStringLiteral("Trigger.bytes"));
    EXPECT_EQ(actionRow(trigger)->text(1), QStringLiteral("No"));
}

TEST_F(PackManagerLanguageTest, MovingLocalizedSourcesRefreshesTheActionClosure)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    auto* container = pm.createItem(nullptr, QStringLiteral("Mixed"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(pm.createItem(container, QStringLiteral("Effect"), VNX::ProjectItem::Type::Sound), nullptr);
    action_->comp<VNX::VAction>()->steps.front().targetObjectID = container->objectID;
    emit pm.itemPropertyChanged(action_, QStringLiteral("steps"), nullptr);
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    ASSERT_TRUE(pm.moveItem(sound_, container));
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));

    auto* outside = pm.createItem(nullptr, QStringLiteral("Outside"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(outside, nullptr);
    ASSERT_TRUE(pm.moveItem(sound_, outside));
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    ASSERT_TRUE(pm.moveItem(sound_, container));
    ASSERT_TRUE(pm.deleteItem(sound_));
    QCoreApplication::processEvents();
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));
}

TEST_F(PackManagerLanguageTest, RenamingContentResolvesNameOnlyTargetsAndRefreshesVariants)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    auto& step = action_->comp<VNX::VAction>()->steps.front();
    step.targetObjectID = 0;
    step.targetName = QStringLiteral("RenamedVoice");
    emit pm.itemPropertyChanged(action_, QStringLiteral("steps"), nullptr);
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    ASSERT_TRUE(pm.renameItem(sound_, QStringLiteral("RenamedVoice")));
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));
}

TEST_F(PackManagerLanguageTest, ExternalTransitionBridgesRefreshTheLanguageDimension)
{
    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    auto* container = pm.createItem(nullptr, QStringLiteral("External"), VNX::ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    action_->comp<VNX::VAction>()->steps.front().targetObjectID = container->objectID;
    emit pm.itemPropertyChanged(action_, QStringLiteral("steps"), nullptr);
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->transitions = VNX::AuthoringPolicy::defaultTransitionTable();
    selection->transitions.rules.front().bridge = sound_->guid;
    emit pm.itemPropertyChanged(container, QStringLiteral("transitions"), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    selection->playType = VNX::SelectionPlayType::Queue;
    emit pm.itemPropertyChanged(container, QStringLiteral("playType"), nullptr);
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));

    selection->transitions.rules.front().bridge.reset();
    emit pm.itemPropertyChanged(container, QStringLiteral("transitions"), nullptr);
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));
}

}
