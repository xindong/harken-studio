#pragma once

#include "ThemeSnapshot.h"

#include <array>
#include <optional>
#include <variant>

namespace harken::studio {

enum class FontRole { Title, Body, Caption, Mono };

struct ThemeMetricToken
{
    const char* key;
    const char* group;
    const char* label;
    const char* description;
    int minimum;
    static constexpr int maximum = 4096;
    std::variant<int ThemeMetrics::*, int ThemeMetrics::Control::*> member;

    int value(const ThemeMetrics& metrics) const;
    void setValue(ThemeMetrics& metrics, int value) const;
    bool accepts(double value) const;
};

struct ThemeFontToken
{
    const char* key;
    const char* label;
    const char* description;
    FontRole role;
    ThemeFontStyle ThemeTypography::*member;
};

struct ThemeFontOverride
{
    std::optional<QString> family;
    std::optional<int> pointSize;
    std::optional<int> weight;
    std::optional<ThemeFontSource> source;

    static constexpr int maximumFamilyLength = 128;
    static constexpr int minimumPointSize = 6;
    static constexpr int maximumPointSize = 72;
    static constexpr int minimumWeight = 1;
    static constexpr int maximumWeight = 1000;

    bool empty() const { return !family && !pointSize && !weight; }
    const char* invalidField() const;
    void applyTo(ThemeFontStyle& style, const QStringList& fallbackFamilies = {}) const;
    bool operator==(const ThemeFontOverride&) const = default;
};

const std::array<ThemeMetricToken, 18>& themeMetricTokens();
const std::array<ThemeFontToken, 4>& themeFontTokens();
const ThemeFontToken& themeFontToken(FontRole role);
const ThemeMetricToken* findThemeMetricToken(const QString& key);
const ThemeFontToken* findThemeFontToken(const QString& key);

}
