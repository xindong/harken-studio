#include "Query/LMDBBenchmark.h"
#include <harken_buildinfo.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    int objectCount = 0;
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--objects") &&
            i + 1 < argc) {
            bool ok = false;
            objectCount = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || objectCount <= 0)
                return 2;
        } else {
            return 2;
        }
    }
    if (objectCount <= 0)
        return 2;

    auto result = VNX::LMDBBenchmark::runSynthetic(objectCount);
    const bool complete = result.complete && result.objectCount == objectCount &&
        result.wordResultCount > 0 && result.phraseResultCount > 0 &&
        result.trigramResultCount > 0 && result.boundedBatchCount == 256;
    QJsonObject record = result.toJson();
    record.insert(QStringLiteral("benchmark"),
                  QStringLiteral("pack-foundation-studio"));
    record.insert(QStringLiteral("schema"),
                  QStringLiteral("plan05-f3-studio-v1"));
    record.insert(QStringLiteral("status"),
                  complete ? QStringLiteral("complete")
                           : QStringLiteral("failed"));
    record.insert(QStringLiteral("buildCommit"),
                  QString::fromUtf8(Harken::BuildInfo::GitCommit.data(),
                                    static_cast<int>(
                                        Harken::BuildInfo::GitCommit.size())));
    record.insert(QStringLiteral("buildDirty"),
                  Harken::BuildInfo::GitDirty);
    record.insert(QStringLiteral("buildConfiguration"),
                  QStringLiteral(HARKEN_QUALIFICATION_CONFIGURATION));
#if defined(NDEBUG)
    record.insert(QStringLiteral("releaseBuild"), true);
#else
    record.insert(QStringLiteral("releaseBuild"), false);
#endif
    const QByteArray json = QJsonDocument(record).toJson(QJsonDocument::Compact);
    std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
    std::fputc('\n', stdout);
    return complete ? 0 : 1;
}
