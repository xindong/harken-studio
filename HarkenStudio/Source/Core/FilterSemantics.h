#pragma once

#include <QJsonObject>
#include <QString>

#include <array>
#include <optional>
#include <span>

namespace VNX {

enum class FilterKind { LowPass, HighPass, LowShelf, HighShelf };
enum class FilterParameter { Frequency, Q, Gain };

struct FilterSection {
    double frequency = 20000.0;
    double q = 0.70710678;
    double gain = 0.0;

    bool operator==(const FilterSection&) const = default;
};

struct FilterState {
    std::array<FilterSection, 4> sections{{
        {20000.0, 0.70710678, 0.0},
        {20.0, 0.70710678, 0.0},
        {200.0, 0.70710678, 0.0},
        {6000.0, 0.70710678, 0.0}}};

    bool operator==(const FilterState&) const = default;
};

struct FilterField {
    const char* field;
    FilterKind kind;
    FilterParameter parameter;
    double minimum;
    double maximum;
    double defaultValue;
};

namespace FilterSemantics {

FilterState defaultState();
std::span<const FilterField> fields();
const FilterField* fieldInfo(const QString& field);
const char* sectionKey(FilterKind kind);
const char* parameterKey(FilterParameter parameter);
double value(const FilterState& state, const FilterField& field);
bool setValue(FilterState& state, const FilterField& field, double value);
bool sectionIsDefault(FilterKind kind, const FilterSection& section);
bool isDefault(const FilterState& state);
bool isValid(const FilterState& state);
QJsonObject toJson(const FilterState& state);
std::optional<FilterState> fromJson(const QJsonObject& json);
std::optional<FilterState> applyOverlay(const FilterState& base, const QJsonObject& json);
double sectionResponseDb(FilterKind kind, const FilterSection& section,
                         double frequency, double sampleRate = 48000.0);
double responseDb(const FilterState& state, double frequency,
                  double sampleRate = 48000.0);

}

}
