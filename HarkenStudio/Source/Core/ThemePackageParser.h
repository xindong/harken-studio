#pragma once

#include "ThemePackageDto.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace VNX {

enum class ThemePackageErrorCode
{
    InvalidJson,
    DuplicateJsonKey,
    RootTypeMismatch,
    MissingField,
    FieldTypeMismatch,
    UnsupportedSchemaVersion,
    UnknownField,
    InvalidValue,
    FileNotFound,
    PathEscape,
    ResourceLimit,
    UnsupportedCompatibility,
    DuplicateThemeId,
    MissingBaseTheme,
    InheritanceCycle,
    InheritanceDepthExceeded,
    UnknownSemanticKey,
    InvalidColor,
    MetricOutOfRange,
    RuntimeThreadViolation,
    PersistenceUnavailable,
    AssetNotFound,
    AssetTypeMismatch,
    AssetDecodeFailed,
    AssetDimensionsExceeded,
    AssetThreadViolation,
    UnknownRecipeHost,
    UnknownRecipeSlot,
    DuplicateRecipe,
    DuplicateRecipeSlot,
    RecipeSizeConflict,
    UnsupportedRecipeVariant,
    UnsupportedRecipePresenter
};

struct ThemePackageDiagnostic
{
    ThemePackageErrorCode code = ThemePackageErrorCode::InvalidJson;
    QString file;
    QString path;
    QString message;
};

template<typename T>
struct ThemePackageParseResult
{
    std::optional<T> value;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const
    {
        return value.has_value() && diagnostics.isEmpty();
    }
};

QString themePackageErrorCodeName(ThemePackageErrorCode code);

ThemePackageParseResult<ThemeManifestDto> parseThemeManifest(
    const QByteArray& json,
    const QString& file = QStringLiteral("manifest.json"));

ThemePackageParseResult<ThemeColorDefinitionDto> parseThemeColorDefinition(
    const QByteArray& json,
    const QString& file = QStringLiteral("colors.json"));

ThemePackageParseResult<ThemeTokenDto> parseThemeTokens(
    const QByteArray& json,
    const QString& file = QStringLiteral("tokens.json"));

ThemePackageParseResult<ThemeVisualDto> parseThemeVisual(
    const QByteArray& json,
    const QString& file);

ThemePackageParseResult<ThemePresenterMapDto> parseThemePresenters(
    const QByteArray& json,
    const QString& file = QStringLiteral("presenters.json"));

ThemePackageParseResult<ThemeRecipeMapDto> parseThemeRecipes(
    const QByteArray& json,
    const QString& file = QStringLiteral("recipes.json"));

ThemePackageParseResult<ThemeAssetMapDto> parseThemeAssetMap(
    const QByteArray& json,
    const QString& file = QStringLiteral("assets.json"));

}
