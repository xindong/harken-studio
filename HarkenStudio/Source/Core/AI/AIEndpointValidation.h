#pragma once

#include <QString>
#include <QUrl>

namespace VNX {

inline bool isValidAIEndpoint(const QString& baseUrl, const QString& model,
                              QUrl* acceptedEndpoint = nullptr)
{
    const QUrl endpoint(baseUrl);
    const bool accepted = !baseUrl.isEmpty() && endpoint.isValid() && !endpoint.isRelative()
        && endpoint.userInfo().isEmpty()
        && endpoint.scheme() == QLatin1String("https")
        && !endpoint.host().isEmpty() && !model.isEmpty();
    if (accepted && acceptedEndpoint)
        *acceptedEndpoint = endpoint;
    return accepted;
}

}
