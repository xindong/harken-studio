#pragma once

#include <QMap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace VNX {

enum class ThemeUnknownFieldPolicy
{
    Reject
};

enum class ThemeDeprecatedFieldPolicy
{
    Reject
};

enum class ThemeArrayInheritancePolicy
{
    Replace
};

struct ThemePackageContract
{
    static constexpr int minimumSchemaVersion = 1;
    static constexpr int maximumSchemaVersion = 5;
    static constexpr ThemeUnknownFieldPolicy unknownFieldPolicy =
        ThemeUnknownFieldPolicy::Reject;
    static constexpr ThemeDeprecatedFieldPolicy deprecatedFieldPolicy =
        ThemeDeprecatedFieldPolicy::Reject;
    static constexpr ThemeArrayInheritancePolicy arrayInheritancePolicy =
        ThemeArrayInheritancePolicy::Replace;
};

struct ThemeCompatibilityDto
{
    std::optional<QString> harkenVersionRange;
    QStringList capabilities;
};

struct ThemeVisualEntriesDto
{
    QString curve;
    QString meter;
    QString waveform;
    std::optional<QString> nodeGraph;
    std::optional<QString> blueprint;
    std::optional<QString> timeline;
    std::optional<QString> music;
    std::optional<QString> log;
    std::optional<QString> monitor;
    std::optional<QString> ai;
    std::optional<QString> scene3d;
    std::optional<QString> spatial;
    std::optional<QString> feedback;
};

struct ThemePackageEntriesDto
{
    QString tokens;
    ThemeVisualEntriesDto visuals;
    QString assetMap;
    std::optional<QString> presenters;
    std::optional<QString> recipes;
};

struct ThemePresenterMapDto
{
    QMap<QString, QString> contexts;
};

struct ThemeRecipeSlotDto
{
    QString id;
    bool visible = true;
    std::optional<int> minimum;
    std::optional<int> preferred;
    std::optional<int> maximum;
    QString alignment = QStringLiteral("fill");
    std::optional<QString> presenter;
    std::optional<QString> asset;
};

struct ThemeComponentRecipeDto
{
    QString host;
    QString breakpoint = QStringLiteral("default");
    QString variant;
    QVector<ThemeRecipeSlotDto> items;
};

struct ThemeRecipeMapDto
{
    QVector<ThemeComponentRecipeDto> recipes;
};

struct ThemeManifestDto
{
    int schemaVersion = 0;
    QString id;
    QString displayName;
    QString version;
    std::optional<QString> colorScheme;
    std::optional<QString> baseThemeId;
    ThemeCompatibilityDto compatibility;
    ThemePackageEntriesDto entries;
    std::optional<QString> previewAssetId;
};

struct ThemeTokenDto
{
    QMap<QString, QString> colors;
    QMap<QString, double> metrics;
    struct FontRole {
        std::optional<QString> family;
        std::optional<int> pointSize;
        std::optional<int> weight;
    };
    QMap<QString, FontRole> fonts;
};

struct ThemeVisualDto
{
    QMap<QString, QString> colors;
    QMap<QString, double> metrics;
};

enum class ThemeAssetKind
{
    Image,
    NinePatch,
    FrameSequence,
    Font
};

struct ThemeAssetInsetsDto
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct ThemeAssetSourceDto
{
    QString path;
    double scale = 1.0;
};

struct ThemeAssetDto
{
    ThemeAssetKind kind = ThemeAssetKind::Image;
    QVector<ThemeAssetSourceDto> sources;
    std::optional<ThemeAssetInsetsDto> insets;
};

struct ThemeAssetMapDto
{
    QMap<QString, ThemeAssetDto> assets;
};

}
