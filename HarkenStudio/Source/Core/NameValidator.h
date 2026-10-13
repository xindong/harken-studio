
#pragma once

#include <QString>
#include <QUuid>
#include <functional>
#include "ProjectTypes.h"

namespace VNX {

class NameValidator
{
public:
    static constexpr int MAX_NAME_LENGTH = 128;

    enum class Result : uint8_t {
        Ok = 0,
        Empty,
        TooLong,
        IllegalCharacter,
        WindowsReserved,
        InvalidDisplayCharacter,
        InvalidFileName,
        InvalidIdentifier
    };

    static bool validate(const QString& name, Result* result = nullptr);
    static bool validateItemName(const QString& name, ProjectItem::Type type,
                                 Result* result = nullptr);
    static bool validateDisplayName(const QString& name, Result* result = nullptr);
    static bool validateFileName(const QString& name, Result* result = nullptr);
    static QString objectFileName(const QString& name, const QUuid& guid);

    static QString resultMessage(Result result);

    static bool isAllowedChar(QChar c);

    static bool isWindowsReserved(const QString& name);

    static QString makeUniqueName(const QString& baseName,
                                  const std::function<bool(const QString&)>& isAvailable,
                                  int maxLength = MAX_NAME_LENGTH);
};

}
