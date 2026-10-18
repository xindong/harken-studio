#include "Platform/AtomicFile.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

namespace VNH {

bool writeAtomicFile(const QString& path, const QByteArray& contents,
                     QString* error)
{
    if (error)
        error->clear();
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        qWarning().noquote() << message;
        return false;
    };
    if (!QDir().mkpath(QFileInfo(path).dir().path())) {
        return fail(QStringLiteral("Cannot create file directory: %1")
                        .arg(QFileInfo(path).dir().path()));
    }
    for (int attempt = 0; ; ++attempt) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("Cannot open file for writing: %1 (Qt error %2: %3)")
                            .arg(path).arg(file.error()).arg(file.errorString()));
        if (file.write(contents) != contents.size())
            return fail(QStringLiteral("Cannot write file: %1 (Qt error %2: %3)")
                            .arg(path).arg(file.error()).arg(file.errorString()));
        if (file.commit())
            return true;
#if defined(Q_OS_WIN)
        if (file.error() == QFileDevice::RenameError && attempt < 4) {
            qWarning().noquote() << "Retrying atomic file commit:" << path
                                << file.error() << file.errorString();
            QThread::msleep(10u << attempt);
            continue;
        }
#endif
        return fail(QStringLiteral("Cannot commit file: %1 (Qt error %2: %3)")
                        .arg(path).arg(file.error()).arg(file.errorString()));
    }
}

}
