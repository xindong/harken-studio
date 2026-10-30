#include "Core/ThemePackageParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <gtest/gtest.h>

TEST(ThemePackageParser, RejectsRecipeSizeOutsideIntegerRange)
{
    const auto result = VNX::parseThemeRecipes(R"json({
        "recipes": [{
            "host": "track.header",
            "variant": "track.compact",
            "slots": [{"id": "identity", "preferred": 1e30}]
        }]
    })json");

    ASSERT_FALSE(result.ok());
}

namespace {

QJsonObject validColorDefinition()
{
    return QJsonDocument::fromJson(R"json({
        "roles": [{
            "key": "custom.surface",
            "group": "Surfaces",
            "label": "Canvas",
            "description": "The theme's canvas color.",
            "bindings": [{"target": "panel.background"}]
        }],
        "palettes": [{
            "id": "night", "displayName": "Night", "colorScheme": "dark",
            "colors": {"custom.surface": "#80123456"}
        }],
        "defaultPalette": "night"
    })json").object();
}

}

TEST(ThemePackageParser, RequiresUniqueRolesAndPaletteIdentities)
{
    const QJsonObject valid = validColorDefinition();
    ASSERT_TRUE(VNX::parseThemeColorDefinition(QJsonDocument(valid).toJson()).ok());
    for (const QString& collection : {QStringLiteral("roles"), QStringLiteral("palettes")}) {
        QJsonObject document = valid;
        QJsonArray entries = document.value(collection).toArray();
        entries.append(entries.first());
        document.insert(collection, entries);
        EXPECT_FALSE(VNX::parseThemeColorDefinition(QJsonDocument(document).toJson()).ok());
    }
    QJsonObject document = valid;
    document.insert(QStringLiteral("defaultPalette"), QStringLiteral("missing"));
    EXPECT_FALSE(VNX::parseThemeColorDefinition(QJsonDocument(document).toJson()).ok());
}

TEST(ThemePackageParser, RequiresUsableUnambiguousColorBindings)
{
    const QJsonObject target{{QStringLiteral("target"), QStringLiteral("panel.background")}};
    const QList<QJsonArray> invalidBindings{
        QJsonArray{}, QJsonArray{target, target},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("")}}},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")},
                              {QStringLiteral("alpha"), -1}}},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")},
                              {QStringLiteral("alpha"), 256}}},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")},
                              {QStringLiteral("alpha"), 0.5}}},
        QJsonArray{QJsonObject{{QStringLiteral("target"), QStringLiteral("panel.background")},
                              {QStringLiteral("alpha"), QStringLiteral("100")}}}};
    for (const auto& bindings : invalidBindings) {
        QJsonObject document = validColorDefinition();
        QJsonObject role = document.value(QStringLiteral("roles")).toArray().first().toObject();
        role.insert(QStringLiteral("bindings"), bindings);
        document.insert(QStringLiteral("roles"), QJsonArray{role});
        EXPECT_FALSE(VNX::parseThemeColorDefinition(QJsonDocument(document).toJson()).ok());
    }
}

TEST(ThemePackageParser, PaletteValuesMustExactlyCoverTheirThemeRoles)
{
    const QList<QJsonObject> invalidColors{
        QJsonObject{},
        QJsonObject{{QStringLiteral("custom.surface"), QStringLiteral("#123456")},
                    {QStringLiteral("unrelated.role"), QStringLiteral("#abcdef")}},
        QJsonObject{{QStringLiteral("custom.surface"), QStringLiteral("red")}},
        QJsonObject{{QStringLiteral("custom.surface"), QStringLiteral("#12345")}},
        QJsonObject{{QStringLiteral("custom.surface"), 123456}}};
    for (const auto& colors : invalidColors) {
        QJsonObject document = validColorDefinition();
        QJsonObject palette = document.value(QStringLiteral("palettes")).toArray().first().toObject();
        palette.insert(QStringLiteral("colors"), colors);
        document.insert(QStringLiteral("palettes"), QJsonArray{palette});
        EXPECT_FALSE(VNX::parseThemeColorDefinition(QJsonDocument(document).toJson()).ok());
    }
    QJsonObject fixedTheme = validColorDefinition();
    fixedTheme.insert(QStringLiteral("roles"), QJsonArray{});
    QJsonObject palette = fixedTheme.value(QStringLiteral("palettes")).toArray().first().toObject();
    palette.insert(QStringLiteral("colors"), QJsonObject{});
    fixedTheme.insert(QStringLiteral("palettes"), QJsonArray{palette});
    EXPECT_TRUE(VNX::parseThemeColorDefinition(QJsonDocument(fixedTheme).toJson()).ok());
}
