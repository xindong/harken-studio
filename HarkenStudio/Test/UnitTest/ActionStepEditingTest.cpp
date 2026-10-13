#include "ActionStepEditing.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <limits>
#include <vector>

using VNX::VActionStep;
using VNX::VariableScope;
namespace Edit = VNX::ActionStepEditing;

namespace {

QJsonObject step(const QString& type, qint64 targetObjectID,
                 const QString& targetName, double delay, double fadeTime = 0.0)
{
    QJsonObject o;
    o[QStringLiteral("type")] = type;
    o[QStringLiteral("targetObjectID")] = targetObjectID;
    o[QStringLiteral("targetName")] = targetName;
    o[QStringLiteral("delay")] = delay;
    o[QStringLiteral("fadeTime")] = fadeTime;
    return o;
}

QJsonArray array(std::initializer_list<QJsonObject> objs)
{
    QJsonArray a;
    for (const auto& o : objs)
        a.append(o);
    return a;
}

QJsonObject quantize(qint64 referenceObjectID, const QString& referenceName,
                     const QString& timing, const QString& ifSilent)
{
    return QJsonObject{
        {QStringLiteral("referenceObjectID"), referenceObjectID},
        {QStringLiteral("referenceName"), referenceName},
        {QStringLiteral("timing"), timing},
        {QStringLiteral("ifSilent"), ifSilent},
    };
}

} // namespace

// --- stepsFromJson: type filtering ---

TEST(ActionStepEditing, KeepsThreeSupportedTypes)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 10, QStringLiteral("A"), 0.0),
        step(QStringLiteral("Stop"), 11, QStringLiteral("B"), 0.0, 250.0),
        step(QStringLiteral("Post Action"), 12, QStringLiteral("C"), 0.0),
    }));
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_EQ(steps[0].type, VActionStep::Type::Play);
    EXPECT_EQ(steps[1].type, VActionStep::Type::Stop);
    EXPECT_EQ(steps[2].type, VActionStep::Type::PostAction);
}

TEST(ActionStepEditing, KeepsFiveSupportedTypes)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0),
        step(QStringLiteral("Stop"), 2, QStringLiteral("B"), 0.0),
        step(QStringLiteral("SetVariable"), 3, QStringLiteral("C"), 0.0),
        step(QStringLiteral("PostAction"), 4, QStringLiteral("D"), 0.0),
        step(QStringLiteral("Reset"), 5, QStringLiteral("E"), 0.0),
    }));
    ASSERT_EQ(steps.size(), 5u);
    EXPECT_EQ(steps[0].type, VActionStep::Type::Play);
    EXPECT_EQ(steps[1].type, VActionStep::Type::Stop);
    EXPECT_EQ(steps[2].type, VActionStep::Type::SetVariable);
    EXPECT_EQ(steps[3].type, VActionStep::Type::PostAction);
    EXPECT_EQ(steps[4].type, VActionStep::Type::Reset);
}

TEST(ActionStepEditing, DropsRetiredTypesAndPreservesOrder)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 10, QStringLiteral("A"), 0.0),
        step(QStringLiteral("Pause"), 20, QStringLiteral("X"), 0.0),
        step(QStringLiteral("Set Voice Volume"), 21, QStringLiteral("Y"), 0.0),
        step(QStringLiteral("Post Action"), 12, QStringLiteral("C"), 0.0),
    }));
    ASSERT_EQ(steps.size(), 2u);
    EXPECT_EQ(steps[0].type, VActionStep::Type::Play);
    EXPECT_EQ(steps[0].targetObjectID, 10u);
    EXPECT_EQ(steps[1].type, VActionStep::Type::PostAction);
    EXPECT_EQ(steps[1].targetObjectID, 12u);
}

TEST(ActionStepEditing, SetVariableScopeIsPerStepAndRoundtrips)
{
    auto local = step(QStringLiteral("SetVariable"), 3, QStringLiteral("X"), 0.0);
    auto global = local;
    global[QStringLiteral("scope")] = QStringLiteral("Global");
    auto rows = Edit::stepsFromJson(array({local, global}));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].scope, VariableScope::Emitter);
    EXPECT_EQ(rows[1].scope, VariableScope::Global);
    rows[1].targetObjectID = 4;
    const auto roundtrip = Edit::stepsFromJson(Edit::stepsToJson(rows));
    ASSERT_EQ(roundtrip.size(), 2u);
    EXPECT_EQ(roundtrip[0].scope, VariableScope::Emitter);
    EXPECT_EQ(roundtrip[1].scope, VariableScope::Global);
    EXPECT_EQ(roundtrip[1].targetObjectID, 4u);
    EXPECT_TRUE(Edit::stepsFromJson(array({
        step(QStringLiteral("SetValue"), 3, QStringLiteral("X"), 0.0),
        step(QStringLiteral("Set Value"), 3, QStringLiteral("X"), 0.0)})).empty());
}

TEST(ActionStepEditing, DropsUnknownAndMissingType)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Garbage"), 1, QStringLiteral("A"), 0.0),
        step(QString(), 2, QStringLiteral("B"), 0.0),
        step(QStringLiteral("Play"), 3, QStringLiteral("C"), 0.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 3u);
}

// --- stepsFromJson: field extraction & normalization ---

TEST(ActionStepEditing, ExtractsTargetAndDelay)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 42, QStringLiteral("Explosion"), 1000.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 42u);
    EXPECT_EQ(steps[0].targetName, QStringLiteral("Explosion"));
    EXPECT_FLOAT_EQ(steps[0].delay, 1000.0f);
}

TEST(ActionStepEditing, FadeTimeSurvivesOnlyForStop)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0, 500.0),
        step(QStringLiteral("Stop"), 2, QStringLiteral("B"), 0.0, 500.0),
        step(QStringLiteral("Post Action"), 3, QStringLiteral("C"), 0.0, 500.0),
    }));
    ASSERT_EQ(steps.size(), 3u);
    EXPECT_FLOAT_EQ(steps[0].fadeTime, 0.0f);
    EXPECT_FLOAT_EQ(steps[1].fadeTime, 500.0f);
    EXPECT_FLOAT_EQ(steps[2].fadeTime, 0.0f);
}

TEST(ActionStepEditing, ClampsNegativeDelayAndFade)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Stop"), 1, QStringLiteral("A"), -5.0, -250.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_FLOAT_EQ(steps[0].delay, 0.0f);
    EXPECT_FLOAT_EQ(steps[0].fadeTime, 0.0f);
}

TEST(ActionStepEditing, MissingFieldsUseDefaults)
{
    QJsonObject minimal;
    minimal[QStringLiteral("type")] = QStringLiteral("Play");
    const auto steps = Edit::stepsFromJson(array({minimal}));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 0u);
    EXPECT_TRUE(steps[0].targetName.isEmpty());
    EXPECT_FLOAT_EQ(steps[0].delay, 0.0f);
}

TEST(ActionStepEditing, KeepsEmptyTargetStep)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 0, QString(), 100.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 0u);
    EXPECT_TRUE(steps[0].targetName.isEmpty());
}

TEST(ActionStepEditing, EmptyArrayYieldsEmpty)
{
    EXPECT_TRUE(Edit::stepsFromJson(QJsonArray{}).empty());
}

// --- write-boundary numeric validation (S4.5-C / S4.5-D) ---

TEST(ActionStepEditing, NegativeObjectIdBecomesEmptyTarget)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), -5, QStringLiteral("A"), 0.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 0u);
}

TEST(ActionStepEditing, OverflowObjectIdDoesNotWrap)
{
    QJsonObject o = step(QStringLiteral("Play"), 0, QStringLiteral("A"), 0.0);
    o[QStringLiteral("targetObjectID")] =
        static_cast<qint64>(std::numeric_limits<uint32_t>::max()) + 1;
    const auto steps = Edit::stepsFromJson(array({o}));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, 0u);
}

TEST(ActionStepEditing, MaxObjectIdIsPreserved)
{
    QJsonObject o = step(QStringLiteral("Play"), 0, QStringLiteral("A"), 0.0);
    o[QStringLiteral("targetObjectID")] =
        static_cast<qint64>(std::numeric_limits<uint32_t>::max());
    const auto steps = Edit::stepsFromJson(array({o}));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetObjectID, std::numeric_limits<uint32_t>::max());
}

TEST(ActionStepEditing, DelayAndFadeClampToDomainMax)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Stop"), 1, QStringLiteral("A"), 90000.0, 90000.0),
    }));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_FLOAT_EQ(steps[0].delay, Edit::kMaxDelayMs);
    EXPECT_FLOAT_EQ(steps[0].fadeTime, Edit::kMaxFadeTimeMs);
}

TEST(ActionStepEditing, NonFiniteDelayAndFadeBecomeZero)
{
    QJsonObject o = step(QStringLiteral("Stop"), 1, QStringLiteral("A"), 0.0, 0.0);
    o[QStringLiteral("delay")] = std::numeric_limits<double>::infinity();
    o[QStringLiteral("fadeTime")] = std::nan("");
    const auto steps = Edit::stepsFromJson(array({o}));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_FLOAT_EQ(steps[0].delay, 0.0f);
    EXPECT_FLOAT_EQ(steps[0].fadeTime, 0.0f);
}

TEST(ActionStepEditing, NoOpDiffIsCanonicallyEqual)
{
    // Mirrors handleStepsItemProperty's no-op guard: canonically identical
    // tables compare equal even when JSON key order / extra keys differ.
    QJsonObject noisy = step(QStringLiteral("Play"), 3, QStringLiteral("C"), 100.0, 999.0);
    noisy[QStringLiteral("garbage")] = 42;
    const QJsonArray a = Edit::stepsToJson(Edit::stepsFromJson(array({noisy})));
    const QJsonArray b = Edit::stepsToJson(Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 3, QStringLiteral("C"), 100.0),
    })));
    EXPECT_EQ(a, b);
}

TEST(ActionStepEditing, CheckedParserRejectsLossyLedgers)
{
    QString error;
    EXPECT_FALSE(Edit::checkedStepsFromJson(
        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("Garbage")}}},
        &error));
    EXPECT_FALSE(error.isEmpty());

    QJsonObject invalid = step(QStringLiteral("Play"), 1,
                               QStringLiteral("A"), 0.0);
    invalid[QStringLiteral("delay")] = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(Edit::checkedStepsFromJson(QJsonArray{invalid}, &error));

    QJsonObject reset = step(QStringLiteral("Reset"), 1,
                             QStringLiteral("Container"), 0.0);
    reset[QStringLiteral("resetScope")] = QStringLiteral("Invalid");
    EXPECT_FALSE(Edit::checkedStepsFromJson(QJsonArray{reset}, &error));
}

TEST(ActionStepEditing, CheckedParserKeepsCanonicalFiveVerbLedger)
{
    QJsonObject play = step(QStringLiteral("Play"), 1,
                            QStringLiteral("A"), 0.0);
    play[QStringLiteral("quantize")] = quantize(
        2, QStringLiteral("Clock"), QStringLiteral("NextMarker"), QStringLiteral("Drop"));
    QJsonObject setValue = step(QStringLiteral("SetVariable"), 3,
                                QStringLiteral("Variable"), 0.0);
    setValue[QStringLiteral("value")] = 2.5;
    QJsonObject reset = step(QStringLiteral("Reset"), 4,
                             QStringLiteral("Container"), 0.0);
    reset[QStringLiteral("resetScope")] = QStringLiteral("Global");

    QString error;
    const auto parsed = Edit::checkedStepsFromJson(
        QJsonArray{play, setValue, reset}, &error);
    ASSERT_TRUE(parsed) << error.toStdString();
    EXPECT_EQ(parsed->size(), 3u);
}

TEST(ActionStepEditing, ToJsonEmitsConvergedKeysWithStopOnlyFade)
{
    const auto steps = Edit::stepsFromJson(array({
        step(QStringLiteral("Play"), 7, QStringLiteral("A"), 100.0, 999.0),
        step(QStringLiteral("Stop"), 8, QStringLiteral("B"), 0.0, 250.0),
    }));
    const QJsonArray out = Edit::stepsToJson(steps);
    ASSERT_EQ(out.size(), 2);

    const QJsonObject play = out[0].toObject();
    EXPECT_EQ(play.value(QStringLiteral("type")).toString(), QStringLiteral("Play"));
    EXPECT_EQ(play.value(QStringLiteral("targetObjectID")).toInteger(), 7);
    EXPECT_EQ(play.value(QStringLiteral("targetName")).toString(), QStringLiteral("A"));
    EXPECT_DOUBLE_EQ(play.value(QStringLiteral("delay")).toDouble(), 100.0);
    EXPECT_FALSE(play.contains(QStringLiteral("fadeTime")));

    const QJsonObject stop = out[1].toObject();
    EXPECT_EQ(stop.value(QStringLiteral("type")).toString(), QStringLiteral("Stop"));
    EXPECT_TRUE(stop.contains(QStringLiteral("fadeTime")));
    EXPECT_DOUBLE_EQ(stop.value(QStringLiteral("fadeTime")).toDouble(), 250.0);
}

TEST(ActionStepEditing, JsonRoundTripIsStable)
{
    const QJsonArray input = array({
        step(QStringLiteral("Play"), 10, QStringLiteral("A"), 0.0),
        step(QStringLiteral("Stop"), 11, QStringLiteral("B"), 500.0, 250.0),
        step(QStringLiteral("Post Action"), 12, QStringLiteral("C"), 1000.0),
    });
    const QJsonArray once = Edit::stepsToJson(Edit::stepsFromJson(input));
    const QJsonArray twice = Edit::stepsToJson(Edit::stepsFromJson(once));
    EXPECT_EQ(once, twice);
}

TEST(ActionStepEditing, FiveVerbFieldsRoundTripCanonically)
{
    QJsonObject play = step(QStringLiteral("Play"), 10, QStringLiteral("A"), 125.0);
    play[QStringLiteral("quantize")] = quantize(
        20, QStringLiteral("Clock"), QStringLiteral("NextMarker"), QStringLiteral("Drop"));
    QJsonObject stop = step(QStringLiteral("Stop"), 11, QStringLiteral("B"),
                            250.0, 400.0);
    stop[QStringLiteral("quantize")] = quantize(
        0, QString(), QStringLiteral("ExitMarker"), QStringLiteral("Immediate"));
    QJsonObject setValue = step(QStringLiteral("SetVariable"), 12,
                                QStringLiteral("Variable"), 0.0);
    setValue[QStringLiteral("value")] = 3.25;
    setValue[QStringLiteral("quantize")] = quantize(
        99, QStringLiteral("Ignored"), QStringLiteral("NextMarker"), QStringLiteral("Drop"));
    QJsonObject reset = step(QStringLiteral("Reset"), 13,
                             QStringLiteral("Container"), 0.0);
    reset[QStringLiteral("resetScope")] = QStringLiteral("Global");

    const QJsonArray once = Edit::stepsToJson(Edit::stepsFromJson(
        array({play, stop, setValue,
               step(QStringLiteral("Post Action"), 14, QStringLiteral("Action"), 0.0),
               reset})));
    const QJsonArray twice = Edit::stepsToJson(Edit::stepsFromJson(once));
    EXPECT_EQ(once, twice);
    ASSERT_EQ(once.size(), 5);

    EXPECT_TRUE(once.at(0).toObject().contains(QStringLiteral("quantize")));
    EXPECT_TRUE(once.at(1).toObject().contains(QStringLiteral("fadeTime")));
    EXPECT_TRUE(once.at(1).toObject().contains(QStringLiteral("quantize")));
    EXPECT_DOUBLE_EQ(once.at(2).toObject().value(QStringLiteral("value")).toDouble(),
                     3.25);
    EXPECT_FALSE(once.at(2).toObject().contains(QStringLiteral("quantize")));
    EXPECT_FALSE(once.at(3).toObject().contains(QStringLiteral("quantize")));
    EXPECT_EQ(once.at(4).toObject().value(QStringLiteral("resetScope")).toString(),
              QStringLiteral("Global"));
}

TEST(ActionStepEditing, MarkerTimingPreservesDelayAndFadeLimits)
{
    QJsonObject stop = step(QStringLiteral("Stop"), 1, QStringLiteral("A"),
                            90000.0, 90000.0);
    stop[QStringLiteral("quantize")] = quantize(
        2, QStringLiteral("Clock"), QStringLiteral("ExitMarker"), QStringLiteral("Immediate"));
    const auto steps = Edit::stepsFromJson(array({stop}));
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_FLOAT_EQ(steps[0].delay, Edit::kMaxDelayMs);
    EXPECT_FLOAT_EQ(steps[0].fadeTime, Edit::kMaxFadeTimeMs);

}

TEST(ActionStepEditing, QuantizedVerbsPreserveAllTimingValues)
{
    for (const QString& timing : {
             QStringLiteral("Immediate"), QStringLiteral("NextBeat"),
             QStringLiteral("NextBar"), QStringLiteral("NextMarker"),
             QStringLiteral("NaturalEnd"), QStringLiteral("CurrentLoopEnd"),
             QStringLiteral("EntryMarker"), QStringLiteral("ExitMarker")}) {
        for (const QString& type : {QStringLiteral("Play"), QStringLiteral("Stop"),
                                    QStringLiteral("PostAction")}) {
            QJsonObject authored = step(type, 1, QStringLiteral("A"), 0.0);
            authored[QStringLiteral("quantize")] = quantize(
                2, QStringLiteral("Piece"), timing, QStringLiteral("Drop"));
            QString error;
            const auto steps = Edit::checkedStepsFromJson(array({authored}), &error);
            ASSERT_TRUE(steps) << error.toStdString();
            const auto serialized = Edit::stepsToJson(*steps);
            ASSERT_EQ(serialized.size(), 1);
            const auto quantize = serialized.at(0).toObject()
                .value(QStringLiteral("quantize")).toObject();
            EXPECT_EQ(quantize.value(QStringLiteral("timing")).toString(), timing);
            EXPECT_FALSE(quantize.contains(QStringLiteral("cooldownSeconds")));
            EXPECT_FALSE(quantize.contains(QStringLiteral("cooldownScope")));
        }
    }
}

TEST(ActionStepEditing, CheckedParserRejectsRetiredQuantizeFields)
{
    QJsonObject play = step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0);
    for (const auto& field : {"cooldownSeconds", "cooldownScope"}) {
        auto retired = quantize(2, QStringLiteral("Piece"), QStringLiteral("NextMarker"),
                                QStringLiteral("Drop"));
        retired[QString::fromLatin1(field)] = 0;
        play[QStringLiteral("quantize")] = retired;
        QString error;
        EXPECT_FALSE(Edit::checkedStepsFromJson(array({play}), &error));
        EXPECT_TRUE(error.contains(QString::fromLatin1(field)));
    }
}

TEST(ActionStepEditing, SetAndResetDiscardQuantize)
{
    for (const auto& type : {"SetVariable", "Reset"}) {
        auto value = step(QString::fromLatin1(type), 1, QStringLiteral("Target"), 250.0);
        value[QStringLiteral("quantize")] = quantize(
            2, QStringLiteral("Piece"), QStringLiteral("EntryMarker"), QStringLiteral("Drop"));
        const auto parsed = Edit::checkedStepsFromJson(array({value}));
        ASSERT_TRUE(parsed);
        ASSERT_EQ(parsed->size(), 1u);
        EXPECT_EQ(parsed->front().quantize.timing, VNX::QuantizeTiming::Immediate);
        EXPECT_EQ(parsed->front().quantize.referenceObjectID, 0u);
        EXPECT_FALSE(Edit::stepsToJson(*parsed).at(0).toObject().contains(QStringLiteral("quantize")));
        EXPECT_FLOAT_EQ(parsed->front().delay, 250.0f);
    }
}

TEST(ActionStepEditing, NormalizesSetValueForVariableDeclaration)
{
    EXPECT_FLOAT_EQ(Edit::normalizeSetVariableValue(
                        12.25f, VNX::VariableKind::Smooth, 0.0f, 10.0f),
                    10.0f);
    EXPECT_FLOAT_EQ(Edit::normalizeSetVariableValue(
                        2.6f, VNX::VariableKind::Step, 0.0f, 10.0f),
                    3.0f);
    EXPECT_FLOAT_EQ(Edit::normalizeSetVariableValue(
                        -4.0f, VNX::VariableKind::Smooth, 10.0f, 0.0f),
                    0.0f);
    EXPECT_FLOAT_EQ(Edit::normalizeSetVariableValue(
                        std::numeric_limits<float>::infinity(),
                        VNX::VariableKind::Smooth, 5.0f, 10.0f),
                    5.0f);
}

// --- full-table replace + undo/redo semantics (mirrors applyStepsList) ---

TEST(ActionStepEditing, FullTableReplaceWithUndoRedoRoundTrip)
{
    struct Case {
        const char* name;
        QJsonArray before;
        QJsonArray after;
    };
    const std::vector<Case> cases = {
        {"add",
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0)}),
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0),
                step(QStringLiteral("Stop"), 2, QStringLiteral("B"), 0.0, 250.0)})},
        {"remove",
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0),
                step(QStringLiteral("Stop"), 2, QStringLiteral("B"), 0.0, 250.0)}),
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0)})},
        {"modify",
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0)}),
         array({step(QStringLiteral("Play"), 9, QStringLiteral("A2"), 300.0)})},
        {"reorder",
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0),
                step(QStringLiteral("Post Action"), 2, QStringLiteral("B"), 0.0)}),
         array({step(QStringLiteral("Post Action"), 2, QStringLiteral("B"), 0.0),
                step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0)})},
        {"clear",
         array({step(QStringLiteral("Play"), 1, QStringLiteral("A"), 0.0)}),
         QJsonArray{}},
    };

    for (const Case& c : cases) {
        const auto oldSteps = Edit::stepsFromJson(c.before);
        const auto newSteps = Edit::stepsFromJson(c.after);
        const QJsonArray oldJson = Edit::stepsToJson(oldSteps);
        const QJsonArray newJson = Edit::stepsToJson(newSteps);

        std::vector<VActionStep> live = oldSteps;
        const std::vector<VActionStep> snapshot = live;

        live = newSteps;
        EXPECT_EQ(Edit::stepsToJson(live), newJson) << c.name << " apply";

        live = snapshot;
        EXPECT_EQ(Edit::stepsToJson(live), oldJson) << c.name << " undo";

        live = newSteps;
        EXPECT_EQ(Edit::stepsToJson(live), newJson) << c.name << " redo";
    }
}
