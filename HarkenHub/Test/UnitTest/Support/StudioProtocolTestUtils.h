#pragma once

#include <QJsonObject>

namespace HarkenHubTest {

inline QJsonObject studioCapabilities(const QString& buildId, const QString& version)
{
    return {{QStringLiteral("studioVersion"), version},
            {QStringLiteral("buildId"), buildId},
            {QStringLiteral("capabilitiesVersion"), 2}};
}

inline QJsonObject studioSelfTest(const QString& buildId = QStringLiteral("run1.aaaa1111"),
                                 const QString& version = QStringLiteral("1.4.0"))
{
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
            {QStringLiteral("version"), version},
            {QStringLiteral("buildId"), buildId},
            {QStringLiteral("gitCommit"), QString(40, QLatin1Char('a'))},
            {QStringLiteral("gitDirty"), false},
            {QStringLiteral("channel"), QStringLiteral("dev")},
            {QStringLiteral("buildType"), QStringLiteral("Debug")},
            {QStringLiteral("capabilitiesVersion"), 2},
            {QStringLiteral("editorSchemaVersion"), 2},
            {QStringLiteral("packSchemaVersion"), 1},
            {QStringLiteral("schemaSnapshotHash"), QString(64, QLatin1Char('b'))},
            {QStringLiteral("schemaArchiveOk"), true}};
}

}
