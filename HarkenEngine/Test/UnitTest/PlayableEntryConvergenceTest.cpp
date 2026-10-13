#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"

#include <array>
#include <functional>

#include <gtest/gtest.h>

namespace {

TEST(PlayableEntryConvergence, CurrentAndFrozenAuthoritiesShareCanonicalRoot) {
    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = harkentest::ObjectId(7001);
    property.programSemanticFingerprint = 9001;
    property.contentRevision = 3;
    property.routeStableId = harkentest::ObjectId(11);
    property.compatibilityFingerprint = 22;

    harken::PlayableWorldViewV1 world{};
    world.generation = 41;
    world.identity.runtimeSemanticFingerprint = 900;
    world.properties = &property;
    world.propertyCount = 1;

    harken::PlayableExecutionRequest request{};
    request.root.kind = harken::PlayableKindV1::Sound;
    request.root.stableId = property.stableId;
    request.root.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    request.root.contentRevision = property.contentRevision;
    request.root.routeStableId = property.routeStableId;
    request.root.compatibilityFingerprint = property.compatibilityFingerprint;

    const auto current = harken::ResolvePlayableExecutionEntry(
        harken::PlayableExecutionAuthority::Current(world), request);
    const auto frozen = harken::ResolvePlayableExecutionEntry(
        harken::PlayableExecutionAuthority::Frozen(world), request);
    EXPECT_EQ(harken::PlayableExecutionResolveStatus::Ok, current.status);
    EXPECT_EQ(current.status, frozen.status);
    EXPECT_EQ(current.root.kind, frozen.root.kind);
    EXPECT_EQ(current.root.stableId, frozen.root.stableId);
    EXPECT_EQ(current.programSemanticFingerprint,
              frozen.programSemanticFingerprint);
    EXPECT_EQ(current.authorityIdentity, frozen.authorityIdentity);
}

TEST(PlayableEntryConvergence, InvalidEntryUsesTypedAdapterNeutralError) {
    harken::PlayableWorldViewV1 world{};
    harken::PlayableExecutionRequest request{};
    request.root.kind = harken::PlayableKindV1::Sound;
    request.root.stableId = harkentest::ObjectId(7001);
    request.root.expectedProgramSemanticFingerprint = 9001;

    const auto current = harken::ResolvePlayableExecutionEntry(
        harken::PlayableExecutionAuthority::Current(world), request);
    const auto frozen = harken::ResolvePlayableExecutionEntry(
        harken::PlayableExecutionAuthority::Frozen(world), request);
    EXPECT_EQ(harken::PlayableExecutionResolveStatus::UnknownPlayable,
              current.status);
    EXPECT_EQ(current.status, frozen.status);
}

TEST(PlayableEntryConvergence,
     SoundAndContainerRequireExactCanonicalReferenceIdentity) {
    harken::PlayablePropertyProgramViewV1 property{};
    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Sound;
    property.stableId = harkentest::ObjectId(7002);
    property.programSemanticFingerprint = 9002;
    property.contentRevision = 4;
    property.routeStableId = harkentest::ObjectId(12);
    property.compatibilityFingerprint = 23;

    harken::PlayableWorldViewV1 world{};
    world.generation = 42;
    world.properties = &property;
    world.propertyCount = 1;

    harken::PlayableExecutionRequest request{};
    request.root.kind = harken::PlayableKindV1::Sound;
    request.root.stableId = property.stableId;
    request.root.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    request.root.contentRevision = property.contentRevision;
    request.root.routeStableId = property.routeStableId;
    request.root.compatibilityFingerprint =
        property.compatibilityFingerprint;

    const auto authority = harken::PlayableExecutionAuthority::Current(world);
    EXPECT_EQ(harken::PlayableExecutionResolveStatus::Ok,
              authority.Resolve(request).status);

    const std::array<std::function<void(harken::PlayableExecutionRequest&)>, 4>
        mutations = {{
            [](auto& candidate) {
                ++candidate.root.expectedProgramSemanticFingerprint;
            },
            [](auto& candidate) { ++candidate.root.contentRevision; },
            [](auto& candidate) { candidate.root.routeStableId = harkentest::OffsetObjectId(candidate.root.routeStableId, 1); },
            [](auto& candidate) {
                ++candidate.root.compatibilityFingerprint;
            },
        }};
    for (const auto& mutate : mutations) {
        auto candidate = request;
        mutate(candidate);
        EXPECT_EQ(harken::PlayableExecutionResolveStatus::FingerprintMismatch,
                  authority.Resolve(candidate).status);
    }

    property.contentPropertyKind =
        harken::playableproperties::ContentPropertyKindV1::Container;
    request.root.kind = harken::PlayableKindV1::Container;
    EXPECT_EQ(harken::PlayableExecutionResolveStatus::Ok,
              authority.Resolve(request).status);
    for (const auto& mutate : mutations) {
        auto candidate = request;
        mutate(candidate);
        EXPECT_EQ(harken::PlayableExecutionResolveStatus::FingerprintMismatch,
                  authority.Resolve(candidate).status);
    }
}

} // namespace
