#pragma once

#include <QJsonObject>

namespace HarkenHubTest {

inline QJsonObject studioCapabilities(const QString& buildId, const QString& version)
{
    return {{QStringLiteral("studioVersion"), version},
            {QStringLiteral("buildId"), buildId},
            {QStringLiteral("capabilitiesVersion"), 2}};
}

inline QJsonObject studioVersionReport(const QString& buildId = QStringLiteral("run1.aaaa1111"),
                                       const QString& version = QStringLiteral("1.4.0"))
{
    return {{QStringLiteral("status"), QStringLiteral("ok")},
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("product"), QStringLiteral("HarkenStudio")},
                {QStringLiteral("version"), version},
                {QStringLiteral("buildId"), buildId},
                {QStringLiteral("channel"), QStringLiteral("dev")}}}};
}

}
