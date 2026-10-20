#include "Core/ItemTypeCatalog.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace Catalog = VNX::ItemTypeCatalog;
using T = VNX::ProjectItem::Type;
using Catalog::Bucket;
using Catalog::NameNamespace;

namespace {

const T kUnknownType = static_cast<T>(99);

const std::array<T, 9>& types() { return Catalog::allTypes(); }

bool isContent(T t)
{
    return t == T::Sound || t == T::Container || t == T::Track || t == T::Piece;
}
}

TEST(ItemTypeCatalog, EnumValuesAreTheFrozenV6Contract)
{
    EXPECT_EQ(int(T::None), 0);
    EXPECT_EQ(int(T::Sound), 1);
    EXPECT_EQ(int(T::Container), 2);
    EXPECT_EQ(int(T::Track), 3);
    EXPECT_EQ(int(T::Piece), 4);
    EXPECT_EQ(int(T::Layer), 5);
    EXPECT_EQ(int(T::Bus), 6);
    EXPECT_EQ(int(T::Action), 7);
    EXPECT_EQ(int(T::Variable), 8);
    EXPECT_EQ(int(T::Attenuation), 9);
}

TEST(ItemTypeCatalog, AllTypesCoversFamily)
{
    const auto& all = types();
    EXPECT_EQ(all.size(), 9u);
    const std::array<T, 9> expected = {
        T::Sound, T::Container, T::Track, T::Piece,
        T::Layer, T::Bus, T::Action, T::Variable, T::Attenuation,
    };
    EXPECT_EQ(all, expected);
    for (T t : all) EXPECT_NE(Catalog::factsOrNull(t), nullptr);
    EXPECT_EQ(Catalog::factsOrNull(kUnknownType), nullptr);
}

TEST(ItemTypeCatalog, ParentMatrixGoldenTable)
{
    const auto allowed = [](T child, T parent) {
        switch (child) {
            case T::Sound:
            case T::Container:   return parent == T::Layer || parent == T::Container
                                     || parent == T::Track;
            case T::Track:       return parent == T::Layer || parent == T::Container
                                     || parent == T::Piece;
            case T::Piece:       return parent == T::Layer || parent == T::Container
                                     || parent == T::Track;
            case T::Layer:       return parent == T::Layer;
            case T::Bus:         return parent == T::Bus;
            default:             return false;
        }
    };
    for (T child : types()) {
        for (T parent : types()) {
            EXPECT_EQ(Catalog::canBeChildOf(child, parent), allowed(child, parent))
                << "child=" << int(child) << " parent=" << int(parent);
        }
    }
    EXPECT_FALSE(Catalog::canBeChildOf(kUnknownType, T::None));
    EXPECT_FALSE(Catalog::canBeChildOf(T::None, kUnknownType));
}

TEST(ItemTypeCatalog, FolderAddressableTypes)
{
    for (T t : types()) {
        const bool expected = t != T::Bus;
        EXPECT_EQ(Catalog::isFolderAddressable(t), expected) << "type=" << int(t);
    }
    EXPECT_FALSE(Catalog::isFolderAddressable(kUnknownType));
}

TEST(ItemTypeCatalog, ChildDerivationsMatchMatrix)
{
    for (T parent : types()) {
        std::vector<T> expected;
        for (T child : types()) {
            if (Catalog::canBeChildOf(child, parent)) expected.push_back(child);
        }
        EXPECT_EQ(Catalog::allowedChildTypesOf(parent), expected);
        EXPECT_EQ(Catalog::canAcceptAnyChild(parent), !expected.empty());
    }
    EXPECT_EQ(std::vector<T>{}, Catalog::allowedChildTypesOf(kUnknownType));
    EXPECT_FALSE(Catalog::canAcceptAnyChild(kUnknownType));

    EXPECT_FALSE(Catalog::canAcceptAnyChild(T::None));
    EXPECT_TRUE(Catalog::canAcceptAnyChild(T::Layer));
    EXPECT_TRUE(Catalog::canAcceptAnyChild(T::Container));
    EXPECT_TRUE(Catalog::canAcceptAnyChild(T::Track));
    EXPECT_TRUE(Catalog::canAcceptAnyChild(T::Piece));
    EXPECT_TRUE(Catalog::canAcceptAnyChild(T::Bus));
    EXPECT_FALSE(Catalog::canAcceptAnyChild(T::Sound));
    EXPECT_FALSE(Catalog::canAcceptAnyChild(T::Action));
    EXPECT_FALSE(Catalog::canAcceptAnyChild(T::Variable));
    EXPECT_FALSE(Catalog::canAcceptAnyChild(T::Attenuation));
}

TEST(ItemTypeCatalog, NameFamiliesPerType)
{
    struct Row {
        T type;
        const char* display;
        const char* canonical;
        const char* ipc;
        const char* blueprint;
        const char* exporter;
    };
    const Row rows[] = {
        { T::Sound,       "Sound",       "Sound",       "Sound",       "Sound",       "Sound"       },
        { T::Container,   "Container",   "Container",   "Container",   "Container",   "Container"   },
        { T::Track,       "Track",       "Track",       "Track",       "Track",       "Track"       },
        { T::Piece,       "Piece",       "Piece",       "Piece",       "Piece",       "Piece"       },
        { T::Layer,       "Layer",       "Layer",       "Layer",       "Layer",       "Layer"       },
        { T::Bus,         "Bus",         "Bus",         "Bus",         "Bus",         ""            },
        { T::Action,      "Action",      "Action",      "Action",      "Action",      "Action"      },
        { T::Variable,    "Variable",    "Variable",    "Variable",    "Unknown",     ""            },
        { T::Attenuation, "Attenuation", "Attenuation", "Attenuation", "Attenuation", ""            },
    };
    for (const Row& row : rows) {
        EXPECT_EQ(Catalog::displayName(row.type), row.display);
        EXPECT_EQ(Catalog::canonicalName(row.type), row.canonical);
        EXPECT_EQ(Catalog::ipcName(row.type), row.ipc);
        EXPECT_EQ(Catalog::blueprintChainName(row.type), row.blueprint);
        EXPECT_EQ(Catalog::packDependencyName(row.type), row.exporter);
    }
    EXPECT_EQ(Catalog::displayName(kUnknownType), "Unknown");
    EXPECT_EQ(Catalog::canonicalName(kUnknownType), "Unknown");
    EXPECT_EQ(Catalog::ipcName(kUnknownType), "Unknown");
    EXPECT_EQ(Catalog::blueprintChainName(kUnknownType), "Unknown");
    EXPECT_EQ(Catalog::packDependencyName(kUnknownType), "");
}

TEST(ItemTypeCatalog, ParserCaseSensitivityAndAliases)
{
    // VQL profile: case-insensitive, aliases on.
    EXPECT_EQ(Catalog::parseTypeName("folder", Qt::CaseInsensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("LAYER", Qt::CaseInsensitive, true), T::Layer);
    EXPECT_EQ(Catalog::parseTypeName("container", Qt::CaseInsensitive, true), T::Container);
    EXPECT_EQ(Catalog::parseTypeName("track", Qt::CaseInsensitive, true), T::Track);
    EXPECT_EQ(Catalog::parseTypeName("piece", Qt::CaseInsensitive, true), T::Piece);
    EXPECT_EQ(Catalog::parseTypeName("variable", Qt::CaseInsensitive, true), T::Variable);
    EXPECT_EQ(Catalog::parseTypeName("soundsfx", Qt::CaseInsensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("audiobus", Qt::CaseInsensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("AudioFile", Qt::CaseInsensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("AudioLayer", Qt::CaseInsensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("Nope", Qt::CaseInsensitive, true), std::nullopt);

    // IPC profile: case-sensitive, aliases on.
    EXPECT_EQ(Catalog::parseTypeName("SoundSFX", Qt::CaseSensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("AudioBus", Qt::CaseSensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("soundsfx", Qt::CaseSensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("folder", Qt::CaseSensitive, true), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("AudioFile", Qt::CaseSensitive, true), std::nullopt);

    // Picker profile: case-sensitive, aliases off.
    EXPECT_EQ(Catalog::parseTypeName("AudioBus", Qt::CaseSensitive, false), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("SoundSFX", Qt::CaseSensitive, false), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("AudioFile", Qt::CaseSensitive, false), std::nullopt);
    EXPECT_EQ(Catalog::parseTypeName("Bus", Qt::CaseSensitive, false), T::Bus);
    EXPECT_EQ(Catalog::parseTypeName("Layer", Qt::CaseSensitive, false), T::Layer);
}

TEST(ItemTypeCatalog, BucketMappingRoundTrip)
{
    EXPECT_EQ(Catalog::bucketOf(T::Layer), Bucket::Layers);
    EXPECT_EQ(Catalog::bucketOf(T::Bus), Bucket::Busses);
    EXPECT_EQ(Catalog::bucketOf(T::Action), Bucket::Actions);
    EXPECT_EQ(Catalog::bucketOf(T::Variable), Bucket::Variables);
    EXPECT_EQ(Catalog::bucketOf(T::Attenuation), Bucket::Attenuations);
    EXPECT_EQ(Catalog::bucketOf(T::None), Bucket::None);
    EXPECT_EQ(Catalog::bucketOf(T::Sound), Bucket::Layers);
    EXPECT_EQ(Catalog::bucketOf(T::Container), Bucket::Layers);
    EXPECT_EQ(Catalog::bucketOf(T::Track), Bucket::Layers);
    EXPECT_EQ(Catalog::bucketOf(T::Piece), Bucket::Layers);
    EXPECT_EQ(Catalog::bucketOf(kUnknownType), Bucket::None);

    const std::array<Bucket, 5> expectedOrder = {
        Bucket::Layers, Bucket::Busses, Bucket::Actions,
        Bucket::Variables, Bucket::Attenuations,
    };
    EXPECT_EQ(Catalog::allBuckets(), expectedOrder);

    struct Row { Bucket bucket; const char* folder; const char* display; };
    const Row rows[] = {
        { Bucket::Layers,       "Layers",       "Layer"       },
        { Bucket::Busses,       "Busses",       "Bus"         },
        { Bucket::Actions,      "Actions",      "Action"      },
        { Bucket::Variables,    "Variables",    "Variable"    },
        { Bucket::Attenuations, "Attenuations", "Attenuation" },
    };
    for (const Row& row : rows) {
        EXPECT_EQ(Catalog::folderNameOf(row.bucket), row.folder);
        const Catalog::BucketFacts* facts = Catalog::bucketFactsForFolder(row.folder);
        ASSERT_NE(facts, nullptr);
        EXPECT_EQ(facts->bucket, row.bucket);
        EXPECT_EQ(Catalog::bucketDisplayName(row.folder), row.display);
        EXPECT_EQ(Catalog::bucketFacts(row.bucket), facts);
    }
    EXPECT_EQ(Catalog::bucketFacts(Bucket::None), nullptr);
    EXPECT_EQ(Catalog::bucketFactsForFolder("AudioLayers"), nullptr);
    EXPECT_EQ(Catalog::bucketFactsForFolder("Triggers"), nullptr);
    EXPECT_EQ(Catalog::folderNameOf(Bucket::None), QString());
    EXPECT_EQ(Catalog::bucketDisplayName("Triggers"), "Triggers");
}

TEST(ItemTypeCatalog, BucketTopLevelAndImportTables)
{
    using V = std::vector<T>;
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Layers"),
              (V{ T::Sound, T::Container, T::Track, T::Piece, T::Layer }));
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Busses"), V{});
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Actions"), (V{ T::Action }));
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Variables"),
              (V{ T::Variable }));
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Attenuations"),
              (V{ T::Attenuation }));
    EXPECT_EQ(Catalog::allowedTopLevelTypesForBucket("Nope"), V{});

    EXPECT_TRUE(Catalog::bucketAllowsFolder("Actions"));
    EXPECT_TRUE(Catalog::bucketAllowsFolder("Layers"));
    EXPECT_FALSE(Catalog::bucketAllowsFolder("Busses"));
    EXPECT_TRUE(Catalog::bucketAllowsFolder("Variables"));
    EXPECT_TRUE(Catalog::bucketAllowsFolder("Attenuations"));
    EXPECT_FALSE(Catalog::bucketAllowsFolder("Nope"));

    EXPECT_TRUE(Catalog::bucketAcceptsAudioImport("Layers"));
    EXPECT_FALSE(Catalog::bucketAcceptsAudioImport("Busses"));
    EXPECT_FALSE(Catalog::bucketAcceptsAudioImport("Actions"));
    EXPECT_FALSE(Catalog::bucketAcceptsAudioImport("Variables"));
    EXPECT_FALSE(Catalog::bucketAcceptsAudioImport("Attenuations"));
    EXPECT_FALSE(Catalog::bucketAcceptsAudioImport("Nope"));
}

TEST(ItemTypeCatalog, TypeLevelPredicates)
{
    for (T t : types()) {
        const bool importable = t == T::Sound || t == T::Container
                             || t == T::Track || t == T::Layer;
        EXPECT_EQ(Catalog::acceptsImportedAudio(t), importable) << int(t);
    }
    EXPECT_FALSE(Catalog::acceptsImportedAudio(kUnknownType));

    const auto facts = [](T t) { return Catalog::factsOrNull(t); };
    EXPECT_TRUE(facts(T::Layer)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Action)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Variable)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Attenuation)->topLevelCreatable);
    EXPECT_FALSE(facts(T::Bus)->topLevelCreatable);
    EXPECT_EQ(facts(T::None), nullptr);
    EXPECT_TRUE(facts(T::Sound)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Container)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Track)->topLevelCreatable);
    EXPECT_TRUE(facts(T::Piece)->topLevelCreatable);

    EXPECT_EQ(facts(T::Bus)->bucket, Bucket::Busses);
}

TEST(ItemTypeCatalog, CapabilityPredicateBoundaries)
{
    for (T t : types()) {
        // Content objects: the playable four, nothing else.
        EXPECT_EQ(Catalog::isContentObject(t), isContent(t)) << int(t);
        // Mixing capability: content four + Layer + Bus.
        EXPECT_EQ(Catalog::hasMixingCapability(t),
                  isContent(t) || t == T::Layer || t == T::Bus) << int(t);
        // Triggerable: content objects and Action; Layer never (hard boundary 2).
        EXPECT_EQ(Catalog::isTriggerable(t), isContent(t) || t == T::Action) << int(t);
        // Loop is routing capability: content four only.
        EXPECT_EQ(Catalog::allowsLoop(t), isContent(t)) << int(t);
        EXPECT_EQ(Catalog::isTimelineClipTarget(t),
                  t == T::Sound || t == T::Container || t == T::Piece) << int(t);
    }
    EXPECT_FALSE(Catalog::isContentObject(kUnknownType));
    EXPECT_FALSE(Catalog::hasMixingCapability(kUnknownType));
    EXPECT_FALSE(Catalog::isTriggerable(kUnknownType));
    EXPECT_FALSE(Catalog::allowsLoop(kUnknownType));
    EXPECT_FALSE(Catalog::isTimelineClipTarget(kUnknownType));

    EXPECT_FALSE(Catalog::isTriggerable(T::Layer));
    EXPECT_FALSE(Catalog::allowsLoop(T::Layer));
    EXPECT_FALSE(Catalog::allowsLoop(T::Bus));
    EXPECT_TRUE(Catalog::hasMixingCapability(T::Layer));
    EXPECT_TRUE(Catalog::hasMixingCapability(T::Bus));
}

TEST(ItemTypeCatalog, NameNamespaceValuesFrozen)
{
    EXPECT_EQ(int(NameNamespace::None), 0);
    EXPECT_EQ(int(NameNamespace::AudioNode), 1);
    EXPECT_EQ(int(NameNamespace::Action), 2);
    EXPECT_EQ(int(NameNamespace::Attenuation), 3);
    EXPECT_EQ(int(NameNamespace::Bus), 4);
    EXPECT_EQ(int(NameNamespace::Variable), 5);

    EXPECT_EQ(Catalog::nameNamespaceOf(T::Sound), NameNamespace::AudioNode);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Container), NameNamespace::AudioNode);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Track), NameNamespace::AudioNode);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Piece), NameNamespace::AudioNode);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Layer), NameNamespace::AudioNode);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Action), NameNamespace::Action);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Variable), NameNamespace::Variable);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Attenuation), NameNamespace::Attenuation);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::Bus), NameNamespace::Bus);
    EXPECT_EQ(Catalog::nameNamespaceOf(T::None), NameNamespace::None);
    EXPECT_EQ(Catalog::nameNamespaceOf(kUnknownType), NameNamespace::None);
}

TEST(ItemTypeCatalog, CreationSpecsCurrentValues)
{
    using CP = Catalog::CreateParentPolicy;
    const auto& specs = Catalog::creationSpecs();
    ASSERT_EQ(specs.size(), 9u);

    struct Row {
        T type;
        const char* defaultName;
        CP policy;
        T requiredParent;
        bool selectAfter;
    };
    const Row rows[] = {
        { T::Action,      "Play_",               CP::DefaultForType, T::None, true  },
        { T::Sound,       "New_Sound",           CP::DefaultForType, T::None, true  },
        { T::Container,   "New_Container",       CP::DefaultForType, T::None, true  },
        { T::Track,       "New_Track",           CP::DefaultForType, T::None, true  },
        { T::Piece,       "New_Piece",           CP::DefaultForType, T::None, true  },
        { T::Layer,       "New_Layer",           CP::DefaultForType, T::None, true  },
        { T::Attenuation, "Default_Attenuation", CP::DefaultForType, T::None, true  },
        { T::Variable,    "New_Variable",        CP::DefaultForType, T::None, true  },
        { T::Bus,         "New_Bus",             CP::SelectedItem,   T::Bus,    true  },
    };
    for (const Row& row : rows) {
        const auto spec = std::find_if(specs.begin(), specs.end(),
            [&row](const Catalog::CreationSpec& entry) { return entry.type == row.type; });
        ASSERT_NE(spec, specs.end());
        EXPECT_STREQ(spec->defaultName, row.defaultName);
        EXPECT_EQ(spec->parentPolicy, row.policy);
        EXPECT_EQ(spec->requiredParent, row.requiredParent);
        EXPECT_EQ(spec->selectAfter, row.selectAfter);
        EXPECT_FALSE(spec->separatorAfter);
        EXPECT_FALSE(spec->comingSoon);
    }
}

TEST(ItemTypeCatalog, TrackAndPieceParticipateInAuthoringWithoutChangingTopology)
{
    EXPECT_TRUE(Catalog::isUserFacingType(T::Track));
    EXPECT_TRUE(Catalog::isUserFacingType(T::Piece));
    EXPECT_EQ(Catalog::userFacingChildTypesOf(T::Layer),
              (std::vector<T>{T::Sound, T::Container, T::Track, T::Piece, T::Layer}));
    EXPECT_EQ(Catalog::userFacingChildTypesOf(T::Track),
              (std::vector<T>{T::Sound, T::Container, T::Piece}));
    EXPECT_EQ(Catalog::userFacingChildTypesOf(T::Piece), (std::vector<T>{T::Track}));
}
