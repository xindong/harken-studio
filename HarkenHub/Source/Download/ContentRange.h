#pragma once

#include <QByteArray>

#include <optional>

namespace VNH {

struct ContentRange
{
    qint64 start;
    qint64 end;
    qint64 total;

    static std::optional<ContentRange> parse(const QByteArray& header)
    {
        if (!header.startsWith("bytes "))
            return std::nullopt;
        const auto range = header.mid(6).trimmed();
        const auto dash = range.indexOf('-');
        const auto slash = range.indexOf('/');
        if (dash <= 0 || slash <= dash + 1)
            return std::nullopt;
        bool startOk = false;
        bool endOk = false;
        bool totalOk = false;
        ContentRange parsed{
            range.left(dash).toLongLong(&startOk),
            range.mid(dash + 1, slash - dash - 1).toLongLong(&endOk),
            range.mid(slash + 1).toLongLong(&totalOk)};
        if (!startOk || !endOk || !totalOk || parsed.start < 0 ||
            parsed.end < parsed.start || parsed.total <= parsed.end)
            return std::nullopt;
        return parsed;
    }
};

}
