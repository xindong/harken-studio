#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

class QJsonObject;
class QProcess;

namespace Harkenizer {

const char* resourceCompilerIdentity();

class ResourceCompilerClient
{
public:
    enum class Status { Succeeded, Refused, Crashed, Unavailable, TimedOut, InvalidResponse };

    struct Result {
        Status status = Status::Unavailable;
        QString error;
        QString outputLog;
        QString diagnosticLog;
    };

    explicit ResourceCompilerClient(QString program = {}, int requestTimeoutMs = 300000);
    ~ResourceCompilerClient();
    ResourceCompilerClient(const ResourceCompilerClient&) = delete;
    ResourceCompilerClient& operator=(const ResourceCompilerClient&) = delete;

    QByteArray identity(QString* error);
    Result compile(const QString& manifestPath, const QString& outputPath);

private:
    bool start(QString* error);
    bool readResponse(QJsonObject* response, int timeoutMs, QString* error, Status* status);
    void stop();
    QString executable() const;

    QString program_;
    int requestTimeoutMs_;
    std::unique_ptr<QProcess> process_;
};

}
