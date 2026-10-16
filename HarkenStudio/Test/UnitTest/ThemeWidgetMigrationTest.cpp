#include "ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BaseDialog.h"
#include "Core/BindingEditing.h"
#include "Core/BuiltinTheme.h"
#include "Core/Components/VAttenuation.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Widgets/Common/LayoutDebugTint.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/RangeField.h"
#include "Widgets/PreviewPanel/PreviewPanel.h"
#include "Widgets/PreviewPanel/RehearsalTable.h"
#include "Widgets/PlatformBar/PlaybackControls.h"
#include "Widgets/PlatformBar/PlatformBar.h"
#include "Widgets/Blueprint/BlueprintPanel.h"
#include "Widgets/Inspector/InspectorSlotButton.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/TitleBar/TitleBarButton.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/GridEditor/GridHeaderRef.h"
#include "Widgets/Inspector/BindingFieldEditor.h"
#include "Widgets/Inspector/Drawers/ColorFieldDrawer.h"
#include "Widgets/Inspector/Providers/DimensionsSectionProvider.h"
#include "Widgets/Common/SourceRangeThumb.h"
#include "Widgets/TransitionEditor/TransitionFadeButton.h"

#include <gtest/gtest.h>

#include <vector>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEnterEvent>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

TEST(ThemeWidgetMigration, UsesApplicationSkinWithoutLocalStyleSheets)
{
    ensureApp();

    VNX::BaseDialog dialog;
    VNX::StartupLoadingWindow loading;
    PreviewPanel audition(nullptr);
    VNX::PlaybackControls playback;

    EXPECT_TRUE(dialog.styleSheet().isEmpty());
    EXPECT_TRUE(loading.styleSheet().isEmpty());
    EXPECT_TRUE(audition.styleSheet().isEmpty());
    EXPECT_TRUE(playback.styleSheet().isEmpty());

    EXPECT_NE(loading.findChild<QLabel*>(
                  QStringLiteral("startupLoadingProject")),
              nullptr);
    EXPECT_NE(loading.findChild<QProgressBar*>(
                  QStringLiteral("startupLoadingProgress")),
              nullptr);
}

TEST(ThemeWidgetMigration, StructuralHostsTakeTransparencyFromTheApplicationSkin)
{
    ensureApp();

    VNX::ParameterSlider slider(QStringLiteral("Volume"));
    VNX::RangeField range;
    QWidget container;
    VNX::DebugTint::tintContainer(&container);

    EXPECT_TRUE(slider.styleSheet().isEmpty());
    EXPECT_TRUE(range.styleSheet().isEmpty());
    if (!VNX::DebugTint::kEnabled)
        EXPECT_TRUE(container.styleSheet().isEmpty());

    const QString skin = VNX::QtControlSkin::current().applicationStyleSheet();
    ASSERT_FALSE(skin.isEmpty());
    EXPECT_TRUE(skin.contains(QStringLiteral(
        "VNX--ParameterSlider,\nVNX--RangeField {\n    background: transparent;\n}")));
    EXPECT_TRUE(skin.contains(QStringLiteral(
        ".QWidget {\n    background-color: transparent;\n}")));
}

TEST(ThemeWidgetMigration, PlaybackStateUsesSemanticProperties)
{
    ensureApp();
    VNX::PlaybackControls playback;
    auto* play = playback.findChild<QToolButton*>(QStringLiteral("playBtn"));
    auto* reset = playback.findChild<QToolButton*>(QStringLiteral("resetBtn"));
    ASSERT_NE(play, nullptr);
    ASSERT_NE(reset, nullptr);
    EXPECT_FALSE(reset->isEnabled());
    EXPECT_FALSE(play->property("playing").toBool());

    playback.setPlaying(true);
    EXPECT_TRUE(play->property("playing").toBool());
    playback.setPlaying(false);
    EXPECT_FALSE(play->property("playing").toBool());
}

TEST(ThemeWidgetMigration, IconButtonRolesCoverCommandsWithoutIncludingTextOrValueFields)
{
    ensureApp();
    QWidget host;
    auto* toolbar = new VNX::PlatformBar(&host);
    auto* playback = new VNX::PlaybackControls(toolbar);
    toolbar->addCenterWidget(playback);
    auto* action = new QAction(QStringLiteral("Remote"), toolbar);
    auto* remote = toolbar->addLeadingAction(action, QStringLiteral("remoteConnectButton"));
    auto* blueprint = new VNX::BlueprintPanel(&host);
    auto* reference = new VNX::ObjectRefWidget(&host);
    auto* footer = new VNX::ListFooter(&host);
    auto* textAction = new QAction(QStringLiteral("Export"), footer);
    auto* textButton = footer->addActionButton(textAction);
    auto* slot = new VNX::InspectorSlotButton(VNX::InspectorSlotKind::Binding, &host);
    auto* close = new VNX::TitleBarButton(VNX::TitleBarButtonKind::Close, &host);
    auto* fade = new VNX::TransitionFadeButton(&host);

    for (const auto* button : QList<QAbstractButton*>{
             remote, toolbar->findChild<QToolButton*>(QStringLiteral("platformBarAll")),
             footer->addButton(), footer->removeButton(), slot, close}) {
        ASSERT_NE(button, nullptr);
        EXPECT_TRUE(button->property("flatIconButton").toBool()) << button->objectName().toStdString();
    }
    for (auto* owner : QList<QWidget*>{playback, blueprint, reference}) {
        for (const auto* button : owner->findChildren<QToolButton*>()) {
            if (!button->icon().isNull() && button->toolButtonStyle() == Qt::ToolButtonIconOnly)
                EXPECT_TRUE(button->property("flatIconButton").toBool()) << button->objectName().toStdString();
            else
                EXPECT_FALSE(button->property("flatIconButton").toBool()) << button->text().toStdString();
        }
    }
    EXPECT_FALSE(textButton->property("flatIconButton").toBool());
    EXPECT_FALSE(fade->property("flatIconButton").toBool());
    EXPECT_FALSE(reference->property("flatIconButton").toBool());
    for (auto* widget : host.findChildren<QWidget*>())
        EXPECT_TRUE(widget->styleSheet().isEmpty()) << widget->objectName().toStdString();
}

TEST(ThemeWidgetMigration, PreviewPanelHeightMatchesContent)
{
    ensureApp();
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen())
        manager.closeProject();
    VNX::ActionManager::instance().clearAllForTesting();
    VNX::registerProjectActions(VNX::ActionManager::instance(), &manager,
                                nullptr, {});
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PreviewPanelHeight");
    config.location = temp.path();
    ASSERT_TRUE(manager.createProject(config));
    ASSERT_NE(manager.createItem(nullptr, QStringLiteral("Drive"),
                                 VNX::ProjectItem::Type::Variable),
              nullptr);

    PreviewPanel audition(nullptr);
    ASSERT_NE(audition.variableTable(), nullptr);
    ASSERT_NE(audition.bindingTable(), nullptr);
    audition.ensurePolished();

    EXPECT_EQ(audition.sizePolicy().verticalPolicy(), QSizePolicy::Expanding);
    EXPECT_GE(audition.minimumSizeHint().height(), 28 + 16);
    EXPECT_LE(audition.minimumSizeHint().height(), audition.sizeHint().height());

    manager.closeProject();
    VNX::ActionManager::instance().clearAllForTesting();
}

TEST(ThemeWidgetMigration, TableFieldFramesShareGeometryColorsAndStates)
{
    ensureApp();
    VNX::ThemeManager::instance();
    const QString previousStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    auto& manager = VNX::ProjectManager::instance();
    if (manager.isProjectOpen()) manager.closeProject();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("TableFieldFrames");
    config.location = directory.path();
    ASSERT_TRUE(manager.createProject(config));
    const auto closeProject = qScopeGuard([&] { manager.closeProject(); });
    auto* sound = manager.createItem(nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    auto* variable = manager.createItem(nullptr, QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable);
    auto* attenuation = manager.createItem(nullptr, QStringLiteral("Distance"), VNX::ProjectItem::Type::Attenuation);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(variable, nullptr);
    ASSERT_NE(attenuation, nullptr);
    auto* component = attenuation->comp<VNX::VAttenuation>();
    ASSERT_NE(component, nullptr);
    component->dimensions.emplaceBack(VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume});
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound, VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), variable->guid)));

    QWidget form;
    auto* layout = new QVBoxLayout(&form);
    layout->setAlignment(Qt::AlignTop);
    auto* text = new QLineEdit(QStringLiteral("Name"), &form);
    auto* search = new VNX::SearchField(&form);
    auto* numeric = new QDoubleSpinBox(&form);
    auto* combo = new QComboBox(&form);
    combo->addItem(QStringLiteral("Random"));
    auto* reference = new VNX::ObjectRefWidget(&form);
    reference->setCurrentObjectID(sound->objectID);
    auto* gridReference = new VNX::GridHeaderRef(&form);
    gridReference->setCurrentObjectID(variable->objectID);
    auto* source = new VNX::SourceRangeThumb(&form);
    VNX::AssetInfo asset;
    asset.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    asset.relativePath = QStringLiteral("short.wav");
    asset.absolutePath = QDir(VNX::AssetRegistry::instance().projectRoot())
                             .filePath(QStringLiteral("AudioSources/short.wav"));
    asset.metadata.sampleRate = 48000;
    asset.metadata.channels = 1;
    asset.metadata.totalSamples = 64;
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(
        asset.absolutePath, std::vector<float>(64, 0.0f), 1, 48000));
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(asset));
    VNX::SoundSourceDef trimmed;
    trimmed.sourceId = asset.sourceID;
    source->setSource(trimmed, &asset);
    auto* fade = new VNX::TransitionFadeButton(&form);
    const QList<QWidget*> standalone = {text, search, numeric, combo, reference,
                                        gridReference, source, fade};
    for (auto* field : standalone) layout->addWidget(field);

    VNX::DimensionsSectionProvider dimensions;
    auto* dimensionSection = dimensions.build(&form);
    dimensions.bind(dimensionSection, attenuation);
    layout->addWidget(dimensionSection);
    auto* dimensionCurve = dimensionSection->findChild<QWidget*>(QStringLiteral("curveThumbnail"));
    ASSERT_NE(dimensionCurve, nullptr);
    auto* bindings = new VNX::BindingFieldEditor(&form);
    bindings->bind(sound, VNX::BindingSemantics::volumeAddress());
    layout->addWidget(bindings);
    auto* bindingCurve = bindings->findChild<QWidget*>(QStringLiteral("bindingCurveThumb"));
    ASSERT_NE(bindingCurve, nullptr);

    VNX::ColorFieldDrawer colorDrawer;
    VNX::PropertyDescriptor descriptor;
    descriptor.field = QStringLiteral("color");
    auto* colorField = colorDrawer.build(descriptor, &form);
    layout->addWidget(colorField);
    auto* swatch = colorField->findChild<QPushButton*>();
    ASSERT_NE(swatch, nullptr);

    const auto render = [](QWidget* field) {
        QImage image(field->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        field->render(&image);
        return image;
    };
    const auto expectFrame = [&](QWidget* field, const QColor& border, const QColor& background) {
        SCOPED_TRACE(field->metaObject()->className());
        SCOPED_TRACE(field->objectName().toStdString());
        const QImage image = render(field);
        ASSERT_GE(image.width(), 4);
        ASSERT_GE(image.height(), 4);
        for (const QPoint corner : {QPoint(0, 0), QPoint(image.width() - 1, 0),
                                    QPoint(0, image.height() - 1),
                                    QPoint(image.width() - 1, image.height() - 1)})
            EXPECT_EQ(image.pixelColor(corner), border);
        EXPECT_EQ(image.pixelColor(image.width() / 2, 1), background);
    };
    QList<QWidget*> fields = standalone;
    fields.append(dimensionCurve);
    fields.append(bindingCurve);

    for (const bool custom : {false, true, false}) {
        SCOPED_TRACE(custom);
        auto theme = VNX::createBuiltinDarkTheme();
        if (custom) {
            theme.ui.metrics.controlHeight = 36;
            theme.ui.metrics.radiusSmall = 7;
            theme.ui.colors.bgPrimary = QColor(QStringLiteral("#182430"));
            theme.ui.colors.bgSecondary = QColor(QStringLiteral("#274832"));
            theme.ui.colors.borderPrimary = QColor(QStringLiteral("#695342"));
            theme.ui.colors.borderSecondary = QColor(QStringLiteral("#c1b4a7"));
            theme.ui.colors.accent = QColor(QStringLiteral("#b28be1"));
        }
        const VNX::QtControlSkin skin(std::make_shared<const VNX::ThemeSnapshot>(theme));
        qApp->setStyleSheet(skin.applicationStyleSheet());
        form.resize(600, 720);
        form.show();
        QApplication::processEvents();
        form.setFocus();
        for (auto* field : fields) {
            field->clearFocus();
            field->setAttribute(Qt::WA_UnderMouse, false);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(field, &leave);
            EXPECT_EQ(field->height(), skin.fieldHeight()) << field->objectName().toStdString();
            expectFrame(field, theme.ui.colors.borderSecondary, theme.ui.colors.bgSecondary);
            field->setEnabled(false);
            expectFrame(field, theme.ui.colors.borderPrimary, theme.ui.colors.bgPrimary);
            field->setEnabled(true);
        }
        for (auto* field : {static_cast<QWidget*>(source), static_cast<QWidget*>(reference),
                            static_cast<QWidget*>(fade), bindingCurve}) {
            field->setAttribute(Qt::WA_UnderMouse, true);
            QEnterEvent enter(QPointF(2, 2), QPointF(2, 2), field->mapToGlobal(QPoint(2, 2)));
            QApplication::sendEvent(field, &enter);
            expectFrame(field, theme.ui.colors.accent, theme.ui.colors.bgSecondary);
            field->setAttribute(Qt::WA_UnderMouse, false);
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(field, &leave);
        }
        gridReference->setSelected(true);
        expectFrame(gridReference, theme.ui.colors.accent, theme.ui.colors.bgSelected);
        gridReference->setSelected(false);
        fade->setDimmed(true);
        expectFrame(fade, theme.ui.colors.borderPrimary, theme.ui.colors.bgPrimary);
        fade->setDimmed(false);
        EXPECT_EQ(swatch->height(), skin.fieldHeight());
        expectFrame(swatch, theme.ui.colors.borderSecondary, QColor(QStringLiteral("#808080")));
    }

    trimmed.trimEndFrame = 128;
    source->setSource(trimmed, &asset);
    ASSERT_TRUE(source->property("rangeWarning").toBool());
    EXPECT_EQ(render(source).pixelColor(0, 0), VNX::ThemeManager::instance().colors().warning);
    source->setSource(VNX::SoundSourceDef{}, nullptr);
    EXPECT_FALSE(source->property("rangeWarning").toBool());
    expectFrame(source, VNX::ThemeManager::instance().colors().borderPrimary,
                VNX::ThemeManager::instance().colors().bgPrimary);
}
