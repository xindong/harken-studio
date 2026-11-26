#include "ThemeFontAlias.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QtEndian>

#include <algorithm>

namespace harken::studio {
namespace {

quint16 word(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint16>(bytes.data() + offset);
}

quint32 checksum(QByteArrayView bytes)
{
    quint32 sum = 0;
    for (qsizetype i = 0; i < bytes.size(); ++i)
        sum += quint32(quint8(bytes[i])) << (24 - 8 * (i % 4));
    return sum;
}

QSet<quint16> instancePostScriptNames(QByteArrayView fvar)
{
    QSet<quint16> names;
    if (fvar.size() < 16) return names;
    const int axes = word(fvar, 8);
    const int count = word(fvar, 12);
    const int size = word(fvar, 14);
    const qsizetype instances = word(fvar, 4) + qsizetype(axes) * word(fvar, 10);
    if (size < 6 + 4 * axes || instances + qsizetype(count) * size > fvar.size())
        return names;
    for (int i = 0; i < count; ++i) {
        const auto id = word(fvar, instances + i * qsizetype(size) + 4 + 4 * axes);
        if (id != 0xffff) names.insert(id);
    }
    return names;
}

QByteArray encodedName(const QByteArray& ascii, quint16 platform, quint16 encoding)
{
    if (platform != 0 && platform != 3 && !(platform == 2 && encoding == 1))
        return ascii;
    QByteArray bytes(ascii.size() * 2, '\0');
    for (qsizetype i = 0; i < ascii.size(); ++i)
        bytes[i * 2 + 1] = ascii[i];
    return bytes;
}

QByteArray renamedTable(QByteArrayView source, const QByteArray& alias, const QByteArray& faceName,
                        const QSet<quint16>& instanceNames)
{
    if (source.size() < 6) return {};
    const int format = word(source, 0);
    const int count = word(source, 2);
    const int storage = word(source, 4);
    const int recordsEnd = 6 + count * 12;
    if (format > 1 || recordsEnd + 2 * format > storage || storage > source.size())
        return {};
    const int languages = format ? word(source, recordsEnd) : 0;
    if (format && recordsEnd + 2 + 4 * languages > storage) return {};
    QByteArray records = source.first(storage).toByteArray();
    QByteArray strings;
    QHash<QByteArray, quint16> offsets;
    for (int i = 0; i < count + languages; ++i) {
        const int record = i < count ? 6 + 12 * i : recordsEnd + 2 + 4 * (i - count);
        const int lengthField = i < count ? record + 8 : record;
        const int length = word(source, lengthField);
        const int start = storage + word(source, lengthField + 2);
        if (start + length > source.size()) return {};
        QByteArray value = source.sliced(start, length).toByteArray();
        if (i < count) {
            const quint16 id = word(source, record + 6);
            QByteArray name;
            if (id == 1 || id == 16 || id == 21 || id == 25)
                name = alias;
            else if (id == 3 || id == 4 || id == 6 || id == 18 || id == 20)
                name = faceName;
            else if (instanceNames.contains(id))
                name = faceName + '-' + QByteArray::number(id);
            if (!name.isEmpty())
                value = encodedName(name, word(source, record), word(source, record + 2));
        }
        if (!offsets.contains(value)) {
            if (strings.size() > 0xffff) return {};
            offsets.insert(value, quint16(strings.size()));
            strings += value;
        }
        qToBigEndian<quint16>(quint16(value.size()), records.data() + lengthField);
        qToBigEndian<quint16>(offsets.value(value), records.data() + lengthField + 2);
    }
    return records + strings;
}

}

QByteArray aliasThemeFont(const QByteArray& data, const QString& family)
{
    if (data.size() < 12 || family.isEmpty() || family.size() > 31) return {};
    const QByteArray alias = family.toLatin1();
    for (const auto c : alias)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return {};
    const quint32 signature = qFromBigEndian<quint32>(data.constData());
    if (signature != 0x00010000 && signature != 0x4f54544f && signature != 0x74727565) return {};
    const int count = word(data, 4);
    if (12 + count * 16 > data.size()) return {};
    QMap<QByteArray, QByteArrayView> tables;
    qsizetype contents = 0;
    for (int i = 0; i < count; ++i) {
        const int index = 12 + i * 16;
        const QByteArray tag = data.mid(index, 4);
        const quint32 offset = qFromBigEndian<quint32>(data.constData() + index + 8);
        const quint32 length = qFromBigEndian<quint32>(data.constData() + index + 12);
        contents += length;
        if (quint64(offset) + length > quint64(data.size()) || contents > data.size() || tables.contains(tag))
            return {};
        if (tag != "DSIG") tables.insert(tag, QByteArrayView(data).sliced(offset, length));
    }
    const auto originalHead = tables.value("head");
    if (originalHead.size() < 54 || !tables.contains("name") || tables.size() > 4095) return {};
    const QByteArray faceName = alias + '-' + QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(12);
    const QByteArray names = renamedTable(tables.value("name"), alias, faceName, instancePostScriptNames(tables.value("fvar")));
    if (names.isEmpty()) return {};
    QByteArray head = originalHead.toByteArray();
    qToBigEndian<quint32>(0, head.data() + 8);
    tables["name"] = QByteArrayView(names);
    tables["head"] = QByteArrayView(head);

    const int tableCount = int(tables.size());
    QByteArray result(12 + 16 * tableCount, '\0');
    int selector = 0;
    while ((2 << selector) <= tableCount) ++selector;
    const int searchRange = 16 << selector;
    qToBigEndian(signature, result.data());
    qToBigEndian<quint16>(tableCount, result.data() + 4);
    qToBigEndian<quint16>(searchRange, result.data() + 6);
    qToBigEndian<quint16>(selector, result.data() + 8);
    qToBigEndian<quint16>(16 * tableCount - searchRange, result.data() + 10);
    int index = 12;
    qsizetype headOffset = 0;
    for (auto it = tables.cbegin(); it != tables.cend(); ++it) {
        const auto bytes = it.value();
        const qsizetype offset = result.size();
        std::copy_n(it.key().constData(), 4, result.data() + index);
        qToBigEndian(checksum(bytes), result.data() + index + 4);
        qToBigEndian<quint32>(offset, result.data() + index + 8);
        qToBigEndian<quint32>(bytes.size(), result.data() + index + 12);
        if (it.key() == "head") headOffset = offset;
        result.append(bytes.data(), bytes.size());
        result.append((4 - bytes.size() % 4) % 4, '\0');
        index += 16;
    }
    const quint32 adjustment = 0xb1b0afba - checksum(result);
    qToBigEndian(adjustment, result.data() + headOffset + 8);
    return result;
}

}
