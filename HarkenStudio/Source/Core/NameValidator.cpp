
#include "NameValidator.h"

#include <QChar>
#include <QRegularExpression>
#include <QStringList>
#include <limits>

namespace VNX {

bool NameValidator::isAllowedChar(QChar c)
{
    const ushort u = c.unicode();
    if (u >= 'A' && u <= 'Z') return true;
    if (u >= 'a' && u <= 'z') return true;
    if (u >= '0' && u <= '9') return true;
    return u == '_' || u == '-' || u == '.' || u == ' ';
}

bool NameValidator::isWindowsReserved(const QString& name)
{
    const int dot = name.indexOf('.');
    const QString stem = (dot >= 0) ? name.left(dot) : name;
    const QString upper = stem.toUpper();

    if (upper == QStringLiteral("CON")) return true;
    if (upper == QStringLiteral("PRN")) return true;
    if (upper == QStringLiteral("AUX")) return true;
    if (upper == QStringLiteral("NUL")) return true;

    if (upper.length() == 4 &&
        (upper.startsWith(QStringLiteral("COM")) || upper.startsWith(QStringLiteral("LPT"))))
    {
        const QChar tail = upper.at(3);
        if (tail >= '1' && tail <= '9') return true;
    }
    return false;
}

bool NameValidator::validate(const QString& name, Result* result)
{
    auto fail = [result](Result r) { if (result) *result = r; return false; };
    auto pass = [result]()         { if (result) *result = Result::Ok; return true; };

    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return fail(Result::Empty);
    }
    if (name.length() > MAX_NAME_LENGTH) {
        return fail(Result::TooLong);
    }
    for (const QChar c : name) {
        if (!isAllowedChar(c)) {
            return fail(Result::IllegalCharacter);
        }
    }
    if (isWindowsReserved(name)) {
        return fail(Result::WindowsReserved);
    }
    return pass();
}

bool NameValidator::validateDisplayName(const QString& name, Result* result)
{
    const auto fail = [result](Result value) { if (result) *result = value; return false; };
    if (name.trimmed().isEmpty()) return fail(Result::Empty);
    if (name.normalized(QString::NormalizationForm_C).size() > MAX_NAME_LENGTH) return fail(Result::TooLong);
    if (!name.isValidUtf16()) return fail(Result::InvalidDisplayCharacter);
    for (const auto c : name.toUcs4()) {
        const auto category = QChar::category(c);
        if (c == '/' || c == '\\' || category == QChar::Other_Control
            || category == QChar::Separator_Line || category == QChar::Separator_Paragraph)
            return fail(Result::InvalidDisplayCharacter);
    }
    if (result) *result = Result::Ok;
    return true;
}

bool NameValidator::validateFileName(const QString& name, Result* result)
{
    if (!validateDisplayName(name, result)) return false;
    if (isWindowsReserved(name)) {
        if (result) *result = Result::WindowsReserved;
        return false;
    }
    if (name != name.trimmed() || name.endsWith(QLatin1Char('.'))
        || name.contains(QRegularExpression(QStringLiteral("[<>:\"|?*]")))) {
        if (result) *result = Result::InvalidFileName;
        return false;
    }
    return true;
}

bool NameValidator::validateItemName(const QString& name, ProjectItem::Type type,
                                      Result* result)
{
    if (type == ProjectItem::Type::Action || type == ProjectItem::Type::Variable) {
        if (!validateDisplayName(name, result)) return false;
        static const QRegularExpression identifier(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
        if (!identifier.match(name).hasMatch()) {
            if (result) *result = Result::InvalidIdentifier;
            return false;
        }
        return true;
    }
    return validateDisplayName(name, result);
}

QString NameValidator::objectFileName(const QString& name, const QUuid& guid)
{
    if (guid.isNull()) return {};
    QString clean = name.normalized(QString::NormalizationForm_C);
    clean.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x{0000}-\\x{001f}]")),
                  QStringLiteral("_"));
    while (!clean.isEmpty() && (clean.endsWith(QLatin1Char('.')) || clean.back().isSpace()))
        clean.chop(1);
    while (clean.toUtf8().size() > 200) {
        clean.chop(1);
        if (!clean.isEmpty() && clean.back().isHighSurrogate()) clean.chop(1);
    }
    if (clean.isEmpty()) clean = QStringLiteral("Object");
    if (isWindowsReserved(clean)) clean.prepend(QLatin1Char('_'));
    return clean + QStringLiteral("__") + guid.toString(QUuid::WithoutBraces)
        + QStringLiteral(".vp");
}

QString NameValidator::resultMessage(Result result)
{
    switch (result) {
        case Result::Ok:
            return QStringLiteral("OK");
        case Result::Empty:
            return QStringLiteral("Name must not be empty.");
        case Result::TooLong:
            return QStringLiteral("Name exceeds %1-character limit.")
                       .arg(MAX_NAME_LENGTH);
        case Result::IllegalCharacter:
            return QStringLiteral(
                "This name has a character that can't be used. Names can only "
                "use English letters, numbers, spaces, and _ - . characters.");
        case Result::WindowsReserved:
            return QStringLiteral(
                "Name matches a Windows reserved device (CON / PRN / AUX / NUL / "
                "COM1-9 / LPT1-9).");
        case Result::InvalidDisplayCharacter:
            return QStringLiteral("Names cannot contain control characters, line breaks, or path separators.");
        case Result::InvalidFileName:
            return QStringLiteral("Names cannot contain < > : \" | ? *, surrounding whitespace, or a trailing period.");
        case Result::InvalidIdentifier:
            return QStringLiteral("Action and Variable names must start with an English letter and contain only English letters, digits, and underscores.");
    }
    return QStringLiteral("Unknown name validation error.");
}

QString NameValidator::makeUniqueName(
    const QString& baseName, const std::function<bool(const QString&)>& isAvailable,
    int maxLength)
{
    if (baseName.isEmpty()) return {};
    if (baseName.size() <= maxLength && isAvailable(baseName)) return baseName;

    QString stem = baseName;
    qulonglong number = 1;
    static const QRegularExpression suffixPattern(QStringLiteral("_([0-9]+)$"));
    const auto match = suffixPattern.match(stem);
    if (match.hasMatch()) {
        stem.truncate(match.capturedStart());
        bool ok = false;
        const auto previous = match.captured(1).toULongLong(&ok);
        if (ok && previous < std::numeric_limits<qulonglong>::max())
            number = previous + 1;
    }
    if (stem.endsWith(QLatin1Char('_'))) stem.chop(1);

    for (;;) {
        const QString suffix = QStringLiteral("_%1").arg(number, 2, 10, QLatin1Char('0'));
        QString prefix = stem.left(maxLength - suffix.size());
        if (!prefix.isEmpty() && prefix.back().isHighSurrogate()) prefix.chop(1);
        const QString candidate = prefix + suffix;
        if (isAvailable(candidate)) return candidate;
        number = number == std::numeric_limits<qulonglong>::max() ? 1 : number + 1;
    }
}

}
