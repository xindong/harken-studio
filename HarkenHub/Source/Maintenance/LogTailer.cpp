#include "Maintenance/LogTailer.h"

#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace VNH {

LogTailer::LogTailer(QObject* parent)
    : QObject(parent)
{
    connect(&timer_, &QTimer::timeout, this, &LogTailer::poll);
}

void LogTailer::setFiles(const QStringList& filePaths)
{
    files_ = filePaths;
    states_.clear();
}

void LogTailer::start(int intervalMs)
{
    poll();
    timer_.start(intervalMs);
}

void LogTailer::stop()
{
    timer_.stop();
}

void LogTailer::poll()
{
    QStringList lines;
    for (const QString& path : files_) {
        const QFileInfo info(path);
        if (!info.exists()) {
            states_.remove(path);
            continue;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;

        TailState state = states_.value(path);
        const QByteArray head = file.read(24);
        if (state.offset > 0 &&
            (info.size() < state.offset || !head.startsWith(state.head)))
            state.offset = 0;
        if (head.size() >= state.head.size())
            state.head = head;
        if (info.size() <= state.offset) {
            states_[path] = state;
            continue;
        }
        if (!file.seek(state.offset)) {
            states_[path] = state;
            continue;
        }
        const QByteArray chunk = file.read(info.size() - state.offset);
        const qsizetype lastNewline = chunk.lastIndexOf('\n');
        if (lastNewline < 0) {
            states_[path] = state;
            continue;
        }
        const QByteArray complete = chunk.left(lastNewline + 1);
        state.offset += complete.size();
        states_[path] = state;
        lines.append(QString::fromUtf8(complete)
                         .split(QLatin1Char('\n'), Qt::SkipEmptyParts));
    }

    if (lines.isEmpty())
        return;
    std::stable_sort(lines.begin(), lines.end(),
                     [](const QString& left, const QString& right) {
                         return left.left(24) < right.left(24);
                     });
    emit appended(lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
}

}
