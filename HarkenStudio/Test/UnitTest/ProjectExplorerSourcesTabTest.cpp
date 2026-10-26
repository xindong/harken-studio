#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/ImportAssetsCommand.h"
#include "AssetPipeline/ImportSoundSourcesCommand.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/SourceFileSystem.h"
#include "Core/FileWatcher.h"
#include "Components/VSound.h"
#include "ProjectManager.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <vector>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

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

void writeWavF32(const QString& path, const std::vector<float>& samples)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 1;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = frames * static_cast<uint32_t>(sizeof(float));
    const uint16_t blockAlign = channels * sizeof(float);
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
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
    bytes.append(reinterpret_cast<const char*>(samples.data()),
                 static_cast<qsizetype>(dataBytes));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

class ProjectExplorerSourcesTabTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("SourcesTabProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        auto& manager = ProjectManager::instance();
        QString closeError;
        const auto connection = QObject::connect(&manager, &ProjectManager::errorOccurred,
            [&closeError](const QString& error) { closeError = error; });
        if (manager.isProjectOpen())
            EXPECT_TRUE(manager.closeProject()) << closeError.toStdString();
        QObject::disconnect(connection);
        UndoRedoManager::instance().clear();
        AssetRegistry::instance().clear();
    }

    QTemporaryDir tempDir_;
};

}

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, ImportUndoRollsBackFileLocksAcrossDestinationsAndKeepsRedoCopies)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    MediaPool::instance();
    for (const QString& mode : {QStringLiteral("Sources"), QStringLiteral("Root"),
                               QStringLiteral("Layer"), QStringLiteral("Sound")}) {
        SCOPED_TRACE(mode.toStdString());
        auto* layer = manager.createItem(nullptr, mode, ProjectItem::Type::Layer);
        auto* target = manager.createItem(layer, QStringLiteral("Target"), ProjectItem::Type::Sound);
        auto* binding = findFileBindingForItem(*manager.currentProject(), layer);
        QStringList paths;
        for (const auto& name : {QStringLiteral("A"), QStringLiteral("B")}) {
            const QString path = tempDir_.filePath(mode + name + QStringLiteral(".wav"));
            writeWavF32(path, {0.0f, 0.25f, -0.25f, 0.0f});
            paths.append(path);
        }
        QList<uint32_t> ids;
        if (mode == QLatin1String("Sound"))
            ids = ImportSoundSourcesCommand::pushImport(paths, binding, target)->importedIDs();
        else if (mode == QLatin1String("Sources"))
            ids = ImportAssetsCommand::pushImportSources(paths)->importedIDs();
        else if (mode == QLatin1String("Root"))
            ids = ImportAssetsCommand::pushImportAtRoot(paths)->importedIDs();
        else
            ids = ImportAssetsCommand::pushImport(paths, binding, layer)->importedIDs();
        ASSERT_EQ(ids.size(), 2);
        const AssetInfo first = *registry.getAsset(ids.front());
        const AssetInfo second = *registry.getAsset(ids.back());
        const int index = undo.undoStack()->index();
        const auto authored = target->comp<VSound>()->sources;
        {
            HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(first.absolutePath.utf16()),
                GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            ASSERT_NE(handle, INVALID_HANDLE_VALUE);
            const auto release = qScopeGuard([&] { CloseHandle(handle); });
            EXPECT_FALSE(undo.undo());
            EXPECT_EQ(undo.undoStack()->index(), index);
            EXPECT_EQ(target->comp<VSound>()->sources, authored);
            manager.fileWatcher()->refreshSources();
            for (const auto& asset : {first, second}) {
                ASSERT_NE(registry.findAssetByPath(asset.absolutePath), nullptr);
                EXPECT_EQ(registry.findAssetByPath(asset.absolutePath)->sourceID, asset.sourceID);
                EXPECT_EQ(AssetImporter::computeContentHash(asset.absolutePath), asset.contentHash);
            }
        }
        for (const QString& path : paths) ASSERT_TRUE(QFile::remove(path));
        ASSERT_TRUE(undo.undo());
        manager.fileWatcher()->refreshSources();
        for (uint32_t id : ids) EXPECT_FALSE(registry.contains(id));
        ASSERT_TRUE(undo.redo());
        for (const auto& asset : {first, second}) {
            EXPECT_TRUE(registry.sourceFileExists(asset.sourceID));
            EXPECT_EQ(AssetImporter::computeContentHash(asset.absolutePath), asset.contentHash);
        }
        EXPECT_EQ(target->comp<VSound>()->sources, authored);
    }
}
#endif

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, ReplacementReplayFailurePreservesContentReferencesAndHistory)
{
    auto& registry = AssetRegistry::instance();
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    QStringList files;
    QStringList oldHashes;
    QStringList newHashes;
    QList<uint32_t> ids;
    QMap<QString, QString> replacements;
    for (const QString& name : names) {
        const QString file = tempDir_.filePath(name);
        writeWavF32(file, {0.0f, 0.5f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({file});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
        files.append(file);
        oldHashes.append(AssetImporter::computeContentHash(file));
        replacements.insert(name, file);
        writeWavF32(file, {0.0f, 0.25f, -0.25f, 0.0f});
        newHashes.append(AssetImporter::computeContentHash(file));
    }
    ASSERT_TRUE(manager.saveProject());
    ASSERT_EQ(ImportAssetsCommand::pushImportSources(files, {}, replacements)->successCount(), 2);
    for (bool redo : {false, true}) {
        const int index = undo.undoStack()->index();
        const QString blocked = SourceFileSystem::absolutePath(redo ? names.last() : names.first());
        const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(blocked.utf16()),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        {
            const auto release = qScopeGuard([&]() { CloseHandle(handle); });
            EXPECT_FALSE(redo ? undo.redo() : undo.undo());
        }
        EXPECT_EQ(undo.undoStack()->index(), index);
        for (int i = 0; i < names.size(); ++i) {
            const QString hash = redo ? oldHashes[i] : newHashes[i];
            EXPECT_EQ(registry.getAssetByPath(names[i])->sourceID, ids[i]);
            EXPECT_EQ(registry.getAsset(ids[i])->contentHash, hash);
            EXPECT_EQ(AssetImporter::computeContentHash(SourceFileSystem::absolutePath(names[i])), hash);
        }
        ASSERT_TRUE(redo ? undo.redo() : undo.undo());
    }
}
#endif

TEST_F(ProjectExplorerSourcesTabTest, BatchReplayFailureRestoresFilesReferencesAndHistoryBeforeRetry)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Target")));
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    QList<uint32_t> ids;
    QList<ProjectItem*> sounds;
    QList<QByteArray> contents;
    for (const QString& name : names) {
        const QString input = tempDir_.filePath(name);
        writeWavF32(input, {0.0f, 0.25f + ids.size() * 0.25f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({input});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
        sounds.append(manager.createRootAudioItem(name, name, ids.last()));
        ASSERT_NE(sounds.last(), nullptr);
        QFile file(input);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        contents.append(file.readAll());
    }
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(SourceFileSystem::move(names, QStringLiteral("Target")));
    const auto expectPaths = [&](const QString& prefix) {
        for (int i = 0; i < names.size(); ++i) {
            const QString path = prefix + names[i];
            EXPECT_EQ(registry.getAsset(ids[i])->relativePath, path);
            ASSERT_NE(registry.getAssetByPath(path), nullptr);
            EXPECT_EQ(registry.getAssetByPath(path)->sourceID, ids[i]);
            EXPECT_EQ(sounds[i]->sourcePath, path);
            QFile file(SourceFileSystem::absolutePath(path));
            ASSERT_TRUE(file.open(QIODevice::ReadOnly));
            EXPECT_EQ(file.readAll(), contents[i]);
        }
    };
    for (bool redo : {false, true}) {
        if (redo)
            ASSERT_TRUE(undo.undo());
        const QString before = redo ? QString() : QStringLiteral("Target/");
        const QString after = redo ? QStringLiteral("Target/") : QString();
        const QString conflict = SourceFileSystem::absolutePath(after + (redo ? names.last() : names.first()));
        writeWavF32(conflict, {0.0f, 0.75f, -0.75f, 0.0f});
        const int index = undo.undoStack()->index();
        const int count = undo.undoStack()->count();
        const int cleanIndex = undo.undoStack()->cleanIndex();
        const bool clean = undo.isClean();
        EXPECT_FALSE(redo ? undo.redo() : undo.undo());
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_EQ(undo.undoStack()->count(), count);
        EXPECT_EQ(undo.undoStack()->cleanIndex(), cleanIndex);
        EXPECT_EQ(undo.isClean(), clean);
        expectPaths(before);
        const QString restored = SourceFileSystem::absolutePath(after + (redo ? names.first() : names.last()));
        EXPECT_FALSE(QFile::exists(restored));
        EXPECT_TRUE(QFile::exists(conflict));
        ASSERT_TRUE(QFile::remove(conflict));
        ASSERT_TRUE(redo ? undo.redo() : undo.undo());
        expectPaths(after);
        if (!redo)
            ASSERT_TRUE(undo.redo());
    }
}

TEST_F(ProjectExplorerSourcesTabTest, BatchFolderUndoRestoresEarlierFoldersWhenALaterSourceIsMissing)
{
    auto& undo = UndoRedoManager::instance();
    auto& manager = ProjectManager::instance();
    for (const QString& name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("Target")})
        ASSERT_TRUE(SourceFileSystem::createFolder({}, name));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(SourceFileSystem::move({QStringLiteral("A"), QStringLiteral("B")}, QStringLiteral("Target")));
    const QString missing = SourceFileSystem::absolutePath(QStringLiteral("Target/A"));
    const QString renamed = SourceFileSystem::absolutePath(QStringLiteral("External"));
    ASSERT_TRUE(QDir().rename(missing, renamed));
    const int index = undo.undoStack()->index();
    EXPECT_FALSE(undo.undo());
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("Target/B"))).exists());
    EXPECT_FALSE(QDir(SourceFileSystem::absolutePath(QStringLiteral("B"))).exists());
    ASSERT_TRUE(QDir().rename(renamed, missing));
    ASSERT_TRUE(undo.undo());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("A"))).exists());
    EXPECT_TRUE(QDir(SourceFileSystem::absolutePath(QStringLiteral("B"))).exists());
}

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, InitialBatchFileLockRollsBackWithoutPublishingOrRecordingChanges)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Target")));
    QList<uint32_t> ids;
    const QStringList names{QStringLiteral("A.wav"), QStringLiteral("B.wav")};
    for (const QString& name : names) {
        const QString input = tempDir_.filePath(name);
        writeWavF32(input, {0.0f, 0.25f + ids.size() * 0.25f, -0.5f, 0.0f});
        auto* command = ImportAssetsCommand::pushImportSources({input});
        ASSERT_EQ(command->successCount(), 1);
        ids.append(command->importedIDs().first());
    }
    ASSERT_TRUE(manager.saveProject());
    const QString lockedPath = SourceFileSystem::absolutePath(names.last());
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(lockedPath.utf16()), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto closeHandle = qScopeGuard([&]() { CloseHandle(handle); });
    const int index = undo.undoStack()->index();
    const int count = undo.undoStack()->count();
    EXPECT_FALSE(SourceFileSystem::move(names, QStringLiteral("Target")));
    EXPECT_EQ(undo.undoStack()->index(), index);
    EXPECT_EQ(undo.undoStack()->count(), count);
    for (int i = 0; i < names.size(); ++i) {
        EXPECT_EQ(registry.getAsset(ids[i])->relativePath, names[i]);
        EXPECT_TRUE(QFile::exists(SourceFileSystem::absolutePath(names[i])));
        EXPECT_FALSE(QFile::exists(SourceFileSystem::absolutePath(QStringLiteral("Target/") + names[i])));
    }
}
#endif

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, FailedClosePreservesSavedSourceEditsAndTheirUndoCopies)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Keep.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    const QString savedHash = AssetImporter::computeContentHash(input);
    auto* imported = ImportAssetsCommand::pushImportSources({input});
    ASSERT_EQ(imported->successCount(), 1);
    const uint32_t id = imported->importedIDs().first();
    const QString savedPath = registry.getAsset(id)->absolutePath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Edited")));
    ASSERT_TRUE(SourceFileSystem::move({QStringLiteral("Keep.wav")}, QStringLiteral("Edited")));
    writeWavF32(input, {0.0f, 0.75f, -0.75f, 0.0f});
    const QString editedHash = AssetImporter::computeContentHash(input);
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({input}, QStringLiteral("Edited"),
        {{QStringLiteral("Edited/Keep.wav"), input}})->successCount(), 1);
    ASSERT_TRUE(SourceFileSystem::createFolder({}, QStringLiteral("Scratch")));
    const QString editedPath = registry.getAsset(id)->absolutePath;
    const QString scratchPath = SourceFileSystem::absolutePath(QStringLiteral("Scratch"));
    const auto* project = manager.currentProject();
    const int index = undo.undoStack()->index();
    const int count = undo.undoStack()->count();
    ASSERT_TRUE(QFile::remove(input));
    {
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(editedPath.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([&] { CloseHandle(handle); });
        EXPECT_FALSE(manager.closeProject());
        EXPECT_EQ(manager.currentProject(), project);
        ASSERT_NE(registry.getAsset(id), nullptr);
        EXPECT_EQ(registry.getAsset(id)->relativePath, QStringLiteral("Edited/Keep.wav"));
        EXPECT_EQ(AssetImporter::computeContentHash(editedPath), editedHash);
        EXPECT_FALSE(QFile::exists(savedPath));
        EXPECT_TRUE(QDir(scratchPath).exists());
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_EQ(undo.undoStack()->count(), count);
    }
    ASSERT_TRUE(undo.undo());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(AssetImporter::computeContentHash(editedPath), savedHash);
    ASSERT_TRUE(undo.redo());
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(AssetImporter::computeContentHash(editedPath), editedHash);
}
#endif

#ifdef Q_OS_WIN
TEST_F(ProjectExplorerSourcesTabTest, FailedCloseRollsBackImportCleanupWhenSavedSourceRestoreIsLocked)
{
    auto& manager = ProjectManager::instance();
    auto& registry = AssetRegistry::instance();
    auto& undo = UndoRedoManager::instance();
    const QString input = tempDir_.filePath(QStringLiteral("Saved.wav"));
    writeWavF32(input, {0.0f, 0.25f, -0.25f, 0.0f});
    auto* savedImport = ImportAssetsCommand::pushImportSources({input});
    ASSERT_EQ(savedImport->successCount(), 1);
    const uint32_t savedId = savedImport->importedIDs().first();
    const QString savedPath = registry.getAsset(savedId)->absolutePath;
    ASSERT_TRUE(manager.saveProject());
    writeWavF32(input, {0.0f, 0.75f, -0.75f, 0.0f});
    const QString editedHash = AssetImporter::computeContentHash(input);
    ASSERT_EQ(ImportAssetsCommand::pushImportSources({input}, {},
        {{QStringLiteral("Saved.wav"), input}})->successCount(), 1);
    const QString extra = tempDir_.filePath(QStringLiteral("Unsaved.wav"));
    writeWavF32(extra, {0.0f, 0.5f, -0.5f, 0.0f});
    auto* pendingImport = ImportAssetsCommand::pushImportSources({extra});
    ASSERT_EQ(pendingImport->successCount(), 1);
    const AssetInfo pending = *registry.getAsset(pendingImport->importedIDs().first());
    const auto* project = manager.currentProject();
    const int index = undo.undoStack()->index();
    const int count = undo.undoStack()->count();
    ASSERT_TRUE(QFile::remove(input));
    ASSERT_TRUE(QFile::remove(extra));
    {
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(savedPath.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        const auto release = qScopeGuard([&] { CloseHandle(handle); });
        EXPECT_FALSE(manager.closeProject());
        EXPECT_EQ(manager.currentProject(), project);
        ASSERT_NE(registry.getAsset(savedId), nullptr);
        EXPECT_EQ(AssetImporter::computeContentHash(savedPath), editedHash);
        ASSERT_NE(registry.getAssetByPath(pending.relativePath), nullptr);
        EXPECT_EQ(registry.getAssetByPath(pending.relativePath)->sourceID, pending.sourceID);
        EXPECT_EQ(AssetImporter::computeContentHash(pending.absolutePath), pending.contentHash);
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_EQ(undo.undoStack()->count(), count);
    }
    ASSERT_TRUE(undo.undo());
    EXPECT_FALSE(QFile::exists(pending.absolutePath));
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(AssetImporter::computeContentHash(pending.absolutePath), pending.contentHash);
}
#endif
