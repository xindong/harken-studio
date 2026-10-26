#include <gtest/gtest.h>

#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <string>

namespace {

QJsonObject catalogRoot(const char* language = "en")
{
    QFile file(QStringLiteral(":/Resources/help/help.%1.json")
                   .arg(QString::fromLatin1(language)));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    EXPECT_TRUE(document.isObject());
    return document.object();
}

QJsonObject catalogSection(const char* section, const char* language = "en")
{
    return catalogRoot(language).value(QString::fromLatin1(section)).toObject();
}

QStringList objectKeys(const QJsonObject& object)
{
    QStringList keys = object.keys();
    keys.sort();
    return keys;
}

std::string context(const QString& key)
{
    return key.toStdString();
}

}

TEST(HelpCatalogKey, ChineseCatalogMatchesEnglishKeysAndCarriesCopy)
{
    EXPECT_EQ(objectKeys(catalogSection("types", "zh-CN")),
              objectKeys(catalogSection("types")));
    EXPECT_EQ(objectKeys(catalogSection("fields", "zh-CN")),
              objectKeys(catalogSection("fields")));
    EXPECT_EQ(objectKeys(catalogSection("panels", "zh-CN")),
              objectKeys(catalogSection("panels")));

    const auto checkEntries = [](const QJsonObject& entries, const QString& prefix) {
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
        {
            const QJsonObject entry = it.value().toObject();
            const QString key = prefix + it.key();
            EXPECT_FALSE(entry.value(QStringLiteral("title")).toString().isEmpty())
                << context(key);
            EXPECT_FALSE(entry.value(QStringLiteral("body")).toString().isEmpty())
                << context(key);
        }
    };
    checkEntries(catalogSection("types", "zh-CN"), QStringLiteral("types."));
    checkEntries(catalogSection("fields", "zh-CN"), QStringLiteral("fields."));
    checkEntries(catalogSection("panels", "zh-CN"), QStringLiteral("panels."));

}
