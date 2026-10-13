#pragma once

#include "AICommandCatalog.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace VNX {

class AIDispatchPolicy
{
public:
    struct Request
    {
        QString cmd;
        QJsonObject args;
        QString permissionProfile;
        QString toolProfile;
        QString requestId;
        QString grantId;
    };

    struct Decision
    {
        bool allowed = false;
        QString code;
        QString message;
    };

    using Clock = std::function<qint64()>;

    AIDispatchPolicy();
    explicit AIDispatchPolicy(const QList<AICommandSpec>& catalog);
    void setClockForTesting(Clock clock);
    QByteArray bindingFor(const Request& request) const;
    Decision evaluate(const Request& request, bool consumeGrant = true);
    QString issueGrant(const Request& request, int ttlMs = 300000);
    void revokeGrant(const QString& grantId);
    void revokeRequest(const QString& requestId);

private:
    struct Grant
    {
        QByteArray binding;
        QString requestId;
        qint64 expiresAtMs = 0;
    };

    struct ConsumedGrant
    {
        QByteArray binding;
        qint64 expiresAtMs = 0;
    };

    void pruneExpired(qint64 nowMs);
    qint64 nowMs() const;

    Clock clock_;
    QHash<QString, Grant> grants_;
    QHash<QString, ConsumedGrant> consumed_;
    QHash<QString, AICommandSpec> catalog_;
};

}
