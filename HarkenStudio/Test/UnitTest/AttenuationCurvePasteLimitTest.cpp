#include "Widgets/CurveEditor/CurveCanvas.h"
#include "Core/ShortcutManager.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <cmath>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr float kMaxDistance = 100.0f;

VNX::CurvePointDef makePoint(float distance, float value)
{
    return {distance, value};
}

QVector<VNX::CurvePointDef> makeCurve(int count)
{
    QVector<VNX::CurvePointDef> points;
    for (int i = 0; i < count; ++i) {
        points.push_back(makePoint(
            kMaxDistance * static_cast<float>(i) / static_cast<float>(count - 1),
            -10.0f * static_cast<float>(i)));
    }
    return points;
}

QJsonObject makeClipboardPoint(float x, float value,
                               const QJsonArray& leftHandle = {},
                               const QJsonArray& rightHandle = {})
{
    QJsonObject point{{QStringLiteral("x"), x},
                      {QStringLiteral("value"), value}};
    if (!leftHandle.isEmpty())
        point.insert(QStringLiteral("leftHandle"), leftHandle);
    if (!rightHandle.isEmpty())
        point.insert(QStringLiteral("rightHandle"), rightHandle);
    return point;
}

QJsonObject makePayload(const QJsonArray& points,
                        const QString& family = QStringLiteral("volume"),
                        const QString& format = QStringLiteral("Harken.Curve.v3"))
{
    return QJsonObject{{QStringLiteral("format"), format},
                       {QStringLiteral("family"), family},
                       {QStringLiteral("points"), points}};
}

QJsonObject clipboardObject()
{
    return QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8()).object();
}

std::unique_ptr<VNX::CurveCanvas> makeCanvas(
    const QVector<VNX::CurvePointDef>& points,
    const QVector<VNX::CurveHandlePairDef>& handles = {})
{
    ensureApp();
    auto canvas = std::make_unique<VNX::CurveCanvas>();
    canvas->setXDomain(0.0f, kMaxDistance);
    canvas->setCurve(points, handles);
    return canvas;
}

void expectCurve(const VNX::CurveCanvas& canvas,
                 const QVector<VNX::CurvePointDef>& points,
                 const QVector<VNX::CurveHandlePairDef>& handles)
{
    const auto actualPoints = canvas.curve();
    const auto actualHandles = canvas.curveHandles();
    ASSERT_EQ(actualPoints.size(), points.size());
    ASSERT_EQ(actualHandles.size(), handles.size());
    for (int i = 0; i < points.size(); ++i) {
        EXPECT_FLOAT_EQ(actualPoints[i].x, points[i].x);
        EXPECT_FLOAT_EQ(actualPoints[i].value, points[i].value);
    }
    for (int i = 0; i < handles.size(); ++i) {
        EXPECT_FLOAT_EQ(actualHandles[i].h1x, handles[i].h1x);
        EXPECT_FLOAT_EQ(actualHandles[i].h1y, handles[i].h1y);
        EXPECT_FLOAT_EQ(actualHandles[i].h2x, handles[i].h2x);
        EXPECT_FLOAT_EQ(actualHandles[i].h2y, handles[i].h2y);
    }
}

} // namespace

TEST(AttenuationCurveClipboard, CopySerializesOnlySelectedPointAndOwnedHandles)
{
    const QVector<VNX::CurveHandlePairDef> handles{
        {10.0f, -5.0f, 40.0f, -30.0f},
        {60.0f, -50.0f, 90.0f, -80.0f}};
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)}, handles);
    canvas->setSelectedPointIndices({1}, 1);

    ASSERT_TRUE(canvas->copySelectedPointsToClipboard());
    const QJsonObject payload = clipboardObject();
    EXPECT_EQ(payload.value(QStringLiteral("format")).toString(),
              QStringLiteral("Harken.Curve.v3"));
    EXPECT_FALSE(payload.contains(QStringLiteral("mode")));
    const QJsonArray points = payload.value(QStringLiteral("points")).toArray();
    ASSERT_EQ(points.size(), 1);
    const QJsonObject point = points.first().toObject();
    EXPECT_DOUBLE_EQ(point.value(QStringLiteral("x")).toDouble(), 50.0);
    EXPECT_EQ(point.value(QStringLiteral("leftHandle")).toArray(),
              (QJsonArray{40.0, -30.0}));
    EXPECT_EQ(point.value(QStringLiteral("rightHandle")).toArray(),
              (QJsonArray{60.0, -50.0}));
}

TEST(AttenuationCurveClipboard, SelectAllActionThenCopyIncludesEveryPoint)
{
    auto canvas = makeCanvas(makeCurve(4));
    QAction* selectAllAction = nullptr;
    const auto actions = VNX::ShortcutManager::instance()
        .info(VNX::Shortcuts::SelectAll).actions;
    for (QAction* action : actions) {
        if (action && action->parent() == canvas.get()) {
            selectAllAction = action;
            break;
        }
    }
    ASSERT_NE(selectAllAction, nullptr);
    EXPECT_EQ(selectAllAction->shortcutContext(), Qt::WidgetShortcut);
    selectAllAction->trigger();
    ASSERT_EQ(canvas->selectedPointIndices().size(), 4);

    ASSERT_TRUE(canvas->copySelectedPointsToClipboard());
    const QJsonArray points = clipboardObject().value(QStringLiteral("points")).toArray();
    ASSERT_EQ(points.size(), 4);
    EXPECT_FALSE(points.first().toObject().contains(QStringLiteral("leftHandle")));
    EXPECT_FALSE(points.last().toObject().contains(QStringLiteral("rightHandle")));
}

TEST(AttenuationCurveClipboard, CopyWithNoSelectionDoesNotReplaceClipboard)
{
    auto canvas = makeCanvas(makeCurve(3));
    QApplication::clipboard()->setText(QStringLiteral("unchanged"));
    EXPECT_FALSE(canvas->copySelectedPointsToClipboard());
    EXPECT_EQ(QApplication::clipboard()->text(), QStringLiteral("unchanged"));
}

TEST(AttenuationCurveClipboard, CutCopiesAndDeletesOnlyEditableSelectedPoints)
{
    auto canvas = makeCanvas(makeCurve(4));
    canvas->setSelectedPointIndices({0, 1, 3}, 1);
    int editCount = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, canvas.get(),
                     [&](const QVector<VNX::CurvePointDef>&,
                         const QVector<VNX::CurveHandlePairDef>&,
                         const QString&) {
        ++editCount;
    });

    ASSERT_TRUE(canvas->cutSelectedPointsToClipboard());
    ASSERT_EQ(editCount, 1);
    ASSERT_EQ(canvas->curve().size(), 3);
    EXPECT_FLOAT_EQ(canvas->curve().first().x, 0.0f);
    EXPECT_FLOAT_EQ(canvas->curve().last().x, 100.0f);
    EXPECT_TRUE(canvas->selectedPointIndices().isEmpty());

    const QJsonArray copied = clipboardObject().value(QStringLiteral("points")).toArray();
    ASSERT_EQ(copied.size(), 1);
    EXPECT_DOUBLE_EQ(copied.first().toObject().value(QStringLiteral("x")).toDouble(),
                     static_cast<double>(makeCurve(4).at(1).x));
}

TEST(AttenuationCurveClipboard, PasteReplacesPointAtSameX)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)});
    int editCount = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, canvas.get(),
                     [&](const QVector<VNX::CurvePointDef>&,
                         const QVector<VNX::CurveHandlePairDef>&,
                         const QString&) {
        ++editCount;
    });
    const QJsonObject payload = makePayload(
        QJsonArray{makeClipboardPoint(50.0005f, -12.0f)});

    ASSERT_TRUE(canvas->pastePayload(payload));
    ASSERT_EQ(canvas->curve().size(), 3);
    EXPECT_FLOAT_EQ(canvas->curve().at(1).x, 50.0f);
    EXPECT_FLOAT_EQ(canvas->curve().at(1).value, -12.0f);
    EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{1}));
    ASSERT_EQ(editCount, 1);
}

TEST(AttenuationCurveClipboard, PasteInsertsPointsAtOriginalX)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(100.0f, -96.0f)});
    const QJsonObject payload = makePayload(
        QJsonArray{makeClipboardPoint(25.0f, -10.0f),
                   makeClipboardPoint(75.0f, -70.0f)});

    ASSERT_TRUE(canvas->pastePayload(payload));
    const auto points = canvas->curve();
    ASSERT_EQ(points.size(), 4);
    EXPECT_FLOAT_EQ(points.at(1).x, 25.0f);
    EXPECT_FLOAT_EQ(points.at(1).value, -10.0f);
    EXPECT_FLOAT_EQ(points.at(2).x, 75.0f);
    EXPECT_FLOAT_EQ(points.at(2).value, -70.0f);
    EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{1, 2}));
}

TEST(AttenuationCurveClipboard, PointLimitCountsOnlyInsertedPoints)
{
    auto canvas = makeCanvas(makeCurve(VNX::kCurveMaxPoints));
    const float existingX = canvas->curve().at(3).x;
    ASSERT_TRUE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(existingX, -1.0f)})));
    EXPECT_EQ(canvas->curve().size(), VNX::kCurveMaxPoints);
    EXPECT_FLOAT_EQ(canvas->curve().at(3).value, -1.0f);

    const auto before = canvas->curve();
    EXPECT_FALSE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(7.0f, -2.0f)})));
    ASSERT_EQ(canvas->curve().size(), before.size());
    for (int i = 0; i < before.size(); ++i) {
        EXPECT_FLOAT_EQ(canvas->curve().at(i).x, before.at(i).x);
        EXPECT_FLOAT_EQ(canvas->curve().at(i).value, before.at(i).value);
    }
}

TEST(AttenuationCurveClipboard, PasteAppliesHandlesOwnedByAnchor)
{
    const QVector<VNX::CurveHandlePairDef> handles{
        {10.0f, -5.0f, 40.0f, -30.0f},
        {60.0f, -50.0f, 90.0f, -80.0f}};
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)}, handles);
    const QJsonObject payload = makePayload(QJsonArray{makeClipboardPoint(
        50.0f, -20.0f, QJsonArray{35.0, -25.0}, QJsonArray{65.0, -45.0})});

    ASSERT_TRUE(canvas->pastePayload(payload));
    const auto pastedHandles = canvas->curveHandles();
    ASSERT_EQ(pastedHandles.size(), 2);
    EXPECT_FLOAT_EQ(pastedHandles.at(0).h1x, 10.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(0).h1y, -5.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(0).h2x, 35.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(0).h2y, -25.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(1).h1x, 65.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(1).h1y, -45.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(1).h2x, 90.0f);
    EXPECT_FLOAT_EQ(pastedHandles.at(1).h2y, -80.0f);
}

TEST(AttenuationCurveClipboard, RejectsMismatchedFamilyAndOldFormat)
{
    auto canvas = makeCanvas(makeCurve(2));
    const QJsonArray points{makeClipboardPoint(50.0f, -30.0f)};
    EXPECT_FALSE(canvas->pastePayload(makePayload(points, QStringLiteral("pitch"))));
    EXPECT_FALSE(canvas->pastePayload(makePayload(
        points, QStringLiteral("volume"), QStringLiteral("Harken.Curve.v2"))));
    EXPECT_EQ(canvas->curve().size(), 2);
}

TEST(AttenuationCurveClipboard, PasteAvailabilityRequiresValidPayload)
{
    auto canvas = makeCanvas(makeCurve(3));
    const QJsonArray points{makeClipboardPoint(50.0f, -30.0f)};

    QApplication::clipboard()->setText(QString::fromUtf8(
        QJsonDocument(makePayload(points)).toJson(QJsonDocument::Compact)));
    EXPECT_TRUE(canvas->canPasteFromClipboard());

    QJsonObject invalidPoint{{QStringLiteral("x"), 25.0f}};
    QApplication::clipboard()->setText(QString::fromUtf8(
        QJsonDocument(makePayload(QJsonArray{invalidPoint})).toJson(QJsonDocument::Compact)));
    EXPECT_FALSE(canvas->canPasteFromClipboard());

    QApplication::clipboard()->setText(QString::fromUtf8(
        QJsonDocument(makePayload(points, QStringLiteral("volume"),
                                  QStringLiteral("Harken.Curve.v2")))
            .toJson(QJsonDocument::Compact)));
    EXPECT_FALSE(canvas->canPasteFromClipboard());
}

TEST(AttenuationCurveClipboard, CopyPastePreservesCloselySpacedPoints)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -30.0f),
                              makePoint(50.0005f, -60.0f),
                              makePoint(100.0f, -96.0f)});
    const auto points = canvas->curve();
    const auto handles = canvas->curveHandles();
    canvas->setSelectedPointIndices({1, 2}, 1);

    ASSERT_TRUE(canvas->copySelectedPointsToClipboard());
    EXPECT_TRUE(canvas->canPasteFromClipboard());
    ASSERT_TRUE(canvas->pasteFromClipboard());
    expectCurve(*canvas, points, handles);
    EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{1, 2}));

    auto destination = makeCanvas({points.front(), points.back()});
    EXPECT_TRUE(destination->canPasteFromClipboard());
    ASSERT_TRUE(destination->pasteFromClipboard());
    expectCurve(*destination, points, handles);
    EXPECT_EQ(destination->selectedPointIndices(), (QVector<int>{1, 2}));
}

TEST(AttenuationCurveClipboard, PasteMatchesNearestExistingPoint)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -30.0f),
                              makePoint(50.0009f, -60.0f),
                              makePoint(100.0f, -96.0f)});
    ASSERT_TRUE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(50.0007f, -12.0f)})));
    EXPECT_FLOAT_EQ(canvas->curve().at(1).value, -30.0f);
    EXPECT_FLOAT_EQ(canvas->curve().at(2).value, -12.0f);
    EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{2}));
}

TEST(AttenuationCurveClipboard, InvalidPayloadsLeaveCurveAndSelectionUnchanged)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)});
    canvas->setSelectedPointIndices({1}, 1);
    const auto points = canvas->curve();
    const auto handles = canvas->curveHandles();
    int edits = 0;
    int selections = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, canvas.get(),
                     [&]() { ++edits; });
    QObject::connect(canvas.get(), &VNX::CurveCanvas::selectionChanged, canvas.get(),
                     [&]() { ++selections; });

    const QVector<QJsonArray> invalidPoints{
        {makeClipboardPoint(49.99925f, -10.0f), makeClipboardPoint(50.00075f, -20.0f)},
        {makeClipboardPoint(25.0f, -10.0f), makeClipboardPoint(25.0f, -20.0f)},
        {makeClipboardPoint(25.0f, -10.0f), makeClipboardPoint(101.0f, -20.0f)},
        {makeClipboardPoint(25.0f, -10.0f, QJsonArray{1e100, -20.0})},
        {makeClipboardPoint(25.0f, -10.0f, {}, QJsonArray{50.0, -1e100})},
        {makeClipboardPoint(25.0f, -10.0f), QJsonObject{{"x", 75.0}, {"value", 1e100}}},
        {makeClipboardPoint(25.0f, -10.0f, QJsonArray{QStringLiteral("invalid"), -20.0})}
    };
    for (int i = 0; i < invalidPoints.size(); ++i) {
        SCOPED_TRACE(i);
        QApplication::clipboard()->setText(QString::fromUtf8(
            QJsonDocument(makePayload(invalidPoints[i])).toJson(QJsonDocument::Compact)));
        EXPECT_FALSE(canvas->canPasteFromClipboard());
        EXPECT_FALSE(canvas->pasteFromClipboard());
        expectCurve(*canvas, points, handles);
        EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{1}));
        EXPECT_EQ(canvas->selectedPointIndex(), 1);
        EXPECT_EQ(edits, 0);
        EXPECT_EQ(selections, 0);
    }
}

TEST(AttenuationCurveClipboard, CutAndPasteRestoresAllAnchorHandles)
{
    const QVector<VNX::CurvePointDef> points{
        makePoint(0.0f, 0.0f), makePoint(25.0f, -20.0f), makePoint(50.0f, -40.0f),
        makePoint(75.0f, -60.0f), makePoint(100.0f, -96.0f)};
    const QVector<VNX::CurveHandlePairDef> handles{
        {5.0f, -5.0f, 20.0f, -15.0f}, {30.0f, -25.0f, 45.0f, -35.0f},
        {55.0f, -45.0f, 70.0f, -55.0f}, {80.0f, -70.0f, 95.0f, -85.0f}};
    const QVector<QVector<int>> selections{{2}, {1, 2}, {1, 3}};
    for (const auto domain : {VNX::CurveAxisScale::AmplitudeDb,
                              VNX::CurveAxisScale::MirroredAmplitudeDb,
                              VNX::CurveAxisScale::LinearValue}) {
        for (const auto& selection : selections) {
            auto canvas = makeCanvas(points, handles);
            auto profile = canvas->profile();
            profile.axisScale = domain;
            if (domain == VNX::CurveAxisScale::LinearValue)
                profile.clipboardFamily = QStringLiteral("pitch");
            canvas->setProfile(profile);
            canvas->setSelectedPointIndices(selection, selection.front());

            ASSERT_TRUE(canvas->cutSelectedPointsToClipboard());
            ASSERT_EQ(canvas->curve().size(), points.size() - selection.size());
            ASSERT_TRUE(canvas->pasteFromClipboard());
            expectCurve(*canvas, points, handles);
            EXPECT_EQ(canvas->selectedPointIndices(), selection);
        }
    }
}

TEST(AttenuationCurveClipboard, PastePreservesOtherHandlesWhenDomainCollapsesAnchors)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f), makePoint(25.0f, -20.0f),
                              makePoint(75.0f, -60.0f), makePoint(100.0f, -96.0f)},
                             {{5.0f, -5.0f, 20.0f, -15.0f},
                              {30.0f, -25.0f, 70.0f, -55.0f},
                              {80.0f, -70.0f, 95.0f, -85.0f}});
    canvas->setXDomain(75.0f, 100.0f);
    const auto points = canvas->curve();
    auto handles = canvas->curveHandles();
    auto expected = points;
    expected.back().value = -90.0f;
    handles.back().h2y = static_cast<float>(20.0 * std::log10(
        std::pow(10.0, -85.0 / 20.0) + std::pow(10.0, -90.0 / 20.0) - std::pow(10.0, -96.0 / 20.0)));
    ASSERT_TRUE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(100.0f, -90.0f)})));
    expectCurve(*canvas, expected, handles);
}

TEST(AttenuationCurveClipboard, PasteTranslatesHandlesWhenCoordinatesSnap)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)});
    constexpr float sourceX = 50.0005f;
    ASSERT_TRUE(canvas->pastePayload(makePayload(QJsonArray{makeClipboardPoint(
        sourceX, -20.0f, QJsonArray{35.0, -25.0}, QJsonArray{65.0, -45.0})})));
    const float delta = 50.0f - sourceX;
    EXPECT_FLOAT_EQ(canvas->curveHandles()[0].h2x, 35.0f + delta);
    EXPECT_FLOAT_EQ(canvas->curveHandles()[1].h1x, 65.0f + delta);
}

TEST(AttenuationCurveClipboard, PasteWithoutHandlesTranslatesOwnedHandles)
{
    auto canvas = makeCanvas({makePoint(0.0f, 0.0f),
                              makePoint(50.0f, -40.0f),
                              makePoint(100.0f, -96.0f)},
                             {{10.0f, -5.0f, 40.0f, -30.0f},
                              {60.0f, -50.0f, 90.0f, -80.0f}});
    ASSERT_TRUE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(50.0f, -20.0f)})));
    EXPECT_FLOAT_EQ(canvas->curveHandles()[0].h1y, -5.0f);
    const double amplitudeDelta = std::pow(10.0, -20.0 / 20.0) - std::pow(10.0, -40.0 / 20.0);
    EXPECT_NEAR(std::pow(10.0, canvas->curveHandles()[0].h2y / 20.0) - std::pow(10.0, -30.0 / 20.0),
                amplitudeDelta, 1e-7);
    EXPECT_NEAR(std::pow(10.0, canvas->curveHandles()[1].h1y / 20.0) - std::pow(10.0, -50.0 / 20.0),
                amplitudeDelta, 1e-7);
    EXPECT_FLOAT_EQ(canvas->curveHandles()[1].h2y, -80.0f);
}

TEST(AttenuationCurveClipboard, UnchangedPasteSelectsPointsWithoutEmittingEdit)
{
    auto canvas = makeCanvas(makeCurve(4));
    canvas->setSelectedPointIndices({0, 2, 3}, 2);
    ASSERT_TRUE(canvas->copySelectedPointsToClipboard());
    canvas->setSelectedPointIndex(-1);
    int edits = 0;
    QObject::connect(canvas.get(), &VNX::CurveCanvas::curveEdited, canvas.get(),
                     [&]() { ++edits; });

    ASSERT_TRUE(canvas->pasteFromClipboard());
    EXPECT_EQ(canvas->selectedPointIndices(), (QVector<int>{0, 2, 3}));
    EXPECT_EQ(canvas->selectedPointIndex(), 0);
    EXPECT_EQ(edits, 0);
}

TEST(AttenuationCurveClipboard, LegacyOversizedCurveAllowsReplacementButRejectsGrowth)
{
    auto canvas = makeCanvas(makeCurve(VNX::kCurveMaxPoints + 1));
    const float existingX = canvas->curve().at(3).x;
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(makePayload(
        QJsonArray{makeClipboardPoint(existingX, -1.0f)})).toJson(QJsonDocument::Compact)));
    EXPECT_TRUE(canvas->canPasteFromClipboard());
    ASSERT_TRUE(canvas->pasteFromClipboard());
    EXPECT_FLOAT_EQ(canvas->curve().at(3).value, -1.0f);
    EXPECT_EQ(canvas->curve().size(), VNX::kCurveMaxPoints + 1);
    EXPECT_FALSE(canvas->pastePayload(makePayload(
        QJsonArray{makeClipboardPoint(7.0f, -2.0f)})));
}

TEST(AttenuationCurveClipboard, SinglePointCurveRejectsUnavailableInsertion)
{
    auto canvas = makeCanvas({makePoint(100.0f, -12.0f)});
    QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(makePayload(
        QJsonArray{makeClipboardPoint(50.0f, -20.0f)})).toJson(QJsonDocument::Compact)));
    EXPECT_FALSE(canvas->canPasteFromClipboard());
    EXPECT_FALSE(canvas->pasteFromClipboard());
}
