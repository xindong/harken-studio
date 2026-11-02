#include "FilterSemantics.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace VNX {
namespace FilterSemantics {
namespace {

constexpr std::array<FilterField, 10> filterFields{{
    {"filterLowPassFrequency", FilterKind::LowPass, FilterParameter::Frequency, 20.0, 20000.0, 20000.0},
    {"filterLowPassQ", FilterKind::LowPass, FilterParameter::Q, 0.1, 10.0, 0.70710678},
    {"filterHighPassFrequency", FilterKind::HighPass, FilterParameter::Frequency, 20.0, 20000.0, 20.0},
    {"filterHighPassQ", FilterKind::HighPass, FilterParameter::Q, 0.1, 10.0, 0.70710678},
    {"filterLowShelfFrequency", FilterKind::LowShelf, FilterParameter::Frequency, 20.0, 20000.0, 200.0},
    {"filterLowShelfQ", FilterKind::LowShelf, FilterParameter::Q, 0.1, 10.0, 0.70710678},
    {"filterLowShelfGain", FilterKind::LowShelf, FilterParameter::Gain, -48.0, 48.0, 0.0},
    {"filterHighShelfFrequency", FilterKind::HighShelf, FilterParameter::Frequency, 20.0, 20000.0, 6000.0},
    {"filterHighShelfQ", FilterKind::HighShelf, FilterParameter::Q, 0.1, 10.0, 0.70710678},
    {"filterHighShelfGain", FilterKind::HighShelf, FilterParameter::Gain, -48.0, 48.0, 0.0}}};

constexpr std::array<const char*, 4> sectionKeys{
    "lowPass", "highPass", "lowShelf", "highShelf"};

}

const char* sectionKey(FilterKind kind)
{
    return sectionKeys[static_cast<std::size_t>(kind)];
}

const char* parameterKey(FilterParameter parameter)
{
    switch (parameter) {
    case FilterParameter::Frequency: return "frequency";
    case FilterParameter::Q: return "q";
    case FilterParameter::Gain: return "gain";
    }
    return "";
}

FilterState defaultState() { return {}; }

std::span<const FilterField> fields() { return filterFields; }

const FilterField* fieldInfo(const QString& field)
{
    for (const auto& candidate : filterFields)
        if (field == QLatin1String(candidate.field)) return &candidate;
    return nullptr;
}

double value(const FilterState& state, const FilterField& field)
{
    const auto& section = state.sections[static_cast<std::size_t>(field.kind)];
    switch (field.parameter) {
    case FilterParameter::Frequency: return section.frequency;
    case FilterParameter::Q: return section.q;
    case FilterParameter::Gain: return section.gain;
    }
    return 0.0;
}

bool setValue(FilterState& state, const FilterField& field, double value)
{
    if (!std::isfinite(value) || value < field.minimum || value > field.maximum) return false;
    auto& section = state.sections[static_cast<std::size_t>(field.kind)];
    switch (field.parameter) {
    case FilterParameter::Frequency: section.frequency = value; break;
    case FilterParameter::Q: section.q = value; break;
    case FilterParameter::Gain: section.gain = value; break;
    }
    return true;
}

bool sectionIsDefault(FilterKind kind, const FilterSection& section)
{
    return section == defaultState().sections[static_cast<std::size_t>(kind)];
}

bool isDefault(const FilterState& state) { return state == defaultState(); }

bool isValid(const FilterState& state)
{
    for (const auto& field : filterFields) {
        const double number = value(state, field);
        if (!std::isfinite(number) || number < field.minimum || number > field.maximum) return false;
    }
    return state.sections[0].gain == 0.0 && state.sections[1].gain == 0.0;
}

QJsonObject toJson(const FilterState& state)
{
    QJsonObject json;
    for (std::size_t i = 0; i < state.sections.size(); ++i) {
        const auto& section = state.sections[i];
        QJsonObject object{{QStringLiteral("frequency"), section.frequency},
                           {QStringLiteral("q"), section.q}};
        if (i >= 2) object.insert(QStringLiteral("gain"), section.gain);
        json.insert(QLatin1String(sectionKeys[i]), object);
    }
    return json;
}

std::optional<FilterState> fromJson(const QJsonObject& json)
{
    if (json.size() != sectionKeys.size()) return std::nullopt;
    auto state = defaultState();
    for (std::size_t i = 0; i < sectionKeys.size(); ++i) {
        const auto section = json.value(QLatin1String(sectionKeys[i]));
        if (!section.isObject() || section.toObject().size() != (i < 2 ? 2 : 3)) return std::nullopt;
    }
    for (const auto& field : filterFields) {
        const auto section = json.value(QLatin1String(sectionKeys[static_cast<std::size_t>(field.kind)])).toObject();
        const auto number = section.value(QLatin1String(parameterKey(field.parameter)));
        if (!number.isDouble() || !setValue(state, field, number.toDouble())) return std::nullopt;
    }
    return state;
}

std::optional<FilterState> applyOverlay(const FilterState& base, const QJsonObject& json)
{
    auto state = base;
    for (auto section = json.begin(); section != json.end(); ++section) {
        if (!section.value().isObject() || section.value().toObject().isEmpty()) return std::nullopt;
        const auto values = section.value().toObject();
        for (auto entry = values.begin(); entry != values.end(); ++entry) {
            const auto found = std::find_if(filterFields.begin(), filterFields.end(), [&](const FilterField& field) {
                return section.key() == QLatin1String(sectionKey(field.kind))
                    && entry.key() == QLatin1String(parameterKey(field.parameter));
            });
            if (found == filterFields.end() || !entry.value().isDouble()
                || !setValue(state, *found, entry.value().toDouble())) return std::nullopt;
        }
    }
    return state;
}

double sectionResponseDb(FilterKind kind, const FilterSection& section,
                         double frequency, double sampleRate)
{
    if (sectionIsDefault(kind, section)) return 0.0;
    const double angle = 2.0 * std::numbers::pi * section.frequency / sampleRate;
    const double cosine = std::cos(angle);
    const double alpha = std::sin(angle) / (2.0 * section.q);
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosine;
    double a2 = 1.0 - alpha;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    if (kind == FilterKind::LowPass) {
        b0 = (1.0 - cosine) * 0.5;
        b1 = 1.0 - cosine;
        b2 = b0;
    } else if (kind == FilterKind::HighPass) {
        b0 = (1.0 + cosine) * 0.5;
        b1 = -(1.0 + cosine);
        b2 = b0;
    } else {
        if (section.gain == 0.0) return 0.0;
        const double amplitude = std::pow(10.0, section.gain / 40.0);
        const double beta = 2.0 * std::sqrt(amplitude) * alpha;
        const double plus = amplitude + 1.0;
        const double minus = amplitude - 1.0;
        if (kind == FilterKind::LowShelf) {
            a0 = plus + minus * cosine + beta;
            a1 = -2.0 * (minus + plus * cosine);
            a2 = plus + minus * cosine - beta;
            b0 = amplitude * (plus - minus * cosine + beta);
            b1 = 2.0 * amplitude * (minus - plus * cosine);
            b2 = amplitude * (plus - minus * cosine - beta);
        } else {
            a0 = plus - minus * cosine + beta;
            a1 = 2.0 * (minus - plus * cosine);
            a2 = plus - minus * cosine - beta;
            b0 = amplitude * (plus + minus * cosine + beta);
            b1 = -2.0 * amplitude * (minus + plus * cosine);
            b2 = amplitude * (plus + minus * cosine - beta);
        }
    }
    const auto z = std::polar(1.0, -2.0 * std::numbers::pi * frequency / sampleRate);
    const auto response = (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
    return 20.0 * std::log10(std::max(std::abs(response), 1.0e-12));
}

double responseDb(const FilterState& state, double frequency, double sampleRate)
{
    double result = 0.0;
    for (std::size_t i = 0; i < state.sections.size(); ++i)
        result += sectionResponseDb(static_cast<FilterKind>(i), state.sections[i], frequency, sampleRate);
    return result;
}

}
}
