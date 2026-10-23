#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "AssetPipeline/WaveformCache.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/SoundSourceSemantics.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/NodeItem.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Common/SourceRangeThumb.h"
#include "Widgets/GridEditor/SelectionWaveform.h"
#include "Widgets/Inspector/Providers/SourcesSectionProvider.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/ProjectTree/ProjectTreeWidget.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/SourceEditor/SourceEditorWidget.h"
#include "Widgets/SourceEditor/SourceEditorWaveformView.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGraphicsScene>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrent>
#include <QtCore/qtestsupport_core.h>

#include <array>
#include <vector>

namespace {

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
    out.append(static_cast<char>((value >> 16) & 0xffu));
    out.append(static_cast<char>((value >> 24) & 0xffu));
}

void writeWav(const QString& path, uint32_t frames, float value)
{
    const std::vector<float> samples(frames, value);
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
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

}

TEST(MediaPoolRefreshTest, RefreshesMetadataAndHashForAnExternallyChangedSource)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    ASSERT_TRUE(QDir().mkpath(audioRoot));
    const QString path = QDir(audioRoot).filePath(QStringLiteral("external.wav"));
    writeWav(path, 480, 0.1f);

    VNX::AssetImporter importer;
    importer.setProjectRoot(dir.path());
    VNX::AssetInfo info;
    info.sourceID = 8601;
    info.relativePath = QStringLiteral("external.wav");
    info.absolutePath = path;
    info.contentHash = VNX::AssetImporter::computeContentHash(path);
    info.metadata = importer.analyzeFile(path);
    info.lastModified = QFileInfo(path).lastModified().toSecsSinceEpoch();
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    const QString oldHash = info.contentHash;

    int modified = 0;
    const QMetaObject::Connection connection = QObject::connect(
        &VNX::AssetRegistry::instance(), &VNX::AssetRegistry::assetModified,
        [&modified](uint32_t sourceID) {
            if (sourceID == 8601) ++modified;
        });

    writeWav(path, 240, 0.2f);
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(info.sourceID, path));
    const VNX::AssetInfo* refreshed =
        VNX::AssetRegistry::instance().getAsset(info.sourceID);
    ASSERT_NE(refreshed, nullptr);
    EXPECT_EQ(refreshed->metadata.totalSamples, 240u);
    EXPECT_NE(refreshed->contentHash, oldHash);
    EXPECT_EQ(modified, 1);

    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(info.sourceID, path));
    EXPECT_EQ(modified, 1);

    QObject::disconnect(connection);
    VNX::AssetRegistry::instance().clear();
}

TEST(MediaPoolRefreshTest, ReplacementPreservesFilesOnSelfCopyAndFailure)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto cleanup = qScopeGuard([&] { registry.clear(); });
    const QString input = dir.filePath(QStringLiteral("Hit.wav"));
    writeWav(input, 480, 0.2f);
    AssetImporter importer;
    importer.setProjectRoot(dir.path());
    const auto imported = importer.importFile(input, nullptr);
    ASSERT_TRUE(imported.success);
    const AssetInfo original = *registry.getAsset(imported.sourceID);
    const auto copied = MediaPool::instance().batchReplace({{original.sourceID, original.absolutePath}});
    ASSERT_EQ(copied.size(), 1);
    EXPECT_TRUE(copied.front().success);
    EXPECT_EQ(AssetImporter::computeContentHash(original.absolutePath), original.contentHash);
    const auto rejected = MediaPool::instance().batchReplace(
        {{original.sourceID, dir.filePath(QStringLiteral("absent.wav"))}});
    ASSERT_EQ(rejected.size(), 1);
    EXPECT_FALSE(rejected.front().success);
    EXPECT_EQ(AssetImporter::computeContentHash(original.absolutePath), original.contentHash);

    AssetInfo duplicate = original;
    duplicate.sourceID = registry.allocateStableID();
    EXPECT_FALSE(registry.registerAsset(duplicate));
    EXPECT_EQ(registry.getAssetByPath(original.relativePath)->sourceID, original.sourceID);
    EXPECT_EQ(registry.count(), 1);
}

TEST(MediaPoolRefreshTest, ExternalFileLifecyclePreservesAuthoredReferencesAcrossViewsAndReload)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    const auto cleanup = qScopeGuard([&] {
        UndoRedoManager::instance().clear();
        manager.closeProject();
    });
    ProjectConfig config;
    config.name = QStringLiteral("ExternalSources");
    config.location = dir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    ASSERT_TRUE(manager.createProject(config));
    manager.setProjectLanguagesInternal({QStringLiteral("English"), QStringLiteral("Japanese")});
    auto& languages = LanguageContextService::instance();
    auto& platforms = PlatformContextService::instance();
    const auto windows = HarkenTest::PlatformIds::Windows;
    const auto android = HarkenTest::PlatformIds::Android;
    languages.setLanguage(QStringLiteral("English"));
    platforms.setSelected({windows}, windows, false);
    manager.fileWatcher()->setDebounceInterval(50);
    const auto debounce = qScopeGuard([&] { manager.fileWatcher()->setDebounceInterval(500); });
    const QString projectPath = manager.currentProject()->projectPath;
    const QString audioRoot = PathUtils::getAudioSourcesDir(registry.projectRoot());
    const QString path = QDir(audioRoot).filePath(QStringLiteral("nested/external.wav"));
    const QString otherPath = QDir(audioRoot).filePath(QStringLiteral("other.wav"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    writeWav(path, 480, 0.1f);
    writeWav(otherPath, 480, 0.2f);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return registry.findAssetByPath(path) && registry.findAssetByPath(otherPath);
    }, 5000));
    const uint32_t sourceID = registry.findAssetByPath(path)->sourceID;
    const uint32_t otherID = registry.findAssetByPath(otherPath)->sourceID;
    EXPECT_FALSE(registry.isUncommitted(sourceID));
    auto* layer = manager.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = manager.createItem(layer, QStringLiteral("ActiveMissing"), ProjectItem::Type::Sound);
    auto* other = manager.createItem(layer, QStringLiteral("InactiveMissing"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(other, nullptr);
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = sourceID;
    source.trimStartFrame = 12;
    source.trimEndFrame = 240;
    SoundSourceDef alternate;
    alternate.entryId = QUuid::createUuid();
    alternate.sourceId = otherID;
    sound->comp<VSound>()->sources.plain.entries = {source, alternate};
    other->comp<VSound>()->sources.plain.entries = {source, alternate};
    SoundSourceSemantics::assignActive(sound->comp<VSound>()->sources.plain, source.entryId);
    SoundSourceSemantics::assignActive(other->comp<VSound>()->sources.plain, alternate.entryId);
    auto* component = sound->comp<VSound>();
    component->distinguishLanguage = true;
    component->sources.localized[QStringLiteral("English")] = component->sources.plain;
    component->sources.plain = {};
    auto& english = component->sources.localized[QStringLiteral("English")];
    SoundSourceSemantics::assignActive(english, alternate.entryId, ProjectPlatformSemantics::idString(android));
    auto& japanese = component->sources.localized[QStringLiteral("Japanese")];
    alternate.entryId = QUuid::createUuid();
    japanese.entries = {alternate};
    SoundSourceSemantics::assignActive(japanese, alternate.entryId);
    sound->markDirty();
    other->markDirty();
    const auto authored = SoundSourceSemantics::toJson(sound->comp<VSound>()->sources);
    const QUuid soundGuid = sound->guid;
    ProjectExplorer explorer;
    BlueprintCanvasWidget blueprint;
    blueprint.useSignalChain(layer);
    BlueprintCanvasWidget projectBlueprint;
    const auto originalFields = blueprint.nodeContentFields();
    const auto restoreFields = qScopeGuard([&] {
        blueprint.setNodeContentFields(originalFields);
        projectBlueprint.setNodeContentFields(originalFields);
    });
    blueprint.setNodeContentFields(kAllNodeContentFields);
    projectBlueprint.setNodeContentFields(kAllNodeContentFields);
    InspectorPanel inspector;
    SourcesSectionProvider provider;
    std::unique_ptr<QWidget> sources(provider.build(&inspector));
    provider.bind(sources.get(), sound);
    int editorRequests = 0;
    QObject::connect(&inspector, &InspectorPanel::externalEditorRequested, sources.get(),
        [&](const ExternalEditorSelection& selection, bool) {
            EXPECT_EQ(selection.kind, ExternalEditorSelection::Kind::Source);
            EXPECT_EQ(selection.ownerGuid, soundGuid);
            EXPECT_EQ(selection.sourceEntryId, source.entryId);
            ++editorRequests;
        });
    SourceEditorWidget editor;
    editor.bindSource(sourceID, soundGuid, source.entryId);
    SelectionWaveform selectionWaveform(soundGuid);
    int selectionEditorRequests = 0;
    QObject::connect(&selectionWaveform, &SelectionWaveform::sourceEditorRequested,
        &selectionWaveform, [&](const QUuid& owner, const QUuid& entry) {
            EXPECT_EQ(owner, soundGuid);
            EXPECT_EQ(entry, source.entryId);
            ++selectionEditorRequests;
        });
    const auto* waveform = editor.findChild<SourceEditorWaveformView*>();
    ASSERT_NE(waveform, nullptr);
    const auto sourceVisible = [&] {
        return !explorer.sourcesTree()->findItems(QStringLiteral("external.wav"),
            Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
    };
    const auto sourceReference = [&](uint32_t id) -> ObjectRefWidget* {
        for (auto* ref : sources->findChildren<ObjectRefWidget*>())
            if (ref->currentObjectID() == id && ref->isVisibleTo(sources.get()))
                return ref;
        return nullptr;
    };
    const auto referenceInvalid = [&] {
        const auto* ref = sourceReference(sourceID);
        return ref && ref->property("inputInvalid").toBool();
    };
    const auto clickWave = [&](bool enabled) {
        auto* ref = sourceReference(sourceID);
        ASSERT_NE(ref, nullptr);
        QWidget* row = ref->parentWidget();
        while (row && row->objectName() != QStringLiteral("listRow"))
            row = row->parentWidget();
        ASSERT_NE(row, nullptr);
        auto* thumb = row->findChild<SourceRangeThumb*>();
        ASSERT_NE(thumb, nullptr);
        for (auto* preview : {thumb, static_cast<SourceRangeThumb*>(&selectionWaveform)}) {
            EXPECT_EQ(preview->isEnabled(), enabled);
            const QPoint point = preview->rect().center();
            QMouseEvent press(QEvent::MouseButtonPress, point, preview->mapToGlobal(point),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(preview, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, point, preview->mapToGlobal(point),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(preview, &release);
        }
        EXPECT_EQ(selectionEditorRequests, editorRequests);
    };
    const auto hasErrorPixels = [](const QImage& image) {
        const QColor error = ThemeManager::instance().colors().error;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 128 && qAbs(pixel.red() - error.red()) < 3
                    && qAbs(pixel.green() - error.green()) < 3
                    && qAbs(pixel.blue() - error.blue()) < 3)
                    return true;
            }
        }
        return false;
    };
    const auto treeError = [&](const QString& name, bool icon = false) {
        const auto rows = explorer.findChild<ProjectTreeWidget*>()->findItems(
            name, Qt::MatchStartsWith | Qt::MatchRecursive);
        if (rows.isEmpty()) return false;
        return icon ? hasErrorPixels(rows.front()->icon(0).pixmap(QSize(20, 20)).toImage())
            : rows.front()->foreground(0).color() == ThemeManager::instance().colors().error;
    };
    const auto blueprintError = [&](const QString& name, bool icon = false) {
        for (auto* item : blueprint.scene()->items()) {
            auto* node = dynamic_cast<NodeItem*>(item);
            if (!node || node->title() != name) continue;
            if (!icon)
                return node->titleColor() == ThemeManager::instance().colors().error;
            QImage image(QSize(qCeil(node->titleRect().left()), qCeil(node->titleRect().bottom())),
                QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            node->paint(&painter, nullptr, nullptr);
            painter.end();
            return hasErrorPixels(image);
        }
        return false;
    };
    const auto expectIcons = [&](uint32_t id, bool missing, bool active) {
        auto* ref = sourceReference(id);
        ASSERT_NE(ref, nullptr);
        bool foundIcon = false;
        for (auto* label : ref->findChildren<QLabel*>()) {
            if (label->pixmap().isNull()) continue;
            foundIcon = true;
            EXPECT_EQ(hasErrorPixels(label->pixmap().toImage()), missing);
        }
        EXPECT_TRUE(foundIcon);
        EXPECT_EQ(treeError(sound->name, true), active);
        EXPECT_EQ(blueprintError(sound->name, true), active);
        EXPECT_FALSE(treeError(other->name, true));
        EXPECT_FALSE(blueprintError(other->name, true));
    };
    const auto soundNode = [&](const BlueprintCanvasWidget& canvas) -> NodeItem* {
        for (auto* item : canvas.scene()->items())
            if (auto* node = dynamic_cast<NodeItem*>(item); node && node->objectID() == sound->objectID)
                return node;
        return nullptr;
    };
    const QColor waveformBackground = ThemeManager::instance().snapshot()->domains.nodeGraphNode.waveformBg;
    const auto renderWaveform = [&](const BlueprintCanvasWidget& canvas) {
        auto* node = soundNode(canvas);
        if (!node) return QImage{};
        QImage image(node->shape().boundingRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        node->paint(&painter, nullptr, nullptr);
        painter.end();
        QRect background;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y) == waveformBackground)
                    background = background.united(QRect(x, y, 1, 1));
        return background.isEmpty() ? QImage{} : image.copy(background.adjusted(3, 3, -3, -3));
    };
    const auto waveformsMatch = [&](bool blank) {
        for (const auto* canvas : {&blueprint, &projectBlueprint}) {
            const auto image = renderWaveform(*canvas);
            if (image.isNull()) return false;
            bool empty = true;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    empty &= image.pixelColor(x, y) == waveformBackground;
            if (empty != blank) return false;
        }
        return true;
    };
    ASSERT_TRUE(sourceVisible());
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(false); }, 5000));
    const std::array<std::pair<BlueprintCanvasWidget*, QSizeF>, 2> waveformViews{{
        {&blueprint, soundNode(blueprint)->shape().boundingRect().size()},
        {&projectBlueprint, soundNode(projectBlueprint)->shape().boundingRect().size()}
    }};
    clickWave(true);
    EXPECT_EQ(editorRequests, 1);
    expectIcons(sourceID, false, false);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QFile::remove(path));
    clickWave(true);
    EXPECT_EQ(editorRequests, 1);
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return !sourceVisible() && referenceInvalid()
            && !waveform->hasWaveform()
            && media.cachedStatus(sourceID) == MediaStatus::Missing
            && treeError(sound->name) && blueprintError(sound->name);
    }, 5000));
    EXPECT_FALSE(treeError(other->name));
    EXPECT_FALSE(blueprintError(other->name));
    clickWave(false);
    EXPECT_EQ(editorRequests, 1);
    expectIcons(sourceID, true, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(true); }, 5000));
    for (const auto& [canvas, size] : waveformViews) {
        EXPECT_EQ(soundNode(*canvas)->shape().boundingRect().size(), size);
        canvas->setNodeContentFields(kAllNodeContentFields & ~NodeContentFields(NodeContentField::Waveform));
        EXPECT_TRUE(renderWaveform(*canvas).isNull());
        EXPECT_LT(soundNode(*canvas)->contentHeight(), size.height());
        canvas->setNodeContentFields(kAllNodeContentFields);
        EXPECT_EQ(soundNode(*canvas)->shape().boundingRect().size(), size);
    }
    EXPECT_TRUE(waveformsMatch(true));
    languages.setLanguage(QStringLiteral("Japanese"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return !treeError(sound->name) && !blueprintError(sound->name); }, 5000));
    EXPECT_TRUE(selectionWaveform.isEnabled());
    EXPECT_EQ(sourceReference(sourceID), nullptr);
    expectIcons(otherID, false, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(false); }, 5000));
    languages.setLanguage(QStringLiteral("English"));
    ASSERT_TRUE(QTest::qWaitFor([&] { return treeError(sound->name) && blueprintError(sound->name); }, 5000));
    EXPECT_FALSE(selectionWaveform.isEnabled());
    expectIcons(sourceID, true, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(true); }, 5000));
    platforms.setSelected({android}, android, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !treeError(sound->name) && !blueprintError(sound->name); }, 5000));
    EXPECT_TRUE(selectionWaveform.isEnabled());
    expectIcons(sourceID, true, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(false); }, 5000));
    platforms.setSelected({windows}, windows, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return treeError(sound->name) && blueprintError(sound->name); }, 5000));
    EXPECT_FALSE(selectionWaveform.isEnabled());
    platforms.setAll(true);
    EXPECT_FALSE(selectionWaveform.isEnabled());
    EXPECT_FALSE(selectionWaveform.toolTip().isEmpty());
    platforms.setSelected({windows}, windows, false);
    expectIcons(sourceID, true, true);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(true); }, 5000));
    EXPECT_EQ(SoundSourceSemantics::toJson(sound->comp<VSound>()->sources), authored);
    EXPECT_EQ(registry.findAssetByPath(path)->sourceID, sourceID);
    ASSERT_TRUE(manager.saveProject());

    writeWav(path, 480, 0.1f);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return sourceVisible() && !referenceInvalid()
            && media.cachedStatus(sourceID) != MediaStatus::Missing
            && !treeError(sound->name) && !blueprintError(sound->name)
            && waveform->hasWaveform()
            && WaveformCache::isValid(path);
    }, 5000));
    clickWave(true);
    EXPECT_EQ(editorRequests, 2);
    expectIcons(sourceID, false, false);
    ASSERT_TRUE(QTest::qWaitFor([&] { return waveformsMatch(false); }, 5000));
    for (const auto& [canvas, size] : waveformViews)
        EXPECT_EQ(soundNode(*canvas)->shape().boundingRect().size(), size);
    EXPECT_EQ(registry.findAssetByPath(path)->sourceID, sourceID);
    const QString oldHash = registry.getAsset(sourceID)->contentHash;
    writeWav(path, 960, 0.3f);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return registry.getAsset(sourceID)->contentHash != oldHash && WaveformCache::isValid(path);
    }, 5000));
    EXPECT_EQ(registry.getAsset(sourceID)->metadata.totalSamples, 960u);
    EXPECT_EQ(SoundSourceSemantics::toJson(sound->comp<VSound>()->sources), authored);

    ASSERT_TRUE(audioRoot.startsWith(dir.path()));
    ASSERT_TRUE(QDir(audioRoot).removeRecursively());
    ASSERT_TRUE(QTest::qWaitFor([&] { return media.cachedStatus(sourceID) == MediaStatus::Missing; }, 5000));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    writeWav(path, 480, 0.4f);
    ASSERT_TRUE(QTest::qWaitFor([&] { return sourceVisible() && !referenceInvalid(); }, 5000));
    ASSERT_TRUE(QFile::remove(path));
    ASSERT_TRUE(QTest::qWaitFor([&] { return media.cachedStatus(sourceID) == MediaStatus::Missing; }, 5000));
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    ASSERT_TRUE(manager.loadProject(projectPath));
    const auto* restored = manager.findItemByGuid(soundGuid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(SoundSourceSemantics::toJson(restored->comp<VSound>()->sources), authored);
    EXPECT_EQ(registry.findAssetByPath(path)->sourceID, sourceID);
    EXPECT_FALSE(registry.sourceFileExists(sourceID));
}

TEST(MediaPoolRefreshTest, ProjectOpenRefreshPreservesNewerEditsAndDiscardsClosedProjects)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    const auto cleanup = qScopeGuard([&] {
        UndoRedoManager::instance().clear();
        manager.closeProject();
        QThreadPool::globalInstance()->waitForDone();
        QApplication::processEvents();
    });
    ProjectConfig config;
    config.name = QStringLiteral("SourceRefresh");
    config.location = dir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    ASSERT_TRUE(manager.createProject(config));
    manager.fileWatcher()->stopWatching();
    const QString projectPath = manager.currentProject()->projectPath;
    const QString audioRoot = PathUtils::getAudioSourcesDir(registry.projectRoot());
    const QString path = QDir(audioRoot).filePath(QStringLiteral("existing.wav"));
    writeWav(path, 480, 0.1f);
    AssetImporter importer;
    AssetInfo info;
    info.sourceID = registry.allocateStableID();
    info.relativePath = QStringLiteral("existing.wav");
    info.absolutePath = path;
    info.metadata = importer.analyzeFile(path);
    info.contentHash = AssetImporter::computeContentHash(path);
    ASSERT_TRUE(registry.registerAsset(info, false));
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();

    writeWav(path, 960, 0.2f);
    const QString added = QDir(audioRoot).filePath(QStringLiteral("added.wav"));
    writeWav(added, 240, 0.3f);
    ASSERT_TRUE(manager.loadProject(projectPath));
    manager.fileWatcher()->stopWatching();
    ASSERT_NE(registry.getAsset(info.sourceID), nullptr);
    EXPECT_EQ(registry.getAsset(info.sourceID)->metadata.totalSamples, 480u);
    int conversions = 0;
    int succeeded = 0;
    int failed = 0;
    const auto conversionConnection = QObject::connect(&media, &MediaPool::batchOperationComplete, &manager,
        [&](int successCount, int failureCount) {
            ++conversions;
            succeeded = successCount;
            failed = failureCount;
        });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(conversionConnection); });
    QThreadPool::globalInstance()->waitForDone();
    media.batchConvert({info.sourceID}, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(QTest::qWaitFor([&] {
        return registry.getAsset(info.sourceID)->metadata.totalSamples == 960
            && registry.findAssetByPath(added) && conversions == 1;
    }, 5000));
    EXPECT_EQ(succeeded, 1);
    EXPECT_EQ(failed, 0);
    ASSERT_TRUE(registry.isCacheValid(info.sourceID, HarkenTest::PlatformIds::Windows));
    const auto cache = registry.getAsset(info.sourceID)->platformCaches.value(HarkenTest::PlatformIds::Windows);
    const QString convertedPath = QDir(PathUtils::getCacheDir(registry.projectRoot())).filePath(cache.cachePath);
    EXPECT_EQ(importer.analyzeFile(convertedPath).totalSamples, 960u);
    EXPECT_FALSE(registry.isUncommitted(registry.findAssetByPath(added)->sourceID));
    EXPECT_EQ(registry.getAsset(info.sourceID)->contentHash, AssetImporter::computeContentHash(path));
    manager.closeProject();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();

    writeWav(path, 1440, 0.4f);
    ASSERT_TRUE(manager.loadProject(projectPath));
    manager.fileWatcher()->stopWatching();
    QThreadPool::globalInstance()->waitForDone();
    media.batchConvert({info.sourceID}, HarkenTest::PlatformIds::Windows);
    auto platforms = manager.currentProject()->platforms;
    platforms.front().profile.performResample = true;
    platforms.front().profile.sizePolicies.front().config.targetSampleRate = 22050;
    manager.setProjectPlatformsInternal(platforms);
    writeWav(path, 1920, 0.5f);
    ASSERT_TRUE(media.refreshSourceFile(info.sourceID, path));
    QApplication::processEvents();
    ASSERT_TRUE(QTest::qWaitFor([&] { return !ProjectPlatformActivity::instance().isBusy(); }, 5000));
    EXPECT_EQ(conversions, 1);
    EXPECT_EQ(registry.getAsset(info.sourceID)->metadata.totalSamples, 1920u);
    EXPECT_EQ(registry.getAsset(info.sourceID)->contentHash, AssetImporter::computeContentHash(path));
    manager.closeProject();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();

    const QString abandoned = QDir(audioRoot).filePath(QStringLiteral("abandoned.wav"));
    writeWav(abandoned, 240, 0.6f);
    ASSERT_TRUE(manager.loadProject(projectPath));
    QThreadPool::globalInstance()->waitForDone();
    media.batchConvert({info.sourceID}, HarkenTest::PlatformIds::Windows);
    manager.closeProject();
    config.name = QStringLiteral("OtherProject");
    ASSERT_TRUE(manager.createProject(config));
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    EXPECT_EQ(registry.count(), 0);
    ASSERT_TRUE(QTest::qWaitFor([&] { return !ProjectPlatformActivity::instance().isBusy(); }, 5000));
    EXPECT_EQ(conversions, 1);
}

TEST(MediaPoolRefreshTest, WaveformNotificationsUpdateSharedNodesWithoutClearingOtherSources)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    const auto cleanup = qScopeGuard([&] {
        UndoRedoManager::instance().clear();
        manager.closeProject();
        QThreadPool::globalInstance()->waitForDone();
        QApplication::processEvents();
    });
    ProjectConfig config;
    config.name = QStringLiteral("SharedWaveforms");
    config.location = dir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    ASSERT_TRUE(manager.createProject(config));
    manager.fileWatcher()->stopWatching();
    const QString audioRoot = PathUtils::getAudioSourcesDir(registry.projectRoot());
    std::array<AssetInfo, 2> assets;
    AssetImporter importer;
    for (size_t index = 0; index < assets.size(); ++index) {
        auto& asset = assets[index];
        asset.sourceID = registry.allocateStableID();
        asset.relativePath = QStringLiteral("source%1.wav").arg(index);
        asset.absolutePath = QDir(audioRoot).filePath(asset.relativePath);
        writeWav(asset.absolutePath, 480, index == 0 ? 0.2f : 0.6f);
        asset.metadata = importer.analyzeFile(asset.absolutePath);
        asset.contentHash = AssetImporter::computeContentHash(asset.absolutePath);
        ASSERT_TRUE(registry.registerAsset(asset, false));
        ASSERT_TRUE(WaveformCache::generate(asset.absolutePath));
    }
    auto* layer = manager.createItem(nullptr, "Layer", ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    std::array<uint32_t, 3> sounds;
    for (size_t index = 0; index < sounds.size(); ++index) {
        auto* sound = manager.createItem(layer, QStringLiteral("Sound%1").arg(index), ProjectItem::Type::Sound);
        ASSERT_NE(sound, nullptr);
        sounds[index] = sound->objectID;
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = assets[index == 2 ? 1 : 0].sourceID;
        auto& group = sound->comp<VSound>()->sources.plain;
        group.entries = {source};
        SoundSourceSemantics::assignActive(group, source.entryId);
    }
    BlueprintCanvasWidget blueprint;
    blueprint.setNodeContentFields(kAllNodeContentFields);
    blueprint.rebuildFromProject();
    BlueprintCanvasWidget chain;
    chain.setNodeContentFields(kAllNodeContentFields);
    chain.useSignalChain(layer);
    const auto render = [](BlueprintCanvasWidget& canvas, uint32_t id) {
        for (auto* item : canvas.scene()->items()) {
            auto* node = dynamic_cast<NodeItem*>(item);
            if (!node || node->objectID() != id) continue;
            QImage image(node->shape().boundingRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            node->paint(&painter, nullptr, nullptr);
            return image;
        }
        return QImage{};
    };
    for (auto* canvas : {&blueprint, &chain}) {
        std::array<QImage, 3> before;
        for (size_t index = 0; index < sounds.size(); ++index) {
            before[index] = render(*canvas, sounds[index]);
            ASSERT_FALSE(before[index].isNull());
        }
        WaveformCache::remove(assets[0].absolutePath);
        WaveformCache::remove(assets[1].absolutePath);
        emit media.waveformReady(assets[0].sourceID);
        EXPECT_NE(render(*canvas, sounds[0]), before[0]);
        EXPECT_NE(render(*canvas, sounds[1]), before[1]);
        EXPECT_EQ(render(*canvas, sounds[2]), before[2]);
        ASSERT_TRUE(WaveformCache::generate(assets[0].absolutePath));
        ASSERT_TRUE(WaveformCache::generate(assets[1].absolutePath));
        emit media.waveformReady(assets[0].sourceID);
        emit media.waveformReady(assets[1].sourceID);
        for (size_t index = 0; index < sounds.size(); ++index)
            EXPECT_EQ(render(*canvas, sounds[index]), before[index]);
    }
}

TEST(MediaPoolRefreshTest, SharedWaveformsFollowPreviewLifetimeAcrossRangeEdits)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto cleanup = qScopeGuard([&] { registry.clear(); });
    const QString audioRoot = PathUtils::getAudioSourcesDir(dir.path());
    ASSERT_TRUE(QDir().mkpath(audioRoot));
    AssetInfo asset;
    asset.sourceID = registry.allocateStableID();
    asset.relativePath = QStringLiteral("shared.wav");
    asset.absolutePath = QDir(audioRoot).filePath(asset.relativePath);
    writeWav(asset.absolutePath, 480, 0.2f);
    AssetImporter importer;
    asset.metadata = importer.analyzeFile(asset.absolutePath);
    ASSERT_TRUE(registry.registerAsset(asset, false));
    ASSERT_TRUE(WaveformCache::generate(asset.absolutePath));

    std::weak_ptr<const WaveformPeakData> shared;
    {
        SourceRangeThumb first;
        SourceRangeThumb second;
        SoundSourceDef source;
        source.sourceId = asset.sourceID;
        first.setSource(source, &asset);
        shared = WaveformCache::loadShared(asset.absolutePath);
        ASSERT_FALSE(shared.expired());
        EXPECT_EQ(shared.lock(), WaveformCache::loadShared(QDir(audioRoot).filePath("./shared.wav")));

        source.trimStartFrame = 120;
        second.setSource(source, &asset);
        first.clearSource();
        ASSERT_FALSE(shared.expired());

        second.resize(240, 36);
        const QImage before = second.grab().toImage();
        source.trimEndFrame = 240;
        second.setSource(source, &asset);
        EXPECT_FALSE(shared.expired());
        EXPECT_NE(second.grab().toImage(), before);
    }
    EXPECT_TRUE(shared.expired());
}

TEST(MediaPoolRefreshTest, SharedWaveformsRefreshAfterSourceAndCacheChanges)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("shared.wav"));
    writeWav(path, 480, 0.2f);
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto original = WaveformCache::loadShared(path);
    ASSERT_NE(original, nullptr);
    ASSERT_TRUE(original->isValid());
    EXPECT_FLOAT_EQ(original->peaks[1], 0.2f);
    QFile cache(WaveformCache::cachePath(path));
    ASSERT_TRUE(cache.open(QIODevice::ReadOnly));
    const QByteArray originalCache = cache.readAll();
    cache.close();

    ASSERT_TRUE(WaveformCache::generate(path, 64));
    const auto rebuilt = WaveformCache::loadShared(path);
    ASSERT_NE(rebuilt, nullptr);
    EXPECT_EQ(rebuilt->numPeaks, 64u);
    EXPECT_EQ(original->numPeaks, 128u);

    ASSERT_TRUE(cache.open(QIODevice::WriteOnly));
    ASSERT_EQ(cache.write(originalCache), originalCache.size());
    cache.close();
    const auto replaced = WaveformCache::loadShared(path);
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->numPeaks, 128u);
    EXPECT_EQ(rebuilt->numPeaks, 64u);

    ASSERT_TRUE(WaveformCache::remove(path));
    EXPECT_EQ(WaveformCache::loadShared(path), nullptr);
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto restored = WaveformCache::loadShared(path);
    ASSERT_NE(restored, nullptr);
    ASSERT_TRUE(QFile::remove(WaveformCache::cachePath(path)));
    EXPECT_EQ(WaveformCache::loadShared(path), nullptr);
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto beforeEdit = WaveformCache::loadShared(path);
    ASSERT_NE(beforeEdit, nullptr);

    const QDateTime changedTime = QFileInfo(path).lastModified().addSecs(1);
    writeWav(path, 480, 0.6f);
    QFile audio(path);
    ASSERT_TRUE(audio.open(QIODevice::ReadWrite));
    ASSERT_TRUE(audio.setFileTime(changedTime, QFileDevice::FileModificationTime));
    audio.close();
    EXPECT_EQ(WaveformCache::loadShared(path), nullptr);
    ASSERT_TRUE(WaveformCache::generate(path, 128));
    const auto updated = WaveformCache::loadShared(path);
    ASSERT_NE(updated, nullptr);
    EXPECT_FLOAT_EQ(updated->peaks[1], 0.6f);
    EXPECT_FLOAT_EQ(beforeEdit->peaks[1], 0.2f);
    ASSERT_TRUE(QFile::remove(path));
    EXPECT_EQ(WaveformCache::loadShared(path), nullptr);
}

TEST(MediaPoolRefreshTest, CacheRemovalInvalidatesPendingWaveformPublication)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("pending.wav"));
    writeWav(path, 480, 0.1f);
    ASSERT_TRUE(WaveformCache::generate(path));

    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threadCount = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started;
    QSemaphore resume;
    auto blocker = QtConcurrent::run([&] {
        started.release();
        resume.acquire();
    });
    const auto cleanup = qScopeGuard([&] {
        resume.release();
        blocker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount(threadCount);
    });
    started.acquire();
    auto waveform = WaveformCache::generateAsync(path);
    ASSERT_TRUE(WaveformCache::remove(path));
    resume.release();
    waveform.waitForFinished();
    EXPECT_FALSE(waveform.result());
    EXPECT_FALSE(WaveformCache::isValid(path));
}

TEST(MediaPoolRefreshTest, SourceChangesInvalidateQueuedReadsAndResumeDeferredWaveforms)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    const QString path = QDir(PathUtils::getAudioSourcesDir(dir.path()))
        .filePath(QStringLiteral("pending.wav"));
    const QString replacement = dir.filePath(QStringLiteral("replacement.wav"));
    writeWav(path, 480, 0.1f);
    writeWav(replacement, 240, 0.7f);
    AssetImporter importer;
    importer.setProjectRoot(dir.path());
    AssetInfo asset;
    asset.sourceID = 8699;
    asset.relativePath = QStringLiteral("pending.wav");
    asset.absolutePath = path;
    asset.metadata = importer.analyzeFile(path);
    asset.contentHash = AssetImporter::computeContentHash(path);
    ASSERT_TRUE(registry.registerAsset(asset, false));

    auto* pool = QThreadPool::globalInstance();
    pool->waitForDone();
    const int threadCount = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started;
    QSemaphore resume;
    auto blocker = QtConcurrent::run([&] {
        started.release();
        resume.acquire();
    });
    const auto restorePool = qScopeGuard([&] {
        resume.release();
        blocker.waitForFinished();
        pool->waitForDone();
        pool->setMaxThreadCount(threadCount);
    });
    started.acquire();
    auto queued = WaveformCache::generateAsync(path, 64);
    {
        media.beginSourceFileChange();
        const auto finishChange = qScopeGuard([&] { media.endSourceFileChange(); });
        ASSERT_TRUE(SourceFileSystem::copyFile(replacement, path));
    }
    resume.release();
    queued.waitForFinished();
    EXPECT_FALSE(queued.result());
    pool->waitForDone();
    const auto resumed = WaveformCache::loadShared(path);
    ASSERT_NE(resumed, nullptr);
    EXPECT_EQ(resumed->numPeaks, 64u);
    EXPECT_FLOAT_EQ(resumed->peaks[1], 0.7f);
    EXPECT_EQ(AssetImporter::computeContentHash(path), AssetImporter::computeContentHash(replacement));

    ASSERT_TRUE(WaveformCache::remove(path));
    {
        media.beginSourceFileChange();
        const auto finishOuter = qScopeGuard([&] { media.endSourceFileChange(); });
        {
            media.beginSourceFileChange();
            const auto finishInner = qScopeGuard([&] { media.endSourceFileChange(); });
            media.refreshWaveform(asset.sourceID);
        }
        EXPECT_FALSE(WaveformCache::isValid(path));
    }
    pool->waitForDone();
    const auto waveform = WaveformCache::loadShared(path);
    ASSERT_NE(waveform, nullptr);
    EXPECT_FLOAT_EQ(waveform->peaks[1], 0.7f);
    const auto range = WaveformCache::readRange(path, 0, 240, 32);
    ASSERT_TRUE(range.isValid());
    EXPECT_FLOAT_EQ(range.peaks[1], 0.7f);
}

TEST(MediaPoolRefreshTest, SourceChangesPublishCompletedReplacementsBeforeQueuedNotifications)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    const auto cleanup = qScopeGuard([&] {
        if (manager.isProjectOpen())
            EXPECT_TRUE(manager.closeProject());
    });
    ProjectConfig config;
    config.name = QStringLiteral("ReplacementOrdering");
    config.location = dir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    ASSERT_TRUE(manager.createProject(config));
    manager.fileWatcher()->stopWatching();
    const QString path = QDir(PathUtils::getAudioSourcesDir(registry.projectRoot()))
        .filePath(QStringLiteral("replace.wav"));
    const QString replacement = dir.filePath(QStringLiteral("replacement.wav"));
    const QString nextReplacement = dir.filePath(QStringLiteral("next.wav"));
    writeWav(path, 480, 0.1f);
    writeWav(replacement, 240, 0.7f);
    writeWav(nextReplacement, 120, 0.3f);
    const QString replacementHash = AssetImporter::computeContentHash(replacement);
    const QString nextHash = AssetImporter::computeContentHash(nextReplacement);
    AssetImporter importer;
    importer.setProjectRoot(registry.projectRoot());
    AssetInfo asset;
    asset.sourceID = 8700;
    asset.relativePath = QStringLiteral("replace.wav");
    asset.absolutePath = path;
    asset.metadata = importer.analyzeFile(path);
    asset.contentHash = AssetImporter::computeContentHash(path);
    ASSERT_TRUE(registry.registerAsset(asset, false));
    int completed = 0;
    const auto connection = QObject::connect(&media, &MediaPool::batchReplaceFinished,
        [&](const QVector<BatchReplaceResult>& results) {
            ++completed;
            ASSERT_EQ(results.size(), 1);
            EXPECT_TRUE(results.front().success);
            const auto* updated = registry.getAsset(asset.sourceID);
            ASSERT_NE(updated, nullptr);
            EXPECT_EQ(updated->metadata.totalSamples, 240u);
            EXPECT_EQ(updated->contentHash, replacementHash);
        });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    media.batchReplaceAsync({{asset.sourceID, replacement}});
    QString error;
    auto command = SourceFileSystem::replaceFiles({{asset.relativePath, nextReplacement}}, &error);
    ASSERT_NE(command, nullptr) << error.toStdString();
    EXPECT_EQ(completed, 1);
    EXPECT_EQ(AssetImporter::computeContentHash(path), nextHash);
    ASSERT_NE(registry.getAsset(asset.sourceID), nullptr);
    EXPECT_EQ(registry.getAsset(asset.sourceID)->contentHash, nextHash);
    ASSERT_TRUE(command->tryReplay(false));
    EXPECT_EQ(AssetImporter::computeContentHash(path), replacementHash);
    ASSERT_NE(registry.getAsset(asset.sourceID), nullptr);
    EXPECT_EQ(registry.getAsset(asset.sourceID)->metadata.totalSamples, 240u);
    EXPECT_EQ(registry.getAsset(asset.sourceID)->contentHash, replacementHash);
    QCoreApplication::processEvents();
    EXPECT_EQ(completed, 1);
}

TEST(MediaPoolRefreshTest, SupersedingWaveformJobsNotifyViewsAfterProjectReopen)
{
    using namespace VNX;
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& media = MediaPool::instance();
    const auto cleanup = qScopeGuard([&] {
        QThreadPool::globalInstance()->waitForDone();
        QApplication::processEvents();
        UndoRedoManager::instance().clear();
        manager.closeProject();
    });
    ProjectConfig config;
    config.name = QStringLiteral("WaveformReadyProbe");
    config.location = dir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    ASSERT_TRUE(manager.createProject(config));
    manager.fileWatcher()->stopWatching();
    const QString audioRoot = PathUtils::getAudioSourcesDir(registry.projectRoot());
    const QString path = QDir(audioRoot).filePath(QStringLiteral("long.wav"));
    writeWav(path, 48000 * 120, 0.2f);
    AssetImporter importer;
    AssetInfo info;
    info.sourceID = registry.allocateStableID();
    info.relativePath = "long.wav";
    info.absolutePath = path;
    info.metadata = importer.analyzeFile(path);
    info.contentHash = AssetImporter::computeContentHash(path);
    ASSERT_TRUE(registry.registerAsset(info, false));
    auto* layer = manager.createItem(nullptr, "Layer", ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* sound = manager.createItem(layer, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = info.sourceID;
    sound->comp<VSound>()->sources.plain.entries = {source};
    SoundSourceSemantics::assignActive(sound->comp<VSound>()->sources.plain, source.entryId);
    sound->markDirty();
    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    WaveformCache::remove(path);
    ASSERT_FALSE(WaveformCache::isValid(path));
    int readyCount = 0;
    const auto readyConnection = QObject::connect(&media, &MediaPool::waveformReady, &manager,
        [&](uint32_t id) { if (id == info.sourceID) ++readyCount; });
    QFuture<int> rebuild;
    const auto rebuildConnection = QObject::connect(&manager, &ProjectManager::projectOpened, &manager,
        [&](ProjectData* opened) { rebuild = WaveformCache::rebuildAllAsync(opened, audioRoot); });
    const auto disconnect = qScopeGuard([&] {
        QObject::disconnect(readyConnection);
        QObject::disconnect(rebuildConnection);
    });
    BlueprintCanvasWidget blueprint;
    blueprint.setNodeContentFields(kAllNodeContentFields);
    ASSERT_TRUE(manager.loadProject(projectPath));
    ASSERT_TRUE(QTest::qWaitFor([&] { return rebuild.isFinished(); }, 10000));
    QThreadPool::globalInstance()->waitForDone();
    QApplication::processEvents();
    ASSERT_TRUE(WaveformCache::isValid(path));
    EXPECT_GT(readyCount, 0);
    auto renderSound = [&]() {
        for (auto* item : blueprint.scene()->items()) {
            auto* node = dynamic_cast<NodeItem*>(item);
            if (!node || node->title() != "Sound") continue;
            QImage image(node->shape().boundingRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            node->paint(&painter, nullptr, nullptr);
            return image;
        }
        return QImage{};
    };
    const QImage actual = renderSound();
    ASSERT_FALSE(actual.isNull());
    emit media.waveformReady(info.sourceID);
    const QImage refreshed = renderSound();
    EXPECT_TRUE(actual == refreshed) << "Blueprint stayed stale until a manual waveformReady notification";
}
