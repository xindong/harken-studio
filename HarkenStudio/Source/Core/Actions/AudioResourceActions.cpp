#include "../ActionManager.h"
#include "../AssetPipeline/AssetImporter.h"
#include "../AssetPipeline/AssetRegistry.h"
#include "../AssetPipeline/AudioImportGate.h"
#include "../AssetPipeline/ImportAssetsCommand.h"
#include "../AssetPipeline/SourceFileSystem.h"
#include "../CommandArgumentValidator.h"
#include "../CommandSchemas.h"
#include "../ProjectManager.h"

#include <QDir>
#include <QFileInfo>

namespace harken::studio {
namespace {

QString childPath(const QString& folder, const QString& name)
{
    return folder.isEmpty() ? name : folder + QLatin1Char('/') + name;
}

ActionResult listResources(const QString& folder)
{
    const QString directory = SourceFileSystem::absolutePath(folder);
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        return {false, {}, "INVALID_ARGUMENTS", "folder must identify an existing Resources folder"};
    QJsonArray folders;
    QJsonArray files;
    const auto entries = QDir(directory).entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const auto& entry : entries) {
        const QString path = childPath(folder, entry.fileName());
        if (SourceFileSystem::absolutePath(path).isEmpty()) continue;
        if (entry.isDir()) {
            folders.append(path);
        } else if (AssetImporter::isFormatSupported(path)) {
            QJsonObject file{{"path", path}};
            const auto* asset = AssetRegistry::instance().findAssetByPath(entry.absoluteFilePath());
            if (asset) file.insert("source_id", static_cast<qint64>(asset->sourceID));
            files.append(file);
        }
    }
    return {true, {{"folder", folder}, {"folders", folders}, {"files", files}}};
}

ActionResult importResources(const QJsonObject& args)
{
    const QString folder = args.value("folder").toString();
    const QString directory = SourceFileSystem::absolutePath(folder);
    if (directory.isEmpty() || !QFileInfo(directory).isDir())
        return {false, {}, "INVALID_ARGUMENTS", "folder must identify an existing Resources folder"};
    QStringList files;
    QMap<QString, QString> hashes;
    QMap<QString, QString> replacements;
    QStringList paths;
    for (const auto& value : args.value("files").toArray()) paths.append(value.toString());
    const auto candidates = AudioImportGate::expandDirectories(paths);
    if (candidates.isEmpty()) return {false, {}, "IMPORT_FAILED", "No audio files were found"};
    for (const auto& path : candidates) {
        QString relative = childPath(folder, QFileInfo(path).fileName());
        const QString destination = SourceFileSystem::absolutePath(relative);
        if (destination.isEmpty() || QFileInfo(destination).isDir())
            return {false, {}, "INVALID_ARGUMENTS", QString("Invalid Resources destination: %1").arg(relative)};
        const QString hash = AssetImporter::computeContentHash(path);
        if (hash.isEmpty())
            return {false, {}, "IMPORT_FAILED", QString("Could not read %1").arg(path)};
        QString key = relative;
#ifdef Q_OS_WIN
        key = key.toCaseFolded();
#endif
        if (hashes.contains(key)) {
            if (hashes.value(key) != hash)
                return {false, {}, "IMPORT_CONFLICT", QString("Choose one incoming version for %1").arg(relative)};
            continue;
        }
        hashes.insert(key, hash);
        if (QFileInfo::exists(destination)) {
            const QString current = AssetImporter::computeContentHash(destination);
            if (current.isEmpty())
                return {false, {}, "IMPORT_FAILED", QString("Could not read %1").arg(relative)};
            if (current != hash) {
                if (!args.value("replace").toBool())
                    return {false, {}, "IMPORT_CONFLICT", QString("Different contents at %1; set replace to overwrite").arg(relative)};
                if (const auto* asset = AssetRegistry::instance().findAssetByPath(destination))
                    relative = asset->relativePath;
                replacements.insert(relative, path);
            }
        }
        files.append(path);
    }
    const auto* command = ImportAssetsCommand::pushImportResources(files, folder, replacements);
    QJsonArray ids;
    for (uint32_t id : command->importedIDs()) ids.append(static_cast<qint64>(id));
    const QJsonObject data{{"source_ids", ids}, {"success_count", command->successCount()},
                           {"duplicate_count", command->duplicateCount()}, {"fail_count", command->failCount()}};
    if (command->failCount() > 0)
        return {false, data, command->successCount() > 0 ? "IMPORT_PARTIAL_FAILURE" : "IMPORT_FAILED",
                "One or more files could not be imported into Resources"};
    return {true, data};
}

ActionResult editResources(const QString& name, const QJsonObject& args)
{
    const QString path = args.value("path").toString();
    const QString folder = args.value("folder").toString();
    QString error;
    QJsonObject result;
    bool ok = false;
    if (name == "create_audio_folder") {
        QString actual;
        ok = SourceFileSystem::createFolder(folder, args.value("name").toString(), &error, &actual);
        result.insert("path", childPath(folder, actual));
    } else if (name == "rename_audio_resource") {
        QString actual = args.value("name").toString();
        ok = SourceFileSystem::rename(path, args.value("name").toString(), &error, &actual);
        result.insert("path", path.left(path.lastIndexOf('/') + 1) + actual);
    } else if (name == "move_audio_resources") {
        QStringList paths;
        for (const auto& value : args.value("paths").toArray()) paths.append(value.toString());
        ok = SourceFileSystem::move(paths, folder, &error);
        result.insert("folder", folder);
    } else if (name == "relink_audio_resource") {
        const auto sourceId = static_cast<uint32_t>(args.value("source_id").toInteger());
        const auto* asset = AssetRegistry::instance().getAsset(sourceId);
        if (!asset) return {false, {}, "ASSET_NOT_FOUND", "Unknown source_id"};
        const QString file = args.value("file").toString();
        if (!QFileInfo(file).isFile())
            return {false, {}, "INVALID_ARGUMENTS", "file must identify a readable audio file"};
        const QString relative = asset->relativePath;
        const QString destination = SourceFileSystem::absolutePath(relative);
        if (destination.isEmpty())
            return {false, {}, "INVALID_ARGUMENTS", "Source path must stay inside AudioSources"};
        ok = QFileInfo(file).canonicalFilePath() == QFileInfo(destination).canonicalFilePath();
        if (!ok) {
            auto replacement = SourceFileSystem::replaceFiles({{relative, file}}, &error);
            ok = bool(replacement);
            if (replacement) UndoRedoManager::instance().push(replacement.release());
        }
        result = {{"path", relative}, {"source_id", static_cast<qint64>(sourceId)}};
    } else if (name == "restore_audio_resource") {
        const auto sourceId = static_cast<uint32_t>(args.value("source_id").toInteger());
        const auto missingId = static_cast<uint32_t>(args.value("missing_id").toInteger());
        ok = SourceFileSystem::restoreByRename(sourceId, missingId, &error);
        result.insert("source_id", static_cast<qint64>(sourceId));
    } else if (name == "delete_audio_folder") {
        ok = SourceFileSystem::removeEmptyFolder(path, &error);
        result.insert("path", path);
    }
    if (!ok)
        return {false, {}, "RESOURCE_EDIT_FAILED", error.isEmpty() ? QStringLiteral("No resources were moved") : error};
    return {true, result};
}

}

void registerAudioResourceActions(ActionManager& manager)
{
    for (const auto* name : {"list_audio_resources", "import_audio_resources", "create_audio_folder",
             "rename_audio_resource", "move_audio_resources", "relink_audio_resource", "restore_audio_resource", "delete_audio_folder"}) {
        ActionDef action;
        action.id = QLatin1String(name);
        action.displayName = QString(action.id).replace('_', ' ');
        action.category = QStringLiteral("asset");
        action.paramSchema = commandInputSchema(action.id);
        action.undoable = action.id != "list_audio_resources";
        action.handler = [id = action.id](const QJsonObject& args) -> ActionResult {
            QString error;
            if (!validateCommandArguments(args, commandInputSchema(id), &error))
                return {false, {}, "INVALID_ARGUMENTS", error};
            if (!ProjectManager::instance().isProjectOpen())
                return {false, {}, "NO_PROJECT_OPEN", "No project is currently open"};
            if (id == "list_audio_resources") return listResources(args.value("folder").toString());
            if (id == "import_audio_resources") return importResources(args);
            return editResources(id, args);
        };
        manager.registerAction(action);
    }
}

}
