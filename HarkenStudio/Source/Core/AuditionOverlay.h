
#pragma once

#include "Utils/ObjectId.h"
#include <QObject>
#include <QSet>

#include <cstdint>

namespace harken::studio {

class AuditionOverlay : public QObject
{
    Q_OBJECT

public:
    explicit AuditionOverlay(QObject* parent = nullptr);

    bool isMuted(harken::studio::types::ObjectId stableId) const { return muted_.contains(stableId); }
    bool isSoloed(harken::studio::types::ObjectId stableId) const { return soloed_.contains(stableId); }
    bool anySolo() const { return !soloed_.isEmpty(); }
    const QSet<harken::studio::types::ObjectId>& mutedIds() const { return muted_; }
    const QSet<harken::studio::types::ObjectId>& soloedIds() const { return soloed_; }

    bool setMuted(harken::studio::types::ObjectId stableId, bool muted);
    bool setSoloed(harken::studio::types::ObjectId stableId, bool soloed);
    void clearAll();

signals:
    void auditionChanged(harken::studio::types::ObjectId stableId);

private:
    QSet<harken::studio::types::ObjectId> muted_;
    QSet<harken::studio::types::ObjectId> soloed_;
};

}
