#include <harken_studio_build/BindingTransition.h>
#include "BindingSemantics.h"

#include <algorithm>
#include <cmath>

namespace VNX {

namespace {

QJsonArray pointsToJson(const QVector<CurvePointDef>& points)
{
    QJsonArray array;
    for (const CurvePointDef& point : points)
        array.append(QJsonArray{point.x, point.value});
    return array;
}

QJsonArray handlesToJson(const QVector<CurveHandlePairDef>& handles)
{
    QJsonArray array;
    for (const CurveHandlePairDef& handle : handles)
        array.append(QJsonArray{handle.h1x, handle.h1y,
                                handle.h2x, handle.h2y});
    return array;
}

QVector<CurvePointDef> pointsFromJson(const QJsonArray& array)
{
    QVector<CurvePointDef> points;
    for (const QJsonValue& value : array) {
        const QJsonArray point = value.toArray();
        if (point.size() < 2) continue;
        points.push_back({static_cast<float>(point.at(0).toDouble()),
                          static_cast<float>(point.at(1).toDouble())});
    }
    return points;
}

QVector<CurveHandlePairDef> handlesFromJson(const QJsonArray& array)
{
    QVector<CurveHandlePairDef> handles;
    for (const QJsonValue& value : array) {
        const QJsonArray handle = value.toArray();
        if (handle.size() < 4) {
            handles.clear();
            break;
        }
        handles.push_back({static_cast<float>(handle.at(0).toDouble()),
                           static_cast<float>(handle.at(1).toDouble()),
                           static_cast<float>(handle.at(2).toDouble()),
                           static_cast<float>(handle.at(3).toDouble())});
    }
    return handles;
}

float remapValue(float value, float oldMin, float oldMax,
                 float newMin, float newMax)
{
    if (oldMin == oldMax) return newMin;
    const float t = (value - oldMin) / (oldMax - oldMin);
    return newMin + t * (newMax - newMin);
}

bool validTypedBinding(const ParameterBindingDef& binding)
{
    return binding.typed && !binding.variable.isNull()
        && !harkenstudio::IsNull(binding.runtimeId())
        && binding.address.slotId != 0
        && binding.curve.size() >= 2
        && binding.curve.size() <= kCurveMaxPoints;
}

ProjectItemType hostTypeOf(BindingHostClass hostClass)
{
    return hostClass == BindingHostClass::Bus
        ? ProjectItemType::Bus : ProjectItemType::Sound;
}

bool isBindableAddress(const BindingAddress& address)
{
    return BindingContractSemantics::isBindable(
        BindingContractSemantics::derive(address.slotId, hostTypeOf(address.hostClass)));
}

void normalizeTypedBinding(ParameterBindingDef& binding)
{
    const auto contract = BindingContractSemantics::derive(
        binding.address.slotId, hostTypeOf(binding.address.hostClass));
    if (contract.outputUnit == BindingOutputUnit::Cent) {
        const bool reversed = binding.outputMin > binding.outputMax;
        binding.outputMin = reversed ? contract.legalOutputRange.max : contract.legalOutputRange.min;
        binding.outputMax = reversed ? contract.legalOutputRange.min : contract.legalOutputRange.max;
    }
    for (CurvePointDef& point : binding.curve)
        point.x = std::clamp(point.x, 0.0f, 1.0f);
    std::sort(binding.curve.begin(), binding.curve.end(),
        [](const CurvePointDef& a, const CurvePointDef& b) {
            return a.x < b.x;
        });
    binding.curve.front().x = 0.0f;
    binding.curve.back().x = 1.0f;
    float yMin = std::min(binding.outputMin, binding.outputMax);
    float yMax = std::max(binding.outputMin, binding.outputMax);
    if (contract.outputUnit == BindingOutputUnit::Cent) {
        for (const auto& point : binding.curve) {
            yMin = std::min(yMin, point.value);
            yMax = std::max(yMax, point.value);
        }
        for (const auto& handle : binding.handles) {
            yMin = std::min({yMin, handle.h1y, handle.h2y});
            yMax = std::max({yMax, handle.h1y, handle.h2y});
        }
    }
    curveNormalize(yMin, yMax, binding.curve, binding.handles);
}

}

QJsonValue ParameterBindingDef::toJson() const
{
    if (!typed) return opaqueValue;
    QJsonObject obj = extensions;
    obj[QStringLiteral("guid")] = guid.toString(QUuid::WithoutBraces);
    QJsonObject addressObject = addressExtensions;
    const QJsonObject knownAddress = BindingSemantics::addressToJson(address);
    for (auto it = knownAddress.begin(); it != knownAddress.end(); ++it)
        addressObject.insert(it.key(), it.value());
    obj[QStringLiteral("address")] = addressObject;
    obj[QStringLiteral("variable")] = variable.toString(QUuid::WithoutBraces);
    obj[QStringLiteral("range")] = QJsonArray{outputMin, outputMax};
    obj[QStringLiteral("transitionMs")] = static_cast<qint64>(transitionMs);
    QJsonObject curveObject = curveExtensions;
    curveObject[QStringLiteral("points")] = pointsToJson(curve);
    curveObject[QStringLiteral("handles")] = handlesToJson(handles);
    obj[QStringLiteral("curve")] = curveObject;
    return obj;
}

ParameterBindingDef ParameterBindingDef::fromJson(const QJsonValue& value)
{
    ParameterBindingDef binding;
    if (!value.isObject()) {
        binding.typed = false;
        binding.opaqueValue = value;
        return binding;
    }
    const QJsonObject obj = value.toObject();
    const auto address = BindingSemantics::parseAddress(obj);
    const QUuid variable = QUuid::fromString(
        obj.value(QStringLiteral("variable")).toString());
    if (!address || variable.isNull()) {
        binding.typed = false;
        binding.opaqueValue = value;
        return binding;
    }

    binding.guid = QUuid::fromString(obj.value(QStringLiteral("guid")).toString());
    binding.address = *address;
    binding.addressExtensions = obj.value(QStringLiteral("address")).toObject();
    binding.addressExtensions.remove(QStringLiteral("hostClass"));
    binding.addressExtensions.remove(QStringLiteral("slotId"));
    binding.addressExtensions.remove(QStringLiteral("path"));
    binding.variable = variable;
    const QJsonArray range = obj.value(QStringLiteral("range")).toArray();
    const QJsonObject curve = obj.value(QStringLiteral("curve")).toObject();
    const QVector<CurvePointDef> points = pointsFromJson(
        curve.value(QStringLiteral("points")).toArray());
    if (range.size() < 2 || points.size() < 2 || points.size() > kCurveMaxPoints) {
        binding.typed = false;
        binding.opaqueValue = value;
        return binding;
    }
    binding.outputMin = static_cast<float>(range.at(0).toDouble());
    binding.outputMax = static_cast<float>(range.at(1).toDouble());
    binding.curve = points;
    binding.handles = handlesFromJson(curve.value(QStringLiteral("handles")).toArray());
    if (!obj.contains(QStringLiteral("transitionMs"))) {
        binding.transitionMs = BindingContractSemantics::defaultTransitionMs(binding.address);
    } else {
        const QJsonValue raw = obj.value(QStringLiteral("transitionMs"));
        if (!raw.isDouble()) {
            binding.typed = false;
            binding.opaqueValue = value;
            return binding;
        }
        const double ms = raw.toDouble();
        if (!std::isfinite(ms) || std::trunc(ms) != ms || ms < 0.0 || ms > harkenstudiobuild::kMaxBindingTransitionMs) {
            binding.typed = false;
            binding.opaqueValue = value;
            return binding;
        }
        binding.transitionMs = static_cast<uint32_t>(ms);
    }
    binding.curveExtensions = curve;
    binding.curveExtensions.remove(QStringLiteral("points"));
    binding.curveExtensions.remove(QStringLiteral("handles"));
    binding.extensions = obj;
    binding.extensions.remove(QStringLiteral("guid"));
    binding.extensions.remove(QStringLiteral("address"));
    binding.extensions.remove(QStringLiteral("variable"));
    binding.extensions.remove(QStringLiteral("range"));
    binding.extensions.remove(QStringLiteral("curve"));
    binding.extensions.remove(QStringLiteral("transitionMs"));
    normalizeTypedBinding(binding);
    return binding;
}

const ParameterBindingDef* BindingLedger::find(harkenstudio::ObjectId runtimeStableId) const
{
    if (harkenstudio::IsNull(runtimeStableId)) return nullptr;
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [runtimeStableId](const ParameterBindingDef& binding) {
            return binding.typed && binding.runtimeId() == runtimeStableId;
        });
    return it == entries_.end() ? nullptr : &*it;
}

ParameterBindingDef* BindingLedger::find(harkenstudio::ObjectId runtimeStableId)
{
    return const_cast<ParameterBindingDef*>(
        static_cast<const BindingLedger*>(this)->find(runtimeStableId));
}

const ParameterBindingDef* BindingLedger::findFirst(const BindingAddress& address) const
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [&address](const ParameterBindingDef& binding) {
            return binding.typed && binding.address == address;
        });
    return it == entries_.end() ? nullptr : &*it;
}

ParameterBindingDef* BindingLedger::findFirst(const BindingAddress& address)
{
    return const_cast<ParameterBindingDef*>(
        static_cast<const BindingLedger*>(this)->findFirst(address));
}

std::vector<const ParameterBindingDef*> BindingLedger::findAll(
    const BindingAddress& address) const
{
    std::vector<const ParameterBindingDef*> matched;
    for (const ParameterBindingDef& binding : entries_)
        if (binding.typed && binding.address == address)
            matched.push_back(&binding);
    return matched;
}

bool BindingLedger::upsert(ParameterBindingDef binding)
{
    if (!validTypedBinding(binding)) return false;
    if (!isBindableAddress(binding.address)) return false;
    for (const ParameterBindingDef& entry : entries_) {
        if (!entry.typed) continue;
        if (entry.address.hostClass != binding.address.hostClass)
            return false;
    }
    normalizeTypedBinding(binding);
    if (!harkenstudio::IsNull(binding.runtimeId()))
        if (ParameterBindingDef* current = find(binding.runtimeId())) {
            *current = std::move(binding);
            return true;
        }
    entries_.push_back(std::move(binding));
    return true;
}

bool BindingLedger::remove(harkenstudio::ObjectId runtimeStableId)
{
    if (harkenstudio::IsNull(runtimeStableId)) return false;
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [runtimeStableId](const ParameterBindingDef& binding) {
            return binding.typed && binding.runtimeId() == runtimeStableId;
        });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

bool BindingLedger::removeAll(const BindingAddress& address)
{
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&address](const ParameterBindingDef& binding) {
            return binding.typed && binding.address == address;
        }), entries_.end());
    return entries_.size() != before;
}

bool BindingLedger::move(harkenstudio::ObjectId runtimeStableId, int toIndex)
{
    if (harkenstudio::IsNull(runtimeStableId) || toIndex < 0
        || toIndex >= static_cast<int>(entries_.size()))
        return false;
    const auto it = std::find_if(entries_.begin(), entries_.end(),
        [runtimeStableId](const ParameterBindingDef& binding) {
            return binding.typed && binding.runtimeId() == runtimeStableId;
        });
    if (it == entries_.end()) return false;
    const int from = static_cast<int>(std::distance(entries_.begin(), it));
    if (from == toIndex) return false;
    ParameterBindingDef moved = std::move(*it);
    entries_.erase(it);
    entries_.insert(entries_.begin() + toIndex, std::move(moved));
    return true;
}

bool BindingLedger::reorder(const BindingAddress& address, int fromLocal, int toLocal)
{
    std::vector<size_t> indices;
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].typed && entries_[i].address == address)
            indices.push_back(i);
    if (fromLocal < 0 || toLocal < 0
        || fromLocal >= static_cast<int>(indices.size())
        || toLocal >= static_cast<int>(indices.size())
        || fromLocal == toLocal)
        return false;
    ParameterBindingDef moved = std::move(entries_[indices[static_cast<size_t>(fromLocal)]]);
    if (fromLocal < toLocal) {
        for (int i = fromLocal; i < toLocal; ++i)
            entries_[indices[static_cast<size_t>(i)]] =
                std::move(entries_[indices[static_cast<size_t>(i + 1)]]);
    } else {
        for (int i = fromLocal; i > toLocal; --i)
            entries_[indices[static_cast<size_t>(i)]] =
                std::move(entries_[indices[static_cast<size_t>(i - 1)]]);
    }
    entries_[indices[static_cast<size_t>(toLocal)]] = std::move(moved);
    return true;
}

bool BindingLedger::replace(std::vector<ParameterBindingDef> entries)
{
    std::optional<BindingHostClass> hostClass;
    for (ParameterBindingDef& binding : entries) {
        if (!binding.typed) continue;
        if (!validTypedBinding(binding)) return false;
        if (hostClass && *hostClass != binding.address.hostClass) return false;
        hostClass = binding.address.hostClass;
        normalizeTypedBinding(binding);
    }
    entries_ = std::move(entries);
    return true;
}

QJsonArray BindingLedger::toJson() const
{
    QJsonArray array;
    for (const ParameterBindingDef& binding : entries_)
        array.append(binding.toJson());
    return array;
}

BindingLedger BindingLedger::fromJson(const QJsonArray& array)
{
    BindingLedger ledger;
    for (const QJsonValue& value : array)
        ledger.entries_.push_back(ParameterBindingDef::fromJson(value));
    return ledger;
}

namespace BindingSemantics {

QString slotName(uint8_t slotId)
{
    const auto* meta = BindingContractSemantics::playableMeta(slotId);
    return meta ? QString::fromUtf8(meta->name) : QString();
}

BindingAddress volumeAddress(ProjectItemType hostType)
{
    return BindingContractSemantics::addressOf(
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Volume),
        hostType);
}

BindingAddress pitchAddress(ProjectItemType hostType)
{
    return BindingContractSemantics::addressOf(
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Pitch),
        hostType);
}

QString addressKey(const BindingAddress& address)
{
    QString key = BindingContractSemantics::hostClassName(address.hostClass)
        + QLatin1Char('.')
        + QString::number(address.slotId);
    if (address.path)
        key += QLatin1Char('.') + QString::number(address.path->index);
    return key;
}

QJsonObject addressToJson(const BindingAddress& address)
{
    QJsonObject object;
    object[QStringLiteral("hostClass")] =
        BindingContractSemantics::hostClassName(address.hostClass);
    object[QStringLiteral("slotId")] = address.slotId;
    if (address.path)
        object[QStringLiteral("path")] = static_cast<int>(address.path->index);
    return object;
}

std::optional<BindingAddress> parseAddress(const QJsonObject& object)
{
    const QJsonObject addressObject = object.value(QStringLiteral("address")).toObject();
    if (addressObject.isEmpty()) return std::nullopt;
    const auto hostClass = BindingContractSemantics::parseHostClass(
        addressObject.value(QStringLiteral("hostClass")).toString());
    const int slotId = addressObject.value(QStringLiteral("slotId")).toInt();
    if (!hostClass || slotId < 1 || slotId > 16) return std::nullopt;
    BindingAddress address;
    address.hostClass = *hostClass;
    address.slotId = static_cast<uint8_t>(slotId);
    if (addressObject.contains(QStringLiteral("path"))) {
        const int index = addressObject.value(QStringLiteral("path")).toInt(-1);
        if (index < 0) return std::nullopt;
        address.path = BindingPath{static_cast<uint32_t>(index)};
    }
    return address;
}

QString targetUnit(const BindingContract& contract)
{
    return BindingContractSemantics::outputUnitName(contract.outputUnit);
}

QString targetUnit(const BindingAddress& address)
{
    const ProjectItemType hostType = address.hostClass == BindingHostClass::Bus
        ? ProjectItemType::Bus : ProjectItemType::Sound;
    return targetUnit(BindingContractSemantics::derive(address.slotId, hostType));
}

ParameterBindingDef defaultBinding(const BindingAddress& address, const QUuid& variable)
{
    const ProjectItemType hostType = address.hostClass == BindingHostClass::Bus
        ? ProjectItemType::Bus : ProjectItemType::Sound;
    const BindingContract contract =
        BindingContractSemantics::derive(address.slotId, hostType);
    ParameterBindingDef binding;
    binding.address = address;
    binding.variable = variable;
    binding.outputMin = contract.defaultOutputRange.min;
    binding.outputMax = contract.defaultOutputRange.max;
    binding.transitionMs = contract.defaultTransitionMs;
    binding.curve = {{0.0f, binding.outputMin}, {1.0f, binding.outputMax}};
    if (address == volumeAddress(hostType)) {
        binding.curve = {{0.0f, 0.0f}, {1.0f, -96.0f}};
    }
    if (address == pitchAddress(hostType))
        binding.curve = {{0.0f, 0.0f}, {1.0f, 0.0f}};
    normalizeTypedBinding(binding);
    return binding;
}

void remapOutputDomain(ParameterBindingDef& binding, float outputMin, float outputMax)
{
    const float oldMin = binding.outputMin;
    const float oldMax = binding.outputMax;
    for (CurvePointDef& point : binding.curve)
        point.value = remapValue(point.value, oldMin, oldMax, outputMin, outputMax);
    for (CurveHandlePairDef& handle : binding.handles) {
        handle.h1y = remapValue(handle.h1y, oldMin, oldMax, outputMin, outputMax);
        handle.h2y = remapValue(handle.h2y, oldMin, oldMax, outputMin, outputMax);
    }
    binding.outputMin = outputMin;
    binding.outputMax = outputMax;
    curveNormalize(std::min(outputMin, outputMax), std::max(outputMin, outputMax),
                   binding.curve, binding.handles);
}

float contribution(const ParameterBindingDef& binding, VariableKind kind,
                   float variableValue, float inputMin, float inputMax)
{
    const float x = kind == VariableKind::Step
        ? static_cast<float>(VariableSemantics::discreteRead(variableValue))
        : variableValue;
    const float normalized = inputMax == inputMin
        ? 0.0f : (x - inputMin) / (inputMax - inputMin);
    const ProjectItemType hostType = hostTypeOf(binding.address.hostClass);
    const auto contract = BindingContractSemantics::derive(binding.address.slotId, hostType);
    return curveBezierValueAt(binding.curve, binding.handles, normalized,
        contract.outputUnit == BindingOutputUnit::Decibel
            ? CurveAxisScale::MirroredAmplitudeDb : CurveAxisScale::LinearValue);
}

float combineContributions(BindingCombine combine, float accumulated, float contribution)
{
    if (combine == BindingCombine::Replace)
        return contribution;
    return accumulated + contribution;
}

float combineWithBase(BindingCombine combine, float base, float contribution)
{
    if (combine == BindingCombine::Replace)
        return contribution;
    return base + contribution;
}

DisplayDomain displayDomain(float rangeMin, float rangeMax)
{
    DisplayDomain domain;
    domain.min = rangeMin;
    domain.max = rangeMax;
    domain.degenerate = !(rangeMax > rangeMin);
    return domain;
}

float storedToDisplayX(float stored, const DisplayDomain& domain)
{
    if (domain.degenerate) return domain.min;
    return domain.min + stored * (domain.max - domain.min);
}

float displayToStoredX(float display, const DisplayDomain& domain)
{
    if (domain.degenerate) return 0.0f;
    return (display - domain.min) / (domain.max - domain.min);
}

QVector<CurvePointDef> storedToDisplay(
    const QVector<CurvePointDef>& points, const DisplayDomain& domain)
{
    QVector<CurvePointDef> mapped = points;
    for (CurvePointDef& point : mapped)
        point.x = storedToDisplayX(point.x, domain);
    return mapped;
}

QVector<CurveHandlePairDef> storedToDisplay(
    const QVector<CurveHandlePairDef>& handles, const DisplayDomain& domain)
{
    QVector<CurveHandlePairDef> mapped = handles;
    for (CurveHandlePairDef& handle : mapped) {
        handle.h1x = storedToDisplayX(handle.h1x, domain);
        handle.h2x = storedToDisplayX(handle.h2x, domain);
    }
    return mapped;
}

QVector<CurvePointDef> displayToStored(
    const QVector<CurvePointDef>& points, const DisplayDomain& domain)
{
    QVector<CurvePointDef> mapped = points;
    for (CurvePointDef& point : mapped)
        point.x = displayToStoredX(point.x, domain);
    return mapped;
}

QVector<CurveHandlePairDef> displayToStored(
    const QVector<CurveHandlePairDef>& handles, const DisplayDomain& domain)
{
    QVector<CurveHandlePairDef> mapped = handles;
    for (CurveHandlePairDef& handle : mapped) {
        handle.h1x = displayToStoredX(handle.h1x, domain);
        handle.h2x = displayToStoredX(handle.h2x, domain);
    }
    return mapped;
}

QString axisTickLabel(float displayX, const std::vector<VariableLabel>& labels)
{
    if (const auto named = VariableSemantics::labelTextForValue(displayX, labels))
        return *named;
    if (std::floor(displayX) == displayX)
        return QString::number(static_cast<int>(displayX));
    return QString::number(static_cast<double>(displayX), 'f', 1);
}

}

}
