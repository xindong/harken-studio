#include "Core/AppSettings.h"
#include "Core/ComponentCatalog.h"
#include "Core/ContextInfo.h"
#include "Core/HelpCatalog.h"
#include "Core/ItemTypeCatalog.h"
#include "Core/PropertyDescriptor.h"
#include "Core/Components/VSelection.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <optional>
#include <string>
#include <vector>

using VNX::ComponentType;
using VNX::ContextTopic;
using VNX::HelpEntry;
using VNX::ProjectItem;
using VNX::PropertyDescriptor;
namespace ComponentCatalog = VNX::ComponentCatalog;
namespace HelpCatalog = VNX::HelpCatalog;
namespace ItemTypeCatalog = VNX::ItemTypeCatalog;

namespace {

struct InspectorPage
{
    ProjectItem::Type type;
    const VNX::PropertySchema& schema;
};

const std::vector<InspectorPage>& inspectorPages()
{
    static const std::vector<InspectorPage> pages = {
        { ProjectItem::Type::Sound,       VNX::soundInspectorSchema() },
        { ProjectItem::Type::Container,   VNX::containerInspectorSchema() },
        { ProjectItem::Type::Track,       VNX::trackInspectorSchema() },
        { ProjectItem::Type::Piece,       VNX::pieceInspectorSchema() },
        { ProjectItem::Type::Layer,       VNX::layerInspectorSchema() },
        { ProjectItem::Type::Bus,         VNX::busInspectorSchema() },
        { ProjectItem::Type::Action,      VNX::actionInspectorSchema() },
        { ProjectItem::Type::Attenuation, VNX::attenuationInspectorSchema() },
        { ProjectItem::Type::Variable,    VNX::variableInspectorSchema() },
    };
    return pages;
}
const PropertyDescriptor* matchDescriptor(const PropertyDescriptor& descriptor,
                                           std::optional<ComponentType> owner,
                                           const QString& field,
                                           std::optional<ComponentType> inheritedOwner)
{
    const auto effectiveOwner = descriptor.ownerComponent
        ? descriptor.ownerComponent : inheritedOwner;
    if (descriptor.field == field && effectiveOwner == owner)
        return &descriptor;
    for (const PropertyDescriptor& child : descriptor.elementSchema)
        if (const PropertyDescriptor* found = matchDescriptor(child, owner, field, effectiveOwner))
            return found;
    return nullptr;
}

const PropertyDescriptor* rowIn(const VNX::PropertySchema& schema,
                                std::optional<ComponentType> owner,
                                const QString& field)
{
    for (const PropertyDescriptor& descriptor : schema)
        if (const PropertyDescriptor* found = matchDescriptor(descriptor, owner, field, descriptor.ownerComponent))
            return found;
    return nullptr;
}

const PropertyDescriptor* customSectionRow(std::optional<ComponentType> owner,
                                           const QString& field)
{
    if (owner != ComponentType::Selection || field != QLatin1String("mode"))
        return nullptr;
    return rowIn(VNX::VSelection::schema(), owner, field);
}

const PropertyDescriptor* anyInspectorRow(std::optional<ComponentType> owner,
                                          const QString& field)
{
    for (const InspectorPage& page : inspectorPages())
        if (const PropertyDescriptor* descriptor = rowIn(page.schema, owner, field))
            return descriptor;
    return customSectionRow(owner, field);
}

const PropertyDescriptor* pageRowOn(ProjectItem::Type type,
                                    std::optional<ComponentType> owner,
                                    const QString& field)
{
    for (const InspectorPage& page : inspectorPages())
        if (page.type == type)
            if (const PropertyDescriptor* descriptor = rowIn(page.schema, owner, field))
                return descriptor;
    return nullptr;
}

struct FieldKey
{
    std::optional<ComponentType> owner;
    QString field;
    bool valid = false;
};

FieldKey parseFieldKey(const QString& qualified)
{
    FieldKey key;
    const qsizetype dot = qualified.indexOf(QLatin1Char('.'));
    if (dot <= 0) return key;

    key.field = qualified.mid(dot + 1);
    const QString ownerName = qualified.left(dot);
    if (ownerName == QLatin1String("item"))
    {
        key.valid = !key.field.isEmpty();
        return key;
    }
    key.owner = ComponentCatalog::parseComponentName(ownerName);
    key.valid = key.owner.has_value() && !key.field.isEmpty();
    return key;
}

ContextTopic fieldTopic(std::optional<ComponentType> owner,
                        const QString& field,
                        ProjectItem::Type hostType = ProjectItem::Type::Sound)
{
    return ContextTopic::forField(owner, field, field, QUuid::createUuid(),
                                  hostType, QStringLiteral("Rain_Loop"));
}

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

TEST(HelpCatalogKey, KeyForNamesEachTopicSpace)
{
    EXPECT_EQ(HelpCatalog::keyFor(ContextTopic()), QString());
    EXPECT_EQ(HelpCatalog::keyFor(ContextTopic::forObject(QUuid::createUuid(),
                                                          ProjectItem::Type::Sound,
                                                          QStringLiteral("Rain_Loop"))),
              QStringLiteral("types.Sound"));
    EXPECT_EQ(HelpCatalog::keyFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"))),
              QStringLiteral("fields.mixnode.volume"));
    EXPECT_EQ(HelpCatalog::keyFor(fieldTopic(std::nullopt, QStringLiteral("notes"))),
              QStringLiteral("fields.item.notes"));
    EXPECT_EQ(HelpCatalog::keyFor(ContextTopic::forPanel(QStringLiteral("information"))),
              QStringLiteral("panels.information"));
}

TEST(HelpCatalogKey, ObjectTopicsResolveForEveryType)
{
    for (ProjectItem::Type type : ItemTypeCatalog::allTypes())
    {
        const QString name = ItemTypeCatalog::canonicalName(type);
        const ContextTopic topic = ContextTopic::forObject(QUuid::createUuid(), type,
                                                           QStringLiteral("Anything"));
        EXPECT_EQ(HelpCatalog::keyFor(topic), QStringLiteral("types.") + name);

        const HelpEntry* entry = HelpCatalog::entryFor(topic);
        ASSERT_NE(entry, nullptr) << context(name);
        EXPECT_FALSE(entry->body.isEmpty()) << context(name);
    }
}

TEST(HelpCatalogKey, OneBaseEntryServesEveryHostAndTheHostAddsItsOwnParagraph)
{
    const HelpEntry* onSound =
        HelpCatalog::entryFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"),
                                         ProjectItem::Type::Sound));
    const HelpEntry* onLayer =
        HelpCatalog::entryFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"),
                                         ProjectItem::Type::Layer));
    const HelpEntry* onBus =
        HelpCatalog::entryFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"),
                                         ProjectItem::Type::Bus));

    ASSERT_NE(onSound, nullptr);
    EXPECT_EQ(onSound, onLayer);
    EXPECT_EQ(onSound, onBus);
    EXPECT_FALSE(onSound->body.isEmpty());

    const QString soundBody =
        HelpCatalog::bodyFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"),
                                        ProjectItem::Type::Sound));
    const QString busBody =
        HelpCatalog::bodyFor(fieldTopic(ComponentType::MixNode, QStringLiteral("volume"),
                                        ProjectItem::Type::Bus));

    EXPECT_EQ(soundBody, onSound->body);
    EXPECT_TRUE(busBody.startsWith(onSound->body));
    EXPECT_GT(busBody.size(), soundBody.size());
}

TEST(HelpCatalogKey, TopicsWithoutCopyResolveToNothing)
{
    EXPECT_EQ(HelpCatalog::entryFor(ContextTopic()), nullptr);
    EXPECT_TRUE(HelpCatalog::bodyFor(ContextTopic()).isEmpty());
    EXPECT_EQ(HelpCatalog::entryFor(fieldTopic(ComponentType::MixNode,
                                               QStringLiteral("noSuchField"))),
              nullptr);
    EXPECT_TRUE(HelpCatalog::bodyFor(fieldTopic(ComponentType::MixNode,
                                                QStringLiteral("noSuchField"))).isEmpty());
    EXPECT_EQ(HelpCatalog::entryFor(ContextTopic::forPanel(QStringLiteral("noSuchPanel"))),
              nullptr);
}

TEST(HelpCatalogKey, TheCatalogLoadsFromTheResourceBundle)
{
    EXPECT_NE(HelpCatalog::entryFor(ContextTopic::forObject(
                  QUuid::createUuid(), ProjectItem::Type::Sound, QStringLiteral("Anything"))),
              nullptr);
}
TEST(HelpCatalogKey, ChineseCatalogMatchesEnglishKeysAndCarriesCopy)
{
    const QJsonObject zhRoot = catalogRoot("zh-CN");
    EXPECT_EQ(zhRoot.value(QStringLiteral("schema")).toInt(), 1);

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

    const QJsonObject fields = catalogSection("fields", "zh-CN");
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
    {
        const QJsonObject entry = it.value().toObject();
        const QJsonObject byType = entry.value(QStringLiteral("byType")).toObject();
        if (byType.isEmpty())
            continue;

        const QString key = QStringLiteral("fields.") + it.key();
        const FieldKey parsed = parseFieldKey(it.key());
        ASSERT_TRUE(parsed.valid) << context(key);
        EXPECT_FALSE(entry.value(QStringLiteral("body")).toString().isEmpty()) << context(key);

        for (auto type = byType.constBegin(); type != byType.constEnd(); ++type)
        {
            const QString supplement = key + QStringLiteral(".byType.") + type.key();
            const auto hostType =
                ItemTypeCatalog::parseTypeName(type.key(), Qt::CaseSensitive, false);
            ASSERT_TRUE(hostType.has_value()) << context(supplement);
            EXPECT_NE(pageRowOn(*hostType, parsed.owner, parsed.field), nullptr)
                << context(supplement);
            EXPECT_FALSE(type.value().toString().isEmpty()) << context(supplement);
        }
    }
}

TEST(HelpCatalogKey, SwitchingLanguageReloadsCatalogBodies)
{
    const QString previous = VNX::AppSettings::instance().language();
    const ContextTopic topic = ContextTopic::forObject(
        QUuid::createUuid(), ProjectItem::Type::Sound, QStringLiteral("Rain_Loop"));

    VNX::AppSettings::instance().setLanguage(QStringLiteral("en"));
    const QString english = HelpCatalog::bodyFor(topic);

    VNX::AppSettings::instance().setLanguage(QStringLiteral("zh-CN"));
    const QString chinese = HelpCatalog::bodyFor(topic);

    VNX::AppSettings::instance().setLanguage(previous);

    EXPECT_FALSE(english.isEmpty());
    EXPECT_FALSE(chinese.isEmpty());
    EXPECT_NE(english, chinese);
}
