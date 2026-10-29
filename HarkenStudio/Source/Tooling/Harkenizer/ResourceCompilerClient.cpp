#include "ResourceCompilerClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <utility>

namespace Harkenizer {

ResourceCompilerClient::ResourceCompilerClient(QString program, int requestTimeoutMs)
    : program_(std::move(program)), requestTimeoutMs_(requestTimeoutMs)
{
}

ResourceCompilerClient::~ResourceCompilerClient()
{
    if (!process_) return;
    process_->closeWriteChannel();
    if (!process_->waitForFinished(1000)) stop();
}

QString ResourceCompilerClient::executable() const
{
    if (!program_.isEmpty()) return QFileInfo::exists(program_) ? program_ : QString();
    const QString overridePath = qEnvironmentVariable("HARKENIZER_PATH");
    if (!overridePath.isEmpty() && QFileInfo::exists(overridePath)) return overridePath;
#ifdef _WIN32
    const QString name = QStringLiteral("Harkenizer.exe");
#else
    const QString name = QStringLiteral("Harkenizer");
#endif
    const QStringList roots = {
        QCoreApplication::applicationDirPath(), QDir::currentPath(),
        QDir(QDir::currentPath()).filePath(QStringLiteral("bin")),
        QDir(QDir::currentPath()).filePath(QStringLiteral("../bin"))
    };
    for (const auto& root : roots) {
        const QString path = QDir(root).filePath(name);
        if (QFileInfo::exists(path)) return path;
    }
    return QStandardPaths::findExecutable(name);
}

QByteArray ResourceCompilerClient::identity(QString* error)
{
    QString startError;
    if (!start(&startError)) {
        if (error) *error = startError;
        return {};
    }
    return QByteArray(resourceCompilerIdentity());
}

void ResourceCompilerClient::stop()
{
    if (!process_) return;
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished();
    }
    process_.reset();
}

bool ResourceCompilerClient::readResponse(QJsonObject* response, int timeoutMs, QString* error, Status* status)
{
    QElapsedTimer timer;
    timer.start();
    while (!process_->canReadLine()) {
        const auto remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0) {
            *status = Status::TimedOut;
            *error = QStringLiteral("Harkenizer resource compiler timed out");
            return false;
        }
        if (!process_->waitForReadyRead(static_cast<int>(remaining))) {
            if (process_->canReadLine()) break;
            const QString diagnostics = QString::fromUtf8(process_->readAllStandardError()).trimmed();
            if (process_->error() == QProcess::Timedout) {
                *status = Status::TimedOut;
                *error = QStringLiteral("Harkenizer resource compiler timed out");
            } else {
                *status = Status::Crashed;
                *error = diagnostics.isEmpty()
                    ? QStringLiteral("Harkenizer resource compiler exited before completing the request")
                    : diagnostics;
            }
            return false;
        }
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(process_->readLine(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *status = Status::InvalidResponse;
        *error = QStringLiteral("Invalid Harkenizer resource compiler response");
        return false;
    }
    *response = document.object();
    return true;
}

bool ResourceCompilerClient::start(QString* error)
{
    if (process_ && process_->state() == QProcess::Running) return true;
    stop();
    const QString path = executable();
    if (path.isEmpty()) {
        *error = QStringLiteral("Harkenizer executable was not found");
        return false;
    }
    process_ = std::make_unique<QProcess>();
    process_->setProgram(path);
    process_->setArguments({QStringLiteral("--resource-worker")});
    process_->start();
    if (!process_->waitForStarted()) {
        *error = QStringLiteral("Cannot start Harkenizer: %1").arg(path);
        stop();
        return false;
    }
    QJsonObject ready;
    Status status = Status::Unavailable;
    if (!readResponse(&ready, 30000, error, &status)) {
        stop();
        return false;
    }
    if (ready.value(QStringLiteral("protocol")).toInt() != 1
        || ready.value(QStringLiteral("compiler")).toString() != QLatin1String(resourceCompilerIdentity())) {
        *error = QStringLiteral("Harkenizer resource compiler does not match this build");
        stop();
        return false;
    }
    return true;
}

ResourceCompilerClient::Result ResourceCompilerClient::compile(
    const QString& manifestPath, const QString& outputPath)
{
    Result result;
    if (!start(&result.error)) return result;
    const QByteArray request = QJsonDocument(QJsonObject{
        {QStringLiteral("manifest"), manifestPath}}).toJson(QJsonDocument::Compact) + '\n';
    if (process_->write(request) != request.size()) {
        result.error = QStringLiteral("Cannot submit Harkenizer resource compilation for %1").arg(outputPath);
        stop();
        return result;
    }
    QJsonObject response;
    if (!readResponse(&response, requestTimeoutMs_, &result.error, &result.status)) {
        result.error += QStringLiteral(" for %1").arg(outputPath);
        stop();
        return result;
    }
    if (!response.value(QStringLiteral("success")).isBool()) {
        result.status = Status::InvalidResponse;
        result.error = QStringLiteral("Invalid Harkenizer resource compiler result");
        stop();
        return result;
    }
    result.status = response.value(QStringLiteral("success")).toBool() ? Status::Succeeded : Status::Refused;
    result.error = response.value(QStringLiteral("error")).toString();
    result.outputLog = response.value(QStringLiteral("outputLog")).toString();
    result.diagnosticLog = response.value(QStringLiteral("diagnosticLog")).toString();
    return result;
}

}
