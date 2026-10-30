#include "CacheArtifactWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cstdio>
#endif

namespace VNX {

bool writeCacheArtifact(const QString& path, const std::function<bool(QIODevice&)>& write)
{
    const QFileInfo destination(path);
    if (!QDir().mkpath(destination.absolutePath())) return false;
    QString temporaryPath;
    {
        QTemporaryFile temporary(destination.absoluteFilePath() + QStringLiteral(".XXXXXX"));
        if (!temporary.open() || !write(temporary) || !temporary.flush()) return false;
        temporaryPath = temporary.fileName();
        temporary.setAutoRemove(false);
    }
#ifdef Q_OS_WIN
    const auto windowsPath = [](const QString& path) {
        const auto native = QDir::toNativeSeparators(path);
        if (native.startsWith(QStringLiteral("\\\\?\\"))) return native;
        return native.startsWith(QStringLiteral("\\\\"))
            ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
    };
    const auto sourcePath = windowsPath(temporaryPath);
    const auto targetPath = windowsPath(destination.absoluteFilePath());
    const bool replaced = MoveFileExW(
        reinterpret_cast<LPCWSTR>(sourcePath.utf16()),
        reinterpret_cast<LPCWSTR>(targetPath.utf16()),
        MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool replaced = std::rename(QFile::encodeName(temporaryPath).constData(),
        QFile::encodeName(destination.absoluteFilePath()).constData()) == 0;
#endif
    if (!replaced) QFile::remove(temporaryPath);
    return replaced;
}

}
