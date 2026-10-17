#include "ActionManager.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "Components/VSound.h"
#include "LanguageContextService.h"
#include "MemorySettingsStorage.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "SoundSourceSemantics.h"
#include "UndoCommands.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Common/SourceRangeThumb.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "Widgets/SourceEditor/SourceRangeEditor.h"
#include "Widgets/SourceEditor/SourceEditorWaveformView.h"
#include "Widgets/ExternalEditor/ExternalEditorPanel.h"
#include "Widgets/Inspector/InspectorPanel.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtCore/qtestsupport_core.h>
#include <QtTest/qtestspontaneevent.h>

#include <algorithm>
#include <memory>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void clickWidget(QWidget* widget)
{
    const QPointF local = widget->rect().center();
    const QPointF global = widget->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &release);
    QCoreApplication::processEvents();
}

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWav(const QString& path, float value)
{
    const std::vector<float> samples(480, value);
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 48000;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    constexpr uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVEfmt ", 8);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

class SourcesSectionProviderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        VNX::registerBuiltinInspectorCustomizations();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(VNX::ActionManager::instance(),
                                    &VNX::ProjectManager::instance(), nullptr, {});
        ASSERT_TRUE(temp_.isValid());
        VNX::PlatformContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        VNX::LanguageContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("SourcesProviderProject");
        config.location = temp_.path();
        config.platforms = HarkenTest::platforms(QStringList{
            QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(VNX::ProjectManager::instance().createProject(config));
        layer_ = VNX::ProjectManager::instance().createItem(
            nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
        sound_ = VNX::ProjectManager::instance().createItem(
            layer_, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        VNX::UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        VNX::SelectorWindow::dismiss();
        widget_.reset();
        VNX::UndoRedoManager::instance().clear();
        VNX::ProjectManager::instance().closeProject();
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::PlatformContextService::instance().setStorageForTesting(
            VNX::SettingsStorageFactory::create(
                VNX::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"),
                QStringLiteral("HarkenStudio")));
        VNX::LanguageContextService::instance().setStorageForTesting(
            VNX::SettingsStorageFactory::create(
                VNX::SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"),
                QStringLiteral("HarkenStudio")));
    }

    void bindSection()
    {
        auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
            VNX::ProjectItem::Type::Sound, QStringLiteral("sources"));
        ASSERT_NE(provider, nullptr);
        auto* section = new VNX::CollapsibleSection(QStringLiteral("Source"), 0);
        widget_.reset(section);
        auto* content = provider->build(section);
        auto* layout = new QVBoxLayout();
        layout->addWidget(content);
        section->setHeaderWidget(provider->buildHeader(content, section));
        section->setContentLayout(layout);
        section->setDynamicContent(true);
        section->setExpanded(true);
        provider->bind(content, sound_);
        QCoreApplication::processEvents();
    }

    bool applySourceState(const VNX::SoundSources& sources)
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(sound_->objectID)},
             {QStringLiteral("property"), QStringLiteral("sources")},
             {QStringLiteral("value"), VNX::SoundSourceSemantics::toJson(sources)}}).success;
    }

    bool applySources(const std::vector<VNX::SoundSourceDef>& rows,
                      const QMap<QString, VNX::SoundSourcePlatformOverride>& platforms = {})
    {
        VNX::SoundSources sources;
        sources.plain.entries = rows;
        sources.plain.platforms = platforms;
        return applySourceState(sources);
    }

    VNX::SoundSourceDef makeRow(uint32_t id)
    {
        makeAsset(id, QString::number(id) + ".wav");
        VNX::SoundSourceDef row;
        row.entryId = QUuid::createUuid();
        row.sourceId = id;
        return row;
    }

    VNX::AssetInfo makeAsset(uint32_t sourceId, const QString& name,
                             uint64_t samples = 48000)
    {
        VNX::AssetInfo asset;
        asset.sourceID = sourceId;
        asset.relativePath = name;
        asset.absolutePath = temp_.path() + QLatin1Char('/') + name;
        asset.contentHash = name;
        asset.metadata.sampleRate = 48000;
        asset.metadata.totalSamples = samples;
        asset.metadata.durationSec = static_cast<double>(samples) / 48000.0;
        EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
        return asset;
    }

    QList<QWidget*> visibleRows() const
    {
        QList<QWidget*> rows;
        for (QWidget* row : widget_->findChildren<QWidget*>(QStringLiteral("listRow"))) {
            if (!row->isHidden() && row->property("rowIndex").isValid())
                rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(), [](QWidget* a, QWidget* b) {
            QWidget* parent = a->parentWidget();
            QLayout* layout = parent ? parent->layout() : nullptr;
            if (layout && parent == b->parentWidget())
                return layout->indexOf(a) < layout->indexOf(b);
            return a->property("rowIndex").toInt() < b->property("rowIndex").toInt();
        });
        return rows;
    }

    QAction* rowAction(QWidget* row, const QString& name) const
    {
        for (QAction* action : row->actions()) {
            if (action->objectName() == name)
                return action;
            if (QMenu* menu = action->menu()) {
                for (QAction* nested : menu->actions()) {
                    if (nested->objectName() == name)
                        return nested;
                }
            }
        }
        return nullptr;
    }

    QList<QWidget*> groupHeaders() const
    {
        QList<QWidget*> headers;
        for (QWidget* header : widget_->findChildren<QWidget*>(
                 QStringLiteral("sourceLanguageGroup"))) {
            if (!header->isHidden())
                headers.push_back(header);
        }
        return headers;
    }

    VNX::ListFooter* footerForLanguage(const QString& language) const
    {
        for (auto* footer : widget_->findChildren<VNX::ListFooter*>())
            if (!footer->isHidden() && footer->property("groupLanguage").toString() == language)
                return footer;
        return nullptr;
    }

    void triggerMissingLanguageAction(const QString& name)
    {
        auto* message = widget_->findChild<QWidget*>("sourceMissingLanguages");
        ASSERT_NE(message, nullptr);
        ASSERT_FALSE(message->isHidden());
        QTimer::singleShot(0, widget_.get(), [this, name] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            ASSERT_NE(menu, nullptr);
            auto* action = rowAction(menu, name);
            menu->close();
            ASSERT_NE(action, nullptr);
            action->trigger();
        });
        QMetaObject::invokeMethod(message, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, QPoint()));
        QCoreApplication::processEvents();
    }

    bool setDistinguishLanguage(bool enabled)
    {
        return VNX::ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"), static_cast<qint64>(sound_->objectID)},
             {QStringLiteral("property"), QStringLiteral("distinguishLanguage")},
             {QStringLiteral("value"), enabled}}).success;
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* layer_ = nullptr;
    VNX::ProjectItem* sound_ = nullptr;
    std::unique_ptr<QWidget> widget_;
};

}

QDialog* openTrimInput(VNX::SourceEditorWaveformView* waveform, int handle)
{
    const QPoint point = waveform->handleHitPoint(handle);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, point, waveform->mapToGlobal(point),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(waveform, &doubleClick);
    for (auto* dialog : waveform->findChildren<QDialog*>(QStringLiteral("sourceTrimValueEditor")))
        if (dialog->isVisible()) return dialog;
    return nullptr;
}

void enterTrimValue(QWidget* editor, int handle, double value)
{
    auto* waveform = editor->findChild<VNX::SourceEditorWaveformView*>();
    ASSERT_NE(waveform, nullptr);
    auto* dialog = openTrimInput(waveform, handle);
    ASSERT_NE(dialog, nullptr);
    auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("sourceTrimValueInput"));
    ASSERT_NE(spin, nullptr);
    spin->setValue(value);
    dialog->accept();
}

void waveMouse(VNX::SourceEditorWaveformView* waveform, QEvent::Type type,
               const QPoint& position, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, waveform->mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(waveform, &event);
}

TEST_F(SourcesSectionProviderTest, WheelScrollsInspectorAndPreservesRowSelection)
{
    ASSERT_TRUE(applySources({makeRow(8101), makeRow(8102)}));
    VNX::InspectorPanel inspector;
    inspector.showItemInspector(sound_);
    inspector.resize(480, 260);
    inspector.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    auto* list = inspector.findChild<QWidget*>(QStringLiteral("sourcesListWidget"));
    ASSERT_NE(list, nullptr);
    auto* table = list->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    const auto rows = table->rowsWidget()->findChildren<QWidget*>(
        QStringLiteral("listRow"), Qt::FindDirectChildrenOnly);
    ASSERT_EQ(rows.size(), 2);
    QScrollArea* scroll = nullptr;
    for (auto* parent = list->parentWidget(); parent; parent = parent->parentWidget()) {
        scroll = qobject_cast<QScrollArea*>(parent);
        if (scroll) break;
    }
    ASSERT_NE(scroll, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return scroll->verticalScrollBar()->maximum() > 0; }, 1000));
    ASSERT_EQ(table->scrollArea()->verticalScrollBar()->maximum(), 0);

    clickWidget(rows.front());
    ASSERT_TRUE(rows.front()->property("selected").toBool());
    const QList<QWidget*> targets{
        list,
        inspector.findChild<QCheckBox*>(QStringLiteral("sourceDistinguishLanguage")),
        table->header(),
        table->scrollArea()->viewport(),
        rows.front(),
        rows.front()->findChild<QCheckBox*>(QStringLiteral("sourceActive")),
        rows.front()->findChild<VNX::ObjectRefWidget*>(QStringLiteral("sourceRef")),
        rows.front()->findChild<VNX::SourceRangeThumb*>(),
        table->findChild<QWidget*>(QStringLiteral("listFoot"))
    };
    for (auto* target : targets) {
        ASSERT_NE(target, nullptr);
        SCOPED_TRACE(target->objectName().toStdString());
        scroll->verticalScrollBar()->setValue(0);
        const QPoint position = target->rect().center();
        QWheelEvent down(position, target->mapToGlobal(position), {}, QPoint(0, -120),
                         Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QSpontaneKeyEvent::setSpontaneous(&down);
        qApp->notify(target, &down);
        const int value = scroll->verticalScrollBar()->value();
        EXPECT_GT(value, 0);
        QWheelEvent up(position, target->mapToGlobal(position), {}, QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QSpontaneKeyEvent::setSpontaneous(&up);
        qApp->notify(target, &up);
        EXPECT_LT(scroll->verticalScrollBar()->value(), value);
        EXPECT_TRUE(rows.front()->property("selected").toBool());
    }

    clickWidget(rows.back());
    EXPECT_FALSE(rows.front()->property("selected").toBool());
    EXPECT_TRUE(rows.back()->property("selected").toBool());
}

TEST_F(SourcesSectionProviderTest, ObjectRefChangeRelinksRow)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"), 96000);
    VNX::SoundSourceDef row;
    row.entryId = QUuid::createUuid();
    row.sourceId = 8101;
    row.trimStartFrame = 12000;
    row.trimEndFrame = 36000;
    ASSERT_TRUE(applySources({row}));
    bindSection();
    auto refs = widget_->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("sourceRef"));
    ASSERT_EQ(refs.size(), 1);
    emit refs.front()->objectChanged(8102);
    QCoreApplication::processEvents();
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().sourceId, 8102u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().trimStartFrame, 12000u);
    ASSERT_TRUE(sound_->comp<VNX::VSound>()->sources.plain.entries.front().trimEndFrame);
    EXPECT_EQ(*sound_->comp<VNX::VSound>()->sources.plain.entries.front().trimEndFrame, 36000u);
}

TEST_F(SourcesSectionProviderTest, RemoveSelectedIsUndoable)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"));
    VNX::SoundSourceDef a;
    a.entryId = QUuid::createUuid();
    a.sourceId = 8101;
    VNX::SoundSourceDef b;
    b.entryId = QUuid::createUuid();
    b.sourceId = 8102;
    ASSERT_TRUE(applySources({a, b}, {{HarkenTest::platformIdString(QStringLiteral("Windows")), {b.entryId}}}));
    bindSection();
    auto rows = visibleRows();
    ASSERT_EQ(rows.size(), 2);
    auto* list = widget_->findChild<QWidget*>(QStringLiteral("sourcesListWidget"));
    ASSERT_NE(list, nullptr);
    list->setProperty("selectedRow", 1);
    auto* remove = widget_->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_sources"));
    ASSERT_NE(remove, nullptr);
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    remove->click();
    QCoreApplication::processEvents();
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().sourceId, 8101u);
    EXPECT_TRUE(sound_->comp<VNX::VSound>()->sources.plain.platforms.isEmpty());
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 2u);
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.first().active, b.entryId);
}

TEST_F(SourcesSectionProviderTest, MoveUpDownChangesDisplayOrderOnly)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"));
    VNX::SoundSourceDef a;
    a.entryId = QUuid::createUuid();
    a.sourceId = 8101;
    VNX::SoundSourceDef b;
    b.entryId = QUuid::createUuid();
    b.sourceId = 8102;
    const QMap<QString, VNX::SoundSourcePlatformOverride> matrix{
        {HarkenTest::platformIdString(QStringLiteral("Windows")), {b.entryId}}};
    ASSERT_TRUE(applySources({a, b}, matrix));
    bindSection();
    auto rows = visibleRows();
    ASSERT_EQ(rows.size(), 2);
    auto* down = rowAction(rows[0], QStringLiteral("sourceMoveDown"));
    ASSERT_NE(down, nullptr);
    down->trigger();
    QCoreApplication::processEvents();
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 2u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().sourceId, 8102u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.back().sourceId, 8101u);
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms, matrix);
    rows = visibleRows();
    ASSERT_EQ(rows.size(), 2);
    auto* up = rowAction(rows[1], QStringLiteral("sourceMoveUp"));
    ASSERT_NE(up, nullptr);
    up->trigger();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().sourceId, 8101u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms, matrix);
}

TEST_F(SourcesSectionProviderTest, LedgerRejectsRetiredActiveSourceIdKey)
{
    makeAsset(8601, QStringLiteral("reject.wav"));
    VNX::SoundSourceDef row;
    row.entryId = QUuid::createUuid();
    row.sourceId = 8601;
    VNX::SoundSources sources;
    sources.plain.entries = {row};
    sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = row.entryId;
    const QJsonObject ledger = VNX::SoundSourceSemantics::toJson(sources);
    ASSERT_TRUE(VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(sound_->objectID)},
         {QStringLiteral("property"), QStringLiteral("sources")},
         {QStringLiteral("value"), ledger}}).success);
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 1u);
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.size(), 1u);
    const auto beforeSources = sound_->comp<VNX::VSound>()->sources.plain.entries;
    const auto beforeSelections = sound_->comp<VNX::VSound>()->sources.plain.platforms;

    QJsonObject rejected = ledger;
    rejected[QStringLiteral("activeSourceId")] = row.entryId.toString(QUuid::WithoutBraces);
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(sound_->objectID)},
         {QStringLiteral("property"), QStringLiteral("sources")},
         {QStringLiteral("value"), rejected}});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries, beforeSources);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms, beforeSelections);
}

TEST_F(SourcesSectionProviderTest, ActiveWritesCurrentPlatformOrSharedAccordingToAll)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"));
    VNX::SoundSourceDef a;
    a.entryId = QUuid::createUuid();
    a.sourceId = 8101;
    VNX::SoundSourceDef b;
    b.entryId = QUuid::createUuid();
    b.sourceId = 8102;
    ASSERT_TRUE(applySources({a, b}));
    auto& platforms = VNX::PlatformContextService::instance();
    auto& languages = VNX::LanguageContextService::instance();
    languages.setLanguage(QString());
    platforms.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    bindSection();
    auto rows = visibleRows();
    ASSERT_EQ(rows.size(), 2);
    auto* check = rows[1]->findChild<QCheckBox*>(QStringLiteral("sourceActive"));
    ASSERT_NE(check, nullptr);
    check->click();
    QCoreApplication::processEvents();
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.firstKey(),
              HarkenTest::platformIdString(QStringLiteral("Windows")));
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms.first().active, b.entryId);

    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    platforms.setSelected({HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android},
                          HarkenTest::PlatformIds::Windows, true);
    bindSection();
    rows = visibleRows();
    ASSERT_EQ(rows.size(), 2);
    check = rows[0]->findChild<QCheckBox*>(QStringLiteral("sourceActive"));
    ASSERT_NE(check, nullptr);
    check->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
    const auto& set = sound_->comp<VNX::VSound>()->sources.plain;
    EXPECT_TRUE(set.platforms.isEmpty());
    EXPECT_EQ(set.active, a.entryId);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    ASSERT_EQ(set.platforms.size(), 1);
    EXPECT_EQ(set.platforms.firstKey(), HarkenTest::platformIdString("Windows"));
    EXPECT_EQ(set.platforms.first().active, b.entryId);
}

TEST_F(SourcesSectionProviderTest, SingleTargetAllKeepsSharedSourceEditable)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 8101;
    ASSERT_TRUE(applySources({source}));
    VNX::ProjectManager::instance().setProjectPlatformsInternal(HarkenTest::platforms({QStringLiteral("Windows")}));
    VNX::PlatformContextService::instance().setAll(true);
    bindSection();
    const auto rows = visibleRows();
    ASSERT_EQ(rows.size(), 1);
    auto* check = rows.front()->findChild<QCheckBox*>(QStringLiteral("sourceActive"));
    ASSERT_NE(check, nullptr);
    check->click();
    QCoreApplication::processEvents();
    const auto& set = sound_->comp<VNX::VSound>()->sources.plain;
    EXPECT_TRUE(set.platforms.isEmpty());
    EXPECT_EQ(set.active, source.entryId);
}

TEST_F(SourcesSectionProviderTest, PlainActiveIgnoresLanguageBar)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef row;
    row.entryId = QUuid::createUuid();
    row.sourceId = 8101;
    ASSERT_TRUE(applySources(
        {row}, {{HarkenTest::platformIdString(QStringLiteral("Windows")), {row.entryId}}}));
    VNX::ProjectManager::instance().setProjectLanguagesInternal(
        {QStringLiteral("English")});
    auto& languages = VNX::LanguageContextService::instance();
    languages.setLanguage(QStringLiteral("English"));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    bindSection();
    auto rows = visibleRows();
    ASSERT_EQ(rows.size(), 1);
    auto* check = rows.front()->findChild<QCheckBox*>(QStringLiteral("sourceActive"));
    ASSERT_NE(check, nullptr);
    EXPECT_TRUE(check->isChecked());
    check->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VSound>()->sources.localized.isEmpty());
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms[
        HarkenTest::platformIdString("Windows")].active, row.entryId);
}

TEST_F(SourcesSectionProviderTest, TrimEditorChangesOnlySelectedReferenceAndSupportsUndo)
{
    auto asset = makeAsset(8101, QStringLiteral("shared.wav"), 480);
    writeWav(asset.absolutePath, 0.25f);
    QFile file(asset.absolutePath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = file.readAll();
    file.close();
    VNX::SoundSourceDef first;
    first.entryId = QUuid::createUuid();
    first.sourceId = asset.sourceID;
    first.loopRegion = VNX::SoundLoopRegion{100, 300};
    VNX::SoundSourceDef second = first;
    second.entryId = QUuid::createUuid();
    second.trimStartFrame = 48;
    second.trimEndFrame = 420;
    const QMap<QString, VNX::SoundSourcePlatformOverride> matrix{
        {HarkenTest::platformIdString(QStringLiteral("Windows")), {first.entryId}}};
    ASSERT_TRUE(applySources({first, second}, matrix));
    const auto before = sound_->comp<VNX::VSound>()->sources.plain.entries;
    VNX::ExternalEditorPanel editor;
    editor.bindSelection(VNX::ExternalEditorSelection::source(sound_->guid, first.entryId));
    editor.resize(480, 320);
    editor.show();
    QApplication::processEvents();
    editor.findChild<QAction*>(QStringLiteral("sourceRangeFrames"))->trigger();
    const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
    enterTrimValue(&editor, 0, 120);
    enterTrimValue(&editor, 3, 360);
    QApplication::processEvents();
    const auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_EQ(sound->sources.plain.entries.size(), 2u);
    EXPECT_EQ(sound->sources.plain.entries.front().trimStartFrame, 120u);
    EXPECT_EQ(sound->sources.plain.entries.front().trimEndFrame, 360u);
    EXPECT_EQ(sound->sources.plain.entries.front().loopRegion, before.front().loopRegion);
    EXPECT_EQ(sound->sources.plain.entries.front().loopProvenance, before.front().loopProvenance);
    EXPECT_EQ(sound->sources.plain.entries.back(), before.back());
    EXPECT_EQ(sound->sources.plain.platforms, matrix);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount + 2);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), originalBytes);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_EQ(sound->sources.plain.entries, before);
    EXPECT_EQ(sound->sources.plain.platforms, matrix);
}

TEST_F(SourcesSectionProviderTest, TrimEditorPreservesOutOfRangeValuesUntilEdited)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimEndFrame = 96000;
    int commits = 0;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [&](const VNX::SoundSourceDef&) { ++commits; });
    editor.show();
    QApplication::processEvents();
    editor.findChild<QAction*>(QStringLiteral("sourceRangeFrames"))->trigger();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    auto* dialog = openTrimInput(waveform, 3);
    ASSERT_NE(dialog, nullptr);
    auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("sourceTrimValueInput"));
    ASSERT_NE(spin, nullptr);
    EXPECT_DOUBLE_EQ(spin->value(), 96000);
    dialog->accept();
    EXPECT_EQ(commits, 0);
    EXPECT_EQ(waveform->previewSource().trimEndFrame, 96000u);
}

TEST_F(SourcesSectionProviderTest, UnitsAndUnchangedExactInputPreserveSamplePrecision)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 12345;
    source.trimEndFrame = 36001;
    int commits = 0;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [&](const VNX::SoundSourceDef&) { ++commits; });
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    auto* frames = editor.findChild<QAction*>(QStringLiteral("sourceRangeFrames"));
    auto* seconds = editor.findChild<QAction*>(QStringLiteral("sourceRangeSeconds"));
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(seconds, nullptr);
    for (auto* unit : {seconds, frames, seconds}) {
        unit->trigger();
        auto* dialog = openTrimInput(waveform, 0);
        ASSERT_NE(dialog, nullptr);
        auto* spin = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("sourceTrimValueInput"));
        ASSERT_NE(spin, nullptr);
        EXPECT_NEAR(spin->value(), unit == frames ? 12345.0 : 12345.0 / 48000, 0.000001);
        dialog->accept();
    }
    EXPECT_EQ(waveform->previewSource(), source);
    EXPECT_EQ(commits, 0);
    auto* dialog = openTrimInput(waveform, 0);
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>()->setValue(0.4);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(dialog, &escape);
    EXPECT_EQ(commits, 0);
    EXPECT_EQ(waveform->previewSource(), source);
    QApplication::processEvents();
    dialog = openTrimInput(waveform, 0);
    ASSERT_NE(dialog, nullptr);
    dialog->findChild<QDoubleSpinBox*>()->setValue(0.5);
    editor.setSource({}, nullptr);
    EXPECT_FALSE(dialog->isVisible());
    EXPECT_EQ(commits, 0);
}

TEST_F(SourcesSectionProviderTest, TrimDragDoesNotJumpAndCommitsOnceOrCancels)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 12345;
    source.trimEndFrame = 36001;
    source.loopRegion = VNX::SoundLoopRegion{14000, 22000};
    int commits = 0;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [&](const VNX::SoundSourceDef&) { ++commits; });
    editor.resize(640, 300);
    editor.move(100, 100);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    ASSERT_NE(waveform, nullptr);
    const QPoint start = waveform->handleHitPoint(0);
    waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    EXPECT_EQ(waveform->previewSource(), source);
    waveMouse(waveform, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(commits, 0);
    EXPECT_EQ(waveform->previewSource(), source);
    waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    const QPoint moved = start + QPoint(60, 0);
    waveMouse(waveform, QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
    EXPECT_GT(waveform->previewSource().trimStartFrame, source.trimStartFrame);
    EXPECT_EQ(waveform->previewSource().loopRegion, source.loopRegion);
    EXPECT_EQ(commits, 0);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(waveform, &escape);
    EXPECT_EQ(waveform->previewSource(), source);
    waveMouse(waveform, QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(commits, 0);
    waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    waveMouse(waveform, QEvent::MouseMove, moved, Qt::NoButton, Qt::LeftButton);
    waveMouse(waveform, QEvent::MouseButtonRelease, moved, Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(waveform->previewSource().loopRegion, source.loopRegion);
    EXPECT_LT(waveform->previewSource().trimStartFrame, *source.trimEndFrame);
}

TEST_F(SourcesSectionProviderTest, ExactTrimInputCommitsWithReturn)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    int commits = 0;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [&](const VNX::SoundSourceDef&) { ++commits; });
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    auto* dialog = openTrimInput(waveform, 0);
    ASSERT_NE(dialog, nullptr);
    auto* spin = dialog->findChild<QDoubleSpinBox*>();
    ASSERT_NE(spin, nullptr);
    dialog->activateWindow();
    spin->setFocus();
    QApplication::processEvents();
    spin->setValue(0.125);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(spin, &enter);
    EXPECT_EQ(commits, 1);
    EXPECT_EQ(waveform->previewSource().trimStartFrame, 6000u);
}

TEST_F(SourcesSectionProviderTest, TimelineZoomAnchorsAtPointerAndPreservesReference)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 12000;
    source.trimEndFrame = 36000;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [](const VNX::SoundSourceDef&) { ADD_FAILURE() << "View navigation committed a source edit"; });
    editor.resize(640, 300);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    const int anchorX = waveform->handlePosition(0);
    QWheelEvent wheel(QPointF(anchorX, 100), waveform->mapToGlobal(QPoint(anchorX, 100)),
        QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(waveform, &wheel);
    EXPECT_LT(waveform->viewFrameCount(), asset.metadata.totalSamples);
    EXPECT_NEAR(waveform->handlePosition(0), anchorX, 1);
    const double zoomedSpan = waveform->viewFrameCount();
    const double zoomedStart = waveform->viewStartFrame();
    editor.setSource(source, &asset);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), zoomedSpan);
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), zoomedStart);
    auto* scroll = waveform->findChild<QScrollBar*>(QStringLiteral("sourceWaveScroll"));
    ASSERT_NE(scroll, nullptr);
    scroll->setValue(scroll->maximum());
    EXPECT_NEAR(waveform->viewStartFrame() + waveform->viewFrameCount(), 48000, 0.1);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), zoomedSpan);
    waveform->findChild<QToolButton*>(QStringLiteral("sourceWaveZoomOut"))->click();
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), 0);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), 48000);
    EXPECT_EQ(waveform->previewSource(), source);
    waveform->setViewRange(20000, 8);
    EXPECT_EQ(waveform->handlePosition(0), -1);
    EXPECT_EQ(waveform->handlePosition(3), -1);
    scroll->setValue(scroll->maximum());
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame() + waveform->viewFrameCount(), 48000);
    source.entryId = QUuid::createUuid();
    editor.setSource(source, &asset);
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), 0);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), 48000);
}

TEST_F(SourcesSectionProviderTest, TimeRulerPanningClampsViewportWithoutEditingSource)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 12000;
    source.trimEndFrame = 36000;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [](const VNX::SoundSourceDef&) { ADD_FAILURE() << "Panning committed a source edit"; });
    editor.resize(640, 300);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    waveform->setViewRange(12000, 24000);
    const QRect plot = waveform->waveformRect();
    const QPoint start(plot.center().x(), plot.top() / 2);
    waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    const QPoint end = start + QPoint(50, 0);
    waveMouse(waveform, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_NEAR(waveform->viewStartFrame(), 12000.0 - 50.0 / (plot.width() - 1) * 24000, 0.01);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), 24000);
    waveMouse(waveform, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    const double releasedStart = waveform->viewStartFrame();
    waveMouse(waveform, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), releasedStart);
    for (const int delta : {plot.width(), -plot.width()}) {
        waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        const QPoint outside = start + QPoint(delta, 0);
        waveMouse(waveform, QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
        EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), delta > 0 ? 0 : 24000);
        waveMouse(waveform, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
    }
    EXPECT_EQ(waveform->previewSource(), source);
}

TEST_F(SourcesSectionProviderTest, BlankCanvasPansBothAxesWithoutEditingSource)
{
    const auto asset = makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 16000;
    source.trimEndFrame = 32000;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [](const VNX::SoundSourceDef&) { ADD_FAILURE() << "Panning committed a source edit"; });
    editor.resize(640, 300);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    ASSERT_NE(waveform, nullptr);
    QObject::connect(waveform, &VNX::SourceEditorWaveformView::locateRequested,
        [](uint64_t) { ADD_FAILURE() << "Panning requested a seek"; });
    waveform->setViewRange(12000, 24000);
    for (int i = 0; i < 4; ++i)
        waveform->findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomIn"))->click();
    auto* vertical = waveform->findChild<QScrollBar*>(QStringLiteral("sourceWaveChannelScroll"));
    ASSERT_NE(vertical, nullptr);
    vertical->setValue(vertical->maximum() / 2);
    const int scrollStart = vertical->value();
    ASSERT_GT(scrollStart, 17);
    const QRect plot = waveform->waveformRect();
    const QPoint start(plot.center().x(), plot.top() + 24);
    waveMouse(waveform, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
    waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    const QPoint end = start + QPoint(35, 17);
    waveMouse(waveform, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_NEAR(waveform->viewStartFrame(), 12000.0 - 35.0 / (plot.width() - 1) * 24000, 0.01);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), 24000);
    EXPECT_EQ(vertical->value(), scrollStart - 17);
    EXPECT_EQ(waveform->previewSource(), source);
    waveMouse(waveform, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
    const double releasedStart = waveform->viewStartFrame();
    waveMouse(waveform, QEvent::MouseMove, start, Qt::NoButton, Qt::NoButton);
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), releasedStart);
    EXPECT_EQ(vertical->value(), scrollStart - 17);

    const QPoint rulerStart(plot.center().x(), plot.top() / 2);
    waveMouse(waveform, QEvent::MouseButtonPress, rulerStart, Qt::LeftButton, Qt::LeftButton);
    waveMouse(waveform, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    EXPECT_EQ(vertical->value(), scrollStart - 17);
    waveMouse(waveform, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);

    for (const QPoint delta : {QPoint(plot.width(), vertical->maximum()),
                               QPoint(-plot.width(), -vertical->maximum())}) {
        waveMouse(waveform, QEvent::MouseButtonPress, start, Qt::MiddleButton, Qt::MiddleButton);
        waveMouse(waveform, QEvent::MouseMove, start + delta, Qt::NoButton, Qt::MiddleButton);
        EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), delta.x() > 0 ? 0 : 24000);
        EXPECT_EQ(vertical->value(), delta.y() > 0 ? 0 : vertical->maximum());
        waveMouse(waveform, QEvent::MouseButtonRelease, start + delta, Qt::MiddleButton, Qt::NoButton);
    }
    EXPECT_EQ(waveform->previewSource(), source);
}

TEST_F(SourcesSectionProviderTest, VerticalZoomExpandsScrollableCanvasAroundPointer)
{
    const auto asset = makeAsset(8101, QStringLiteral("mono.wav"));
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    source.trimStartFrame = 12000;
    source.trimEndFrame = 36000;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [](const VNX::SoundSourceDef&) { ADD_FAILURE() << "View zoom committed a source edit"; });
    editor.resize(640, 300);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    auto* vertical = waveform->findChild<QScrollBar*>(QStringLiteral("sourceWaveChannelScroll"));
    auto* zoomIn = waveform->findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomIn"));
    auto* zoomOut = waveform->findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomOut"));
    ASSERT_NE(vertical, nullptr);
    ASSERT_NE(zoomIn, nullptr);
    ASSERT_NE(zoomOut, nullptr);
    ASSERT_EQ(vertical->maximum(), 0);
    const int page = vertical->pageStep();
    const int anchor = page / 3;
    const int viewportTop = waveform->waveformRect().bottom() + 1 - page;
    const QPoint position(waveform->waveformRect().center().x(), viewportTop + anchor);
    const int trimX = waveform->handlePosition(0);
    QWheelEvent wheel(position, waveform->mapToGlobal(position), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(waveform, &wheel);
    EXPECT_TRUE(wheel.isAccepted());
    EXPECT_GT(vertical->maximum(), 0);
    EXPECT_EQ(vertical->pageStep(), page);
    const int content = vertical->maximum() + page;
    EXPECT_NEAR(double(vertical->value() + anchor) / content, double(anchor) / page, 1.0 / content);
    EXPECT_DOUBLE_EQ(waveform->viewStartFrame(), 0);
    EXPECT_DOUBLE_EQ(waveform->viewFrameCount(), 48000);
    EXPECT_EQ(waveform->handlePosition(0), trimX);
    vertical->setValue(vertical->maximum());
    const int scrolled = vertical->value();
    editor.setSource(source, &asset);
    EXPECT_EQ(vertical->value(), scrolled);
    EXPECT_EQ(vertical->maximum() + page, content);
    zoomIn->click();
    EXPECT_GT(vertical->maximum() + page, content);
    zoomOut->click();
    EXPECT_EQ(vertical->maximum() + page, content);
    zoomOut->click();
    EXPECT_EQ(vertical->maximum(), 0);
    EXPECT_EQ(vertical->value(), 0);
    EXPECT_EQ(waveform->previewSource(), source);
}

TEST_F(SourcesSectionProviderTest, WheelOverScrollbarsZoomsTimeWithoutScrollingVertically)
{
    auto asset = makeAsset(8101, QStringLiteral("surround.wav"));
    asset.metadata.channels = 6;
    VNX::SoundSourceDef source;
    source.sourceId = asset.sourceID;
    VNX::SourceRangeEditor editor(source, &asset, nullptr,
        [](const VNX::SoundSourceDef&) { ADD_FAILURE() << "Zooming committed a source edit"; });
    editor.resize(640, 300);
    editor.show();
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    waveform->setViewRange(12000, 24000);
    auto* horizontal = waveform->findChild<QScrollBar*>(QStringLiteral("sourceWaveScroll"));
    auto* vertical = waveform->findChild<QScrollBar*>(QStringLiteral("sourceWaveChannelScroll"));
    ASSERT_NE(horizontal, nullptr);
    ASSERT_NE(vertical, nullptr);
    waveform->findChild<QToolButton*>(QStringLiteral("sourceWaveVerticalZoomIn"))->click();
    ASSERT_GT(vertical->maximum(), 0);
    vertical->setValue(vertical->maximum() / 2);
    const int channelOffset = vertical->value();
    for (auto* target : {horizontal, vertical}) {
        const QPoint position = target->rect().center();
        const double count = waveform->viewFrameCount();
        QWheelEvent wheel(position, target->mapToGlobal(position), QPoint(), QPoint(0, 120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &wheel);
        EXPECT_TRUE(wheel.isAccepted());
        EXPECT_LT(waveform->viewFrameCount(), count);
        EXPECT_EQ(vertical->value(), channelOffset);
    }
    EXPECT_EQ(waveform->previewSource(), source);
}

TEST_F(SourcesSectionProviderTest, ExternalRangeEditorMergesOntoCurrentModelNotStaleCache)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"));
    makeAsset(8103, QStringLiteral("third.wav"));
    VNX::SoundSourceDef a;
    a.entryId = QUuid::createUuid();
    a.sourceId = 8101;
    VNX::SoundSourceDef b;
    b.entryId = QUuid::createUuid();
    b.sourceId = 8102;
    const QMap<QString, VNX::SoundSourcePlatformOverride> matrix{
        {HarkenTest::platformIdString(QStringLiteral("Windows")), {a.entryId}}};
    ASSERT_TRUE(applySources({a, b}, matrix));
    VNX::ExternalEditorPanel editor;
    editor.bindSelection(VNX::ExternalEditorSelection::source(sound_->guid, a.entryId));
    auto* popup = editor.findChild<VNX::SourceRangeEditor*>();
    ASSERT_NE(popup, nullptr);

    b.sourceId = 8103;
    ASSERT_TRUE(applySources({a, b}, matrix));
    enterTrimValue(popup, 0, 0.25);

    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 2u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().trimStartFrame, 12000u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.back().sourceId, 8103u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.platforms, matrix);
    editor.clearEditor();
    QCoreApplication::processEvents();
}

TEST_F(SourcesSectionProviderTest, ExternalRangeEditorKeepsEntryIdentityAcrossDeleteAndUndo)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    makeAsset(8102, QStringLiteral("second.wav"));
    VNX::SoundSourceDef first;
    first.entryId = QUuid::createUuid();
    first.sourceId = 8101;
    VNX::SoundSourceDef second;
    second.entryId = QUuid::createUuid();
    second.sourceId = 8102;
    ASSERT_TRUE(applySources({first, second}));
    VNX::ExternalEditorPanel editor;
    editor.bindSelection(VNX::ExternalEditorSelection::source(sound_->guid, first.entryId));
    const auto saved = editor.snapshotState();
    ASSERT_TRUE(applySources({second, first}));
    QApplication::processEvents();
    auto* waveform = editor.findChild<VNX::SourceEditorWaveformView*>();
    ASSERT_NE(waveform, nullptr);
    enterTrimValue(&editor, 0, 0.25);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.back().trimStartFrame, 12000u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().trimStartFrame, 0u);
    ASSERT_TRUE(applySources({second}));
    QApplication::processEvents();
    EXPECT_EQ(waveform->handlePosition(0), -1);
    EXPECT_EQ(editor.selection().sourceEntryId, first.entryId);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QApplication::processEvents();
    EXPECT_GE(waveform->handlePosition(0), 0);
    EXPECT_EQ(waveform->previewSource().trimStartFrame, 12000u);
    VNX::ExternalEditorPanel restored;
    restored.restoreState(saved);
    EXPECT_EQ(restored.selection().sourceEntryId, first.entryId);
    EXPECT_EQ(restored.boundItem(), sound_);
    EXPECT_EQ(restored.findChild<VNX::SourceEditorWaveformView*>()->previewSource().trimStartFrame, 12000u);
}

TEST_F(SourcesSectionProviderTest, OsWavDropOnSourceRefImportsThenAppends)
{
    makeAsset(8101, QStringLiteral("first.wav"));
    VNX::SoundSourceDef existing;
    existing.entryId = QUuid::createUuid();
    existing.sourceId = 8101;
    ASSERT_TRUE(applySources({existing}));
    bindSection();

    auto refs = widget_->findChildren<VNX::ObjectRefWidget*>(QStringLiteral("sourceRef"));
    ASSERT_EQ(refs.size(), 1);
    auto* sourceRef = refs.front();
    ASSERT_NE(sourceRef, nullptr);

    const QString wavPath = QDir(temp_.path()).filePath(QStringLiteral("os-column2.wav"));
    writeWav(wavPath, 0.25f);

    const int assetCountBefore = VNX::AssetRegistry::instance().count();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(wavPath)});

    QDropEvent ignored(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    static_cast<QObject*>(sourceRef)->event(&ignored);
    EXPECT_FALSE(ignored.isAccepted());
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources.plain.entries.front().sourceId, 8101u);
    EXPECT_EQ(VNX::AssetRegistry::instance().count(), assetCountBefore);

    auto* list = widget_->findChild<QWidget*>(QStringLiteral("sourcesListWidget"));
    ASSERT_NE(list, nullptr);
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    EXPECT_TRUE(list->eventFilter(sourceRef, &drop));
    EXPECT_TRUE(drop.isAccepted());
    QCoreApplication::processEvents();

    const auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    ASSERT_EQ(sound->sources.plain.entries.size(), 2u);
    EXPECT_EQ(sound->sources.plain.entries.front().sourceId, 8101u);
    const uint32_t importedId = sound->sources.plain.entries.back().sourceId;
    EXPECT_NE(importedId, 0u);
    EXPECT_NE(importedId, 8101u);
    EXPECT_TRUE(VNX::AssetRegistry::instance().contains(importedId));
    EXPECT_GT(VNX::AssetRegistry::instance().count(), assetCountBefore);

    const VNX::AssetInfo* imported = VNX::AssetRegistry::instance().getAsset(importedId);
    ASSERT_NE(imported, nullptr);
    EXPECT_TRUE(imported->relativePath.contains(QStringLiteral("os-column2.wav")));
    EXPECT_TRUE(QFile::exists(imported->absolutePath));
    EXPECT_TRUE(VNX::PathUtils::isWithinProject(
        imported->absolutePath, VNX::PathUtils::getAudioSourcesDir(
            VNX::AssetRegistry::instance().projectRoot())));
}
TEST_F(SourcesSectionProviderTest, ToggleStartsEmptyAndRestoresPlainSourcesWithUndo)
{
    const auto first = makeRow(8101);
    const auto second = makeRow(8102);
    ASSERT_TRUE(applySources({first, second}, {{HarkenTest::platformIdString("Windows"), {second.entryId}}}));
    const auto before = sound_->comp<VNX::VSound>()->sources;
    bindSection();
    auto* toggle = widget_->findChild<QCheckBox*>("sourceDistinguishLanguage");
    ASSERT_NE(toggle, nullptr);
    EXPECT_TRUE(toggle->isEnabled());
    toggle->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VSound>()->distinguishLanguage);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources, before);
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_FALSE(sound_->comp<VNX::VSound>()->distinguishLanguage);
    VNX::UndoRedoManager::instance().redo();
    QCoreApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VSound>()->distinguishLanguage);
    toggle->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(sound_->comp<VNX::VSound>()->distinguishLanguage);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources, before);
    toggle->click();
    QCoreApplication::processEvents();
    EXPECT_TRUE(sound_->comp<VNX::VSound>()->distinguishLanguage);
    EXPECT_EQ(sound_->comp<VNX::VSound>()->sources, before);
}

TEST_F(SourcesSectionProviderTest, BothBanksRestoreTheirRowsActiveAndRanges)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::SoundSources sources;
    auto plain = makeRow(8101);
    plain.trimStartFrame = 12;
    sources.plain = {{plain}, plain.entryId, {}};
    const auto english = makeRow(8102);
    sources.localized["English"] = {{english}, english.entryId, {}};
    const auto japanese = makeRow(8103);
    sources.localized["Japanese"] = {{japanese}, japanese.entryId, {}};
    ASSERT_TRUE(applySourceState(sources));
    const auto before = sound_->comp<VNX::VSound>()->sources;
    bindSection();
    for (bool enabled : {true, false, true, false}) {
        ASSERT_TRUE(setDistinguishLanguage(enabled));
        QCoreApplication::processEvents();
        const int undoCount = VNX::UndoRedoManager::instance().undoStack()->count();
        for (const auto& language : {QStringLiteral("English"), QStringLiteral("Japanese")}) {
            VNX::LanguageContextService::instance().setLanguage(language);
            QCoreApplication::processEvents();
            ASSERT_EQ(visibleRows().size(), 1);
            EXPECT_EQ(QUuid(visibleRows().front()->property("entryId").toString()),
                      enabled ? before.localized[language].entries.front().entryId : plain.entryId);
            EXPECT_TRUE(visibleRows().front()->findChild<QCheckBox*>("sourceActive")->isChecked());
            EXPECT_EQ(sound_->comp<VNX::VSound>()->sources, before);
            EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoCount);
        }
    }
}

TEST_F(SourcesSectionProviderTest, AppendUsesContextLanguageAndPreservesOtherBanks)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::LanguageContextService::instance().setLanguage("Japanese");
    VNX::SoundSources sources;
    const auto plain = makeRow(8101);
    sources.plain = {{plain}, plain.entryId, {}};
    const auto english = makeRow(8102);
    sources.localized["English"].entries = {english};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    bindSection();
    auto* list = widget_->findChild<QWidget*>("sourcesListWidget");
    EXPECT_TRUE(visibleRows().isEmpty());
    ASSERT_NE(footerForLanguage("Japanese"), nullptr);
    EXPECT_TRUE(footerForLanguage("Japanese")->addButton()->isEnabled());
    makeRow(8103);
    list->setProperty("appendSourceId", 8103);
    QCoreApplication::processEvents();
    const auto* sound = sound_->comp<VNX::VSound>();
    EXPECT_EQ(sound->sources.plain.entries[0].entryId, plain.entryId);
    EXPECT_EQ(sound->sources.plain.active, plain.entryId);
    ASSERT_EQ(sound->sources.localized["English"].entries.size(), 1u);
    EXPECT_TRUE(sound->sources.localized["English"].active.isNull());
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_EQ(sound->sources.localized["Japanese"].entries.front().sourceId, 8103u);
    ASSERT_EQ(visibleRows().size(), 1);
    EXPECT_EQ(visibleRows().front()->findChild<QLabel*>("tableRowNumber")->text(), "1");
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources.localized["English"].entries.size(), 1u);
    EXPECT_FALSE(sound->sources.localized.contains("Japanese"));
}

TEST_F(SourcesSectionProviderTest, GroupClicksPreserveContextAndAppendDestination)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    ASSERT_TRUE(setDistinguishLanguage(true));
    bindSection();
    auto* list = widget_->findChild<QWidget*>("sourcesListWidget");
    ASSERT_EQ(groupHeaders().size(), 1);
    EXPECT_EQ(groupHeaders().front()->property("groupLanguage").toString(), "English");
    clickWidget(groupHeaders().front());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), "English");
    VNX::LanguageContextService::instance().setLanguage("Japanese");
    QCoreApplication::processEvents();
    ASSERT_EQ(groupHeaders().size(), 1);
    EXPECT_EQ(groupHeaders().front()->property("groupLanguage").toString(), "Japanese");
    clickWidget(groupHeaders().front());
    EXPECT_EQ(VNX::LanguageContextService::instance().language(), "Japanese");
    makeRow(8101);
    list->setProperty("appendSourceId", 8101);
    QCoreApplication::processEvents();
    ASSERT_EQ(sound_->comp<VNX::VSound>()->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_FALSE(sound_->comp<VNX::VSound>()->sources.localized.contains("English"));
}

TEST_F(SourcesSectionProviderTest, GroupActionsKeepTheirDestinationAcrossContextChangesAndUndo)
{
    using namespace VNX;
    const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restoreDialogs = qScopeGuard([=] {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
    });
    ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    SoundSources initial;
    initial.plain.entries = {makeRow(8101)};
    initial.localized["Japanese"].entries = {makeRow(8102)};
    const auto existing = makeAsset(8103, "Existing.wav");
    writeWav(existing.absolutePath, 0.25f);
    const QString input = temp_.filePath("Imported.wav");
    writeWav(input, 0.25f);
    ASSERT_TRUE(setDistinguishLanguage(true));

    for (bool fromFile : {false, true}) {
        SCOPED_TRACE(fromFile);
        ASSERT_TRUE(applySourceState(initial));
        const auto before = sound_->comp<VSound>()->sources;
        LanguageContextService::instance().setLanguage("English");
        bindSection();
        widget_->show();
        QCoreApplication::processEvents();
        auto* english = footerForLanguage("English");
        ASSERT_NE(english, nullptr);
        EXPECT_EQ(footerForLanguage("Japanese"), nullptr);
        EXPECT_EQ(footerForLanguage({}), nullptr);
        ASSERT_TRUE(english->addButton()->isEnabled());
        EXPECT_FALSE(english->removeButton()->isEnabled());
        const int undoCount = UndoRedoManager::instance().undoStack()->count();
        QTimer::singleShot(0, widget_.get(), [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            ASSERT_NE(menu, nullptr);
            auto* action = menu->actions().at(fromFile ? 0 : 1);
            menu->close();
            if (fromFile) {
                QTimer::singleShot(0, widget_.get(), [&] {
                    auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                    ASSERT_NE(dialog, nullptr);
                    LanguageContextService::instance().setLanguage("Japanese");
                    dialog->selectFile(input);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                });
            }
            action->trigger();
        });
        english->addButton()->click();
        if (!fromFile) {
            SelectorWindow* selector = nullptr;
            for (auto* window : QApplication::topLevelWidgets())
                if (auto* candidate = qobject_cast<SelectorWindow*>(window); candidate && candidate->isVisible())
                    selector = candidate;
            ASSERT_NE(selector, nullptr);
            auto* tree = selector->findChild<QTreeWidget*>();
            ASSERT_NE(tree, nullptr);
            LanguageContextService::instance().setLanguage("Japanese");
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            EXPECT_TRUE(selector->isVisible());
            for (QTreeWidgetItemIterator it(tree); *it; ++it)
                if ((*it)->data(0, kRoleObjectID).toUInt() == 8103u)
                    tree->setCurrentItem(*it);
            ASSERT_NE(tree->currentItem(), nullptr);
            ASSERT_EQ(tree->currentItem()->data(0, kRoleObjectID).toUInt(), 8103u);
            selector->accept();
        }
        QCoreApplication::processEvents();
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("Japanese") != nullptr; }));
        const auto* sound = sound_->comp<VSound>();
        ASSERT_EQ(sound->sources.localized["English"].entries.size(), 1u);
        EXPECT_EQ(sound->sources.localized["Japanese"], before.localized["Japanese"]);
        EXPECT_EQ(sound->sources.plain, before.plain);
        EXPECT_EQ(LanguageContextService::instance().language(), "Japanese");
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
        ASSERT_EQ(visibleRows().size(), 1);
        EXPECT_EQ(QUuid(visibleRows().front()->property("entryId").toString()),
                  before.localized["Japanese"].entries.front().entryId);
        EXPECT_EQ(footerForLanguage("English"), nullptr);
        LanguageContextService::instance().setLanguage("English");
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
        const auto entryId = sound->sources.localized["English"].entries.front().entryId;
        for (auto* row : visibleRows())
            if (QUuid(row->property("entryId").toString()) == entryId) {
                auto* number = row->findChild<QWidget*>("tableRowNumber");
                ASSERT_NE(number, nullptr);
                clickWidget(number);
            }
        EXPECT_EQ(LanguageContextService::instance().language(), "English");
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
        english = footerForLanguage("English");
        ASSERT_NE(english, nullptr);
        EXPECT_TRUE(english->removeButton()->isEnabled());
        LanguageContextService::instance().setLanguage("Japanese");
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("Japanese") != nullptr; }));
        auto* japanese = footerForLanguage("Japanese");
        ASSERT_NE(japanese, nullptr);
        EXPECT_FALSE(japanese->removeButton()->isEnabled());
        japanese->removeButton()->click();
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount + 1);
        LanguageContextService::instance().setLanguage("English");
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
        english = footerForLanguage("English");
        ASSERT_NE(english, nullptr);
        EXPECT_FALSE(english->removeButton()->isEnabled());
        ASSERT_EQ(visibleRows().size(), 1);
        EXPECT_FALSE(visibleRows().front()->property("selected").toBool());
        clickWidget(visibleRows().front()->findChild<QWidget*>("tableRowNumber"));
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
        english = footerForLanguage("English");
        english->removeButton()->click();
        QCoreApplication::processEvents();
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
        EXPECT_FALSE(sound->sources.localized.contains("English"));
        EXPECT_EQ(sound->sources.localized["Japanese"], before.localized["Japanese"]);
        EXPECT_TRUE(visibleRows().isEmpty());
        ASSERT_NE(footerForLanguage("English"), nullptr);
        EXPECT_TRUE(footerForLanguage("English")->addButton()->isEnabled());
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        QCoreApplication::processEvents();
        EXPECT_EQ(sound->sources, before);
        ASSERT_TRUE(setDistinguishLanguage(false));
        QCoreApplication::processEvents();
        EXPECT_NE(footerForLanguage({}), nullptr);
        EXPECT_EQ(footerForLanguage("English"), nullptr);
        ASSERT_TRUE(setDistinguishLanguage(true));
        QCoreApplication::processEvents();
        ASSERT_TRUE(QTest::qWaitFor([&] { return footerForLanguage("English") != nullptr; }));
    }
}

TEST_F(SourcesSectionProviderTest, ActiveWritesOnlyTheChosenLanguageAndPlatform)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::SoundSources sources;
    const auto english = makeRow(8101);
    const auto japanese = makeRow(8102);
    sources.localized["English"].entries = {english};
    sources.localized["Japanese"].entries = {japanese};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    VNX::PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    bindSection();
    visibleRows()[0]->findChild<QCheckBox*>("sourceActive")->click();
    QCoreApplication::processEvents();
    const auto* sound = sound_->comp<VNX::VSound>();
    EXPECT_EQ(sound->sources.localized["English"].platforms[HarkenTest::platformIdString("Windows")].active,
              english.entryId);
    EXPECT_TRUE(sound->sources.localized["Japanese"].platforms.isEmpty());
    VNX::PlatformContextService::instance().setAll(true);
    VNX::LanguageContextService::instance().setLanguage("Japanese");
    QCoreApplication::processEvents();
    ASSERT_EQ(visibleRows().size(), 1);
    visibleRows().front()->findChild<QCheckBox*>("sourceActive")->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources.localized["Japanese"].active, japanese.entryId);
    EXPECT_TRUE(sound->sources.localized["Japanese"].platforms.isEmpty());
    EXPECT_EQ(sound->sources.plain, sources.plain);
}

TEST_F(SourcesSectionProviderTest, MovingBetweenLanguagesPreservesActiveAndCanBeUndone)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::SoundSources sources;
    const auto plain = makeRow(8101);
    sources.plain = {{plain}, plain.entryId, {}};
    const auto english = makeRow(8102);
    sources.localized["English"] = {{english}, english.entryId,
        {{HarkenTest::platformIdString("Windows"), {english.entryId}}}};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    const auto before = sound_->comp<VNX::VSound>()->sources;
    bindSection();
    auto* move = rowAction(visibleRows()[0], "sourceMoveTo_Japanese");
    ASSERT_NE(move, nullptr);
    move->trigger();
    QCoreApplication::processEvents();
    const auto* sound = sound_->comp<VNX::VSound>();
    EXPECT_FALSE(sound->sources.localized.contains("English"));
    EXPECT_EQ(sound->sources.localized["Japanese"], before.localized["English"]);
    EXPECT_EQ(sound->sources.plain, before.plain);
    EXPECT_TRUE(visibleRows().isEmpty());
    VNX::LanguageContextService::instance().setLanguage("Japanese");
    QCoreApplication::processEvents();
    ASSERT_EQ(visibleRows().size(), 1);
    EXPECT_EQ(QUuid(visibleRows().front()->property("entryId").toString()), english.entryId);
    EXPECT_FALSE(visibleRows().front()->property("selected").toBool());
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources, before);
    EXPECT_TRUE(visibleRows().isEmpty());
}

TEST_F(SourcesSectionProviderTest, ReorderingStaysInsideItsLanguageAndRetainsSelection)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::LanguageContextService::instance().setLanguage("Japanese");
    VNX::SoundSources sources;
    const auto first = makeRow(8101);
    const auto second = makeRow(8102);
    const auto english = makeRow(8103);
    sources.localized["English"] = {{english}, english.entryId, {}};
    sources.localized["Japanese"] = {{first, second}, first.entryId, {}};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    const auto beforeEnglish = sound_->comp<VNX::VSound>()->sources.localized["English"];
    bindSection();
    auto* list = widget_->findChild<QWidget*>("sourcesListWidget");
    ASSERT_EQ(visibleRows().size(), 2);
    EXPECT_EQ(visibleRows().front()->findChild<QLabel*>("tableRowNumber")->text(), "1");
    EXPECT_EQ(visibleRows().back()->findChild<QLabel*>("tableRowNumber")->text(), "2");
    list->setProperty("selectedRow", visibleRows().front()->property("rowIndex"));
    auto* move = rowAction(visibleRows()[0], "sourceMoveDown");
    ASSERT_NE(move, nullptr);
    move->trigger();
    QCoreApplication::processEvents();
    const auto* sound = sound_->comp<VNX::VSound>();
    EXPECT_EQ(sound->sources.localized["Japanese"].entries[0].entryId, second.entryId);
    EXPECT_EQ(sound->sources.localized["Japanese"].entries[1].entryId, first.entryId);
    EXPECT_EQ(sound->sources.localized["Japanese"].active, first.entryId);
    EXPECT_EQ(sound->sources.localized["English"], beforeEnglish);
    EXPECT_TRUE(visibleRows()[1]->property("selected").toBool());
    auto* table = list->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    table->moveRow(2, 0);
    EXPECT_EQ(sound->sources.localized["English"], beforeEnglish);
    table->moveRow(2, 1);
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources.localized["Japanese"].entries[0].entryId, first.entryId);
    EXPECT_EQ(sound->sources.localized["Japanese"].active, first.entryId);
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources.localized["Japanese"].entries[0].entryId, second.entryId);
    EXPECT_EQ(sound->sources.localized["English"], beforeEnglish);
}

TEST_F(SourcesSectionProviderTest, MissingLanguageSourcesCanMergeWithoutDataLoss)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese", "French"});
    VNX::SoundSources sources;
    const auto english = makeRow(8101);
    const auto japanese = makeRow(8102);
    const auto french = makeRow(8103);
    sources.localized["English"] = {{english}, english.entryId, {}};
    sources.localized["Japanese"] = {{japanese}, japanese.entryId, {}};
    sources.localized["French"] = {{french}, french.entryId, {}};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    const auto before = sound_->comp<VNX::VSound>()->sources;
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English"});
    bindSection();
    ASSERT_EQ(visibleRows().size(), 1);
    EXPECT_FALSE(footerForLanguage("English")->addButton()->isEnabled());
    triggerMissingLanguageAction("sourceMoveAllMissingLanguagesTo_English");
    const auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_EQ(sound->sources.localized.size(), 1);
    EXPECT_EQ(sound->sources.localized["English"].entries.size(), 3u);
    EXPECT_EQ(sound->sources.localized["English"].active, english.entryId);
    EXPECT_EQ(visibleRows().size(), 3);
    EXPECT_TRUE(widget_->findChild<QWidget*>("sourceMissingLanguages")->isHidden());
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources, before);
}

TEST_F(SourcesSectionProviderTest, RemovingMissingLanguagesKeepsDormantBankAndIsUndoable)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English", "Japanese"});
    VNX::SoundSources sources;
    const auto plain = makeRow(8101);
    sources.plain = {{plain}, plain.entryId, {}};
    const auto japanese = makeRow(8103);
    sources.localized["Japanese"] = {{japanese}, japanese.entryId, {}};
    ASSERT_TRUE(applySourceState(sources));
    ASSERT_TRUE(setDistinguishLanguage(true));
    const auto before = sound_->comp<VNX::VSound>()->sources;
    VNX::ProjectManager::instance().setProjectLanguagesInternal({"English"});
    bindSection();
    EXPECT_TRUE(visibleRows().isEmpty());
    triggerMissingLanguageAction("sourceRemoveAllMissingLanguages");
    const auto* sound = sound_->comp<VNX::VSound>();
    EXPECT_EQ(sound->sources.plain, before.plain);
    EXPECT_EQ(sound->sources.localized["English"], before.localized["English"]);
    EXPECT_FALSE(sound->sources.localized.contains("Japanese"));
    EXPECT_TRUE(footerForLanguage("English")->addButton()->isEnabled());
    EXPECT_TRUE(widget_->findChild<QWidget*>("sourceMissingLanguages")->isHidden());
    VNX::UndoRedoManager::instance().undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(sound->sources, before);
    EXPECT_FALSE(widget_->findChild<QWidget*>("sourceMissingLanguages")->isHidden());
}

TEST_F(SourcesSectionProviderTest, MissingSourceDropUsesContextLanguageAndPreservesAuthoring)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    manager.setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    LanguageContextService::instance().setLanguage(QStringLiteral("English"));
    ASSERT_TRUE(setDistinguishLanguage(true));
    const QString input = temp_.filePath(QStringLiteral("Line.wav"));
    writeWav(input, 0.25f);
    SoundSources sources;
    QList<AssetInfo> assets;
    for (const auto& language : {QStringLiteral("English"), QStringLiteral("Japanese")}) {
        ASSERT_TRUE(QDir().mkdir(SourceFileSystem::absolutePath(language)));
        auto* command = ImportAssetsCommand::pushImportSources({input}, language);
        ASSERT_EQ(command->successCount(), 1);
        assets.append(*registry.getAsset(command->importedIDs().first()));
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = assets.back().sourceID;
        source.trimStartFrame = 12;
        source.trimEndFrame = 240;
        sources.localized[language].entries = {source};
        sources.localized[language].active = source.entryId;
    }
    ASSERT_TRUE(applySourceState(sources));
    const auto authored = sound_->comp<VSound>()->sources;
    ASSERT_TRUE(manager.saveProject());
    for (const auto& asset : assets)
        ASSERT_TRUE(QFile::remove(asset.absolutePath));
    bindSection();
    auto* list = widget_->findChild<QWidget*>(QStringLiteral("sourcesListWidget"));
    ASSERT_NE(list, nullptr);
    list->setProperty("selectedRow", 0);
    ASSERT_EQ(visibleRows().size(), 1);
    ObjectRefWidget* target = nullptr;
    for (auto* row : visibleRows())
        if (QUuid(row->property("entryId").toString()) == sources.localized["English"].entries.front().entryId)
            target = row->findChild<ObjectRefWidget*>(QStringLiteral("sourceRef"));
    ASSERT_NE(target, nullptr);
    writeWav(input, 0.75f);
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(input)});
    const QPoint position = target->mapTo(list, QPoint(5, 5));
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &enter);
    ASSERT_TRUE(enter.isAccepted());
    QDropEvent drop(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &drop);
    ASSERT_TRUE(drop.isAccepted());
    EXPECT_EQ(sound_->comp<VSound>()->sources, authored);
    EXPECT_TRUE(registry.sourceFileExists(assets.front().sourceID));
    EXPECT_FALSE(registry.sourceFileExists(assets.back().sourceID));
    EXPECT_EQ(registry.count(), 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_FALSE(registry.sourceFileExists(assets.front().sourceID));
    EXPECT_EQ(sound_->comp<VSound>()->sources, authored);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_TRUE(registry.sourceFileExists(assets.front().sourceID));
    EXPECT_EQ(sound_->comp<VSound>()->sources, authored);
}

TEST_F(SourcesSectionProviderTest, ExternalDropsUseCurrentContextRegardlessOfSelectionAndOneUndo)
{
    VNX::ProjectManager::instance().setProjectLanguagesInternal(
        {QStringLiteral("English"), QStringLiteral("Japanese")});
    VNX::LanguageContextService::instance().setLanguage(QStringLiteral("Japanese"));
    ASSERT_TRUE(setDistinguishLanguage(true));
    bindSection();
    auto* list = widget_->findChild<QWidget*>(QStringLiteral("sourcesListWidget"));
    ASSERT_NE(list, nullptr);
    const int initialUndoCount = VNX::UndoRedoManager::instance().undoStack()->count();

    const QString first = QDir(temp_.path()).filePath(QStringLiteral("japanese-first.wav"));
    writeWav(first, 0.25f);
    QMimeData firstMime;
    firstMime.setUrls({QUrl::fromLocalFile(first)});
    QDragEnterEvent firstEnter(QPoint(5, 5), Qt::CopyAction, &firstMime,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &firstEnter);
    ASSERT_TRUE(firstEnter.isAccepted());
    QDropEvent firstDrop(QPointF(5, 5), Qt::CopyAction, &firstMime,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &firstDrop);
    EXPECT_TRUE(firstDrop.isAccepted());
    QCoreApplication::processEvents();
    auto* sound = sound_->comp<VNX::VSound>();
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), initialUndoCount + 1);

    ASSERT_EQ(visibleRows().size(), 1);
    clickWidget(visibleRows().front()->findChild<QWidget*>("tableRowNumber"));
    VNX::LanguageContextService::instance().setLanguage(QStringLiteral("English"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(visibleRows().isEmpty());
    EXPECT_FALSE(footerForLanguage("English")->removeButton()->isEnabled());
    const QString second = QDir(temp_.path()).filePath(QStringLiteral("english-first.wav"));
    writeWav(second, 0.5f);
    QMimeData secondMime;
    secondMime.setUrls({QUrl::fromLocalFile(second)});
    QDragEnterEvent secondEnter(QPoint(5, 5), Qt::CopyAction, &secondMime,
                               Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &secondEnter);
    ASSERT_TRUE(secondEnter.isAccepted());
    QDropEvent secondDrop(QPointF(5, 5), Qt::CopyAction, &secondMime,
                         Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &secondDrop);
    EXPECT_TRUE(secondDrop.isAccepted());
    QCoreApplication::processEvents();
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    ASSERT_EQ(sound->sources.localized["English"].entries.size(), 1u);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), initialUndoCount + 2);
    const uint32_t importedId = sound->sources.localized["English"].entries.back().sourceId;
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_FALSE(sound->sources.localized.contains("English"));
    EXPECT_FALSE(VNX::AssetRegistry::instance().contains(importedId));
    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    ASSERT_EQ(sound->sources.localized["English"].entries.size(), 1u);
    EXPECT_EQ(sound->sources.localized["English"].entries.back().sourceId, importedId);

    list->setProperty("selectedRow", -1);
    auto* footer = footerForLanguage("English");
    ASSERT_NE(footer, nullptr);
    const QString third = QDir(temp_.path()).filePath(QStringLiteral("english.wav"));
    writeWav(third, 0.75f);
    QMimeData thirdMime;
    thirdMime.setUrls({QUrl::fromLocalFile(third)});
    const QPoint position = footer->mapTo(list, QPoint(5, 5));
    QDragEnterEvent thirdEnter(position, Qt::CopyAction, &thirdMime,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &thirdEnter);
    ASSERT_TRUE(thirdEnter.isAccepted());
    QDropEvent thirdDrop(position, Qt::CopyAction, &thirdMime,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list, &thirdDrop);
    EXPECT_TRUE(thirdDrop.isAccepted());
    QCoreApplication::processEvents();
    ASSERT_EQ(sound->sources.localized["English"].entries.size(), 2u);
    EXPECT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_FALSE(sound->sources.localized["English"].active.isNull());
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), initialUndoCount + 3);
}
