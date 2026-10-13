#pragma once

#include "ProjectPlatform.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

struct ProjectItem;
enum class SelectionMode : uint8_t;

enum class RandomDiscipline : uint8_t;
enum class SelectionMemoryScope : uint8_t;
struct SelectionBehaviorDef;
class VSelection;

namespace ContainerBehaviorSemantics {

constexpr int kDefaultChildWeight = 50;
constexpr int kMinDrawChildWeight = 0;
constexpr int kMinOrderChildWeight = 1;
constexpr int kMaxChildWeight = 100;

struct Visibility
{
    bool discipline = false;
    bool childWeights = false;
    bool avoidRepeat = false;
    bool memoryScope = false;
    bool resumeFromLast = false;
};

struct StepResult
{
    std::optional<uint32_t> child;
    bool roundCompleted = false;
};

struct Memory
{
    std::vector<uint32_t> history;
    std::vector<uint32_t> order;
    int cursor = 0;
};

Visibility visibilityFor(const VSelection& selection);
QString childWeightLabel(const VSelection& selection);
int effectiveChildWeight(const VSelection& selection,
                         uint32_t stableOccurrenceOrdinal,
                         PlatformScope scope = std::nullopt);
int minimumChildWeight(const VSelection& selection);
int maxAvoidRepeat(const VSelection& selection,
                   const QList<uint32_t>& occurrenceOrdinals,
                   PlatformScope scope = std::nullopt);
int effectiveAvoidRepeat(const VSelection& selection, PlatformScope scope = std::nullopt);
bool validateSelections(const VSelection& selection, QString* error = nullptr);
QJsonObject toJson(const SelectionBehaviorDef& behavior);
std::optional<SelectionBehaviorDef> fromJson(const QJsonObject& json,
                                             QString* error = nullptr);
StepResult advance(const VSelection& selection,
                   const QList<uint32_t>& occurrenceOrdinals,
                   uint64_t entropy,
                   Memory& memory, PlatformScope scope = std::nullopt);

}
}
