#include "Update/UpdateFile.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QThread>

namespace VNH {

bool writeUpdateFile(const QString& path, const QByteArray& contents)
{
    if (!QDir().mkpath(QFileInfo(path).dir().path())) {
        qWarning().noquote() << "Cannot create update file directory:" << path;
        return false;
    }
    for (int attempt = 0; ; ++attempt) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) {
            qWarning().noquote() << "Cannot write update file:" << path
                                << file.error() << file.errorString();
            return false;
        }
        if (file.commit())
            return true;
#if defined(Q_OS_WIN)
        if (file.error() == QFileDevice::RenameError && attempt < 4) {
            qWarning().noquote() << "Retrying update file commit:" << path
                                << file.error() << file.errorString();
            QThread::msleep(10u << attempt);
            continue;
        }
#endif
        qWarning().noquote() << "Cannot commit update file:" << path
                            << file.error() << file.errorString();
        return false;
    }
}

}
