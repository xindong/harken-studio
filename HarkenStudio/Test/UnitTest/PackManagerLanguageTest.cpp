#include "Core/AppSettings.h"
#include "Core/ActionManager.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VSound.h"
#include "Core/PackBuildPlan.h"
#include "Core/PackArtifactCache.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Widgets/Common/MultiSelectComboBox.h"
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
        panel_->show();
        QApplication::processEvents();
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
        QApplication::processEvents();
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
        VNX::PackArtifactMetadata metadata;
        metadata.unitKey = artifactName.compare(QStringLiteral("Init"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("Init") : QStringLiteral("a:") + VNX::objectIdString(
                members.empty() ? action_->runtimeId() : members.front());
        metadata.platform = HarkenTest::platformIdString("Windows");
        metadata.language = language;
        metadata.packName = artifactName;
        metadata.outputPath = file.fileName();
        metadata.bytes = 4;
        metadata.modified = QDateTime::currentDateTimeUtc();
        metadata.version = 2;
        cachedArtifacts_.append(metadata);
        VNX::PackArtifactCache::instance().publish(metadata.platform, cachedArtifacts_);
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

    QVector<VNX::PackArtifactMetadata> cachedArtifacts_;
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

TEST_F(PackManagerLanguageTest, SectionSelectionKeepsPlatformScopeWithoutChangingProjectUndo)
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
    ASSERT_TRUE(VNX::TestSupport::spinUntil([&] { return actionRow(other)->checkState(0) == Qt::Unchecked; }));
    EXPECT_EQ(stack->index(), undoIndex);
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString linuxId = HarkenTest::platformIdString("Linux");
    EXPECT_FALSE(pm.packExportSelection().includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_TRUE(pm.packExportSelection().includesActionPack(action_->runtimeId(), linuxId, "English"));
    EXPECT_TRUE(pm.packExportSelection().includesActionPack(action_->runtimeId(), windows, "Japanese"));
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
    emit tree->groupCheckStateRequested(actionRow(other)->parent(), Qt::Checked);
    QApplication::processEvents();
    EXPECT_EQ(stack->index(), undoIndex);
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

TEST_F(PackManagerLanguageTest, CheckGesturesPreservePlatformScopeWithoutChangingProjectUndo)
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
    EXPECT_EQ(stack->index(), undoIndex);
    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    clickCheck(actionRow(other));
    context.setAll(true);
    EXPECT_EQ(actionRow(other)->checkState(0), Qt::PartiallyChecked);
    tree->setCurrentItem(actionRow(other));
    QKeyEvent space(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(tree, &space);
    QApplication::processEvents();
    EXPECT_TRUE(included(windows));
    EXPECT_TRUE(included(linuxId));
    EXPECT_EQ(stack->index(), undoIndex);

    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const QPoint start = checkPoint(actionRow());
    const QPoint end = checkPoint(actionRow(other));
    const QPoint section = tree->visualItemRect(actionRow(other)->parent()).center();
    mouse(QEvent::MouseButtonPress, start, Qt::LeftButton);
    QApplication::processEvents();
    mouse(QEvent::MouseMove, end, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, section, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_FALSE(pm.packExportSelection().includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_TRUE(pm.packExportSelection().includesActionPack(action_->runtimeId(), linuxId, "English"));
    EXPECT_FALSE(included(windows));
    EXPECT_TRUE(included(linuxId));
    EXPECT_TRUE(actionRow()->data(0, Qt::CheckStateRole).isValid());
    EXPECT_EQ(stack->index(), undoIndex);
    mouse(QEvent::MouseMove, end, Qt::NoButton);
    QApplication::processEvents();
    EXPECT_EQ(stack->index(), undoIndex);
    EXPECT_FALSE(pm.packExportSelection().includesActionPack(action_->runtimeId(), windows, "English"));
    EXPECT_FALSE(included(windows));
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
        if (!developer) header->moveSection(header->visualIndex(3), 2);
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

TEST_F(PackManagerLanguageTest, ExportPlatformSelectorKeepsNamesAcrossResizeAndRefresh)
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
            auto* selector = panel_->findChild<VNX::MultiSelectComboBox*>("packPlatformCombo");
            ASSERT_NE(selector, nullptr);
            QStringList names;
            for (const auto& platform : platforms) names.append(platform.name);
            EXPECT_EQ(selector->selectedText(), names.join(QStringLiteral(", ")));
            EXPECT_EQ(selector->toolTip(), selector->selectedText());
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
        auto* control = panel_->findChild<VNX::MultiSelectComboBox*>("packPlatformCombo");
        ASSERT_NE(control, nullptr);
        EXPECT_TRUE(panel_->rect().contains(QRect(control->mapTo(panel_.get(), QPoint()), control->size())));
        EXPECT_GT(control->width(), 0);
        QStringList chosen;
        for (const auto& value : control->selectedData()) chosen.append(value.toString());
        EXPECT_EQ(chosen, selected);
        EXPECT_EQ(control->count(), selected.size());
        EXPECT_EQ(panel_->generationPlatforms(), selected);
    }
}

TEST_F(PackManagerLanguageTest, CachedArtifactsAndSessionResultsHaveIndependentLifetimes)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.saveProject());
    const QString projectPath = pm.currentProject()->projectPath;
    const auto actionId = action_->runtimeId();
    const QString platform = pm.currentProject()->platformIds().first();
    writeArtifact(QStringLiteral("English"));
    VNX::PackGenerationRecord record;
    record.unitKey = QStringLiteral("a:") + VNX::objectIdString(actionId);
    record.packName = action_->name;
    record.platform = platform;
    record.language = QStringLiteral("English");
    record.status = QStringLiteral("Failed");
    record.elapsedMs = 12;
    panel_->addGenerationResult(record);
    pm.setPackExportSelectedPlatformsInternal({});
    pm.setActionPackTargetingInternal({{actionId, false, {}, QStringLiteral("English")}});

    const QString outputPath = cachedArtifacts_.first().outputPath;
    ASSERT_TRUE(QFile::remove(outputPath));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->text(4), QStringLiteral("Failed"));
    EXPECT_EQ(actionRow()->text(5), QStringLiteral("12 ms"));
    const QString cacheRoot = QDir(QFileInfo(projectPath).absolutePath()).filePath(".cache");
    QFile metadataFile(QDir(cacheRoot).filePath("pack-artifacts.json"));
    ASSERT_TRUE(metadataFile.open(QIODevice::ReadOnly));
    const auto saved = QJsonDocument::fromJson(metadataFile.readAll()).object();
    metadataFile.close();
    for (const auto& value : saved.value("artifacts").toArray()) {
        const auto artifact = value.toObject();
        EXPECT_FALSE(artifact.contains("status"));
        EXPECT_FALSE(artifact.contains("elapsedMs"));
        EXPECT_FALSE(artifact.contains("result"));
    }

    ASSERT_TRUE(pm.closeProject());
    EXPECT_TRUE(VNX::PackGenerationRecordStore::instance().records().isEmpty());
    ASSERT_TRUE(pm.loadProject(projectPath));
    action_ = pm.findItemByRuntimeStableId(actionId);
    ASSERT_NE(action_, nullptr);
    panel_->refreshPackList();
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_TRUE(actionRow()->text(4).isEmpty());
    EXPECT_TRUE(actionRow()->text(5).isEmpty());
    EXPECT_EQ(actionRow()->checkState(0), Qt::Unchecked);
    EXPECT_TRUE(panel_->generationPlatforms().isEmpty());

    QFile output(outputPath);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_EQ(output.write("external"), 8);
    output.close();
    ASSERT_TRUE(pm.closeProject());
    ASSERT_TRUE(QDir(cacheRoot).removeRecursively());
    ASSERT_TRUE(pm.loadProject(projectPath));
    action_ = pm.findItemByRuntimeStableId(actionId);
    ASSERT_NE(action_, nullptr);
    panel_->refreshPackList();
    EXPECT_EQ(actionRow()->checkState(0), Qt::Checked);
    EXPECT_EQ(panel_->generationPlatforms(), pm.currentProject()->platformIds());
    for (int column = 1; column < panel_->findChild<QTreeWidget*>()->columnCount(); ++column)
        EXPECT_TRUE(actionRow()->text(column).isEmpty());
}

TEST_F(PackManagerLanguageTest, KnownOutputCleanupRemovesOnlyArtifactsInThatOutputRoot)
{
    auto& cache = VNX::PackArtifactCache::instance();
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString linuxId = HarkenTest::platformIdString("Linux");
    writeArtifact(QStringLiteral("English"));
    auto linuxArtifact = cachedArtifacts_.first();
    linuxArtifact.platform = linuxId;
    linuxArtifact.outputPath = temp_.filePath("Generated/Linux/English/PlayVoice.bytes");
    auto otherArtifact = linuxArtifact;
    otherArtifact.language = QStringLiteral("Japanese");
    otherArtifact.outputPath = temp_.filePath("Other/Linux/Japanese/PlayVoice.bytes");
    cache.publish(linuxId, {linuxArtifact, otherArtifact});
    cache.removeUnselectedPlatforms(temp_.filePath("Generated"), {windows});
    EXPECT_EQ(cache.find(linuxId, linuxArtifact.language, linuxArtifact.unitKey, linuxArtifact.outputPath), nullptr);
    EXPECT_NE(cache.find(linuxId, otherArtifact.language, otherArtifact.unitKey, otherArtifact.outputPath), nullptr);
    EXPECT_NE(cache.find(windows, cachedArtifacts_.first().language,
        cachedArtifacts_.first().unitKey, cachedArtifacts_.first().outputPath), nullptr);
}

TEST_F(PackManagerLanguageTest, LanguageChangesRefreshActualVariantPaths)
{
    writeArtifact(QStringLiteral("English"));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(nullptr, QStringLiteral("English")), nullptr);
    ASSERT_NE(actionRow(nullptr, QStringLiteral("Japanese")), nullptr);
    EXPECT_EQ(actionRow(nullptr, QStringLiteral("English"))->text(1), QStringLiteral("Yes"));
    EXPECT_TRUE(actionRow(nullptr, QStringLiteral("Japanese"))->text(1).isEmpty());
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
    EXPECT_TRUE(actionRow(nullptr, QStringLiteral("Japanese"))->text(1).isEmpty());
}

TEST_F(PackManagerLanguageTest, ToggleRefreshesBetweenFlatAndLocalizedArtifacts)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English")});
    writeArtifact({});
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_TRUE(actionRow()->text(1).isEmpty());

    setLocalized(false);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->text(6), baseName() + QStringLiteral(".bytes"));

    setLocalized(true);
    EXPECT_TRUE(actionRow()->text(1).isEmpty());
    EXPECT_TRUE(actionRow()->text(6).isEmpty());
}

TEST_F(PackManagerLanguageTest, ReservedActionNameCannotReuseSystemPack)
{
    auto& pm = VNX::ProjectManager::instance();
    ASSERT_TRUE(pm.renameItem(action_, QStringLiteral("Init")));
    setLocalized(false);
    writeArtifact({});
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_TRUE(actionRow()->text(1).isEmpty());
    EXPECT_TRUE(actionRow()->toolTip(0).contains(QStringLiteral("reserved")));
    EXPECT_TRUE(actionRow()->text(4).isEmpty());
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());

    ASSERT_TRUE(pm.renameItem(action_, QStringLiteral("init")));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_TRUE(actionRow()->text(1).isEmpty());
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());
}

TEST_F(PackManagerLanguageTest, CachedArtifactsRequireStableActionIdentity)
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
    EXPECT_TRUE(actionRow(other)->text(1).isEmpty());
    EXPECT_TRUE(actionRow(other)->data(0, Qt::UserRole + 5).toString().isEmpty());

    ASSERT_TRUE(pm.deleteItem(action_));
    ASSERT_TRUE(pm.renameItem(other, originalName));
    panel_->refreshPackList();
    ASSERT_NE(actionRow(other), nullptr);
    EXPECT_TRUE(actionRow(other)->text(1).isEmpty());
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
    EXPECT_TRUE(actionRow()->text(1).isEmpty());

    auto* effect = pm.createItem(nullptr, QStringLiteral("Effect"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(effect, nullptr);
    action_->comp<VNX::VAction>()->steps.front().targetObjectID = effect->objectID;
    emit pm.itemPropertyChanged(action_, QStringLiteral("steps"), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

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
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());
    EXPECT_EQ(actionRow(voice)->data(0, Qt::UserRole + 6).toString(), QStringLiteral("English"));
    ASSERT_NE(actionRow(trigger), nullptr);
    EXPECT_EQ(actionRow(trigger)->data(0, Qt::UserRole + 6).toString(), QStringLiteral("English"));
    EXPECT_EQ(actionRow(trigger)->text(1), QStringLiteral("Yes"));

    setLocalized(false);
    EXPECT_EQ(actionRow(voice)->data(0, Qt::UserRole + 6).toString(), QString());
    EXPECT_EQ(actionRow(trigger)->data(0, Qt::UserRole + 6).toString(), QString());
    EXPECT_TRUE(actionRow(trigger)->text(1).isEmpty());
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
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

    ASSERT_TRUE(pm.moveItem(sound_, container));
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QStringLiteral("English"));

    auto* outside = pm.createItem(nullptr, QStringLiteral("Outside"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(outside, nullptr);
    ASSERT_TRUE(pm.moveItem(sound_, outside));
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

    ASSERT_TRUE(pm.moveItem(sound_, container));
    ASSERT_TRUE(pm.deleteItem(sound_));
    QCoreApplication::processEvents();
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());
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
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

    ASSERT_TRUE(pm.renameItem(sound_, QStringLiteral("RenamedVoice")));
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QStringLiteral("English"));
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
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->transitions = VNX::AuthoringPolicy::defaultTransitionTable();
    selection->transitions.rules.front().bridge = sound_->guid;
    emit pm.itemPropertyChanged(container, QStringLiteral("transitions"), nullptr);
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());

    selection->playType = VNX::SelectionPlayType::Queue;
    emit pm.itemPropertyChanged(container, QStringLiteral("playType"), nullptr);
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QStringLiteral("English"));

    selection->transitions.rules.front().bridge.reset();
    emit pm.itemPropertyChanged(container, QStringLiteral("transitions"), nullptr);
    EXPECT_EQ(actionRow()->data(0, Qt::UserRole + 6).toString(), QString());
}

}
