#include "Core/ProjectInstance.h"

#include <QApplication>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <cstring>
#include <memory>

int main(int argc, char* argv[])
{
    bool denyActivation = false;
    for (int index = 1; index < argc; ++index)
        denyActivation = denyActivation || std::strcmp(argv[index], "--deny-activation") == 0;
    std::unique_ptr<QCoreApplication> app;
    if (denyActivation) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        auto gui = std::make_unique<QApplication>(argc, argv);
        gui->setQuitOnLastWindowClosed(false);
        app = std::move(gui);
    }
    const QStringList arguments = app->arguments();
    if (arguments.size() < 2)
        return 2;
    const auto result = VNX::ProjectInstance::acquire(arguments.at(1), denyActivation
        ? VNX::ProjectInstance::ActivationPolicy::Deny : VNX::ProjectInstance::ActivationPolicy::Allow);
    QTextStream output(stdout);
    if (result.status == VNX::ProjectInstance::Status::Forwarded) {
        output << "forwarded" << Qt::endl;
        return 0;
    }
    if (result.status == VNX::ProjectInstance::Status::Failed) {
        output << "failed: " << result.error << Qt::endl;
        return 1;
    }
    QObject::connect(result.instance.get(), &VNX::ProjectInstance::activationRequested,
                     app.get(), [&output]() { output << "activated" << Qt::endl; });
    output << "acquired" << Qt::endl;
    if (arguments.contains(QStringLiteral("--unresponsive")))
        QThread::sleep(20);
    QTimer::singleShot(20000, app.get(), &QCoreApplication::quit);
    return app->exec();
}
