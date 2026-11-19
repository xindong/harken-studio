#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace VNH {

struct SelfTestResult
{
    bool ok = false;
    QString error;
    QString product;
    QString reportedBuildId;
    QString reportedVersion;
    QString channel;
};

class SelfTestRunner
{
public:
    static SelfTestResult parseOutput(const QByteArray& output,
                                      const QString& expectedBuildId);
    static SelfTestResult run(const QString& program, const QStringList& args,
                              const QString& expectedBuildId, int timeoutMs);
    static SelfTestResult runForInstall(const QString& installPath,
                                        const QString& expectedBuildId);
};

}
