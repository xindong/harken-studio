// Resolved-slot ticket validation tests.
//
// The runtime half of reference-slot resolution consumes a decided value and
// checks it. What has to be pinned is the refusing: a dangling bus, an
// incompatible layout and a zero target each have to fail for their own reason
// and before admission, because the alternative is finding out while a voice is
// already starting. The lease cases pin that a validated target stops being
// valid when the bus it was validated against is replaced.

#include "Common/ObjectIdTestSupport.h"
#include "harken/media/ResolvedSlotTicket.h"

#include <gtest/gtest.h>

#include <cstdint>

using harken::BusTargetLease;
using harken::BusTargetLeaseStillValid;
using harken::ResolvedSlotEnvironment;
using harken::ResolvedSlotKind;
using harken::ResolvedSlotRefusal;
using harken::ResolvedSlotTicket;
using harken::ValidateResolvedSlot;

namespace {

// A session small enough to state in a line: one bus that may or may not exist,
// carry a compatible layout, and sit at some generation.
struct Session {
    VnBusId bus = harkentest::ObjectId(7);
    bool busPresent = true;
    bool layoutCompatible = true;
    std::uint32_t generation = 3;
    VnObjectId attenuation = harkentest::ObjectId(55);
    bool attenuationPresent = true;
};

bool BusExists(void* ctx, VnBusId bus)
{
    const Session* s = static_cast<const Session*>(ctx);
    return s->busPresent && bus == s->bus;
}

bool BusLayoutCompatible(void* ctx, VnBusId bus)
{
    const Session* s = static_cast<const Session*>(ctx);
    return s->layoutCompatible && bus == s->bus;
}

std::uint32_t BusGeneration(void* ctx, VnBusId bus)
{
    const Session* s = static_cast<const Session*>(ctx);
    return (s->busPresent && bus == s->bus) ? s->generation : 0;
}

bool AttenuationExists(void* ctx, VnObjectId id)
{
    const Session* s = static_cast<const Session*>(ctx);
    return s->attenuationPresent && id == s->attenuation;
}


ResolvedSlotEnvironment EnvOver(Session& session)
{
    ResolvedSlotEnvironment env;
    env.ctx = &session;
    env.busExists = &BusExists;
    env.busLayoutCompatible = &BusLayoutCompatible;
    env.busGeneration = &BusGeneration;
    env.attenuationExists = &AttenuationExists;
    return env;
}

ResolvedSlotTicket BusTicket(std::uint64_t value)
{
    ResolvedSlotTicket ticket;
    ticket.kind = ResolvedSlotKind::OutputBus;
    ticket.value = harkentest::ObjectId(value);
    return ticket;
}

} // namespace

TEST(ResolvedSlotTicket, ValidatedBusTargetYieldsALeaseAtItsGeneration)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    BusTargetLease lease;

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), env, &lease),
              ResolvedSlotRefusal::None);
    EXPECT_EQ(lease.bus, harkentest::ObjectId(7u));
    EXPECT_EQ(lease.generation, 3u);
    EXPECT_TRUE(BusTargetLeaseStillValid(lease, env));
}

// A target that no longer exists must fail here rather than at play time.
TEST(ResolvedSlotTicket, RefusesADanglingBusTarget)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    BusTargetLease lease;

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(999), env, &lease),
              ResolvedSlotRefusal::DanglingTarget);
    // A refusal writes nothing, so a caller cannot mistake a stale lease for a
    // validated one.
    EXPECT_EQ(lease.bus, harkentest::ObjectId(0u));
    EXPECT_EQ(lease.generation, 0u);
}

// Existing but unusable is its own refusal: the diagnostic has to say which of
// the two problems the target has, because the fixes differ.
TEST(ResolvedSlotTicket, RefusesAnIncompatibleLayoutSeparatelyFromAbsence)
{
    Session session;
    session.layoutCompatible = false;
    ResolvedSlotEnvironment env = EnvOver(session);

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), env, nullptr),
              ResolvedSlotRefusal::IncompatibleLayout);
}

// 0 reaches the runtime only from a program that failed to resolve. Treating it
// as "use the default" would put a fallback back into the runtime, which is
// exactly what keeping resolution on the compile side rules out.
TEST(ResolvedSlotTicket, RefusesAZeroTargetInsteadOfSubstitutingADefault)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(0), env, nullptr),
              ResolvedSlotRefusal::MissingTarget);
}

// An environment that cannot answer is not an environment in which the target
// may be used, so absent callbacks refuse rather than pass.
TEST(ResolvedSlotTicket, RefusesWhenTheEnvironmentCannotConfirmTheTarget)
{
    ResolvedSlotEnvironment silent;

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), silent, nullptr),
              ResolvedSlotRefusal::DanglingTarget);
}

TEST(ResolvedSlotTicket, RefusesABusWhenGenerationCannotBeQueried)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    env.busGeneration = nullptr;
    BusTargetLease lease{harkentest::ObjectId(99), 99};

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), env, &lease),
              ResolvedSlotRefusal::DanglingTarget);
    EXPECT_EQ(lease.bus, harkentest::ObjectId(0u));
    EXPECT_EQ(lease.generation, 0u);
    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), env, nullptr),
              ResolvedSlotRefusal::DanglingTarget);
    EXPECT_FALSE(BusTargetLeaseStillValid(BusTargetLease{harkentest::ObjectId(7), 3}, env));
}

TEST(ResolvedSlotTicket, RefusesZeroAsABusGeneration)
{
    Session session;
    session.generation = 0;
    ResolvedSlotEnvironment env = EnvOver(session);
    BusTargetLease lease{harkentest::ObjectId(99), 99};

    EXPECT_EQ(ValidateResolvedSlot(BusTicket(7), env, &lease),
              ResolvedSlotRefusal::DanglingTarget);
    EXPECT_EQ(lease.bus, harkentest::ObjectId(0u));
    EXPECT_EQ(lease.generation, 0u);
    EXPECT_FALSE(BusTargetLeaseStillValid(BusTargetLease{harkentest::ObjectId(7), 0}, env));
    EXPECT_FALSE(BusTargetLeaseStillValid(BusTargetLease{harkentest::ObjectId(7), 3}, env));
}

TEST(ResolvedSlotTicket, ValidatesAttenuationTarget)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);

    ResolvedSlotTicket attenuation;
    attenuation.kind = ResolvedSlotKind::Attenuation;
    attenuation.value = harkentest::ObjectId(55);
    EXPECT_EQ(ValidateResolvedSlot(attenuation, env, nullptr),
              ResolvedSlotRefusal::None);
    attenuation.value = harkentest::ObjectId(56);
    EXPECT_EQ(ValidateResolvedSlot(attenuation, env, nullptr),
              ResolvedSlotRefusal::DanglingTarget);


}

// A ticket naming an ordinal this runtime does not know is refused rather than
// coerced into one it does, which is what stops a program from a newer numbering
// being read as an older kind of target.
TEST(ResolvedSlotTicket, RefusesAnUnknownSlotOrdinal)
{
    static_assert(static_cast<std::uint32_t>(harken::ResolvedSlotTargetKind::Bus) == 2);
    static_assert(static_cast<std::uint32_t>(harken::ResolvedSlotTargetKind::Attenuation) == 0);
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    ResolvedSlotTicket ticket;
    ticket.value = harkentest::ObjectId(7);

    for (const auto ordinal : {2u, 99u}) {
        ticket.kind = static_cast<ResolvedSlotKind>(ordinal);
        EXPECT_EQ(ValidateResolvedSlot(ticket, env, nullptr),
                  ResolvedSlotRefusal::UnknownSlotKind);
    }
}

// The lease exists to make a replacement visible. Holding only the id would let
// audio keep routing into whatever now occupies it.
TEST(ResolvedSlotTicket, LeaseStopsBeingValidWhenTheBusGenerationMoves)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    BusTargetLease lease;
    ASSERT_EQ(ValidateResolvedSlot(BusTicket(7), env, &lease),
              ResolvedSlotRefusal::None);

    session.generation = 4;
    EXPECT_FALSE(BusTargetLeaseStillValid(lease, env));
}

TEST(ResolvedSlotTicket, LeaseStopsBeingValidWhenTheBusIsGone)
{
    Session session;
    ResolvedSlotEnvironment env = EnvOver(session);
    BusTargetLease lease;
    ASSERT_EQ(ValidateResolvedSlot(BusTicket(7), env, &lease),
              ResolvedSlotRefusal::None);

    session.busPresent = false;
    EXPECT_FALSE(BusTargetLeaseStillValid(lease, env));

    BusTargetLease never;
    EXPECT_FALSE(BusTargetLeaseStillValid(never, env));
}
