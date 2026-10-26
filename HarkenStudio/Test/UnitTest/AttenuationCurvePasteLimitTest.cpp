#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

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
