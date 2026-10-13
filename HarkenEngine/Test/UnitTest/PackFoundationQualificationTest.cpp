#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"
#include "UnitTest/CanonicalPackSetActivate.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using namespace harkentest::packfoundation;

std::filesystem::path FixtureDirectory(const char* name) {
    return std::filesystem::path(HARKEN_TEMP_DIR) / name;
}

VnConfig QualificationConfig(const Fixture& fixture) {
    VnConfig config = MakeNullConfig();
    config.maxPacks = 4;
    config.maxActions = fixture.residentCount;
    config.maxCatalogEntries = fixture.catalogCount;
    config.maxCatalogBytes = fixture.catalogBytes;
    config.maxResidentDefinitionBytes = fixture.initBodyBytes + fixture.residentBodyBytes;
    config.maxCompositeIndexBytes = 4ull << 20;
    config.maxPackDecodeStagingBytes = 32ull << 20;
    return config;
}

VnResult ActivateFixture(const std::filesystem::path& directory,
                         bool includeContent,
                         VnCanonicalPackSetMemberResultV1 (&rows)[2],
                         VnCanonicalPackSetActivationReportV1& report,
                         VnPackHandle previous = 0,
                         std::uint64_t base = 0) {
    const std::string root = directory.string();
    const std::string paths[] = {(directory / "Init.pack").string(),
                                 (directory / "Content.pack").string()};
    VnCanonicalPackSetMemberV1 members[2]{};
    for (size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        members[i].utf8Path = paths[i].c_str();
        members[i].utf8ContainmentRoot = root.c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].packStableId = kInitPackStableId;
    members[0].expectedHandle = previous;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].packStableId = kResidentPackStableId;
    return harkentest::ActivateCanonicalMembersV1(
        members, includeContent ? 2u : 1u, rows, report, previous, base);
}

TEST(PackFoundationQualification, CatalogMayExceedResidentWindow) {
    Fixture fixture;
    ASSERT_TRUE(BuildFixture(1000, 64, &fixture));
    const std::filesystem::path directory =
        FixtureDirectory("f3-catalog-resident-window");
    ASSERT_TRUE(WriteFixture(directory, fixture));

    const std::string initPath = (directory / "Init.pack").string();
    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.utf8Path = initPath.c_str();
    VnInitPackInspectReport inspection{};
    inspection.structSize = sizeof(inspection);
    ASSERT_EQ(VN_OK, Vn_InspectInitPack(&inspectDesc, &inspection));
    EXPECT_EQ(1000u, inspection.catalogEntries);
    EXPECT_EQ(fixture.catalogBytes, inspection.catalogBytes);

    EngineSession session(QualificationConfig(fixture));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    VnCanonicalPackSetActivationReportV1 activation{};
    ASSERT_EQ(VN_OK, ActivateFixture(directory, true, rows, activation));

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ObjectId(kActionBase);
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    EXPECT_NE(0u, firing);
    EXPECT_TRUE(session.Engine()->packComposition.HasLiveLease(
        kResidentPackStableId, 1, 1));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    EXPECT_FALSE(session.Engine()->packComposition.HasLiveLease(
        kResidentPackStableId, 1, 1));

    post.actionId = harkentest::ObjectId(kActionBase + fixture.residentCount);
    firing = 0;
    EXPECT_EQ(VN_CONTENT_NOT_LOADED, Vn_PostAction(&post, &firing));
    EXPECT_EQ(0u, firing);

    post.actionId = harkentest::ObjectId(kActionBase + fixture.catalogCount);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_PostAction(&post, &firing));
}

TEST(PackFoundationQualification, CatalogBudgetsPinMinusExactPlus) {
    Fixture fixture;
    ASSERT_TRUE(BuildFixture(1000, 64, &fixture));
    const std::filesystem::path directory =
        FixtureDirectory("f3-catalog-budgets");
    ASSERT_TRUE(WriteFixture(directory, fixture));

    {
        VnConfig config = QualificationConfig(fixture);
        config.maxCatalogEntries = fixture.catalogCount - 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        VnCanonicalPackSetActivationReportV1 activation{};
        EXPECT_EQ(VN_PACK_CATALOG_CAPACITY,
                  ActivateFixture(directory, false, rows, activation));
        EXPECT_EQ(0u, rows[0].handle);
    }
    {
        VnConfig config = QualificationConfig(fixture);
        config.maxCatalogBytes = fixture.catalogBytes - 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        VnCanonicalPackSetActivationReportV1 activation{};
        EXPECT_EQ(VN_PACK_CATALOG_CAPACITY,
                  ActivateFixture(directory, false, rows, activation));
        EXPECT_EQ(0u, rows[0].handle);
    }
    {
        VnConfig config = QualificationConfig(fixture);
        --config.maxResidentDefinitionBytes;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 initialRows[2]{};
        VnCanonicalPackSetActivationReportV1 initial{};
        ASSERT_EQ(VN_OK, ActivateFixture(directory, false, initialRows, initial));
        const auto generation = session.Engine()->packComposition.CompositeGeneration();
        VnCanonicalPackSetMemberResultV1 refusedRows[2]{};
        VnCanonicalPackSetActivationReportV1 rejection{};
        EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
                  ActivateFixture(directory, true, refusedRows, rejection,
                                  initial.initHandle, initial.resultSetRevision));
        EXPECT_EQ(0u, refusedRows[1].handle);
        EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
        EXPECT_EQ(harken::PackIdentityReadiness::KnownUnloaded,
                  session.Engine()->packComposition.Lookup(harkentest::ObjectId(kActionBase))
                      .readiness);
        EXPECT_EQ(fixture.initBodyBytes + fixture.residentBodyBytes,
                  session.Engine()->packComposition.Facts()
                      .lastRefusalRequired);
        EXPECT_EQ(config.maxResidentDefinitionBytes,
                  session.Engine()->packComposition.Facts()
                      .lastRefusalConfigured);
    }
    {
        VnConfig config = QualificationConfig(fixture);
        config.maxActions = fixture.residentCount - 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 initialRows[2]{};
        VnCanonicalPackSetActivationReportV1 initial{};
        ASSERT_EQ(VN_OK, ActivateFixture(directory, false, initialRows, initial));
        const auto generation = session.Engine()->packComposition.CompositeGeneration();
        VnCanonicalPackSetMemberResultV1 refusedRows[2]{};
        VnCanonicalPackSetActivationReportV1 rejection{};
        EXPECT_EQ(VN_PACK_RESIDENT_CAPACITY,
                  ActivateFixture(directory, true, refusedRows, rejection,
                                  initial.initHandle, initial.resultSetRevision));
        EXPECT_EQ(0u, refusedRows[1].handle);
        EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
        EXPECT_EQ(harken::PackIdentityReadiness::KnownUnloaded,
                  session.Engine()->packComposition.Lookup(harkentest::ObjectId(kActionBase))
                      .readiness);
        EXPECT_EQ(fixture.residentCount,
                  session.Engine()->packComposition.Facts()
                      .lastRefusalRequired);
        EXPECT_EQ(fixture.residentCount - 1,
                  session.Engine()->packComposition.Facts()
                      .lastRefusalConfigured);
    }
    {
        EngineSession session(QualificationConfig(fixture));
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        VnCanonicalPackSetActivationReportV1 activation{};
        EXPECT_EQ(VN_OK, ActivateFixture(directory, true, rows, activation));
        EXPECT_NE(0u, rows[0].handle);
        EXPECT_NE(0u, rows[1].handle);
    }
    {
        VnConfig config = QualificationConfig(fixture);
        config.maxCatalogEntries = fixture.catalogCount + 1;
        config.maxCatalogBytes = fixture.catalogBytes + 1;
        ++config.maxResidentDefinitionBytes;
        config.maxActions = fixture.residentCount + 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        VnCanonicalPackSetActivationReportV1 activation{};
        EXPECT_EQ(VN_OK, ActivateFixture(directory, true, rows, activation));
        EXPECT_NE(0u, rows[0].handle);
        EXPECT_NE(0u, rows[1].handle);
    }
    const std::uint64_t indexBytes = (fixture.residentCount + 1ull) *
        sizeof(harken::PackComposition::IdentityIndexEntry);
    for (int delta = -1; delta <= 1; ++delta) {
        SCOPED_TRACE(delta);
        VnConfig config = QualificationConfig(fixture);
        config.maxCompositeIndexBytes = indexBytes + delta;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        VnCanonicalPackSetMemberResultV1 initialRows[2]{};
        VnCanonicalPackSetActivationReportV1 initial{};
        ASSERT_EQ(VN_OK, ActivateFixture(directory, false, initialRows, initial));
        const auto generation = session.Engine()->packComposition.CompositeGeneration();
        VnCanonicalPackSetMemberResultV1 rows[2]{};
        VnCanonicalPackSetActivationReportV1 activation{};
        EXPECT_EQ(delta < 0 ? VN_PACK_RESIDENT_CAPACITY : VN_OK,
                  ActivateFixture(directory, true, rows, activation,
                                  initial.initHandle, initial.resultSetRevision));
        const VnPackHandle resident = rows[1].handle;
        if (delta < 0) {
            EXPECT_EQ(0u, resident);
            EXPECT_EQ(generation, session.Engine()->packComposition.CompositeGeneration());
            EXPECT_EQ(indexBytes, session.Engine()->packComposition.Facts().lastRefusalRequired);
            EXPECT_EQ(config.maxCompositeIndexBytes,
                      session.Engine()->packComposition.Facts().lastRefusalConfigured);
        } else {
            EXPECT_NE(0u, resident);
            EXPECT_EQ(indexBytes, session.Engine()->packComposition.Facts().loadedBytes);
        }
    }
}

}
