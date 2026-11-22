
#include "AuditionOverlay.h"

namespace harken::studio {

AuditionOverlay::AuditionOverlay(QObject* parent)
    : QObject(parent)
{
}

bool AuditionOverlay::setMuted(harken::studio::types::ObjectId stableId, bool muted)
{
    if (muted == muted_.contains(stableId))
        return false;
    if (muted)
        muted_.insert(stableId);
    else
        muted_.remove(stableId);
    emit auditionChanged(stableId);
    return true;
}

bool AuditionOverlay::setSoloed(harken::studio::types::ObjectId stableId, bool soloed)
{
    if (soloed == soloed_.contains(stableId))
        return false;
    if (soloed)
        soloed_.insert(stableId);
    else
        soloed_.remove(stableId);
    emit auditionChanged(stableId);
    return true;
}

void AuditionOverlay::clearAll()
{
    const QSet<harken::studio::types::ObjectId> touched = muted_ + soloed_;
    muted_.clear();
    soloed_.clear();
    for (harken::studio::types::ObjectId stableId : touched)
        emit auditionChanged(stableId);
}

}
