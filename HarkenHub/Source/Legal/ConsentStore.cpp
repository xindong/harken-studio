#include "Legal/ConsentStore.h"

#include "Platform/PlatformPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace VNH {

namespace {

QJsonArray loadConsents()
{
    QFile file(ConsentStore::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonArray();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isArray() ? doc.array() : QJsonArray();
}

}

QString ConsentStore::filePath()
{
    return userDataDir() + QStringLiteral("/consents.json");
}

bool ConsentStore::hasConsent(const QString& documentId,
                              const QString& documentVersion,
                              const QString& documentSha256)
{
    const QJsonArray consents = loadConsents();
    for (const auto& value : consents) {
        const QJsonObject entry = value.toObject();
        if (entry[QStringLiteral("documentId")].toString() != documentId ||
            entry[QStringLiteral("documentVersion")].toString() !=
                documentVersion)
            continue;
        if (!entry.contains(QStringLiteral("documentSha256")) ||
            entry[QStringLiteral("documentSha256")].toString() ==
                documentSha256)
            return true;
    }
    return false;
}

bool ConsentStore::recordConsent(const QString& documentId,
                                 const QString& documentVersion,
                                 const QString& documentSha256,
                                 const QString& hubVersion,
                                 const QDateTime& at)
{
    if (documentId.isEmpty() || documentVersion.isEmpty())
        return false;
    if (hasConsent(documentId, documentVersion, documentSha256))
        return true;

    QJsonArray consents = loadConsents();
    QJsonObject entry;
    entry[QStringLiteral("documentId")] = documentId;
    entry[QStringLiteral("documentVersion")] = documentVersion;
    entry[QStringLiteral("documentSha256")] = documentSha256;
    entry[QStringLiteral("hubVersion")] = hubVersion;
    entry[QStringLiteral("acceptedAt")] =
        at.toUTC().toString(Qt::ISODateWithMs);
    consents.append(entry);

    const QString path = filePath();
    if (!QDir().mkpath(QFileInfo(path).dir().path()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json =
        QJsonDocument(consents).toJson(QJsonDocument::Indented);
    return file.write(json) == json.size() && file.commit();
}

}
