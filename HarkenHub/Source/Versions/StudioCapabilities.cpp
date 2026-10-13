#include "Versions/StudioCapabilities.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <limits>

namespace VNH {

bool validateStudioCapabilities(const QJsonObject& object,
                                const QString& expectedVersion,
                                const QString& expectedBuildId,
                                QString* error,
                                int* capabilitiesVersion)
{
    const auto fail = [error](const char* message) {
        if (error)
            *error = QCoreApplication::translate("HarkenHub", message);
        return false;
    };
    const QJsonValue protocol = object.value(QStringLiteral("capabilitiesVersion"));
    int version = 0;
    if (!protocol.isUndefined()) {
        const qint64 number = protocol.toInteger(-1);
        if (!protocol.isDouble() || number <= 0
            || number > std::numeric_limits<int>::max()
            || protocol.toDouble() != static_cast<double>(number))
            return fail("Studio capabilities has an invalid protocol version.");
        version = static_cast<int>(number);
    } else {
        const int minimum = object.value(QStringLiteral("readableSchemaMin")).toInt();
        const int maximum = object.value(QStringLiteral("readableSchemaMax")).toInt();
        const int writable = object.value(QStringLiteral("writableSchema")).toInt();
        if (minimum <= 0 || maximum < minimum || writable < minimum || writable > maximum)
            return fail("Studio capabilities has invalid legacy schema information.");
    }
    if (expectedVersion.isEmpty()
        || ((version >= 2 || object.contains(QStringLiteral("studioVersion")))
            && object.value(QStringLiteral("studioVersion")).toString() != expectedVersion))
        return fail("Studio capabilities version does not match the package.");
    if (expectedBuildId.isEmpty()
        || ((version >= 2 || object.contains(QStringLiteral("buildId")))
            && object.value(QStringLiteral("buildId")).toString() != expectedBuildId))
        return fail("Studio capabilities build identity does not match the package.");
    if (capabilitiesVersion)
        *capabilitiesVersion = version;
    return true;
}

}
