#pragma once

#include <QJsonObject>
#include <QMap>
#include <QStringList>

namespace VNH::Cli {

struct Reply
{
    int exitCode = 0;
    QJsonObject body;
};

struct Arguments
{
    QString group;
    QString operation;
    QMap<QString, QString> options;

    QString value(const QString& key, const QString& fallback = {}) const;
    QString required(const QString& key) const;
    bool flag(const QString& key) const;
};

int run(const QStringList& arguments);
QJsonObject commandReference();
QString writeGuides(QString* error);
Reply projects(const Arguments& args);
Reply versions(const Arguments& args);
Reply integrations(const Arguments& args);
Reply updates(const Arguments& args);
Reply success(const QJsonObject& data = {});
void require(bool condition, const QString& message, const QString& code = QStringLiteral("OperationFailed"));
template<class Store> void requireStore(Store& store, bool ok) { require(ok, store.lastError()); }
void confirm(const Arguments& args, QJsonObject plan);
QByteArray readFile(const QString& path);
QString fileHash(const QString& path);
void requireConsent(const QString& product);

}
