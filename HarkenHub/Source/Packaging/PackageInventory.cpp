#include "Packaging/PackageInventory.h"

#include "Packaging/PackageReader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace VNH {

PackageInventory PackageInventory::parse(const QByteArray& data, const QString& version,
                                          const QString& buildId, const QString& channel)
{
    PackageInventory result;
    result.error = QStringLiteral("Package file manifest is invalid or does not match the release");
    if (data.isEmpty() || data.size() > maximumBytes) return result;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return result;
    const auto object = document.object();
    result.channel = object.value(QStringLiteral("channel")).toString();
    if (object.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenStudio")
        || object.value(QStringLiteral("version")).toString() != version
        || object.value(QStringLiteral("buildId")).toString() != buildId
        || (!channel.isEmpty() && result.channel != channel)) return result;
    const auto files = object.value(QStringLiteral("files")).toArray();
    if (files.isEmpty() || files.size() > 100000) return result;
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    static const QRegularExpression invalidCharacters(QStringLiteral("[\\x00-\\x1f<>:\"|?*]"));
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"), QRegularExpression::CaseInsensitiveOption);
    QSet<QString> keys;
    qint64 totalSize = 0;
    for (const auto& value : files) {
        const auto record = value.toObject();
        const QString path = record.value(QStringLiteral("path")).toString();
        const QString hash = record.value(QStringLiteral("sha256")).toString();
        const qint64 size = record.value(QStringLiteral("size")).toInteger(-1);
        result.error = QStringLiteral("Invalid or conflicting package file manifest entry: %1").arg(path);
        if (!PackageReader::isSafeEntryPath(path) || path.endsWith(QLatin1Char('/'))
            || path.contains(invalidCharacters)
            || !hashPattern.match(hash).hasMatch() || size < 0 || size > 8LL * 1024 * 1024 * 1024)
            return result;
        for (const auto& part : path.split(QLatin1Char('/'))) {
            if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))
                || reserved.match(part).hasMatch()) return result;
        }
        const auto key = path.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (keys.contains(key)) return result;
        keys.insert(key);
        totalSize += size;
        if (totalSize > 8LL * 1024 * 1024 * 1024) return result;
        result.files.insert(path, {size, hash.toLower()});
    }
    for (const auto& key : keys) {
        QString parent = key;
        while (parent.contains(QLatin1Char('/'))) {
            parent = parent.left(parent.lastIndexOf(QLatin1Char('/')));
            if (keys.contains(parent)) return result;
        }
    }
    for (const auto& component : object.value(QStringLiteral("components")).toArray()) {
        const auto entry = component.toObject();
        if (entry.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenIntegration")) continue;
        const QString root = entry.value(QStringLiteral("artifacts")).toObject().value(QStringLiteral("unity")).toString();
        if (root.isEmpty()) continue;
        result.error = QStringLiteral("Package file manifest has an invalid Unity component");
        if (!result.unityRoot.isEmpty() || !PackageReader::isSafeEntryPath(root)
            || !result.files.contains(root + QStringLiteral("/package.json"))) return result;
        result.unityRoot = root;
        result.unityVersion = entry.value(QStringLiteral("version")).toString();
        if (result.unityVersion.isEmpty()) return result;
    }
    result.error.clear();
    result.ok = true;
    return result;
}

}
