#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/DecimalSpinBox.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSlider>

#include <tuple>
#include <vector>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void sendKey(QWidget* target, int key, const QString& text)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
}

void typeInto(QWidget* target, const QString& digits)
{
    for (const QChar character : digits)
    {
        const int key = character == QLatin1Char('.')
            ? Qt::Key_Period
            : Qt::Key_0 + character.digitValue();
        sendKey(target, key, QString(character));
    }
}

struct SignalRecorder
{
    std::vector<double> begins;
    std::vector<double> previews;
    std::vector<std::tuple<double, double, bool>> ends;
    std::vector<double> valueChanges;

    explicit SignalRecorder(VNX::ParameterSlider& s)
    {
        QObject::connect(&s, &VNX::ParameterSlider::interactiveEditBegin,
            [this](double v) { begins.push_back(v); });
        QObject::connect(&s, &VNX::ParameterSlider::interactivePreview,
            [this](double v) { previews.push_back(v); });
        QObject::connect(&s, &VNX::ParameterSlider::interactiveEditEnd,
            [this](double start, double end, bool committed) {
                ends.emplace_back(start, end, committed);
            });
        QObject::connect(&s, &VNX::ParameterSlider::valueChanged,
            [this](double v) { valueChanges.push_back(v); });
    }
};

} // namespace

TEST(ParameterSliderFacade, DelegatesStateToBinding)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(-96.0, 24.0, 1);
    ps.setSuffix(QStringLiteral(" dB"));
    ps.setValue(-12.0);

    ASSERT_NE(ps.binding(), nullptr);
    EXPECT_DOUBLE_EQ(ps.value(), ps.binding()->value());
    EXPECT_DOUBLE_EQ(ps.minimum(), ps.binding()->minimum());
    EXPECT_DOUBLE_EQ(ps.maximum(), ps.binding()->maximum());
    EXPECT_EQ(ps.suffix(), ps.binding()->unit());
    EXPECT_EQ(ps.binding()->decimals(), 1);
    EXPECT_EQ(ps.binding()->name(), QStringLiteral("Volume"));
}

TEST(ParameterSliderFacade, PresentersKeepValuesAndUnitsAndTrimOnlyTrailingZeros)
{
    ensureApp();
    VNX::ParameterSlider control(QStringLiteral("Start Offset"));
    control.setRange(-2000.0, 2000.0, 3);
    control.setSuffix(QStringLiteral(" s"));
    for (const auto presenter : {VNX::ParameterPresenterId::SliderWithValue,
                                VNX::ParameterPresenterId::VerticalFader,
                                VNX::ParameterPresenterId::RotaryKnob}) {
        control.setPresenterOverride(presenter);
        control.setValue(0.0);
        auto* editor = control.findChild<QDoubleSpinBox*>();
        ASSERT_NE(editor, nullptr);
        editor->setLocale(QLocale::c());
        EXPECT_EQ(editor->decimals(), 3);
        EXPECT_TRUE(editor->suffix().isEmpty());
        for (const auto& [value, text] : std::vector<std::pair<double, QString>>{
                 {0.0, QStringLiteral("0.0")}, {1.2, QStringLiteral("1.2")},
                 {0.125, QStringLiteral("0.125")}, {-1.2, QStringLiteral("-1.2")},
                 {1000.0, QStringLiteral("1000.0")}}) {
            control.setValue(value);
            EXPECT_EQ(editor->text(), text);
            EXPECT_DOUBLE_EQ(editor->value(), value);
            EXPECT_DOUBLE_EQ(control.value(), value);
        }
        control.setMixedValue(true);
        control.setMixedValue(false);
        EXPECT_TRUE(editor->suffix().isEmpty());
        EXPECT_EQ(editor->text(), QStringLiteral("1000.0"));

        auto* line = editor->findChild<QLineEdit*>();
        ASSERT_NE(line, nullptr);
        line->setText(QStringLiteral("0.125"));
        editor->interpretText();
        EXPECT_DOUBLE_EQ(control.value(), 0.125);
        EXPECT_EQ(editor->decimals(), 3);
    }
}

TEST(ParameterSliderFacade, DecimalEditorPreservesLocaleAndIntegerFormatting)
{
    ensureApp();
    VNX::DecimalSpinBox editor;
    editor.setDecimals(3);
    editor.setRange(-2000.0, 2000.0);
    editor.setLocale(QLocale(QLocale::German));
    editor.setValue(1.2);
    EXPECT_EQ(editor.text(), QStringLiteral("1,2"));
    auto* line = editor.findChild<QLineEdit*>();
    ASSERT_NE(line, nullptr);
    line->setText(QStringLiteral("0,125"));
    editor.interpretText();
    EXPECT_DOUBLE_EQ(editor.value(), 0.125);
    EXPECT_EQ(editor.text(), QStringLiteral("0,125"));
    editor.setDecimals(0);
    editor.setValue(1);
    EXPECT_EQ(editor.text(), QStringLiteral("1"));
}

TEST(ParameterSliderSession, PressDragReleaseEmitsBeginPreviewEnd)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);

    slider->setSliderDown(true);
    slider->setValue(500);
    slider->setValue(750);
    slider->setSliderDown(false);

    ASSERT_EQ(rec.begins.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.begins[0], 20.0);
    ASSERT_EQ(rec.previews.size(), 2u);
    EXPECT_DOUBLE_EQ(rec.previews[0], 50.0);
    EXPECT_DOUBLE_EQ(rec.previews[1], 75.0);
    ASSERT_EQ(rec.ends.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<0>(rec.ends[0]), 20.0);
    EXPECT_DOUBLE_EQ(std::get<1>(rec.ends[0]), 75.0);
    EXPECT_TRUE(std::get<2>(rec.ends[0]));
    EXPECT_TRUE(rec.valueChanges.empty());
    EXPECT_DOUBLE_EQ(ps.value(), 75.0);
}

TEST(ParameterSliderSession, TrackClickJumpsDirectlyInOneEditSession)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);
    slider->resize(400, 24);

    const QPointF target(slider->width() * 0.75, slider->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, target,
                      slider->mapToGlobal(target.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, target,
                        slider->mapToGlobal(target.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &release);

    EXPECT_GT(ps.value(), 70.0);
    EXPECT_LT(ps.value(), 80.0);
    ASSERT_EQ(rec.begins.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.begins[0], 20.0);
    ASSERT_EQ(rec.previews.size(), 1u);
    ASSERT_EQ(rec.ends.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<0>(rec.ends[0]), 20.0);
    EXPECT_DOUBLE_EQ(std::get<1>(rec.ends[0]), ps.value());
    EXPECT_TRUE(std::get<2>(rec.ends[0]));
    EXPECT_TRUE(rec.valueChanges.empty());
}

TEST(ParameterSliderSession, InterruptedTrackDragEndsEditSession)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);
    slider->resize(400, 24);

    const QPointF target(slider->width() * 0.75, slider->height() / 2.0);
    QMouseEvent press(QEvent::MouseButtonPress, target,
                      slider->mapToGlobal(target.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(slider, &press);
    QEvent ungrab(QEvent::UngrabMouse);
    QApplication::sendEvent(slider, &ungrab);

    EXPECT_FALSE(slider->isSliderDown());
    ASSERT_EQ(rec.begins.size(), 1u);
    ASSERT_EQ(rec.ends.size(), 1u);
    EXPECT_TRUE(std::get<2>(rec.ends[0]));

    slider->setValue(500);
    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 50.0);
}

TEST(ParameterSliderSession, QuantizedRangeGivesTheSliderOnePositionPerStep)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Intensity"));
    ps.setRange(0.0, 4.0, 0);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->maximum(), 1000);

    ps.binding()->setQuantum(1.0);
    EXPECT_EQ(slider->minimum(), 0);
    EXPECT_EQ(slider->maximum(), 4);

    // The gesture lands on a step by itself: the position the pointer produces
    // already is the committed value, so nothing has to snap the handle back
    // after it has moved.
    slider->setSliderDown(true);
    slider->setValue(3);
    EXPECT_DOUBLE_EQ(ps.value(), 3.0);
    EXPECT_EQ(slider->value(), 3);
    slider->setSliderDown(false);
    EXPECT_DOUBLE_EQ(ps.value(), 3.0);
}

TEST(ParameterSliderSession, PressReleaseWithoutDragEmitsEmptySession)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);

    slider->setSliderDown(true);
    slider->setSliderDown(false);

    ASSERT_EQ(rec.begins.size(), 1u);
    EXPECT_TRUE(rec.previews.empty());
    ASSERT_EQ(rec.ends.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<0>(rec.ends[0]), 20.0);
    EXPECT_DOUBLE_EQ(std::get<1>(rec.ends[0]), 20.0);
    EXPECT_TRUE(std::get<2>(rec.ends[0]));
    EXPECT_TRUE(rec.valueChanges.empty());
}

TEST(ParameterSliderSession, SliderChangeWithoutPressStaysNonSession)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);

    slider->setValue(500);

    EXPECT_TRUE(rec.begins.empty());
    EXPECT_TRUE(rec.previews.empty());
    EXPECT_TRUE(rec.ends.empty());
    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 50.0);
}

TEST(ParameterSliderSession, SpinBoxChangeStaysNonSession)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* spinBox = ps.findChild<QDoubleSpinBox*>();
    ASSERT_NE(spinBox, nullptr);

    spinBox->setValue(33.0);

    EXPECT_TRUE(rec.begins.empty());
    EXPECT_TRUE(rec.previews.empty());
    EXPECT_TRUE(rec.ends.empty());
    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 33.0);
}

TEST(ParameterSliderSession, TypedDigitsCommitOnceWhenTheEditIsAccepted)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* spinBox = ps.findChild<QDoubleSpinBox*>();
    ASSERT_NE(spinBox, nullptr);
    ASSERT_FALSE(spinBox->keyboardTracking());

    ps.show();
    spinBox->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    spinBox->selectAll();
    typeInto(spinBox, QStringLiteral("37.25"));

    EXPECT_TRUE(rec.valueChanges.empty());
    EXPECT_DOUBLE_EQ(ps.value(), 20.0);

    sendKey(spinBox, Qt::Key_Return, QStringLiteral("\r"));

    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 37.25);
    EXPECT_DOUBLE_EQ(ps.value(), 37.25);
    EXPECT_TRUE(rec.begins.empty());
    EXPECT_TRUE(rec.previews.empty());
    EXPECT_TRUE(rec.ends.empty());
}

TEST(ParameterSliderSession, MixedTypedCachedValueCommitsOnceAcrossPresenters)
{
    ensureApp();
    for (const auto presenter : {VNX::ParameterPresenterId::SliderWithValue,
                                VNX::ParameterPresenterId::VerticalFader,
                                VNX::ParameterPresenterId::RotaryKnob}) {
        SCOPED_TRACE(static_cast<int>(presenter));
        VNX::ParameterSlider ps(QStringLiteral("Volume"));
        ps.setPresenterOverride(presenter);
        ps.setRange(0.0, 100.0, 2);
        ps.setValue(20.0);
        SignalRecorder rec(ps);
        auto* editor = ps.findChild<QDoubleSpinBox*>();
        ASSERT_NE(editor, nullptr);
        ps.show();
        editor->setFocus(Qt::OtherFocusReason);
        QApplication::processEvents();
        ps.setMixedValue(true);
        auto* line = editor->findChild<QLineEdit*>();
        ASSERT_NE(line, nullptr);
        EXPECT_TRUE(line->text().isEmpty());
        typeInto(editor, QStringLiteral("20.0"));
        EXPECT_TRUE(rec.valueChanges.empty());
        sendKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        ASSERT_EQ(rec.valueChanges.size(), 1u);
        EXPECT_DOUBLE_EQ(rec.valueChanges.front(), 20.0);
        EXPECT_FALSE(ps.mixedValue());
        EXPECT_FALSE(ps.binding()->mixedValue());
        EXPECT_TRUE(line->placeholderText().isEmpty());
        sendKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        EXPECT_EQ(rec.valueChanges.size(), 1u);
    }
}

TEST(ParameterSliderSession, MixedExplicitCommitPreservesCallbackRefresh)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    ps.setMixedValue(true);
    int commits = 0;
    QObject::connect(&ps, &VNX::ParameterSlider::valueChanged, &ps, [&](double value) {
        ++commits;
        EXPECT_DOUBLE_EQ(value, 20.0);
        EXPECT_FALSE(ps.mixedValue());
        EXPECT_FALSE(ps.binding()->mixedValue());
        ps.setMixedValue(true);
    });
    ps.simulateValueChange(20.0);
    EXPECT_EQ(commits, 1);
    EXPECT_TRUE(ps.mixedValue());
    EXPECT_TRUE(ps.binding()->mixedValue());
}

TEST(ParameterSliderSession, CancelPendingTextDoesNotCommitIntoRefreshedValue)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* editor = ps.findChild<QDoubleSpinBox*>();
    ASSERT_NE(editor, nullptr);
    ps.show();
    editor->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    for (const bool mixed : {false, true}) {
        ps.setMixedValue(mixed);
        editor->selectAll();
        typeInto(editor, QStringLiteral("37.25"));
        ps.cancelPendingInput();
        ps.setValue(20.0);
        auto* line = editor->findChild<QLineEdit*>();
        ASSERT_NE(line, nullptr);
        EXPECT_TRUE(line->placeholderText().isEmpty());
        sendKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        EXPECT_TRUE(rec.valueChanges.empty());
        EXPECT_DOUBLE_EQ(ps.value(), 20.0);
    }
}

TEST(ParameterSliderSession, UntouchedMixedEditorDoesNotCommitOnAcceptance)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* editor = ps.findChild<QDoubleSpinBox*>();
    ASSERT_NE(editor, nullptr);
    ps.show();
    editor->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    ps.setMixedValue(true);
    sendKey(editor, Qt::Key_Return, QStringLiteral("\r"));
    EXPECT_TRUE(rec.valueChanges.empty());
    EXPECT_TRUE(ps.mixedValue());
}

TEST(ParameterSliderSession, ArrowKeysStillStepTheValueImmediately)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* spinBox = ps.findChild<QDoubleSpinBox*>();
    ASSERT_NE(spinBox, nullptr);

    ps.show();
    spinBox->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    sendKey(spinBox, Qt::Key_Up, QString());

    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 21.0);
}

TEST(ParameterSliderSession, ProgrammaticSetValueEmitsNothing)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);

    ps.setValue(40.0);

    EXPECT_TRUE(rec.begins.empty());
    EXPECT_TRUE(rec.previews.empty());
    EXPECT_TRUE(rec.ends.empty());
    EXPECT_TRUE(rec.valueChanges.empty());
    EXPECT_DOUBLE_EQ(ps.value(), 40.0);
}

TEST(ParameterSliderSession, SetValueDuringSessionEndsUncommitted)
{
    ensureApp();
    VNX::ParameterSlider ps(QStringLiteral("Volume"));
    ps.setRange(0.0, 100.0, 2);
    ps.setValue(20.0);
    SignalRecorder rec(ps);
    auto* slider = ps.findChild<QSlider*>();
    ASSERT_NE(slider, nullptr);

    slider->setSliderDown(true);
    slider->setValue(500);
    ps.setValue(10.0);

    ASSERT_EQ(rec.ends.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<0>(rec.ends[0]), 20.0);
    EXPECT_DOUBLE_EQ(std::get<1>(rec.ends[0]), 50.0);
    EXPECT_FALSE(std::get<2>(rec.ends[0]));
    EXPECT_DOUBLE_EQ(ps.value(), 10.0);

    slider->setValue(250);
    slider->setSliderDown(false);

    ASSERT_EQ(rec.begins.size(), 1u);
    ASSERT_EQ(rec.previews.size(), 1u);
    ASSERT_EQ(rec.ends.size(), 1u);
    ASSERT_EQ(rec.valueChanges.size(), 1u);
    EXPECT_DOUBLE_EQ(rec.valueChanges[0], 25.0);
}
