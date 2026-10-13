#include "Maintenance/LogExporter.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallIndex.h"
#include "Transactions/TransactionJournal.h"
#include "Versions/LaunchDiagnostics.h"

#include <QDir>
#include <QFileInfo>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

namespace VNH {

QList<QPair<QString, QString>> LogExporter::defaultSources()
{
    QList<QPair<QString, QString>> sources;

    auto addIfExists = [&sources](const QString& path, const QString& entry) {
        if (QFileInfo::exists(path))
            sources.append({path, entry});
    };

    addIfExists(TransactionJournal::filePath(),
                QStringLiteral("machine/transactions.log"));
    addIfExists(InstallIndex::filePath(),
                QStringLiteral("machine/install-index.json"));
    addIfExists(LaunchDiagnostics::filePath(),
                QStringLiteral("user/launch-diagnostics.json"));
    addIfExists(userDataDir() + QStringLiteral("/update-state.json"),
                QStringLiteral("user/update-state.json"));

    const QDir logsDir(userDataDir() + QStringLiteral("/logs"));
    if (logsDir.exists()) {
        const auto entries =
            logsDir.entryInfoList({QStringLiteral("*.log")},
                                  QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo& info : entries)
            sources.append({info.filePath(),
                            QStringLiteral("user/logs/") + info.fileName()});
    }
    return sources;
}

bool LogExporter::exportFiles(const QString& zipPath,
                              const QList<QPair<QString, QString>>& sources,
                              QString* error)
{
    if (sources.isEmpty()) {
        *error = QStringLiteral("There is nothing to export");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(zipPath).dir().path())) {
        *error = QStringLiteral("Cannot create the export directory");
        return false;
    }

    void* writer = mz_zip_writer_create();
    if (!writer) {
        *error = QStringLiteral("Cannot create the archive writer");
        return false;
    }
    if (mz_zip_writer_open_file(writer, zipPath.toUtf8().constData(), 0, 0) !=
        MZ_OK) {
        mz_zip_writer_delete(&writer);
        *error = QStringLiteral("Cannot open the archive for writing: %1")
                     .arg(zipPath);
        return false;
    }

    bool ok = true;
    for (const auto& source : sources) {
        if (mz_zip_writer_add_file(writer, source.first.toUtf8().constData(),
                                   source.second.toUtf8().constData()) !=
            MZ_OK) {
            *error = QStringLiteral("Cannot add %1 to the archive")
                         .arg(source.first);
            ok = false;
            break;
        }
    }
    if (mz_zip_writer_close(writer) != MZ_OK && ok) {
        *error = QStringLiteral("Cannot finish the archive");
        ok = false;
    }
    mz_zip_writer_delete(&writer);
    if (!ok)
        QFile::remove(zipPath);
    return ok;
}

bool LogExporter::exportDiagnostics(const QString& zipPath, QString* error)
{
    return exportFiles(zipPath, defaultSources(), error);
}

}
