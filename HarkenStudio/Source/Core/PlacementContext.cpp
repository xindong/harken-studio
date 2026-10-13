#include "PlacementContext.h"

#include <utility>

namespace VNX {

PlacementContext::PlacementContext(std::vector<PlacementOccurrence> occurrences)
    : occurrences_(std::move(occurrences))
{
}

PlacementContext PlacementContext::fromOccurrence(
    const PlacementOccurrence& occurrence)
{
    return PlacementContext({occurrence});
}

PlacementContext PlacementContext::withNearestOccurrence(
    const PlacementOccurrence& occurrence) const
{
    std::vector<PlacementOccurrence> occurrences;
    occurrences.reserve(occurrences_.size() + 1);
    occurrences.push_back(occurrence);
    occurrences.insert(occurrences.end(), occurrences_.begin(), occurrences_.end());
    return PlacementContext(std::move(occurrences));
}

}
