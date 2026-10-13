#include "App/FileLog.h"
#include "Security/TrustAnchors.h"
#include "Update/UpdateBridgeConsumer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>

int main(int argc, char* argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("HarkenSound"));
    QCoreApplication::setApplicationName(QStringLiteral("HarkenHub"));
    VNH::FileLog::install(QStringLiteral("bridge"));
    QCoreApplication app(argc, argv);

    bool consume = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--consume-update-queue") == 0)
            consume = true;
    }
    if (!consume) {
        qInfo().noquote() << QStringLiteral(
            "Usage: HarkenHubUpdateBridge --consume-update-queue");
        return 1;
    }

    const auto report = VNH::UpdateBridgeConsumer::run(
        VNH::TrustAnchors::manifestPublicKey(),
        QDateTime::currentDateTimeUtc(),
        VNH::UpdateBridgeConsumer::defaultHooks());
    for (const QString& detail : report.details)
        qInfo().noquote() << detail;
    qInfo().noquote()
        << QStringLiteral(
               "Update queue consumed: %1 installed, %2 failed, %3 deferred")
               .arg(report.installed)
               .arg(report.failed)
               .arg(report.deferred);
    return report.failed > 0 ? 2 : 0;
}
