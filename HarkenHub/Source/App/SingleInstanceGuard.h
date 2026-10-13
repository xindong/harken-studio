#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QLocalServer;
class QLockFile;

namespace VNH {

class SingleInstanceGuard : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstanceGuard(const QString& key, QObject* parent = nullptr);
    ~SingleInstanceGuard() override;

    bool isPrimary() const { return primary_; }
    qint64 primaryProcessId() const;
    bool notifyPrimary(const QStringList& arguments);
    bool retryPrimary();

    static QString guiServerName();
    static QString backgroundServerName();
    static bool sendTo(const QString& key, const QStringList& arguments);
    static bool isServerActive(const QString& key);

signals:
    void activationRequested(const QStringList& arguments);

private:
    void startServer();

    QString key_;
    bool primary_ = false;
    QLocalServer* server_ = nullptr;
    int activeClients_ = 0;
    std::unique_ptr<QLockFile> lock_;
};

}
