
#pragma once

#include "Utils/ObjectId.h"
#include <QObject>
#include <QSet>

#include <cstdint>

namespace VNX {

class AuditionOverlay : public QObject
{
    Q_OBJECT

public:
    explicit AuditionOverlay(QObject* parent = nullptr);

    bool isMuted(harkenstudio::ObjectId stableId) const { return muted_.contains(stableId); }
    bool isSoloed(harkenstudio::ObjectId stableId) const { return soloed_.contains(stableId); }
    bool anySolo() const { return !soloed_.isEmpty(); }
    const QSet<harkenstudio::ObjectId>& mutedIds() const { return muted_; }
    const QSet<harkenstudio::ObjectId>& soloedIds() const { return soloed_; }

    bool setMuted(harkenstudio::ObjectId stableId, bool muted);
    bool setSoloed(harkenstudio::ObjectId stableId, bool soloed);
    void clearAll();

signals:
    void auditionChanged(harkenstudio::ObjectId stableId);

private:
    QSet<harkenstudio::ObjectId> muted_;
    QSet<harkenstudio::ObjectId> soloed_;
};

}
