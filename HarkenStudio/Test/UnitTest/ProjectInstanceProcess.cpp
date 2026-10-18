#include "Core/ProjectInstance.h"

#include <QApplication>
#include <QTextStream>
#include <QThread>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2)
        return 2;
    const auto result = VNX::ProjectInstance::acquire(arguments.at(1));
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
                     &app, [&output]() { output << "activated" << Qt::endl; });
    output << "acquired" << Qt::endl;
    if (arguments.contains(QStringLiteral("--unresponsive")))
        QThread::sleep(20);
    QTimer::singleShot(20000, &app, &QCoreApplication::quit);
    return app.exec();
}
