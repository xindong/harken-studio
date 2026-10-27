#include "ReleaseNotes/ReleaseNotesCatalog.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace VNH {

namespace {

void setError(QString* error, const QString& value)
{
    if (error)
        *error = value;
}

}

QList<ReleaseNotesEntry> ReleaseNotesCatalog::load(QString* error)
{
    QFile file(QStringLiteral(":/Resources/release-notes.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("The embedded release notes could not be opened."));
        return {};
    }
    QList<ReleaseNotesEntry> entries = fromJson(file.readAll(), error);
    if (!entries.isEmpty() &&
        entries.first().version != QCoreApplication::applicationVersion()) {
        setError(error, QStringLiteral("The release notes do not match this Harken Hub version."));
        return {};
    }
    return entries;
}

QList<ReleaseNotesEntry> ReleaseNotesCatalog::fromJson(
    const QByteArray& data, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("The release notes contain invalid JSON."));
        return {};
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        setError(error, QStringLiteral("The release notes schema is unsupported."));
        return {};
    }

    const QJsonArray releases = root.value(QStringLiteral("releases")).toArray();
    if (releases.isEmpty()) {
        setError(error, QStringLiteral("The release notes do not contain any releases."));
        return {};
    }

    QList<ReleaseNotesEntry> entries;
    for (const QJsonValue& value : releases) {
        if (!value.isObject()) {
            setError(error, QStringLiteral("A release notes entry is not an object."));
            return {};
        }
        const QJsonObject object = value.toObject();
        ReleaseNotesEntry entry;
        entry.version = object.value(QStringLiteral("version")).toString().trimmed();
        entry.date = QDate::fromString(
            object.value(QStringLiteral("date")).toString(), Qt::ISODate);
        entry.introduction = object.value(QStringLiteral("introduction")).toString().trimmed();
        const QJsonArray changes = object.value(QStringLiteral("changes")).toArray();
        const QJsonArray pullRequests =
            object.value(QStringLiteral("pullRequests")).toArray();
        const QJsonArray contributors =
            object.value(QStringLiteral("contributors")).toArray();
        if (entry.version.isEmpty() || !entry.date.isValid() ||
            (object.contains(QStringLiteral("introduction")) && entry.introduction.isEmpty()) ||
            changes.isEmpty() || contributors.isEmpty()) {
            setError(error, QStringLiteral("A release notes entry is incomplete."));
            return {};
        }
        for (const QJsonValue& changeValue : changes) {
            const QJsonObject change = changeValue.toObject();
            ReleaseNotesChange parsedChange;
            parsedChange.category =
                change.value(QStringLiteral("category")).toString();
            parsedChange.text =
                change.value(QStringLiteral("text")).toString().trimmed();
            if ((parsedChange.category != QStringLiteral("added") &&
                 parsedChange.category != QStringLiteral("improved") &&
                 parsedChange.category != QStringLiteral("removed")) ||
                parsedChange.text.isEmpty()) {
                setError(error, QStringLiteral("A release notes change is empty."));
                return {};
            }
            entry.changes.append(parsedChange);
        }
        for (const QJsonValue& pullRequest : pullRequests) {
            const int number = pullRequest.toInt();
            if (number <= 0) {
                setError(error, QStringLiteral("A release notes pull request number is invalid."));
                return {};
            }
            entry.pullRequests.append(number);
        }
        for (const QJsonValue& contributor : contributors) {
            const QString account = contributor.toString().trimmed();
            if (account.isEmpty() || entry.contributors.contains(account)) {
                setError(error, QStringLiteral("A release notes contributor is invalid."));
                return {};
            }
            entry.contributors.append(account);
        }
        entries.append(entry);
    }
    setError(error, QString());
    return entries;
}

QString ReleaseNotesCatalog::localizedChange(const QString& sourceText)
{
    const QByteArray utf8 = sourceText.toUtf8();
    return QCoreApplication::translate("HubReleaseNotes", utf8.constData());
}

}
