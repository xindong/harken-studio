#include "Platform/ProcessScan.h"

#include <QDir>
#include <QFileInfo>

#include <libproc.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <cstring>
#include <cerrno>

#include <vector>

namespace VNH {

bool runningUnrealEditors(QList<RunningProcess>* processes)
{
    processes->clear();
    const int needed = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (needed <= 0) return false;
    std::vector<pid_t> pids(static_cast<size_t>(needed) / sizeof(pid_t) + 64);
    const int used = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                                   static_cast<int>(pids.size() * sizeof(pid_t)));
    if (used <= 0) return false;
    for (int i = 0; i < used / static_cast<int>(sizeof(pid_t)); ++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[i] <= 0 || proc_pidpath(pids[i], path, sizeof(path)) <= 0) continue;
        RunningProcess process;
        process.executable = QString::fromUtf8(path);
        const QString name = QFileInfo(process.executable).fileName();
        if (name != QStringLiteral("UnrealEditor") && name != QStringLiteral("UE4Editor")
            && !name.startsWith(QStringLiteral("UnrealEditor-"))
            && !name.startsWith(QStringLiteral("UE4Editor-"))) continue;
        int limit = 0;
        size_t size = sizeof(limit);
        int argMax[] = {CTL_KERN, KERN_ARGMAX};
        if (sysctl(argMax, 2, &limit, &size, nullptr, 0) != 0 || limit <= 0) return false;
        std::vector<char> data(static_cast<size_t>(limit));
        int query[] = {CTL_KERN, KERN_PROCARGS2, pids[i]};
        size = data.size();
        if (sysctl(query, 3, data.data(), &size, nullptr, 0) != 0
            || size < sizeof(int)) return false;
        int argc = 0;
        std::memcpy(&argc, data.data(), sizeof(argc));
        size_t offset = sizeof(argc);
        while (offset < size && data[offset]) ++offset;
        while (offset < size && !data[offset]) ++offset;
        for (int arg = 0; arg < argc && offset < size; ++arg) {
            const size_t start = offset;
            while (offset < size && data[offset]) ++offset;
            if (offset == size) return false;
            if (arg > 0) process.arguments.append(QString::fromUtf8(data.data() + start,
                                                       static_cast<int>(offset - start)));
            ++offset;
        }
        processes->append(process);
    }
    return true;
}

QStringList runningExecutablesUnder(const QString& directory)
{
    return runningExecutablesUnder(directory, -1);
}

QStringList runningExecutablesUnder(const QString& directory,
                                    qint64 excludePid)
{
    QStringList matches;
    const QString prefix = QDir::cleanPath(QDir(directory).absolutePath()) +
                           QStringLiteral("/");
    if (prefix.length() <= 1)
        return matches;

    const int bytesNeeded = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytesNeeded <= 0)
        return matches;
    std::vector<pid_t> pids(
        static_cast<size_t>(bytesNeeded) / sizeof(pid_t) + 64);
    const int bytesUsed =
        proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                      static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytesUsed <= 0)
        return matches;

    const size_t count = static_cast<size_t>(bytesUsed) / sizeof(pid_t);
    for (size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0)
            continue;
        if (excludePid >= 0 && pid == static_cast<pid_t>(excludePid))
            continue;
        char buffer[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, buffer, sizeof(buffer)) <= 0)
            continue;
        const QString imagePath = QDir::cleanPath(QString::fromUtf8(buffer));
        if (imagePath.startsWith(prefix, Qt::CaseInsensitive))
            matches.append(imagePath);
    }
    return matches;
}

QString executablePathForPid(qint64 pid)
{
    if (pid <= 0)
        return QString();
    char buffer[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(static_cast<pid_t>(pid), buffer, sizeof(buffer)) <= 0)
        return QString();
    return QDir::cleanPath(QString::fromUtf8(buffer));
}

bool isProcessRunning(qint64 pid)
{
    return pid > 0 && (kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH);
}

bool terminateProcessById(qint64 pid)
{
    if (pid <= 0)
        return false;
    return kill(static_cast<pid_t>(pid), SIGKILL) == 0;
}

}
