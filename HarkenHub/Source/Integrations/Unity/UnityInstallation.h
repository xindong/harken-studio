#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace harken::hub::unity::UnityInstallation {

struct Installation
{
    QString path;
};

struct Discovery
{
    QVector<Installation> installations;
    QStringList errors;
    QStringList integrationReferences;
    QStringList localPackagePaths;
};

QString defaultRelativePath();
bool samePath(const QString& left, const QString& right);
bool containsPath(const QString& parent, const QString& child);
bool safeRelativePath(const QString& path);
bool validatePath(const QString& path, QString* error);
bool validateTree(const QString& path, QString* error);
bool resolveTarget(const QString& projectRoot, const QString& input,
                   QString* target, QString* error);
Discovery discover(const QString& projectRoot);
bool validateTarget(const QString& projectRoot, const QString& target,
                    QString* error, bool removing = false);

}
