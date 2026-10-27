
#pragma once

#include "Component.h"

#include <QString>
#include <QVariant>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cstdint>
#include <functional>
#include <optional>
#include <type_traits>
#include <vector>

namespace VNX {

enum class PropType : uint8_t { Float, Int, Bool, Enum, Color, ObjectRef, String, List, Range, Struct };
enum class ListPresentation : uint8_t { Configuration, Display };

enum class PropertyCategory : uint8_t { Basic, Route, Spatial, Limitation };

enum class PropertySection : uint8_t {
    None, Playback, Identity, Advanced, Source, Selection, Step, Dimension,
    Label, Clip, Custom, MainOutput, Input, Send, Receive, Rating, PlaybackPriority
};

struct EnumOption { QString text; QVariant value; };

// One segment of a segmented row. A fixed segment writes `value` when clicked
// and reads as selected while the field holds it. A numeric segment owns a
// sub-range of the field's domain instead of a single value: it shows whatever
// the field currently holds, is editable in place, and `value` is the count it
// falls back to when the field sits on another segment.
struct SegmentSpec
{
    QString  text;
    QVariant value;
    bool     numeric = false;
    QVariant numericMin, numericMax;
};

// Editing domain of a numeric field when the domain is not a constant of the
// schema but a function of the owning component's other fields (a Variable's
// declared range and kind govern its default value's domain and step form).
// The engine re-resolves it on bind, on refresh and after every commit.
struct FieldDomain
{
    double min = 0.0;
    double max = 1.0;
    int    decimals = 2;
    double quantum = 0.0;
    std::function<QString(double)> annotation;
    bool discreteSelection = false;
};

struct PropFlags
{
    bool curve       = false;
    bool sourceIndicator = false;
    bool readOnly    = false;
    bool advanced    = false;
    bool multiline   = false;
    bool serialize   = true;
    bool settableByName = true;
    bool segmented   = false;
    bool state       = false;
};

struct PropertyDescriptor
{
    QString  field;
    QString  jsonKey;
    std::optional<ComponentType> ownerComponent;
    PropType type = PropType::Float;
    QString  label;
    PropertySection section = PropertySection::None;
    QString  unit;
    QString  specialValueText;
    QVariant min, max, def;
    int      decimals = 1;
    double   displayScale = 1.0;
    PropFlags flags;
    std::vector<EnumOption> options;
    std::vector<SegmentSpec> segments;
    std::vector<PropertyDescriptor> elementSchema;
    ListPresentation listPresentation = ListPresentation::Configuration;
    QString  editCondition;
    std::vector<QString> aliases;
    int      refFilter = 0;

    std::function<QVariant(const IComponent*)>        get;
    std::function<bool(IComponent*, const QVariant&)> set;
    std::function<QVariant(const QVariant&)>          coerce;
    std::function<bool(const IComponent*)>            serializeWhen;
    std::function<bool(uint32_t, uint32_t)>           refSelectable;
    std::function<QVariant(const ProjectItem*)>        getItem;
    std::function<std::optional<FieldDomain>(const IComponent*)> domain;
    std::function<QString(const QVariantList&, int, const QString&)> validateElement;
};

using PropertySchema = std::vector<PropertyDescriptor>;

template <class C>
PropertySchema withOwner(PropertySchema schema)
{
    for (auto& d : schema)
        d.ownerComponent = C::kType;
    return schema;
}

inline QJsonValue propToJsonValue(const QVariant& v, PropType t)
{
    switch (t)
    {
        case PropType::Bool:   return QJsonValue(v.toBool());
        case PropType::Int:
        case PropType::Enum:   return QJsonValue(v.toInt());
        case PropType::String: return QJsonValue(v.toString());
        case PropType::Struct: return QJsonValue::fromVariant(v);
        case PropType::Float:
        default:               return QJsonValue(v.toDouble());
    }
}

inline QVariant propFromJsonValue(const QJsonObject& j, const QString& key, PropType t, const QVariant& def)
{
    const QJsonValue jv = j.value(key);
    switch (t)
    {
        case PropType::Bool:   return jv.toBool(def.toBool());
        case PropType::Int:
        case PropType::Enum:   return jv.toInt(def.toInt());
        case PropType::String: return jv.toString(def.toString());
        case PropType::Struct: return jv.isObject() ? jv.toVariant() : def;
        case PropType::Float:
        default:               return jv.toDouble(def.toDouble());
    }
}

inline QJsonValue propToJsonValue(const QVariant& v, const PropertyDescriptor& d)
{
    if (d.type != PropType::List)
        return propToJsonValue(v, d.type);

    QJsonArray arr;
    for (const QVariant& elem : v.toList())
    {
        const QVariantMap em = elem.toMap();
        QJsonObject eo;
        for (const auto& ed : d.elementSchema)
        {
            const QString key = ed.jsonKey.isEmpty() ? ed.field : ed.jsonKey;
            eo[key] = propToJsonValue(em.value(ed.field, ed.def), ed.type);
        }
        arr.append(eo);
    }
    return arr;
}

inline QVariant propFromJsonValue(const QJsonObject& j, const QString& key, const PropertyDescriptor& d)
{
    if (d.type != PropType::List)
        return propFromJsonValue(j, key, d.type, d.def);

    QVariantList list;
    for (const QJsonValue& e : j.value(key).toArray())
    {
        const QJsonObject eo = e.toObject();
        QVariantMap em;
        for (const auto& ed : d.elementSchema)
        {
            const QString ekey = ed.jsonKey.isEmpty() ? ed.field : ed.jsonKey;
            em[ed.field] = propFromJsonValue(eo, ekey, ed.type, ed.def);
        }
        list.append(em);
    }
    return QVariant(list);
}

template <class Derived>
class ReflectedComponent : public IComponent
{
public:
    QVariant getField(const QString& f) const override
    {
        for (const auto& d : Derived::schema())
            if (matches(d, f))
                return d.get ? d.get(this) : QVariant{};
        return {};
    }

    bool setField(const QString& f, const QVariant& v) override
    {
        for (const auto& d : Derived::schema())
            if (matches(d, f) && d.flags.settableByName && d.set)
                return d.set(this, d.coerce ? d.coerce(v) : v);
        return false;
    }

    QJsonObject toJson() const override
    {
        return toJsonImpl(nullptr);
    }

    QJsonObject toJsonExcluding(const QSet<QString>& excluded) const override
    {
        if constexpr (!std::is_same_v<decltype(&Derived::toJson),
                                      decltype(&ReflectedComponent<Derived>::toJson)>)
            return IComponent::toJsonExcluding(excluded);
        return toJsonImpl(&excluded);
    }

    bool fromJson(const QJsonObject& j) override
    {
        for (const auto& d : Derived::schema())
        {
            if (!d.flags.serialize || !d.set) continue;
            const QString key = d.jsonKey.isEmpty() ? d.field : d.jsonKey;
            d.set(this, propFromJsonValue(j, key, d));
        }
        static_cast<Derived*>(this)->fromJsonExtra(j);
        return true;
    }

    virtual void toJsonExtra(QJsonObject&) const {}
    virtual void fromJsonExtra(const QJsonObject&) {}

private:
    QJsonObject toJsonImpl(const QSet<QString>* excluded) const
    {
        QJsonObject j;
        for (const auto& d : Derived::schema())
        {
            if (!d.flags.serialize) continue;
            if (excluded && excluded->contains(d.jsonKey.isEmpty() ? d.field : d.jsonKey)) continue;
            if (d.serializeWhen && !d.serializeWhen(this)) continue;
            const QString key = d.jsonKey.isEmpty() ? d.field : d.jsonKey;
            j[key] = propToJsonValue(d.get(this), d);
        }
        static_cast<const Derived*>(this)->toJsonExtra(j);
        if (excluded)
            for (const auto& key : *excluded) j.remove(key);
        return j;
    }

    static bool matches(const PropertyDescriptor& d, const QString& f)
    {
        if (d.field == f) return true;
        for (const auto& a : d.aliases)
            if (a == f) return true;
        return false;
    }
};

#define VNX_PROP_GET(Comp, member) \
    [](const VNX::IComponent* c) -> QVariant { return QVariant(static_cast<const Comp*>(c)->member); }
#define VNX_PROP_GETI(Comp, member) \
    [](const VNX::IComponent* c) -> QVariant { return QVariant(static_cast<int>(static_cast<const Comp*>(c)->member)); }

#define VNX_PROP_SET_F(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toFloat(); return true; }
#define VNX_PROP_SET_D(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toDouble(); return true; }
#define VNX_PROP_SET_I(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toInt(); return true; }
#define VNX_PROP_SET_B(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toBool(); return true; }
#define VNX_PROP_SET_U32(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toUInt(); return true; }
#define VNX_PROP_SET_U64(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { \
        bool ok = false; \
        const qulonglong raw = v.toULongLong(&ok); \
        if (!ok) return false; \
        static_cast<Comp*>(c)->member = static_cast<uint64_t>(raw); \
        return true; \
    }
#define VNX_PROP_SET_U16(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = static_cast<uint16_t>(v.toInt()); return true; }
#define VNX_PROP_SET_U8(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = static_cast<uint8_t>(v.toInt()); return true; }
#define VNX_PROP_SET_S(Comp, member) \
    [](VNX::IComponent* c, const QVariant& v) -> bool { static_cast<Comp*>(c)->member = v.toString(); return true; }

}
