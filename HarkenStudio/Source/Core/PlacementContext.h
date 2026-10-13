#pragma once

#include <QUuid>

#include <vector>

namespace VNX {

struct PlacementOccurrence
{
    QUuid trackGuid;
    QUuid clipGuid;
    QUuid targetGuid;

    bool operator==(const PlacementOccurrence&) const = default;
};

class PlacementContext
{
public:
    PlacementContext() = default;
    explicit PlacementContext(std::vector<PlacementOccurrence> occurrences);

    static PlacementContext fromOccurrence(const PlacementOccurrence& occurrence);

    PlacementContext withNearestOccurrence(const PlacementOccurrence& occurrence) const;

    bool empty() const { return occurrences_.empty(); }
    const std::vector<PlacementOccurrence>& occurrences() const { return occurrences_; }

    bool operator==(const PlacementContext&) const = default;

private:
    std::vector<PlacementOccurrence> occurrences_;
};

}
