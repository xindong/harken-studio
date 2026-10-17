#include "AssetPipeline/ImportSoundSourcesCommand.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/MediaPool.h"
#include "Components/VSound.h"
#include "LanguageContextService.h"
#include "MemorySettingsStorage.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "SoundSourceEditing.h"
#include "SoundSourceSemantics.h"
#include "UndoCommands.h"
#include "Utils/AudioFileReader.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <memory>
#include <vector>

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

QByteArray smplChunk(uint32_t sampleRate, uint32_t loopStart, uint32_t loopEndInclusive)
{
    QByteArray body;
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, 1000000000u / sampleRate);
    appendU32(body, 60);
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, 1);
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, 0);
    appendU32(body, loopStart);
    appendU32(body, loopEndInclusive);
    appendU32(body, 0);
    appendU32(body, 0);
    QByteArray chunk;
    chunk.append("smpl", 4);
    appendU32(chunk, static_cast<uint32_t>(body.size()));
    chunk.append(body);
    return chunk;
}

void writeWav(const QString& path, float value, const QByteArray& extraChunk = {})
{
    const std::vector<float> samples(480, value);
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 48000;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    constexpr uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize)
        + static_cast<uint32_t>(extraChunk.size()) + (8 + dataBytes);
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
    bytes.append(extraChunk);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

class ImportSoundSourcesCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        LanguageContextService::instance().setStorageForTesting(
            std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("SoundSourcesProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
        layer_ = ProjectManager::instance().createItem(
            nullptr, QStringLiteral("Imported Layer"), ProjectItem::Type::Layer);
        ASSERT_NE(layer_, nullptr);
        binding_ = findFileBindingForItem(
            *ProjectManager::instance().currentProject(), layer_);
        ASSERT_NE(binding_, nullptr);
        sound_ = ProjectManager::instance().createItem(
            layer_, QStringLiteral("MultiSource"), ProjectItem::Type::Sound);
        ASSERT_NE(sound_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        LanguageContextService::instance().setStorageForTesting(
            SettingsStorageFactory::create(
                SettingsStorageFactory::StorageType::QSettings,
                QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    QTemporaryDir tempDir_;
    ProjectFileBinding* binding_ = nullptr;
    ProjectItem* layer_ = nullptr;
    ProjectItem* sound_ = nullptr;
};

}

TEST_F(ImportSoundSourcesCommandTest, FirstImportActivatesFirstRowAndRoundTripsUndo)
{
    const QString first = QDir(tempDir_.path()).filePath(QStringLiteral("first.wav"));
    const QString second = QDir(tempDir_.path()).filePath(QStringLiteral("second.wav"));
    writeWav(first, 0.1f);
    writeWav(second, 0.2f);

    auto* command = ImportSoundSourcesCommand::pushImport(
        {first, second}, binding_, sound_);
    ASSERT_EQ(command->successCount(), 2);
    ASSERT_EQ(layer_->children.size(), 1u);
    auto* ledger = sound_->comp<VSound>();
    ASSERT_NE(ledger, nullptr);
    ASSERT_EQ(ledger->sources.plain.entries.size(), 2u);
    EXPECT_EQ(ledger->sources.plain.entries.front().trimStartFrame, 0u);
    EXPECT_FALSE(ledger->sources.plain.entries.front().trimEndFrame);
    EXPECT_FALSE(ledger->sources.plain.entries.front().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopProvenance, SoundLoopProvenance::Authored);
    EXPECT_FALSE(ledger->sources.plain.entries.front().rangeContentHash.isEmpty());
    const uint32_t firstID = ledger->sources.plain.entries.front().sourceId;
    const uint32_t secondID = ledger->sources.plain.entries.back().sourceId;
    const auto selection = ledger->sources.plain.active;
    EXPECT_EQ(selection, ledger->sources.plain.entries.front().entryId);
    EXPECT_EQ(sound_->sourceID, firstID);
    EXPECT_FALSE(sound_->sourcePath.isEmpty());

    const auto refs = collectMediaFiles(binding_->rootItem.get());
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_TRUE(ownsMedia(binding_->rootItem.get(), firstID));
    EXPECT_TRUE(ownsMedia(binding_->rootItem.get(), secondID));
    EXPECT_NE(MediaPool::instance().computeStatus(secondID), MediaStatus::Orphaned);

    const QString importedPath = AssetRegistry::instance().getAsset(firstID)->absolutePath;
    AudioFileReader reader(importedPath);
    ASSERT_TRUE(reader.openReadOnly());
    const QByteArray originalBytes = reader.readAll();
    ASSERT_FALSE(originalBytes.isEmpty());

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(ledger->sources.plain.entries.empty());
    EXPECT_TRUE(ledger->sources.plain.active.isNull());
    EXPECT_EQ(sound_->sourceID, 0u);
    EXPECT_TRUE(sound_->sourcePath.isEmpty());
    EXPECT_FALSE(AssetRegistry::instance().contains(firstID));
    EXPECT_FALSE(AssetRegistry::instance().contains(secondID));
    EXPECT_FALSE(QFile::exists(importedPath));
    ASSERT_TRUE(reader.seek(0));
    EXPECT_EQ(reader.readAll(), originalBytes);

    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(ledger->sources.plain.entries.size(), 2u);
    EXPECT_EQ(ledger->sources.plain.entries.front().sourceId, firstID);
    EXPECT_EQ(ledger->sources.plain.entries.back().sourceId, secondID);
    EXPECT_EQ(ledger->sources.plain.active, selection);
    EXPECT_EQ(sound_->sourceID, firstID);
    EXPECT_FALSE(sound_->sourcePath.isEmpty());
    EXPECT_TRUE(AssetRegistry::instance().contains(firstID));
    EXPECT_TRUE(AssetRegistry::instance().contains(secondID));
    AudioFileReader restored(importedPath);
    ASSERT_TRUE(restored.openReadOnly());
    EXPECT_EQ(restored.readAll(), originalBytes);
}

TEST_F(ImportSoundSourcesCommandTest, ReusesSharedAssetAndDoesNotChangeExistingActive)
{
    const QString first = QDir(tempDir_.path()).filePath(QStringLiteral("shared.wav"));
    writeWav(first, 0.3f);
    auto* firstCommand = ImportSoundSourcesCommand::pushImport(
        {first}, binding_, sound_);
    ASSERT_EQ(firstCommand->successCount(), 1);
    auto* ledger = sound_->comp<VSound>();
    ASSERT_NE(ledger, nullptr);
    const uint32_t sourceID = ledger->sources.plain.entries.front().sourceId;
    const auto selection = ledger->sources.plain.active;

    auto* secondCommand = ImportSoundSourcesCommand::pushImport(
        {first}, binding_, sound_);
    ASSERT_EQ(secondCommand->successCount(), 1);
    EXPECT_EQ(secondCommand->duplicateCount(), 1);
    ASSERT_EQ(ledger->sources.plain.entries.size(), 2u);
    EXPECT_EQ(ledger->sources.plain.entries.back().sourceId, sourceID);
    EXPECT_EQ(ledger->sources.plain.active, selection);

    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_EQ(ledger->sources.plain.entries.size(), 1u);
    EXPECT_TRUE(AssetRegistry::instance().contains(sourceID));
    EXPECT_EQ(ledger->sources.plain.entries.front().sourceId, sourceID);
    EXPECT_EQ(ledger->sources.plain.active, selection);
}

TEST_F(ImportSoundSourcesCommandTest, NonLocalizedImportDoesNotAuthorRequestedLanguage)
{
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English")});
    LanguageContextService::instance().setLanguage(QStringLiteral("English"));
    const QString path = QDir(tempDir_.path()).filePath(QStringLiteral("effect.wav"));
    writeWav(path, 0.4f);
    auto* command = ImportSoundSourcesCommand::pushImport(
        {path}, binding_, sound_, QStringLiteral("English"));
    ASSERT_EQ(command->successCount(), 1);
    const auto* sound = sound_->comp<VSound>();
    ASSERT_EQ(sound->sources.plain.entries.size(), 1u);
    EXPECT_FALSE(SoundSourceSemantics::entriesToJson(sound->sources.plain.entries).first().toObject()
                     .contains(QStringLiteral("language")));
    EXPECT_EQ(sound->sources.plain.active, sound->sources.plain.entries.front().entryId);
}

TEST_F(ImportSoundSourcesCommandTest, LocalizedImportsUseTheBarOrExplicitGroupAndRetainItOnRedo)
{
    ProjectManager::instance().setProjectLanguagesInternal(
        {QStringLiteral("English"), QStringLiteral("Japanese")});
    LanguageContextService::instance().setLanguage(QStringLiteral("Japanese"));
    auto* sound = sound_->comp<VSound>();
    sound->distinguishLanguage = true;
    const QString first = QDir(tempDir_.path()).filePath(QStringLiteral("first.wav"));
    const QString second = QDir(tempDir_.path()).filePath(QStringLiteral("second.wav"));
    writeWav(first, 0.1f);
    writeWav(second, 0.2f);
    auto* firstCommand = ImportSoundSourcesCommand::pushImport({first}, binding_, sound_);
    ASSERT_EQ(firstCommand->successCount(), 1);
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    const auto selection = sound->sources.localized["Japanese"].active;
    EXPECT_EQ(selection, sound->sources.localized["Japanese"].entries.front().entryId);
    LanguageContextService::instance().setLanguage(QStringLiteral("English"));
    auto* secondCommand = ImportSoundSourcesCommand::pushImport(
        {second}, binding_, sound_, QStringLiteral("Japanese"));
    ASSERT_EQ(secondCommand->successCount(), 1);
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 2u);
    const auto imported = sound->sources.localized["Japanese"].entries.back();
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 1u);
    EXPECT_FALSE(AssetRegistry::instance().contains(imported.sourceId));
    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(sound->sources.localized["Japanese"].entries.size(), 2u);
    EXPECT_EQ(sound->sources.localized["Japanese"].entries.back(), imported);
    EXPECT_TRUE(AssetRegistry::instance().contains(imported.sourceId));
    EXPECT_EQ(sound->sources.localized["Japanese"].active, selection);
    UndoRedoManager::instance().undoStack()->undo();
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(sound->sources.localized.isEmpty());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(sound->sources.localized["Japanese"].active, selection);
}

TEST_F(ImportSoundSourcesCommandTest, AppendingToUnselectedExistingSourcesDoesNotAssignActive)
{
    const QString first = QDir(tempDir_.path()).filePath(QStringLiteral("first.wav"));
    const QString second = QDir(tempDir_.path()).filePath(QStringLiteral("second.wav"));
    writeWav(first, 0.1f);
    writeWav(second, 0.2f);
    ASSERT_EQ(ImportSoundSourcesCommand::pushImport({first}, binding_, sound_)->successCount(), 1);
    auto* sound = sound_->comp<VSound>();
    sound->sources.plain.active = QUuid{};
    ASSERT_EQ(ImportSoundSourcesCommand::pushImport({second}, binding_, sound_)->successCount(), 1);
    ASSERT_EQ(sound->sources.plain.entries.size(), 2u);
    EXPECT_TRUE(sound->sources.plain.active.isNull());
    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_EQ(sound->sources.plain.entries.size(), 1u);
    EXPECT_TRUE(sound->sources.plain.active.isNull());
    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(sound->sources.plain.entries.size(), 2u);
    EXPECT_TRUE(sound->sources.plain.active.isNull());
}

TEST_F(ImportSoundSourcesCommandTest, LocalizedImportRejectsInvalidGroupsBeforeImportingAssets)
{
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("English")});
    auto* sound = sound_->comp<VSound>();
    sound->distinguishLanguage = true;
    const QString path = QDir(tempDir_.path()).filePath(QStringLiteral("rejected.wav"));
    writeWav(path, 0.6f);
    const int assetCount = AssetRegistry::instance().count();
    for (const QString& language : {QStringLiteral("Japanese"), QStringLiteral("english")}) {
        auto* command = ImportSoundSourcesCommand::pushImport({path}, binding_, sound_, language);
        EXPECT_EQ(command->successCount(), 0);
        EXPECT_EQ(command->failCount(), 1);
        EXPECT_TRUE(command->importedIDs().isEmpty());
    }
    EXPECT_EQ(AssetRegistry::instance().count(), assetCount);
    EXPECT_TRUE(sound->sources.plain.entries.empty());
    EXPECT_FALSE(UndoRedoManager::instance().canUndo());
}

TEST_F(ImportSoundSourcesCommandTest, LocalizedImportRequiresExistingDeletedLanguagesToBeRepaired)
{
    ProjectManager::instance().setProjectLanguagesInternal(
        {QStringLiteral("English"), QStringLiteral("Japanese")});
    auto* sound = sound_->comp<VSound>();
    sound->distinguishLanguage = true;
    const QString first = QDir(tempDir_.path()).filePath(QStringLiteral("english.wav"));
    const QString second = QDir(tempDir_.path()).filePath(QStringLiteral("japanese.wav"));
    writeWav(first, 0.1f);
    writeWav(second, 0.2f);
    ASSERT_EQ(ImportSoundSourcesCommand::pushImport(
        {first}, binding_, sound_, QStringLiteral("English"))->successCount(), 1);
    ProjectManager::instance().setProjectLanguagesInternal({QStringLiteral("Japanese")});
    const int assetCount = AssetRegistry::instance().count();
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    auto* command = ImportSoundSourcesCommand::pushImport(
        {second}, binding_, sound_, QStringLiteral("Japanese"));
    EXPECT_EQ(command->successCount(), 0);
    EXPECT_EQ(command->failCount(), 1);
    EXPECT_EQ(AssetRegistry::instance().count(), assetCount);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
    ASSERT_EQ(sound->sources.localized["English"].entries.size(), 1u);
}

TEST_F(ImportSoundSourcesCommandTest, SeedsLoopPointsFromTheFileAndNeverReseedsAnAuthoredRow)
{
    const QString path = QDir(tempDir_.path()).filePath(QStringLiteral("looped.wav"));
    writeWav(path, 0.4f, smplChunk(48000, 100, 199));

    auto* command = ImportSoundSourcesCommand::pushImport({path}, binding_, sound_);
    ASSERT_EQ(command->successCount(), 1);
    auto* ledger = sound_->comp<VSound>();
    ASSERT_NE(ledger, nullptr);
    ASSERT_EQ(ledger->sources.plain.entries.size(), 1u);
    ASSERT_TRUE(ledger->sources.plain.entries.front().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->startFrame, 100u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->endFrame, 200u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopProvenance, SoundLoopProvenance::File);
    EXPECT_EQ(ledger->sources.plain.entries.front().trimStartFrame, 0u);
    EXPECT_FALSE(ledger->sources.plain.entries.front().trimEndFrame);
    EXPECT_FALSE(ledger->sources.plain.entries.front().rangeContentHash.isEmpty());

    const auto resolve = [](uint32_t sourceId) {
        return AssetRegistry::instance().getAsset(sourceId);
    };
    EXPECT_FALSE(SoundSourceSemantics::hasDeepSourceSemantics(*ledger, resolve));

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(ledger->sources.plain.entries.empty());
    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(ledger->sources.plain.entries.size(), 1u);
    ASSERT_TRUE(ledger->sources.plain.entries.front().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->endFrame, 200u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopProvenance, SoundLoopProvenance::File);

    ledger->sources.plain.entries.front().loopRegion = SoundLoopRegion{300, 400};
    ledger->sources.plain.entries.front().loopProvenance = SoundLoopProvenance::Authored;
    const QUuid authored = ledger->sources.plain.entries.front().entryId;
    EXPECT_TRUE(SoundSourceSemantics::hasDeepSourceSemantics(*ledger, resolve));

    auto* again = ImportSoundSourcesCommand::pushImport({path}, binding_, sound_);
    ASSERT_EQ(again->successCount(), 1);
    ASSERT_EQ(ledger->sources.plain.entries.size(), 2u);
    ASSERT_EQ(ledger->sources.plain.entries.front().entryId, authored);
    ASSERT_TRUE(ledger->sources.plain.entries.front().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->startFrame, 300u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->endFrame, 400u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopProvenance, SoundLoopProvenance::Authored);
    ASSERT_TRUE(ledger->sources.plain.entries.back().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.back().loopRegion->endFrame, 200u);
    EXPECT_EQ(ledger->sources.plain.entries.back().loopProvenance, SoundLoopProvenance::File);
}

TEST_F(ImportSoundSourcesCommandTest, SeedsASingleSampleLoopAsOneWholeFrame)
{
    const QString path = QDir(tempDir_.path()).filePath(QStringLiteral("tick.wav"));
    writeWav(path, 0.5f, smplChunk(48000, 409, 409));

    auto* command = ImportSoundSourcesCommand::pushImport({path}, binding_, sound_);
    ASSERT_EQ(command->successCount(), 1);
    auto* ledger = sound_->comp<VSound>();
    ASSERT_EQ(ledger->sources.plain.entries.size(), 1u);
    ASSERT_TRUE(ledger->sources.plain.entries.front().loopRegion);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->startFrame, 409u);
    EXPECT_EQ(ledger->sources.plain.entries.front().loopRegion->endFrame, 410u);

    const auto reloaded = SoundSourceSemantics::fromJson(
        SoundSourceSemantics::toJson(ledger->sources));
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(*reloaded, ledger->sources);
}

TEST_F(ImportSoundSourcesCommandTest, FailedImportDoesNotOccupyUndoStack)
{
    const QString missing = QDir(tempDir_.path()).filePath(QStringLiteral("missing.wav"));
    ASSERT_FALSE(QFile::exists(missing));
    ASSERT_FALSE(UndoRedoManager::instance().canUndo());

    auto* failed = ImportSoundSourcesCommand::pushImport({missing}, binding_, sound_);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->successCount(), 0);
    EXPECT_GE(failed->failCount(), 1);
    EXPECT_FALSE(UndoRedoManager::instance().canUndo());
    EXPECT_TRUE(sound_->comp<VSound>()->sources.plain.entries.empty());

    const QString good = QDir(tempDir_.path()).filePath(QStringLiteral("ok.wav"));
    writeWav(good, 0.6f);
    auto* succeeded = ImportSoundSourcesCommand::pushImport({good}, binding_, sound_);
    ASSERT_EQ(succeeded->successCount(), 1);
    EXPECT_TRUE(UndoRedoManager::instance().canUndo());
    ASSERT_EQ(sound_->comp<VSound>()->sources.plain.entries.size(), 1u);

    auto* failedAgain = ImportSoundSourcesCommand::pushImport({missing}, binding_, sound_);
    EXPECT_EQ(failed->successCount(), 0);
    EXPECT_GE(failed->failCount(), 1);
    EXPECT_EQ(failedAgain->successCount(), 0);
    EXPECT_GE(failedAgain->failCount(), 1);
    ASSERT_EQ(sound_->comp<VSound>()->sources.plain.entries.size(), 1u);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(sound_->comp<VSound>()->sources.plain.entries.empty());
    EXPECT_FALSE(UndoRedoManager::instance().canUndo());
}
TEST_F(ImportSoundSourcesCommandTest, ImportTargetsItsOwnModeAndKeepsDormantAssetReferences)
{
    ProjectManager::instance().setProjectLanguagesInternal({"English"});
    const QString path = QDir(tempDir_.path()).filePath("shared-mode.wav");
    writeWav(path, 0.35f);
    ASSERT_EQ(ImportSoundSourcesCommand::pushImport({path}, binding_, sound_)->successCount(), 1);
    auto* sound = sound_->comp<VSound>();
    const auto plain = sound->sources.plain;
    const auto sourceId = plain.entries[0].sourceId;
    sound->distinguishLanguage = true;
    ASSERT_EQ(ImportSoundSourcesCommand::pushImport({path}, binding_, sound_, "English")->successCount(), 1);
    const auto localized = sound->sources.localized["English"];
    EXPECT_EQ(sound->sources.plain, plain);
    ASSERT_EQ(localized.entries.size(), 1u);
    EXPECT_EQ(localized.entries[0].sourceId, sourceId);
    EXPECT_NE(localized.entries[0].entryId, plain.entries[0].entryId);
    EXPECT_EQ(localized.active, localized.entries[0].entryId);
    EXPECT_EQ(collectMediaFiles(binding_->rootItem.get()).size(), 2u);
    sound->distinguishLanguage = false;
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_TRUE(sound->sources.localized.isEmpty());
    EXPECT_EQ(sound->sources.plain, plain);
    EXPECT_TRUE(AssetRegistry::instance().contains(sourceId));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(sound->sources.localized["English"], localized);
    EXPECT_EQ(sound->sources.plain, plain);
}
