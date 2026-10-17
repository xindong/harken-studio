#include "Core/AppSettings.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VSound.h"
#include "Core/PackBuildPlan.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Widgets/Common/SegmentedControl.h"
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
#include <QMouseEvent>
#include <QScrollBar>
#include <QAbstractButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

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

    QTreeWidgetItem* actionRow(const VNX::ProjectItem* action = nullptr) const
    {
        if (!action) action = action_;
        auto* tree = panel_->findChild<QTreeWidget*>();
        if (!tree) return nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 4).toUInt() == action->objectID) return *it;
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

TEST_F(PackManagerLanguageTest, SectionCaptionsRetainEmphasisAcrossResizeAndColumnMovement)
{
    panel_->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    auto* tree = panel_->findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr);
    const auto& colors = VNX::ThemeManager::instance().colors();
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
            if (!row->data(0, VNX::TableSectionRole).toBool()) continue;
            captions.append(row->text(0));
            SCOPED_TRACE(row->text(0).toStdString());
            const QRect rect = tree->visualItemRect(row);
            ASSERT_FALSE(rect.isEmpty());
            EXPECT_EQ(image.pixelColor(1, rect.center().y()), colors.accent);
            EXPECT_EQ(image.pixel(image.width() - 8, rect.center().y()), colors.headerBackground.lighter(108).rgba());
            EXPECT_EQ(image.pixelColor(image.width() - 8, rect.top()), colors.sectionSeparator);
            EXPECT_EQ(image.pixelColor(image.width() - 8, rect.bottom()), colors.sectionSeparator);
            EXPECT_FALSE(row->flags().testFlag(Qt::ItemIsSelectable));
        }
        EXPECT_EQ(captions, (QStringList{"System", "Content"}));
        ASSERT_NE(actionRow(), nullptr);
        EXPECT_FALSE(actionRow()->data(0, VNX::TableSectionRole).toBool());
    }
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
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_NE(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_TRUE(actionRow()->toolTip(1).contains(QStringLiteral("Japanese: not built")));

    auto& pm = VNX::ProjectManager::instance();
    pm.setProjectLanguagesInternal({QStringLiteral("English")});
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_EQ(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->text(6), QStringLiteral("English/") + baseName() + QStringLiteral(".bytes"));

    pm.setProjectLanguagesInternal({QStringLiteral("Japanese"), QStringLiteral("English")});
    EXPECT_NE(actionRow()->text(1), QStringLiteral("Yes"));
    EXPECT_TRUE(actionRow()->toolTip(1).contains(QStringLiteral("Japanese: not built")));
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

    VNX::PackGroup group;
    group.name = QStringLiteral("Group");
    group.actionIds = {action_->runtimeId(), other->runtimeId()};
    pm.setPackGroupsInternal({group});
    writeArtifact({}, group.name, {action_->runtimeId()});
    panel_->refreshPackList();
    ASSERT_NE(actionRow()->parent(), nullptr);
    EXPECT_EQ(actionRow()->parent()->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow()->parent()->data(0, Qt::UserRole + 5).toString().isEmpty());

    other->comp<VNX::VAction>()->steps.clear();
    panel_->refreshPackList();
    EXPECT_EQ(actionRow()->parent()->text(1), QStringLiteral("Yes"));
    other->comp<VNX::VAction>()->steps = action_->comp<VNX::VAction>()->steps;
    panel_->refreshPackList();
    EXPECT_EQ(actionRow()->parent()->text(1), QStringLiteral("No"));

    writeArtifact({}, group.name, group.actionIds);
    panel_->refreshPackList();
    ASSERT_EQ(actionRow()->parent(), actionRow(other)->parent());
    EXPECT_EQ(actionRow()->parent()->text(1), QStringLiteral("Yes"));
    EXPECT_EQ(actionRow()->parent()->text(6), QStringLiteral("Group.bytes"));
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());
    EXPECT_TRUE(actionRow(other)->data(0, Qt::UserRole + 5).toString().isEmpty());

    group.actionIds = {other->runtimeId()};
    pm.setPackGroupsInternal({group});
    ASSERT_NE(actionRow(other)->parent(), nullptr);
    EXPECT_EQ(actionRow(other)->parent()->text(1), QStringLiteral("No"));
    EXPECT_TRUE(actionRow(other)->parent()->data(0, Qt::UserRole + 5).toString().isEmpty());

    pm.setPackGroupsInternal({});
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
    pm.setProjectLanguagesInternal({});
    ASSERT_NE(actionRow(), nullptr);
    EXPECT_TRUE(actionRow()->data(0, Qt::UserRole + 5).toString().isEmpty());

    setLocalized(false);
    EXPECT_TRUE(QFile::exists(actionRow()->data(0, Qt::UserRole + 5).toString()));
}

TEST_F(PackManagerLanguageTest, ActionTargetsAndGroupsRefreshTheirLanguageDimensions)
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
    VNX::PackGroup group;
    group.name = QStringLiteral("Voices");
    group.actionIds = {action_->runtimeId(), voice->runtimeId()};
    pm.setPackGroupsInternal({group});
    writeArtifact(QStringLiteral("English"), trigger->name, {trigger->runtimeId()});
    panel_->refreshPackList();
    ASSERT_NE(actionRow()->parent(), nullptr);
    EXPECT_EQ(actionRow()->parent()->text(6), QStringLiteral("English/Voices.bytes"));
    ASSERT_NE(actionRow(trigger), nullptr);
    EXPECT_EQ(actionRow(trigger)->text(6), QStringLiteral("English/Trigger.bytes"));
    EXPECT_EQ(actionRow(trigger)->text(1), QStringLiteral("Yes"));

    setLocalized(false);
    EXPECT_EQ(actionRow()->parent()->text(6), QStringLiteral("Voices.bytes"));
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
