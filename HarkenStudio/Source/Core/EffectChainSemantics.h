#pragma once
#include "Utils/ObjectId.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

enum class EffectType : uint32_t
{
    FeedbackDelay = 0,
    ThreeBandTone = 1,
    Gain = 2,
    Limiter = 3,
    Compressor = 4
};

struct EffectEntry
{
    QUuid guid;
    harkenstudio::ObjectId runtimeId() const { return runtimeObjectId(guid); }
    EffectType type = EffectType::Gain;
    std::vector<float> parameters;
    bool bypass = false;
    QJsonObject extensions;
    QJsonValue opaqueValue;
    bool typed = true;

    QJsonValue toJson() const;
    static EffectEntry fromJson(const QJsonValue& value);
};

class EffectChainLedger
{
public:
    bool isEmpty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    const std::vector<EffectEntry>& entries() const { return entries_; }
    QJsonValue operator[](qsizetype index) const { return toJson().at(index); }
    EffectChainLedger& operator=(const QJsonArray& array)
    {
        *this = fromJson(array);
        return *this;
    }
    bool operator==(const EffectChainLedger& other) const
    {
        return toJson() == other.toJson();
    }
    bool operator==(const QJsonArray& other) const
    {
        return toJson() == other;
    }
    friend bool operator==(const QJsonArray& lhs, const EffectChainLedger& rhs)
    {
        return rhs == lhs;
    }

    bool replace(std::vector<EffectEntry> entries);
    QJsonArray toJson() const;
    static EffectChainLedger fromJson(const QJsonArray& array);

private:
    std::vector<EffectEntry> entries_;
};

namespace EffectChainSemantics {

QString typeName(EffectType type);
std::optional<EffectType> parseType(const QString& value);
int parameterCount(EffectType type);
std::vector<float> defaultParameters(EffectType type);
bool validate(const EffectEntry& entry, QString* diagnostic = nullptr);

}
}
