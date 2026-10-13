#include "Core/MemorySettingsStorage.h"
#include "Core/ParameterPresenterRegistry.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/ParameterValueEditor.h"
#include "Widgets/Common/DecimalSpinBox.h"
#include "Widgets/Common/NumericToolTip.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QFile>
#include <QHelpEvent>
#include <QPointer>
#include <QSlider>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QToolTip>

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

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writePresenterTheme(const QString& root,
                         const QString& id,
                         const QString& inspectorPresenter)
{
    const QString package = QDir(root).filePath(id);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":2,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"" + id.toUtf8()
        + "\",\"version\":\"1.0.0\","
          "\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\","
          "\"presenters\":\"presenters.json\"}}";
    const QByteArray presenters = QByteArray("{\"contexts\":{"
        "\"inspector.float\":\"") + inspectorPresenter.toUtf8() + "\"}}";
    return writeFile(QDir(package).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(package).filePath(QStringLiteral("tokens.json")),
                     R"json({"colors":{"accent.primary":"#ff00f0ff"}})json")
        && writeFile(QDir(package).filePath(QStringLiteral("visuals/curve.json")),
                     R"json({"colors":{"curve.line":"#ff112233"}})json")
        && writeFile(QDir(package).filePath(QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(package).filePath(QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(package).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json")
        && writeFile(QDir(package).filePath(QStringLiteral("presenters.json")),
                     presenters);
}

}

TEST(ParameterControlHost, RebuildPreservesBindingMetadataFocusAndSingleCommit)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Volume"));
    auto* binding = host.binding();
    binding->setRange(-96.0, 24.0);
    binding->setDefaultValue(-12.0);
    binding->setDecimals(1);
    binding->setUnit(QStringLiteral(" dB"));
    binding->setValue(-6.0);
    host.show();
    host.activateWindow();
    host.presenter()->spinBox()->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    ASSERT_TRUE(host.presenter()->spinBox()->hasFocus());
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted, [&commits] { ++commits; });
    for (int i = 0; i < 2; ++i) {
        QPointer<VNX::ParameterPresenter> oldPresenter = host.presenter();
        host.rebuildPresenter();
        QApplication::processEvents();
        EXPECT_TRUE(oldPresenter.isNull());
        EXPECT_TRUE(host.presenter()->spinBox()->hasFocus());
    }
    EXPECT_DOUBLE_EQ(binding->value(), -6.0);
    EXPECT_DOUBLE_EQ(binding->minimum(), -96.0);
    EXPECT_DOUBLE_EQ(binding->maximum(), 24.0);
    EXPECT_DOUBLE_EQ(binding->defaultValue(), -12.0);
    EXPECT_EQ(binding->unit(), QStringLiteral(" dB"));
    EXPECT_EQ(host.accessibleName(), QStringLiteral("Volume"));
    EXPECT_EQ(host.presenter()->slider()->accessibleName(),
              QStringLiteral("Volume"));
    EXPECT_EQ(host.presenter()->spinBox()->accessibleName(),
              QStringLiteral("Volume"));
    EXPECT_DOUBLE_EQ(host.presenter()->spinBox()->value(), -6.0);
    EXPECT_EQ(commits, 0);
    host.presenter()->spinBox()->setValue(-3.0);
    EXPECT_DOUBLE_EQ(binding->value(), -3.0);
    EXPECT_EQ(commits, 1);
}

TEST(ParameterControlHost, DiscreteChoicesTrackRangeLabelsAndNumericWritesWithoutRefreshCommits)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Value"));
    auto* binding = host.binding();
    binding->setRange(5, 1000000);
    binding->setDecimals(0);
    binding->setQuantum(1);
    binding->setAnnotation([](double value) {
        return value == 7 ? QStringLiteral("Combat") : QString();
    });
    binding->setDiscreteSelection(true);
    binding->setValue(7);
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted, [&commits] { ++commits; });
    auto* combo = host.presenter()->choiceEditor();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(host.presenter()->valueEditor(), nullptr);
    EXPECT_EQ(combo->count(), 999996);
    EXPECT_EQ(combo->itemText(0), QStringLiteral("5"));
    EXPECT_EQ(combo->currentText(), QStringLiteral("Combat"));
    host.resize(400, 50);
    host.show();
    combo->showPopup();
    QApplication::processEvents();
    combo->hidePopup();
    combo->setCurrentIndex(combo->findData(123456));
    EXPECT_DOUBLE_EQ(binding->value(), 123456);
    EXPECT_EQ(commits, 1);
    binding->setValue(7);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Combat"));
    binding->setAnnotation([](double value) {
        return value == 7 ? QStringLiteral("Battle") : QString();
    });
    EXPECT_EQ(combo->currentText(), QStringLiteral("Battle"));
    binding->setRange(6, 9);
    EXPECT_EQ(combo->count(), 4);
    EXPECT_EQ(combo->currentData().toInt(), 7);
    host.rebuildPresenter();
    combo = host.presenter()->choiceEditor();
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentText(), QStringLiteral("Battle"));
    binding->setDiscreteSelection(false);
    binding->setQuantum(0);
    binding->setDecimals(2);
    binding->setValue(7.25);
    ASSERT_NE(host.presenter()->valueEditor(), nullptr);
    EXPECT_DOUBLE_EQ(host.presenter()->valueEditor()->value(), 7.25);
    EXPECT_TRUE(combo->isHidden());
    EXPECT_EQ(commits, 1);
}

TEST(ParameterControlHost, RebuildKeepsActiveEditAndRoutesNewPresenter)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Volume"));
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setValue(20.0);
    binding->beginEdit();
    binding->previewEdit(40.0);

    host.rebuildPresenter();
    ASSERT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(host.presenter()->spinBox()->value(), 40.0);

    host.presenter()->slider()->setValue(750);
    EXPECT_DOUBLE_EQ(binding->value(), 75.0);
    binding->commitEdit();
    EXPECT_FALSE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->value(), 75.0);
}

TEST(ParameterControlHost, NumericToolTipsTrackDisplayedValuesUnitsAndPresenterChanges)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Offset of Start"));
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setDecimals(3);
    binding->setUnit(QStringLiteral(" s"));
    binding->setValue(0.125);
    for (const auto id : {VNX::ParameterPresenterId::SliderWithValue,
                          VNX::ParameterPresenterId::HorizontalSlider,
                          VNX::ParameterPresenterId::VerticalFader,
                          VNX::ParameterPresenterId::RotaryKnob}) {
        host.setPresenterOverride(id);
        EXPECT_EQ(host.presenter()->primaryEditor()->toolTip(), "0.125 s");
        if (auto* value = host.presenter()->valueEditor()) EXPECT_EQ(value->toolTip(), "0.125 s");
    }
    binding->setDisplayScale(1000.0);
    binding->setUnit(QStringLiteral("ms"));
    binding->setDecimals(0);
    EXPECT_EQ(host.presenter()->primaryEditor()->toolTip(), "125 ms");
    binding->beginEdit();
    binding->previewEdit(0.5);
    EXPECT_EQ(host.presenter()->primaryEditor()->toolTip(), "500 ms");
    binding->cancelEdit();
    EXPECT_EQ(host.presenter()->primaryEditor()->toolTip(), "125 ms");
    EXPECT_TRUE(host.toolTip().isEmpty());
    host.setToolTip(QStringLiteral("Start offset"));
    host.setPresenterOverride(VNX::ParameterPresenterId::SliderWithValue);
    host.rebuildPresenter();
    binding->setValue(0.75);
    ASSERT_NE(host.presenter()->valueEditor(), nullptr);
    EXPECT_EQ(host.presenter()->valueEditor()->toolTip(), QStringLiteral("Start offset\n750 ms"));
}

TEST(ParameterControlHost, StandaloneNumericTipsFollowFormattedAndSilentlyRefreshedValues)
{
    ensureApp();
    VNX::DecimalSpinBox decimal;
    decimal.setDecimals(3);
    new VNX::NumericToolTip(&decimal, QStringLiteral(" s"));
    decimal.setValue(1.2);
    EXPECT_EQ(decimal.toolTip(), "1.2 s");
    {
        const QSignalBlocker block(&decimal);
        decimal.setValue(0.125);
    }
    QHelpEvent hover(QEvent::ToolTip, QPoint(2, 2), QPoint(2, 2));
    QApplication::sendEvent(&decimal, &hover);
    EXPECT_EQ(decimal.toolTip(), "0.125 s");
    QToolTip::hideText();
    QSpinBox integer;
    new VNX::NumericToolTip(&integer, QStringLiteral(" cent"));
    integer.setValue(25);
    EXPECT_EQ(integer.toolTip(), "25 cent");
}

TEST(ParameterControlHost, IndependentValueEditorSharesTheFieldWithoutDependingOnSliderLayout)
{
    ensureApp();
    auto host = std::make_unique<VNX::ParameterControlHost>(QStringLiteral("Mode"));
    host->setPresenterOverride(VNX::ParameterPresenterId::HorizontalSlider);
    auto* binding = host->binding();
    binding->setRange(2, 5);
    binding->setQuantum(1);
    binding->setDiscreteSelection(true);
    binding->setAnnotation([](double value) {
        return value == 3 ? QStringLiteral("Ready") : QString();
    });
    VNX::ParameterValueEditor editor(binding, &VNX::ThemeManager::instance());
    editor.resize(180, 30);
    editor.show();
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted,
                     [&commits](double) { ++commits; });
    ASSERT_EQ(host->presenter()->valueEditor(), nullptr);
    ASSERT_EQ(host->presenter()->choiceEditor(), nullptr);
    auto* choice = editor.choiceEditor();
    ASSERT_NE(choice, nullptr);
    ASSERT_EQ(choice->count(), 4);
    EXPECT_EQ(choice->itemText(0), QStringLiteral("2"));
    EXPECT_EQ(choice->itemText(1), QStringLiteral("Ready"));
    choice->setCurrentIndex(1);
    EXPECT_DOUBLE_EQ(binding->value(), 3);
    EXPECT_EQ(host->presenter()->slider()->value(), 1);
    EXPECT_EQ(commits, 1);
    auto* slider = host->presenter()->slider();
    slider->setSliderDown(true);
    slider->setValue(2);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_EQ(choice->currentData().toInt(), 4);
    slider->setSliderDown(false);
    EXPECT_FALSE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->value(), 4);
    host->rebuildPresenter();
    EXPECT_EQ(editor.choiceEditor(), choice);
    EXPECT_EQ(choice->currentData().toInt(), 4);
    EXPECT_EQ(host->presenter()->valueEditor(), nullptr);
    binding->setDiscreteSelection(false);
    binding->setQuantum(0);
    binding->setDecimals(2);
    auto* spin = editor.valueEditor();
    ASSERT_NE(spin, nullptr);
    const int before = commits;
    spin->setValue(3.25);
    EXPECT_DOUBLE_EQ(binding->value(), 3.25);
    EXPECT_EQ(commits, before + 1);
    EXPECT_EQ(host->presenter()->slider()->value(), binding->positionOf(3.25, 1000));
    host.reset();
    EXPECT_FALSE(editor.isEnabled());
    editor.cancelPendingInput();
    spin->setValue(4.5);
    EXPECT_EQ(commits, before + 1);
}

TEST(ParameterControlHost, SwitchesOneBindingAcrossAllPresenterKinds)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Volume"));
    auto* binding = host.binding();
    binding->setRange(-96.0, 24.0);
    binding->setDefaultValue(0.0);
    binding->setDecimals(1);
    binding->setUnit(QStringLiteral(" dB"));
    binding->setValue(-12.0);

    const VNX::ParameterPresenterId ids[]{
        VNX::ParameterPresenterId::SliderWithValue,
        VNX::ParameterPresenterId::HorizontalSlider,
        VNX::ParameterPresenterId::VerticalFader,
        VNX::ParameterPresenterId::RotaryKnob,
        VNX::ParameterPresenterId::SliderWithValue};
    for (const auto id : ids)
    {
        const auto previousId = host.presenterId();
        QPointer<VNX::ParameterPresenter> previous = host.presenter();
        host.setPresenterOverride(id);
        if (previousId != id)
            EXPECT_TRUE(previous.isNull());
        EXPECT_EQ(host.presenterId(), id);
        EXPECT_EQ(host.binding(), binding);
        EXPECT_DOUBLE_EQ(binding->value(), -12.0);
        EXPECT_DOUBLE_EQ(binding->defaultValue(), 0.0);
        EXPECT_EQ(binding->unit(), QStringLiteral(" dB"));
        EXPECT_EQ(host.presenter()->primaryEditor()->accessibleName(),
                  QStringLiteral("Volume"));
        if (host.presenter()->valueEditor())
        {
            EXPECT_EQ(host.presenter()->valueEditor()->accessibleName(),
                      QStringLiteral("Volume"));
            EXPECT_DOUBLE_EQ(host.presenter()->valueEditor()->value(), -12.0);
        }
    }
}

TEST(ParameterControlHost, PresenterReplacementPreservesActiveSession)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Volume"));
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setValue(20.0);
    binding->beginEdit();
    binding->previewEdit(45.0);

    host.setPresenterOverride(VNX::ParameterPresenterId::VerticalFader);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);

    host.setPresenterOverride(VNX::ParameterPresenterId::RotaryKnob);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);
    binding->cancelEdit();
    EXPECT_DOUBLE_EQ(binding->value(), 20.0);
}

TEST(ParameterControlHost, UnsupportedOverrideFallsBackDeterministically)
{
    ensureApp();
    VNX::ParameterControlHost host(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::EffectParameter);

    host.setPresenterOverride(VNX::ParameterPresenterId::VerticalFader);

    EXPECT_EQ(host.presenterId(), VNX::ParameterPresenterId::RotaryKnob);
}

TEST(ParameterControlHost, ThemeSwitchAndReloadRebuildPresentersAndKeepSession)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writePresenterTheme(root.path(), QStringLiteral("theme.slider"),
                                    QStringLiteral("horizontal-slider")));
    ASSERT_TRUE(writePresenterTheme(root.path(), QStringLiteral("theme.knob"),
                                    QStringLiteral("rotary-knob")));
    VNX::ThemeManager manager(root.path(), std::make_unique<VNX::MemorySettingsStorage>());
    VNX::ParameterControlHost host(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::InspectorFloat, nullptr, &manager);
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setValue(20.0);
    binding->beginEdit();
    binding->previewEdit(45.0);
    QPointer<VNX::ParameterPresenter> builtin = host.presenter();

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.slider")).ok());
    EXPECT_TRUE(builtin.isNull());
    EXPECT_EQ(host.presenterId(),
              VNX::ParameterPresenterId::HorizontalSlider);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);
    QPointer<VNX::ParameterPresenter> slider = host.presenter();

    ASSERT_TRUE(manager.reloadTheme().ok());
    EXPECT_TRUE(slider.isNull());
    EXPECT_EQ(host.presenterId(), VNX::ParameterPresenterId::HorizontalSlider);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);
    slider = host.presenter();

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.knob")).ok());
    EXPECT_TRUE(slider.isNull());
    EXPECT_EQ(host.presenterId(), VNX::ParameterPresenterId::RotaryKnob);
    EXPECT_TRUE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->editStartValue(), 20.0);
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);

    binding->commitEdit();
    EXPECT_FALSE(binding->isEditing());
    EXPECT_DOUBLE_EQ(binding->value(), 45.0);
}

TEST(ParameterControlHost, PresenterRebuildDoesNotCommitRangeClamp)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Distance"));
    auto* binding = host.binding();
    binding->setRange(0.01, 100.0);
    binding->setValue(1.0);
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted,
                     [&commits](double) { ++commits; });

    for (const auto id : {VNX::ParameterPresenterId::SliderWithValue,
                          VNX::ParameterPresenterId::VerticalFader,
                          VNX::ParameterPresenterId::RotaryKnob})
    {
        host.setPresenterOverride(id);
        EXPECT_DOUBLE_EQ(binding->value(), 1.0);
        EXPECT_EQ(commits, 0);
    }
}

TEST(ParameterControlHost, PresenterSyncDoesNotCommitRoundedDisplayValue)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Value"));
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setDecimals(2);
    int commits = 0;
    QObject::connect(binding, &VNX::ParameterBinding::valueCommitted,
                     [&commits](double) { ++commits; });

    for (const auto id : {VNX::ParameterPresenterId::VerticalFader,
                          VNX::ParameterPresenterId::RotaryKnob})
    {
        host.setPresenterOverride(id);
        binding->setValue(0.0);
        commits = 0;
        binding->commitValue(10.005);

        EXPECT_EQ(commits, 1);
        EXPECT_DOUBLE_EQ(binding->value(), 10.005);
        EXPECT_DOUBLE_EQ(host.presenter()->spinBox()->value(), 10.01);
    }
}

TEST(ParameterControlHost, RebuildCommitsActivePresenterGesture)
{
    ensureApp();
    VNX::ParameterControlHost host(QStringLiteral("Volume"));
    auto* binding = host.binding();
    binding->setRange(0.0, 100.0);
    binding->setValue(20.0);
    int ended = 0;
    QObject::connect(binding, &VNX::ParameterBinding::editEnded,
                     [&ended](double, double, bool committed) {
        if (committed)
            ++ended;
    });

    auto* slider = host.presenter()->slider();
    ASSERT_NE(slider, nullptr);
    slider->setSliderDown(true);
    slider->setValue(750);
    ASSERT_TRUE(binding->isEditing());

    host.rebuildPresenter();

    EXPECT_FALSE(binding->isEditing());
    EXPECT_EQ(ended, 1);
    EXPECT_DOUBLE_EQ(binding->value(), 75.0);
}

TEST(ParameterPresenterRegistry, ExposesFiniteIdsCapabilitiesAndContexts)
{
    using Capability = VNX::ParameterPresenterCapability;
    EXPECT_EQ(VNX::parseParameterPresenterId(
                  QStringLiteral("rotary-knob")),
              VNX::ParameterPresenterId::RotaryKnob);
    EXPECT_FALSE(VNX::parseParameterPresenterId(
        QStringLiteral("VNX::ParameterKnob")));
    EXPECT_EQ(VNX::parseParameterPresenterContext(
                  QStringLiteral("effect.parameter")),
              VNX::ParameterPresenterContext::EffectParameter);
    EXPECT_TRUE(VNX::parameterPresenterCapabilities(
                    VNX::ParameterPresenterId::VerticalFader)
                    .testFlag(Capability::Vertical));
    EXPECT_FALSE(VNX::parameterPresenterSupportsContext(
        VNX::ParameterPresenterId::VerticalFader,
        VNX::ParameterPresenterContext::EffectParameter));
}
