#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace VNX::UnityInstallation {

struct Installation
{
    QString path;
    bool managed = false;
};

struct Discovery
{
    QVector<Installation> installations;
    QStringList errors;
    QStringList sdkReferences;
    QStringList localPackagePaths;
};

QString defaultRelativePath();
QString migrationJournalPath(const QString& projectRoot);
bool hasPendingMigration(const QString& projectRoot);
bool samePath(const QString& left, const QString& right);
bool containsPath(const QString& parent, const QString& child);
bool safeRelativePath(const QString& path);
bool isDocumentationOrSample(const QString& relative);
QByteArray normalizedTextHash(const QString& path, QByteArray* crlfHash = nullptr);
bool matchesTextHashIgnoringLineEndings(const QString& path, const QByteArray& expected,
                                       const QByteArray& normalizedExpected = {});
bool matchesTextHashIgnoringEncoding(const QString& path, const QByteArray& expected,
                                    const QByteArray& normalizedExpected = {});
bool validatePath(const QString& path, QString* error);
bool validateTree(const QString& path, QString* error);
bool resolveTarget(const QString& projectRoot, const QString& input,
                   QString* target, QString* error);
Discovery discover(const QString& projectRoot);
bool validateTarget(const QString& projectRoot, const QString& target,
                    QString* error, bool removing = false);

}
