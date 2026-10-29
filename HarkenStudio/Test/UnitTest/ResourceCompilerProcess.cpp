#include "Tooling/Harkenizer/ResourceCompilerClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto respond = [](const QJsonObject& object) {
        const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
        std::cout.write(json.constData(), json.size());
        std::cout << '\n' << std::flush;
    };
    const auto compiler = qEnvironmentVariable("HARKEN_TEST_RESOURCE_COMPILER_IDENTITY",
        QLatin1String(Harkenizer::resourceCompilerIdentity()));
    respond({{"protocol", 1}, {"compiler", compiler}});
    const QByteArray pid = QByteArray::number(QCoreApplication::applicationPid());
    QString root;
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto path = request.value("manifest").toString();
        QFile control(path);
        if (!control.open(QIODevice::ReadOnly)) return 2;
        const auto command = QJsonDocument::fromJson(control.readAll()).object();
        control.close();
        root = QFileInfo(path).absolutePath();
        QFile requests(QDir(root).filePath("requests.log"));
        if (!requests.open(QIODevice::Append) || requests.write(pid + '\n') != pid.size() + 1)
            return 3;
        requests.close();
        const auto mode = command.value("mode").toString();
        const auto token = command.value("token").toString();
        if (mode == "crash") std::_Exit(23);
        if (mode == "timeout") {
            QThread::msleep(30000);
            return 4;
        }
        if (mode == "invalid-json") {
            std::cout << "invalid json\n" << std::flush;
            continue;
        }
        if (mode == "invalid-result") {
            respond({{"success", "true"}});
            continue;
        }
        const bool success = mode != "refuse";
        respond({{"success", success}, {"error", success ? QString() : token + " refused"},
            {"outputLog", token}, {"diagnosticLog", success ? QString() : token + " diagnostics"}});
    }
    if (!root.isEmpty()) {
        QFile stopped(QDir(root).filePath(QString::fromLatin1(pid) + ".stopped"));
        if (!stopped.open(QIODevice::WriteOnly) || stopped.write(pid) != pid.size()) return 5;
    }
    return std::cin.eof() ? 0 : 6;
}
