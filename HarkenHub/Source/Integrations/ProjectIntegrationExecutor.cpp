#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/IntegrationProgress.h"
#include "Integrations/IntegrationFileSystem.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Platform/ProcessScan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QLockFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QRegularExpression>
#include <filesystem>

namespace VNH {
namespace {

constexpr auto kUnrealReceiptFileName = ".harken-integration-state.json";

constexpr int kIntegrationStateVersion = 1;

QString hubTranslate(const QString& source)
{
    const QByteArray utf8 = source.toUtf8();
    return QCoreApplication::translate("HarkenHub", utf8.constData());
}

QString hashFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        IntegrationProgress::checkpoint();
        const QByteArray block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError) return {};
        hash.addData(block);
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject readObject(const QString& path, bool* ok)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *ok = false;
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    *ok = document.isObject();
    return *ok ? document.object() : QJsonObject{};
}

std::vector<QString> files(const QString& root,
                          QDir::Filters filters = QDir::Files | QDir::AllDirs | QDir::Hidden
                              | QDir::System | QDir::NoDotAndDotDot)
{
    std::vector<QString> result;
    QDirIterator iterator(root, filters, QDirIterator::Subdirectories);
    const QDir directory(root);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        IntegrationProgress::inspectedEntry();
        if (QFileInfo(path).isFile())
            result.push_back(directory.relativeFilePath(path));
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool copyTree(const QString& sourceRoot, const QString& targetRoot)
{
    for (const QString& relative : files(sourceRoot)) {
        if (relative == QLatin1String(kUnrealReceiptFileName)) continue;
        const QString source = QDir(sourceRoot).filePath(relative);
        const QString target = QDir(targetRoot).filePath(relative);
        const QString expectedHash = hashFile(source);
        if (expectedHash.isEmpty()) return false;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()))
            return false;
        QFile::remove(target);
        if (!QFile::copy(source, target)
            || expectedHash != hashFile(target))
            return false;
    }
    return true;
}

QJsonObject receipt(const QString& targetRoot)
{
    bool ok = false;
    return readObject(QDir(targetRoot).filePath(
                          QLatin1String(kUnrealReceiptFileName)),
                      &ok);
}

QJsonObject hashes(const QString& root)
{
    QJsonObject object;
    for (const QString& relative : files(root)) {
        if (relative != QLatin1String(kUnrealReceiptFileName))
            object.insert(relative, hashFile(QDir(root).filePath(relative)));
    }
    return object;
}

bool safeTree(const QString& root)
{
    for (QFileInfo parent(root); !parent.isRoot(); parent.setFile(parent.absolutePath()))
        if (parent.isSymLink() || parent.isJunction()) return false;
    QDirIterator iterator(root, QDir::AllEntries | QDir::Hidden | QDir::System
        | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        IntegrationProgress::inspectedEntry();
        if (iterator.fileInfo().isSymLink() || iterator.fileInfo().isJunction()) return false;
    }
    return true;
}

bool validReceipt(const QJsonObject& state)
{
    if (state.value(QStringLiteral("schema_version")).toInt() != kIntegrationStateVersion
        || state.value(QStringLiteral("engine")).toString() != QStringLiteral("unreal")
        || state.value(QStringLiteral("sdk_version")).toString().isEmpty()) return false;
    const auto managed = state.value(QStringLiteral("managed_files")).toObject();
    if (!managed.contains(QStringLiteral("Harken.uplugin"))) return false;
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    for (auto it = managed.begin(); it != managed.end(); ++it) {
        const QString path = it.key();
        if (path.isEmpty() || !QDir::isRelativePath(path) || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char(':')) || QDir::cleanPath(path) != path
            || path == QStringLiteral("..") || path.startsWith(QStringLiteral("../"))
            || path == QLatin1String(kUnrealReceiptFileName)
            || !hashPattern.match(it.value().toString()).hasMatch()) return false;
    }
    return true;
}

QByteArray fingerprint(const EngineTargetRecord& target, const IntegrationPlan& plan,
                       IntegrationOperation operation)
{
    const QJsonObject value{
        {QStringLiteral("project"), target.descriptorPath},
        {QStringLiteral("descriptor"), hashFile(target.descriptorPath)},
        {QStringLiteral("operation"), static_cast<int>(operation)},
        {QStringLiteral("source"), plan.sourceRoot},
        {QStringLiteral("target"), plan.targetRoot},
        {QStringLiteral("sourceFiles"), operation == IntegrationOperation::Remove
            ? QJsonObject{} : hashes(plan.sourceRoot)},
        {QStringLiteral("targetFiles"), hashes(plan.targetRoot)},
        {QStringLiteral("receipt"), receipt(plan.targetRoot)}};
    return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

bool writeReceipt(const QString& root, const QString& version,
                  const QString& managedSourceRoot)
{
    QJsonObject state;
    state.insert(QStringLiteral("schema_version"),
                 kIntegrationStateVersion);
    state.insert(QStringLiteral("engine"), QStringLiteral("unreal"));
    state.insert(QStringLiteral("sdk_version"), version);
    state.insert(QStringLiteral("installed_at"),
                 QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    state.insert(QStringLiteral("managed_files"), hashes(managedSourceRoot));
    QSaveFile file(QDir(root).filePath(
        QLatin1String(kUnrealReceiptFileName)));
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(state).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly)
        && file.write(bytes) == bytes.size() && file.commit();
}

bool copyTargetOnlyFiles(const QString& priorRoot, const QString& stagingRoot,
                         const QStringList& preservedFiles)
{
    for (const QString& relative : preservedFiles) {
        const QString source = QDir(priorRoot).filePath(relative);
        const QString target = QDir(stagingRoot).filePath(relative);
        const QString expectedHash = hashFile(source);
        if (!QFileInfo(source).isFile() || QFileInfo::exists(target)
            || expectedHash.isEmpty()
            || !QDir().mkpath(QFileInfo(target).absolutePath())
            || !QFile::copy(source, target)
            || expectedHash != hashFile(target)) {
            return false;
        }
    }
    return true;
}

QJsonObject inventory(const QString& root, QString* error)
{
    if (!VNX::UnityInstallation::validateTree(root, error)) return {};
    QJsonObject fileHashes;
    QStringList directories;
    QDirIterator entries(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next();
        IntegrationProgress::inspectedEntry();
        const QString relative = QDir(root).relativeFilePath(path);
        if (entries.fileInfo().isDir()) directories << relative;
        else {
            const QString hash = hashFile(path);
            if (hash.isEmpty()) {
                *error = hubTranslate(QStringLiteral("Cannot read SDK file for migration: %1")).arg(path);
                return {};
            }
            fileHashes.insert(relative, hash);
        }
    }
    directories.sort();
    return {{QStringLiteral("files"), fileHashes},
            {QStringLiteral("directories"), QJsonArray::fromStringList(directories)}};
}

bool validInventory(const QJsonObject& value)
{
    if (!value.value(QStringLiteral("files")).isObject()
        || !value.value(QStringLiteral("directories")).isArray()) return false;
    const auto entries = value.value(QStringLiteral("files")).toObject();
    if (!entries.contains(QStringLiteral("Harken.uplugin"))
        || !entries.contains(QLatin1String(kUnrealReceiptFileName))) return false;
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (!VNX::UnityInstallation::safeRelativePath(it.key())
            || !hashPattern.match(it.value().toString()).hasMatch()) return false;
    for (const auto& entry : value.value(QStringLiteral("directories")).toArray())
        if (!entry.isString() || !VNX::UnityInstallation::safeRelativePath(entry.toString())) return false;
    return true;
}

bool copyInventory(const QString& source, const QString& destination,
                   const QJsonObject& expected, QString* error)
{
    if (QFileInfo::exists(destination) || !QDir().mkpath(destination)) return false;
    for (const auto& entry : expected.value(QStringLiteral("directories")).toArray())
        if (!QDir().mkpath(QDir(destination).filePath(entry.toString()))) return false;
    const auto entries = expected.value(QStringLiteral("files")).toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QString output = QDir(destination).filePath(it.key());
        if (!QDir().mkpath(QFileInfo(output).absolutePath())
            || !QFile::copy(QDir(source).filePath(it.key()), output)) return false;
    }
    return inventory(destination, error) == expected;
}

bool emptyLocation(const QString& path)
{
    return !QFileInfo::exists(path) || (QFileInfo(path).isDir()
        && QDir(path).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty());
}

bool validateLocationTree(const QString& path, QString* error)
{
    return VNX::UnityInstallation::validatePath(path, error)
        && (!QFileInfo::exists(path) || VNX::UnityInstallation::validateTree(path, error));
}

QString migrationBackup(const EngineTargetRecord& target, const QString& transaction)
{
    return QDir(target.canonicalRoot).filePath(QStringLiteral("Saved/HarkenHub/Backups/%1/Harken").arg(transaction));
}

bool writeJournal(const QString& path, const QJsonObject& object)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const auto bytes = QJsonDocument(object).toJson();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

QByteArray migrationFingerprint(const EngineTargetRecord& target, const SdkMigrationPlan& plan,
                                const QJsonObject& journal = {})
{
    const QJsonObject value{
        {QStringLiteral("project"), target.descriptorPath},
        {QStringLiteral("descriptor"), hashFile(target.descriptorPath)},
        {QStringLiteral("source"), plan.sourceRoot},
        {QStringLiteral("destination"), plan.targetRoot},
        {QStringLiteral("inventory"), plan.inventory},
        {QStringLiteral("journal"), journal}};
    return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}

}

bool ProjectIntegrationExecutor::checkUnrealClosed(const EngineTargetRecord& target, QString* error)
{
    QList<RunningProcess> processes;
    if (!runningUnrealEditors(&processes)) {
        *error = hubTranslate(QStringLiteral("Could not check Unreal Editor processes. Check permissions and retry the safety check."));
        return false;
    }
    const auto canonical = [](const QString& path) {
        const QFileInfo file(QDir::fromNativeSeparators(path));
        const QString resolved = file.canonicalFilePath();
        return QDir::cleanPath(resolved.isEmpty() ? file.absoluteFilePath() : resolved);
    };
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const QString descriptor = canonical(target.descriptorPath);
    const QFileInfo descriptorInfo(descriptor);
    const QString root = canonical(target.canonicalRoot) + QLatin1Char('/');
    bool unresolvedProject = false;
    for (const auto& process : processes) {
        bool matches = !process.executable.isEmpty()
            && canonical(process.executable).startsWith(root, sensitivity);
        for (QString argument : process.arguments) {
            const bool projectOption = argument.startsWith(QStringLiteral("-project="), Qt::CaseInsensitive);
            if (projectOption)
                argument = argument.mid(9);
            if (QDir::isAbsolutePath(QDir::fromNativeSeparators(argument)))
                matches |= canonical(argument).compare(descriptor, sensitivity) == 0;
            else if (projectOption || argument.endsWith(QStringLiteral(".uproject"), Qt::CaseInsensitive)) {
                const QString name = QFileInfo(QDir::fromNativeSeparators(argument)).fileName();
                unresolvedProject |= name.compare(descriptorInfo.fileName(), sensitivity) == 0
                    || (projectOption && name.compare(descriptorInfo.completeBaseName(), sensitivity) == 0);
            }
        }
        if (matches) {
            *error = hubTranslate(QStringLiteral("This Unreal project is open. Close it before changing the SDK, then run the safety check again."));
            return false;
        }
    }
    if (unresolvedProject) {
        *error = hubTranslate(QStringLiteral("An Unreal Editor uses a relative project path with the same project name as this target. Reopen it with an absolute .uproject path so Hub can identify the project before changing SDK files."));
        return false;
    }
    return true;
}

QString ProjectIntegrationExecutor::locateUnrealSource(const QString& path)
{
    if (path.trimmed().isEmpty()) return {};
    for (const auto* relative : {".", "Harken", "Unreal/Harken", "HarkenSDK/Unreal/Harken"}) {
        const QString candidate = QDir(path).absoluteFilePath(QLatin1String(relative));
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("Harken.uplugin"))))
            return QDir::cleanPath(candidate);
    }
    return {};
}

bool ProjectIntegrationExecutor::resolveUnrealLocation(const EngineTargetRecord& target,
    const QString& input, QString* path, QString* error)
{
    const QString normalized = QDir::fromNativeSeparators(input.trimmed());
    const QDir project(target.canonicalRoot);
    const QString relative = QDir::isAbsolutePath(normalized)
        ? project.relativeFilePath(normalized) : normalized;
    if (!VNX::UnityInstallation::safeRelativePath(relative)
        || !relative.startsWith(QStringLiteral("Plugins/"))) {
        *error = hubTranslate(QStringLiteral("Choose a directory inside this project's Plugins folder, for example Plugins/Audio/Harken."));
        return false;
    }
    *path = QDir::cleanPath(project.absoluteFilePath(relative));
    if (!VNX::UnityInstallation::validatePath(*path, error)) return false;
    const QString plugins = project.absoluteFilePath(QStringLiteral("Plugins"));
    for (QString parent = QFileInfo(*path).absolutePath();
         !VNX::UnityInstallation::samePath(parent, target.canonicalRoot);
         parent = QFileInfo(parent).absolutePath()) {
        if (!QDir(parent).entryList({QStringLiteral("*.uplugin")}, QDir::Files | QDir::Hidden).isEmpty()) {
            *error = hubTranslate(QStringLiteral("The SDK cannot be placed inside another Unreal plugin. Choose a separate directory under Plugins."));
            return false;
        }
        if (VNX::UnityInstallation::samePath(parent, plugins)) break;
    }
    return true;
}

bool ProjectIntegrationExecutor::locateUnrealInstallation(const EngineTargetRecord& target,
    QString* path, QString* error)
{
    path->clear();
    QStringList found;
    QStringList pending{QDir(target.canonicalRoot).filePath(QStringLiteral("Plugins"))};
    while (!pending.isEmpty()) {
        const QString directory = pending.takeLast();
        IntegrationProgress::inspectedEntry();
        if (!VNX::UnityInstallation::validatePath(directory, error)) return false;
        if (!QFileInfo::exists(directory)) continue;
        if (QFileInfo::exists(QDir(directory).filePath(QStringLiteral("Harken.uplugin")))
            || QFileInfo::exists(QDir(directory).filePath(QLatin1String(kUnrealReceiptFileName)))) {
            QString resolved;
            if (!resolveUnrealLocation(target, directory, &resolved, error)) return false;
            found << resolved;
        }
        if (!QDir(directory).entryList({QStringLiteral("*.uplugin")}, QDir::Files | QDir::Hidden).isEmpty())
            continue;
        std::error_code enumerationError;
        std::filesystem::directory_iterator cursor(std::filesystem::path(directory.toStdU16String()), enumerationError);
        const std::filesystem::directory_iterator end;
        while (!enumerationError && cursor != end) {
            const QFileInfo child(QString::fromStdU16String(cursor->path().u16string()));
            IntegrationProgress::inspectedEntry();
            if (!VNX::UnityInstallation::validatePath(child.absoluteFilePath(), error)) return false;
            if (child.isDir()) pending << child.absoluteFilePath();
            cursor.increment(enumerationError);
        }
        if (enumerationError) {
            *error = hubTranslate(QStringLiteral("Cannot enumerate directory %1: %2"))
                .arg(directory, QString::fromStdString(enumerationError.message()));
            return false;
        }
    }
    if (found.size() > 1) {
        *error = hubTranslate(QStringLiteral("Multiple Harken plugins were found. Keep one installation under Plugins and move the others outside Plugins before checking again.\n%1")).arg(found.join(QLatin1Char('\n')));
        return false;
    }
    if (!found.isEmpty()) *path = found.front();
    return true;
}

QString ProjectIntegrationExecutor::unrealMigrationJournalPath(const EngineTargetRecord& target)
{
    return QDir(target.canonicalRoot).filePath(QStringLiteral("Saved/HarkenHub/unreal-sdk-migration.json"));
}

bool ProjectIntegrationExecutor::hasPendingUnrealMigration(const EngineTargetRecord& target)
{
    const QFileInfo journal(unrealMigrationJournalPath(target));
    return journal.exists() || journal.isSymLink() || journal.isJunction();
}

SdkMigrationPlan ProjectIntegrationExecutor::planUnrealMigration(const EngineTargetRecord& target,
    const QString& destination, bool recover)
{
    SdkMigrationPlan plan;
    plan.recovering = recover;
    bool projectOk = false;
    readObject(target.descriptorPath, &projectOk);
    if (target.engineKind != EngineKind::Unreal || !projectOk
        || !VNX::UnityInstallation::samePath(QFileInfo(target.descriptorPath).absolutePath(), target.canonicalRoot)) {
        plan.error = hubTranslate(QStringLiteral("The target is not a readable Unreal project"));
        return plan;
    }
    if (!checkUnrealClosed(target, &plan.error)) return plan;
    const QString journalPath = unrealMigrationJournalPath(target);
    if (!VNX::UnityInstallation::validatePath(journalPath, &plan.error)) return plan;
    QJsonObject journal;
    if (recover) {
        bool ok = false;
        journal = readObject(journalPath, &ok);
        plan.transactionId = journal.value(QStringLiteral("transaction")).toString();
        plan.inventory = journal.value(QStringLiteral("inventory")).toObject();
        if (!ok || journal.value(QStringLiteral("schema_version")).toInt() != 1
            || QUuid(plan.transactionId).isNull()
            || QUuid(plan.transactionId).toString(QUuid::WithoutBraces) != plan.transactionId
            || journal.value(QStringLiteral("project")).toString() != target.descriptorPath
            || !validInventory(plan.inventory)) {
            plan.error = hubTranslate(QStringLiteral("The Unreal migration record is invalid. Preserve both SDK locations and the backup; restore the record from its migration backup before retrying."));
            return plan;
        }
        if (!resolveUnrealLocation(target, journal.value(QStringLiteral("source")).toString(), &plan.sourceRoot, &plan.error)
            || !resolveUnrealLocation(target, journal.value(QStringLiteral("destination")).toString(), &plan.targetRoot, &plan.error)) return plan;
    } else {
        if (hasPendingUnrealMigration(target)) {
            plan.error = hubTranslate(QStringLiteral("The SDK migration is unfinished. Choose \"Recover...\" before continuing."));
            return plan;
        }
        const auto installed = inspectUnreal(target);
        if (!installed.valid) { plan.error = installed.error; return plan; }
        plan.sourceRoot = installed.targetRoot;
        if (!resolveUnrealLocation(target, destination, &plan.targetRoot, &plan.error)) return plan;
        plan.inventory = inventory(plan.sourceRoot, &plan.error);
        bool pluginOk = false;
        readObject(QDir(plan.sourceRoot).filePath(QStringLiteral("Harken.uplugin")), &pluginOk);
        if (!pluginOk || !validInventory(plan.inventory)) {
            if (plan.error.isEmpty()) plan.error = hubTranslate(QStringLiteral("Repair the installed Unreal SDK with Change SDK before moving it."));
            return plan;
        }
        plan.transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    if (VNX::UnityInstallation::containsPath(plan.sourceRoot, plan.targetRoot)
        || VNX::UnityInstallation::containsPath(plan.targetRoot, plan.sourceRoot)) {
        plan.error = hubTranslate(QStringLiteral("The current and new SDK locations must be separate directories."));
        return plan;
    }
    plan.backupRoot = migrationBackup(target, plan.transactionId);
    if (!validateLocationTree(plan.sourceRoot, &plan.error)
        || !validateLocationTree(plan.targetRoot, &plan.error)
        || !validateLocationTree(plan.backupRoot, &plan.error)) return plan;
    if (recover) {
        QString installed;
        if (!locateUnrealInstallation(target, &installed, &plan.error)) return plan;
        if (!installed.isEmpty() && !VNX::UnityInstallation::samePath(installed, plan.sourceRoot)
            && !VNX::UnityInstallation::samePath(installed, plan.targetRoot)) {
            plan.error = hubTranslate(QStringLiteral("Another Harken plugin exists outside the migration locations. Preserve it outside Plugins before recovering."));
            return plan;
        }
        if (inventory(plan.backupRoot, &plan.error) != plan.inventory) {
            plan.error = hubTranslate(QStringLiteral("The migration backup is missing or changed. Restore the original backup before recovering; no SDK files were changed."));
            return plan;
        }
        const bool sourceEmpty = emptyLocation(plan.sourceRoot);
        const bool targetEmpty = emptyLocation(plan.targetRoot);
        if ((!sourceEmpty && inventory(plan.sourceRoot, &plan.error) != plan.inventory)
            || (!targetEmpty && inventory(plan.targetRoot, &plan.error) != plan.inventory)
            || (!sourceEmpty && !targetEmpty)) {
            plan.error = hubTranslate(QStringLiteral("SDK files changed after migration was interrupted, or both locations are occupied. Preserve those files and restore one location to the migration backup's contents before recovering."));
            return plan;
        }
        journal.insert(QStringLiteral("source_empty"), sourceEmpty);
        journal.insert(QStringLiteral("destination_empty"), targetEmpty);
    } else if (!emptyLocation(plan.targetRoot)) {
        plan.error = hubTranslate(QStringLiteral("The migration destination must be an empty directory."));
        return plan;
    }
    plan.fingerprint = migrationFingerprint(target, plan, journal);
    plan.valid = true;
    return plan;
}

IntegrationOperationResult ProjectIntegrationExecutor::executeUnrealMigration(
    const EngineTargetRecord& target, const SdkMigrationPlan& plan, bool confirmed)
{
    IntegrationOperationResult result;
    if (!plan.valid || !confirmed) {
        result.error = hubTranslate(QStringLiteral("Integration operation requires confirmation"));
        return result;
    }
    QLockFile lock(QDir(target.canonicalRoot).filePath(QStringLiteral(".harken-sdk-operation.lock")));
    if (!lock.tryLock()) {
        result.error = hubTranslate(QStringLiteral("Another SDK operation is running for this project."));
        return result;
    }
    const auto revalidate = [&]() {
        const auto current = planUnrealMigration(target, plan.targetRoot, plan.recovering);
        if (!current.valid) { result.error = current.error; return false; }
        if (plan.fingerprint.isEmpty() || current.fingerprint != plan.fingerprint
            || current.sourceRoot != plan.sourceRoot || current.targetRoot != plan.targetRoot
            || current.inventory != plan.inventory || QUuid(plan.transactionId).isNull()
            || QUuid(plan.transactionId).toString(QUuid::WithoutBraces) != plan.transactionId
            || plan.backupRoot != migrationBackup(target, plan.transactionId)
            || (plan.recovering && current.transactionId != plan.transactionId)) {
            result.error = hubTranslate(QStringLiteral("SDK files changed after the safety check. Run the check again before continuing."));
            return false;
        }
        return true;
    };
    if (!revalidate()) return result;
    const QString journalPath = unrealMigrationJournalPath(target);
    result.backupPath = plan.backupRoot;
    if (!plan.recovering) {
        if (!VNX::UnityInstallation::validatePath(plan.backupRoot, &result.error)
            || !copyInventory(plan.sourceRoot, plan.backupRoot, plan.inventory, &result.error)) {
            if (result.error.isEmpty()) result.error = hubTranslate(QStringLiteral("The existing Unreal plugin cannot be backed up"));
            return result;
        }
        if (!revalidate()) return result;
        const QJsonObject journal{
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("project"), target.descriptorPath},
            {QStringLiteral("transaction"), plan.transactionId},
            {QStringLiteral("source"), QDir(target.canonicalRoot).relativeFilePath(plan.sourceRoot)},
            {QStringLiteral("destination"), QDir(target.canonicalRoot).relativeFilePath(plan.targetRoot)},
            {QStringLiteral("inventory"), plan.inventory}};
        if (!writeJournal(QDir(QFileInfo(plan.backupRoot).absolutePath()).filePath(QStringLiteral("migration.json")), journal)
            || !writeJournal(journalPath, journal)) {
            result.error = hubTranslate(QStringLiteral("The migration recovery record could not be saved. Check project permissions and retry."));
            return result;
        }
    }
    const QString output = plan.recovering ? plan.sourceRoot : plan.targetRoot;
    const QString input = plan.recovering ? plan.targetRoot : plan.sourceRoot;
    if (!checkUnrealClosed(target, &result.error)) return result;
    if (plan.recovering && !emptyLocation(output)) {
        if (inventory(output, &result.error) != plan.inventory || !emptyLocation(input)) {
            result.error = hubTranslate(QStringLiteral("SDK files changed after the safety check. Run the check again before continuing."));
            return result;
        }
    } else {
        if (!VNX::UnityInstallation::validatePath(output, &result.error)
            || !emptyLocation(output) || !QDir().mkpath(QFileInfo(output).absolutePath())) {
            result.error = hubTranslate(QStringLiteral("The migration destination must be an empty directory."));
            return result;
        }
        if (QFileInfo::exists(output) && !QDir().rmdir(output)) {
            result.error = hubTranslate(QStringLiteral("The Unreal plugin cannot be activated"));
            return result;
        }
        const bool restored = plan.recovering && emptyLocation(input)
            ? copyInventory(plan.backupRoot, output, plan.inventory, &result.error)
            : inventory(input, &result.error) == plan.inventory && renameIntegrationDirectory(input, output);
        if (!restored) {
            if (result.error.isEmpty()) result.error = hubTranslate(QStringLiteral("The SDK migration is unfinished. Choose \"Recover...\" before continuing."));
            return result;
        }
    }
    QString installed;
    if (inventory(output, &result.error) != plan.inventory
        || !locateUnrealInstallation(target, &installed, &result.error)
        || !VNX::UnityInstallation::samePath(installed, output) || !QFile::remove(journalPath)) {
        if (result.error.isEmpty()) result.error = hubTranslate(QStringLiteral("The SDK migration is unfinished. Choose \"Recover...\" before continuing."));
        return result;
    }
    result.installedPath = QDir(target.canonicalRoot).relativeFilePath(output);
    result.success = true;
    return result;
}

IntegrationPlan ProjectIntegrationExecutor::inspectUnreal(const EngineTargetRecord& target, bool verifyContent)
{
    IntegrationPlan result;
    bool projectOk = false;
    readObject(target.descriptorPath, &projectOk);
    if (target.engineKind != EngineKind::Unreal || !projectOk
        || QFileInfo(target.descriptorPath).absolutePath() != QDir(target.canonicalRoot).absolutePath()) {
        result.error = hubTranslate(QStringLiteral("The target is not a readable Unreal project"));
        return result;
    }
    if (hasPendingUnrealMigration(target)) {
        result.error = hubTranslate(QStringLiteral("The SDK migration is unfinished. Choose \"Recover...\" before continuing."));
        return result;
    }
    if (!locateUnrealInstallation(target, &result.targetRoot, &result.error)) return result;
    if (result.targetRoot.isEmpty()) {
        result.error = hubTranslate(QStringLiteral("The Unreal plugin is not installed"));
        return result;
    }
    const auto state = receipt(result.targetRoot);
    if (!safeTree(result.targetRoot) || !validReceipt(state)) {
        result.error = hubTranslate(QStringLiteral("The existing Unreal plugin has no trusted ownership record"));
        return result;
    }
    result.version = state.value(QStringLiteral("sdk_version")).toString();
    const auto managed = state.value(QStringLiteral("managed_files")).toObject();
    for (auto it = managed.begin(); verifyContent && it != managed.end(); ++it)
        if (hashFile(QDir(result.targetRoot).filePath(it.key())) != it.value().toString())
            result.modifiedManagedFiles.append(it.key());
    result.valid = true;
    return result;
}

IntegrationPlan ProjectIntegrationExecutor::planUnreal(
    const EngineTargetRecord& target,
    const QString& sourceRoot,
    IntegrationOperation operation)
{
    IntegrationPlan plan;
    bool projectOk = false;
    readObject(target.descriptorPath, &projectOk);
    if (target.engineKind != EngineKind::Unreal || !projectOk
        || QFileInfo(target.descriptorPath).absolutePath() != QDir(target.canonicalRoot).absolutePath()) {
        plan.error = hubTranslate(QStringLiteral("The target is not a readable Unreal project"));
        return plan;
    }
    if (!checkUnrealClosed(target, &plan.error)) return plan;
    plan.sourceRoot = operation == IntegrationOperation::Remove ? QString() : locateUnrealSource(sourceRoot);
    if (hasPendingUnrealMigration(target)) {
        plan.error = hubTranslate(QStringLiteral("The SDK migration is unfinished. Choose \"Recover...\" before continuing."));
        return plan;
    }
    if (!locateUnrealInstallation(target, &plan.targetRoot, &plan.error)) return plan;
    if (plan.targetRoot.isEmpty() && !resolveUnrealLocation(target,
            target.sdkInstallPath.isEmpty() ? QStringLiteral("Plugins/Harken") : target.sdkInstallPath,
            &plan.targetRoot, &plan.error)) return plan;
    if (operation != IntegrationOperation::Remove && plan.sourceRoot.isEmpty()) {
        plan.error = hubTranslate(QStringLiteral("The Unreal SDK source is incomplete"));
        return plan;
    }
    if (!safeTree(plan.targetRoot)
        || (operation != IntegrationOperation::Remove && !safeTree(plan.sourceRoot))) {
        plan.error = hubTranslate(QStringLiteral("SDK paths must not contain symbolic links."));
        return plan;
    }
    if (operation != IntegrationOperation::Remove) {
        const QString source = QDir::cleanPath(plan.sourceRoot);
        const QString installed = QDir::cleanPath(plan.targetRoot);
        if (source.compare(installed, Qt::CaseInsensitive) == 0
            || source.startsWith(installed + QLatin1Char('/'), Qt::CaseInsensitive)
            || installed.startsWith(source + QLatin1Char('/'), Qt::CaseInsensitive)) {
            plan.error = hubTranslate(QStringLiteral("SDK source and installation directories must be separate."));
            return plan;
        }
        bool pluginOk = false;
        const QJsonObject plugin = readObject(QDir(plan.sourceRoot).filePath(
            QStringLiteral("Harken.uplugin")), &pluginOk);
        if (!pluginOk) {
            plan.error = hubTranslate(QStringLiteral("The Unreal SDK source is incomplete"));
            return plan;
        }
        plan.version = plugin.value(QStringLiteral("VersionName")).toString();
        if (plan.version.isEmpty()) {
            plan.error = hubTranslate(QStringLiteral("The Unreal SDK version is unavailable"));
            return plan;
        }
        for (const auto* name : {"Harken.h", "ObjectId.h"}) {
            const QString relative = QStringLiteral("Source/HarkenCore/Include/harken/")
                + QLatin1String(name);
            QFile header(QDir(plan.sourceRoot).filePath(relative));
            if (!header.open(QIODevice::ReadOnly) || header.size() == 0
                || header.read(1).size() != 1) {
                plan.error = hubTranslate(QStringLiteral(
                    "The Unreal SDK is missing a readable, nonempty header: %1. Select a complete SDK source."))
                    .arg(relative);
                return plan;
            }
        }
    }
    const bool targetExists = QFileInfo::exists(QDir(plan.targetRoot).filePath(QStringLiteral("Harken.uplugin")))
        || QFileInfo::exists(QDir(plan.targetRoot).filePath(QLatin1String(kUnrealReceiptFileName)));
    if (operation == IntegrationOperation::Install && targetExists) {
        plan.error = hubTranslate(QStringLiteral("The Unreal plugin is already installed"));
        return plan;
    }
    if (operation != IntegrationOperation::Install && !targetExists) {
        plan.error = hubTranslate(QStringLiteral("The Unreal plugin is not installed"));
        return plan;
    }
    if (targetExists && !validReceipt(receipt(plan.targetRoot))) {
        plan.error = hubTranslate(QStringLiteral(
            "The existing Unreal plugin has no trusted ownership record"));
        return plan;
    }
    const QJsonObject priorHashes = targetExists
        ? receipt(plan.targetRoot).value(QStringLiteral("managed_files")).toObject()
        : QJsonObject{};
    QSet<QString> modifiedManagedFiles;
    if (targetExists) {
        for (auto iterator = priorHashes.begin(); iterator != priorHashes.end();
             ++iterator) {
            const QString current = hashFile(
                QDir(plan.targetRoot).filePath(iterator.key()));
            if (current.isEmpty() || current != iterator.value().toString()) {
                modifiedManagedFiles.insert(iterator.key());
                plan.modifiedManagedFiles.push_back(iterator.key());
            }
        }
    }
    const std::vector<QString> sourceFiles = operation == IntegrationOperation::Remove
        ? std::vector<QString>{} : files(plan.sourceRoot);
    for (const QString& relative : files(plan.targetRoot)) {
        if (relative == QLatin1String(kUnrealReceiptFileName) || priorHashes.contains(relative)) continue;
        if (std::find(sourceFiles.begin(), sourceFiles.end(), relative) != sourceFiles.end()) {
            plan.error = hubTranslate(QStringLiteral("The SDK source would overwrite a project-specific file: %1")).arg(relative);
            return plan;
        }
        plan.preservedTargetFiles.append(relative);
    }
    if (operation == IntegrationOperation::Remove) {
        plan.changes.push_back({QDir(target.canonicalRoot).relativeFilePath(plan.targetRoot),
                                QStringLiteral("remove")});
    } else {
        for (const QString& relative : sourceFiles) {
            if (relative == QLatin1String(kUnrealReceiptFileName)) continue;
            const QString source = QDir(plan.sourceRoot).filePath(relative);
            const QString installed = QDir(plan.targetRoot).filePath(relative);
            if (hashFile(source).isEmpty()) {
                plan.error = hubTranslate(QStringLiteral("The Unreal SDK source is incomplete"));
                return plan;
            }
            if (!QFileInfo::exists(installed)) {
                plan.changes.push_back({relative, QStringLiteral("add")});
            } else if (hashFile(source) != hashFile(installed)
                       || modifiedManagedFiles.contains(relative)) {
                plan.changes.push_back({relative, QStringLiteral("replace")});
            }
        }
        for (auto iterator = priorHashes.begin();
             iterator != priorHashes.end(); ++iterator) {
            if (std::find(sourceFiles.begin(), sourceFiles.end(),
                          iterator.key()) == sourceFiles.end()) {
                plan.changes.push_back(
                    {iterator.key(), QStringLiteral("remove")});
            }
        }
    }
    plan.modifiedManagedFiles.sort(Qt::CaseSensitive);
    plan.confirmationRequired = targetExists && !plan.changes.empty();
    plan.fingerprint = fingerprint(target, plan, operation);
    plan.valid = true;
    return plan;
}

IntegrationOperationResult ProjectIntegrationExecutor::executeUnreal(
    const EngineTargetRecord& target,
    const IntegrationPlan& plan,
    IntegrationOperation operation,
    bool confirmed)
{
    IntegrationOperationResult result;
    if (!plan.valid || plan.targetRoot.isEmpty()) {
        result.error = plan.error.isEmpty()
            ? hubTranslate(QStringLiteral("Integration plan is invalid")) : plan.error;
        return result;
    }
    if (plan.confirmationRequired && !confirmed) {
        result.error = hubTranslate(QStringLiteral("Integration operation requires confirmation"));
        return result;
    }
    QLockFile lock(QDir(target.canonicalRoot).filePath(QStringLiteral(".harken-sdk-operation.lock")));
    if (!lock.tryLock()) {
        result.error = hubTranslate(QStringLiteral("Another SDK operation is running for this project."));
        return result;
    }
    const auto revalidate = [&]() {
        const auto current = planUnreal(target, plan.sourceRoot, operation);
        if (!current.valid) {
            result.error = current.error;
            return false;
        }
        if (plan.fingerprint.isEmpty() || current.fingerprint != plan.fingerprint
            || current.targetRoot != plan.targetRoot
            || current.preservedTargetFiles != plan.preservedTargetFiles) {
            result.error = hubTranslate(QStringLiteral("SDK files changed after the safety check. Run the check again before continuing."));
            return false;
        }
        return true;
    };
    if (!revalidate()) return result;
    result.installedPath = QDir(target.canonicalRoot).relativeFilePath(plan.targetRoot);
    if (operation != IntegrationOperation::Remove && plan.changes.empty()) {
        result.success = true;
        return result;
    }
    const QString pluginsRoot = QFileInfo(plan.targetRoot).absolutePath();
    if (!QDir().mkpath(pluginsRoot)) {
        result.error = hubTranslate(QStringLiteral("The Unreal Plugins directory is not writable"));
        return result;
    }
    const QString stagingRoot = QDir(target.canonicalRoot).filePath(QStringLiteral("Saved/HarkenHub/Staging"));
    if (!safeTree(stagingRoot) || !QDir().mkpath(stagingRoot)) {
        result.error = hubTranslate(QStringLiteral("The Unreal plugin staging transaction failed"));
        return result;
    }
    QTemporaryDir staging(QDir(stagingRoot).filePath(QStringLiteral("Harken-XXXXXX")));
    if (!staging.isValid()
        || (operation != IntegrationOperation::Remove && !copyTree(plan.sourceRoot, staging.path()))
        || !copyTargetOnlyFiles(plan.targetRoot, staging.path(), plan.preservedTargetFiles)
        || (operation != IntegrationOperation::Remove
            && !writeReceipt(staging.path(), plan.version, plan.sourceRoot))) {
        result.error = hubTranslate(QStringLiteral("The Unreal plugin staging transaction failed"));
        return result;
    }
    if (!revalidate()) return result;
    if (QDir(plan.targetRoot).exists()) {
        const QString backup = QDir(target.canonicalRoot).filePath(
            QStringLiteral("Saved/HarkenHub/Backups/%1/Harken")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!safeTree(QFileInfo(backup).absolutePath())
            || !QDir().mkpath(QFileInfo(backup).absolutePath())
            || !renameIntegrationDirectory(plan.targetRoot, backup)) {
            result.error = hubTranslate(QStringLiteral("The existing Unreal plugin cannot be backed up"));
            return result;
        }
        result.backupPath = backup;
    }
    if (operation == IntegrationOperation::Remove && plan.preservedTargetFiles.isEmpty()) {
        result.success = true;
        return result;
    }
    if (!renameIntegrationDirectory(staging.path(), plan.targetRoot)) {
        if (!result.backupPath.isEmpty() && renameIntegrationDirectory(result.backupPath, plan.targetRoot))
            result.backupPath.clear();
        result.error = hubTranslate(QStringLiteral("The Unreal plugin cannot be activated"));
        return result;
    }
    staging.setAutoRemove(false);
    if (operation != IntegrationOperation::Remove) {
        const auto verification = inspectUnreal(target);
        if (!verification.valid || !verification.modifiedManagedFiles.isEmpty()) {
            renameIntegrationDirectory(plan.targetRoot, staging.path());
            staging.setAutoRemove(true);
            if (!result.backupPath.isEmpty() && renameIntegrationDirectory(result.backupPath, plan.targetRoot))
                result.backupPath.clear();
            result.error = hubTranslate(QStringLiteral("Installed SDK verification failed. Review the backup and restore the project before retrying."));
            return result;
        }
    }
    result.success = true;
    return result;
}

}
