#include "SchemaArchive.h"

#include "PackSchemaGenerated.h"
#include "ProjectSchemaGenerated.h"
#include "SchemaBuildGenerated.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace VNX {

namespace {

bool snapshotMismatch(QString* error)
{
    if (error) *error = QStringLiteral("SCHEMA_SNAPSHOT_MISMATCH");
    return false;
}

QJsonObject readObject(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        snapshotMismatch(error);
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        snapshotMismatch(error);
        return {};
    }
    return document.object();
}

QByteArray fileHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result() : QByteArray();
}

}

SchemaArchive& SchemaArchive::instance()
{
    static SchemaArchive archive;
    return archive;
}

QString SchemaArchive::rootPath() const
{
    const QString overridePath = qEnvironmentVariable("HARKEN_SCHEMA_ROOT");
    if (!overridePath.isEmpty()) return QDir::cleanPath(overridePath);
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("schema"));
}

QJsonObject SchemaArchive::buildManifest(QString* error) const
{
    const QJsonObject manifest = readObject(
        QDir(rootPath()).filePath(QStringLiteral("build.json")), error);
    QJsonObject identity = manifest;
    identity.remove(QStringLiteral("snapshotHash"));
    const QByteArray hash = QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    const QJsonObject trees = manifest.value(QStringLiteral("trees")).toObject();
    if (manifest.value(QStringLiteral("manifestVersion")).toInt() != 1
        || manifest.value(QStringLiteral("snapshotHash")).toString().toLatin1() != hash
        || hash != QByteArray(SchemaBuild::kSnapshotHash)
        || trees.value(QStringLiteral("Editor")).toObject()
               .value(QStringLiteral("currentVersion")).toInt()
            != ProjectSchema::kProjectSchemaVersion
        || trees.value(QStringLiteral("Pack")).toObject()
               .value(QStringLiteral("currentVersion")).toInt()
            != PackSchema::kPackManifestSchemaVersion
        || manifest.value(QStringLiteral("files")).toArray().isEmpty()) {
        snapshotMismatch(error);
        return {};
    }
    return manifest;
}

bool SchemaArchive::verifyBuildSnapshot(QString* error) const
{
    const QJsonObject manifest = buildManifest(error);
    if (manifest.isEmpty()) return false;
    const QDir root(rootPath());
    const QRegularExpression editorPath(
        QStringLiteral("^Editor/[1-9][0-9]*/ProjectSerializedSchema\\.json$"));
    const QRegularExpression packPath(
        QStringLiteral("^Pack/[1-9][0-9]*/PackManifestSchema\\.json$"));
    const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    QSet<QString> expectedFiles{QStringLiteral("build.json")};
    QSet<QString> expectedDirectories;
    for (const QJsonValue& value : manifest.value(QStringLiteral("files")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QString hash = entry.value(QStringLiteral("sha256")).toString();
        const qint64 size = entry.value(QStringLiteral("size")).toInteger(-1);
        if ((!editorPath.match(path).hasMatch() && !packPath.match(path).hasMatch())
            || !sha256.match(hash).hasMatch() || size < 0 || expectedFiles.contains(path))
            return snapshotMismatch(error);
        const QFileInfo file(root.filePath(path));
        if (!file.isFile() || file.isSymLink() || file.size() != size
            || fileHash(file.absoluteFilePath()).toHex() != hash.toLatin1())
            return snapshotMismatch(error);
        expectedFiles.insert(path);
        const QStringList parts = path.split(QLatin1Char('/'));
        expectedDirectories.insert(parts.at(0));
        expectedDirectories.insert(parts.at(0) + QLatin1Char('/') + parts.at(1));
    }
    QSet<QString> actualFiles;
    QSet<QString> actualDirectories;
    QDirIterator entries(root.absolutePath(),
                         QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                             | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        const QString path = entries.next();
        const QFileInfo info = entries.fileInfo();
        if (info.isSymLink()) return snapshotMismatch(error);
        const QString relative = root.relativeFilePath(path);
        if (info.isDir()) actualDirectories.insert(relative);
        else if (info.isFile()) actualFiles.insert(relative);
        else return snapshotMismatch(error);
    }
    if (actualFiles != expectedFiles || actualDirectories != expectedDirectories)
        return snapshotMismatch(error);
    return true;
}

QJsonObject SchemaArchive::projectSchema(int version, QString* error) const
{
    if (version < 1 || !verifyBuildSnapshot(error)) {
        snapshotMismatch(error);
        return {};
    }
    return readObject(QDir(rootPath()).filePath(
        QStringLiteral("Editor/%1/ProjectSerializedSchema.json").arg(version)), error);
}

QJsonObject SchemaArchive::packSchema(int version, QString* error) const
{
    if (version < 1 || !verifyBuildSnapshot(error)) {
        snapshotMismatch(error);
        return {};
    }
    return readObject(QDir(rootPath()).filePath(
        QStringLiteral("Pack/%1/PackManifestSchema.json").arg(version)), error);
}

}
