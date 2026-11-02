#pragma once

#include "BindingContract.h"
#include "CurveSemantics.h"
#include "VariableSemantics.h"
#include "Utils/ObjectId.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUuid>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

struct ParameterBindingDef
{
    QUuid guid;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(guid); }
    BindingAddress address;
    QUuid variable;
    QVector<CurvePointDef> curve;
    QVector<CurveHandlePairDef> handles;
    float outputMin = 0.0f;
    float outputMax = 0.0f;
    uint32_t transitionMs = 0;
    QJsonObject extensions;
    QJsonObject addressExtensions;
    QJsonObject curveExtensions;
    QJsonValue opaqueValue;
    bool typed = true;

    QJsonValue toJson() const;
    static ParameterBindingDef fromJson(const QJsonValue& value);
};

class BindingLedger
{
public:
    bool isEmpty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }

    const std::vector<ParameterBindingDef>& entries() const { return entries_; }

    const ParameterBindingDef* find(harkenstudio::ObjectId runtimeStableId) const;
    ParameterBindingDef* find(harkenstudio::ObjectId runtimeStableId);
    const ParameterBindingDef* findFirst(const BindingAddress& address) const;
    ParameterBindingDef* findFirst(const BindingAddress& address);
    std::vector<const ParameterBindingDef*> findAll(const BindingAddress& address) const;

    bool upsert(ParameterBindingDef binding);
    bool remove(harkenstudio::ObjectId runtimeStableId);
    bool removeAll(const BindingAddress& address);
    bool move(harkenstudio::ObjectId runtimeStableId, int toIndex);
    bool reorder(const BindingAddress& address, int fromLocal, int toLocal);
    bool replace(std::vector<ParameterBindingDef> entries);

    QJsonArray toJson() const;
    static BindingLedger fromJson(const QJsonArray& array);

private:
    std::vector<ParameterBindingDef> entries_;
};

namespace BindingSemantics {

QString slotName(uint8_t slotId);
QString slotName(const BindingAddress& address);
BindingAddress volumeAddress(ProjectItemType hostType = ProjectItemType::Sound);
BindingAddress pitchAddress(ProjectItemType hostType = ProjectItemType::Sound);
QString addressKey(const BindingAddress& address);
QJsonObject addressToJson(const BindingAddress& address);
std::optional<BindingAddress> parseAddress(const QJsonObject& object);
QString targetUnit(const BindingAddress& address);
QString targetUnit(const BindingContract& contract);

ParameterBindingDef defaultBinding(const BindingAddress& address, const QUuid& variable);

void remapOutputDomain(ParameterBindingDef& binding, float outputMin, float outputMax);

float contribution(const ParameterBindingDef& binding, VariableKind kind,
                   float variableValue, float inputMin, float inputMax);

float combineContributions(BindingCombine combine, float accumulated, float contribution);
float combineWithBase(BindingCombine combine, float base, float contribution);

struct DisplayDomain
{
    float min = 0.0f;
    float max = 0.0f;
    bool degenerate = false;
};

DisplayDomain displayDomain(float rangeMin, float rangeMax);

float storedToDisplayX(float stored, const DisplayDomain& domain);
float displayToStoredX(float display, const DisplayDomain& domain);

QVector<CurvePointDef> storedToDisplay(
    const QVector<CurvePointDef>& points, const DisplayDomain& domain);
QVector<CurveHandlePairDef> storedToDisplay(
    const QVector<CurveHandlePairDef>& handles, const DisplayDomain& domain);
QVector<CurvePointDef> displayToStored(
    const QVector<CurvePointDef>& points, const DisplayDomain& domain);
QVector<CurveHandlePairDef> displayToStored(
    const QVector<CurveHandlePairDef>& handles, const DisplayDomain& domain);

QString axisTickLabel(float displayX, const std::vector<VariableLabel>& labels);

}

}
