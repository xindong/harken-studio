#include "Core/Components/VMixNode.h"
#include "Core/ProjectTypes.h"
#include "Core/ThemeManager.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"
#include "Widgets/Inspector/BindingFieldEditor.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorSlotStrip.h"
#include "Widgets/Inspector/InspectorTabContainer.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Shared/InspectorFormRow.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

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

const VNX::PropertyDescriptor* findField(const VNX::PropertySchema& schema,
                                         QLatin1String field)
{
    for (const auto& descriptor : schema)
        if (descriptor.field == field) return &descriptor;
    return nullptr;
}

}

TEST(InspectorSlotStrip, OccupiedSlotsShareGutterWithoutClipping)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;

    VNX::InspectorSlotStrip strip;
    auto* attenuation = new QToolButton();
    attenuation->setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
    auto* binding = new QToolButton();
    binding->setObjectName(QStringLiteral("bindingPoint_volume"));
    binding->setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
    strip.addSlot(VNX::InspectorSlotKind::Attenuation, attenuation);
    strip.addSlot(VNX::InspectorSlotKind::Binding, binding);
    strip.resize(strip.sizeHint());
    strip.show();
    QApplication::processEvents();

    EXPECT_EQ(strip.occupiedSlotCount(), 2);
    EXPECT_TRUE(attenuation->isVisible());
    EXPECT_TRUE(binding->isVisible());
    EXPECT_GE(strip.width(), attenuation->width() + binding->width());
    EXPECT_EQ(binding->mapTo(&strip, QPoint()).x(), 0);
    EXPECT_EQ(attenuation->mapTo(&strip, QPoint()).x(),
              inspector.bindingSlotWidth + inspector.slotSpacing);
}

TEST(InspectorSlotStrip, EmptyColumnsKeepTheirPositionsAndSeparator)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;

    VNX::InspectorSlotStrip strip;
    auto* attenuation = new QToolButton();
    attenuation->setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
    auto* binding = new QToolButton();
    binding->setFixedSize(inspector.bindingSlotWidth, inspector.slotHeight);
    strip.addSlot(VNX::InspectorSlotKind::Attenuation, attenuation);
    strip.addSlot(VNX::InspectorSlotKind::Binding, binding);
    strip.resize(strip.sizeHint());
    strip.show();
    QApplication::processEvents();

    EXPECT_EQ(strip.occupiedSlotCount(), 2);
    EXPECT_TRUE(attenuation->isVisible());
    EXPECT_TRUE(binding->isVisible());
    EXPECT_EQ(strip.findChildren<QToolButton*>().size(), 2);
    EXPECT_EQ(strip.minimumSizeHint().width(),
              attenuation->width() + binding->width() + inspector.slotSpacing);
    const QPoint position = attenuation->mapTo(&strip, QPoint());
    binding->hide();
    QApplication::processEvents();
    EXPECT_EQ(attenuation->mapTo(&strip, QPoint()), position);
    for (int scale : {1, 2}) {
        SCOPED_TRACE(::testing::Message() << "scale=" << scale);
        QImage image(strip.size() * scale, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        strip.render(&image);
        const int x = (inspector.bindingSlotWidth + inspector.slotSpacing / 2) * scale;
        const int y = inspector.slotHeight * scale / 2;
        const QColor color = VNX::ThemeManager::instance().colors().borderPrimary;
        for (int column = 0; column < scale; ++column)
            EXPECT_EQ(image.pixelColor(x + column, y), color);
        EXPECT_NE(image.pixelColor(x - 1, y), color);
        EXPECT_NE(image.pixelColor(x + scale, y), color);
        const int top = (inspector.slotHeight - 14) / 2 * scale;
        const int bottom = top + 14 * scale;
        for (int row = 0; row < image.height(); ++row)
            EXPECT_EQ(image.pixelColor(x, row) == color, row >= top && row < bottom);
    }
}

TEST(InspectorSlotStrip, VariableAndAttenuationFactoriesFillSeparateColumns)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    const auto* volume = findField(VNX::VMixNode::schema(), QLatin1String("volume"));
    ASSERT_NE(volume, nullptr);
    VNX::PropertyEditorEngine engine;
    engine.setSchema({*volume}, VNX::ComponentType::MixNode);
    engine.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    ASSERT_NE(page, nullptr);

    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume")),
              nullptr);
    auto* strip = page->findChild<VNX::InspectorSlotStrip*>();
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->occupiedSlotCount(), 2);
    EXPECT_NE(strip->slotWidget(VNX::InspectorSlotKind::Binding), nullptr);
    auto* attenuation = strip->slotWidget(VNX::InspectorSlotKind::Attenuation);
    ASSERT_NE(attenuation, nullptr);
    EXPECT_FALSE(attenuation->isEnabled());
    const std::vector<VNX::InspectorSlotKind> expected{
        VNX::InspectorSlotKind::Binding, VNX::InspectorSlotKind::Attenuation};
    EXPECT_EQ(VNX::InspectorCustomizationRegistry::instance().registeredSlotKinds(), expected);

    page->resize(360, 160);
    page->show();
    QApplication::processEvents();
    auto* gutter = strip->parentWidget();
    const int leftPadding = strip->mapTo(gutter, QPoint()).x();
    const int rightPadding = gutter->width() - leftPadding - strip->width();
    EXPECT_LE(qAbs(leftPadding - rightPadding), 1);
    EXPECT_EQ(leftPadding + rightPadding,
              VNX::ThemeManager::instance().metrics().inspector.slotSpacing);
    const int topPadding = strip->mapTo(gutter, QPoint()).y();
    const int bottomPadding = gutter->height() - topPadding - strip->height();
    EXPECT_LE(qAbs(topPadding - bottomPadding), 1);
    EXPECT_EQ(strip->height(), VNX::ThemeManager::instance().metrics().inspector.slotHeight);
}

TEST(InspectorSlotStrip, UnboundFieldsKeepEmptySlot)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::PropertyEditorEngine engine;
    engine.setSchema({{.field = "plainValue", .type = VNX::PropType::Float,
                       .label = "Value", .section = VNX::PropertySection::Playback}}, VNX::ComponentType::MixNode);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* gutter = page->findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_plainValue"));
    ASSERT_NE(gutter, nullptr);
    EXPECT_EQ(gutter->width(),
              VNX::ThemeManager::instance().metrics().inspector.slotWidthFor(2));
    EXPECT_EQ(page->findChild<VNX::InspectorSlotStrip*>(), nullptr);
}

TEST(InspectorSlotStrip, ThreeTabsShareFieldAndTableGutters)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::InspectorCustomization customization;
    customization.tabbedLayout = true;
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Piece),
                     VNX::ComponentType::Audio);
    engine.setItemType(VNX::ProjectItem::Type::Piece);
    engine.setCustomization(&customization);
    std::unique_ptr<QWidget> page(engine.buildPage());
    auto* tabs = page->findChild<VNX::InspectorTabContainer*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->tabCount(), 3);

    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    for (const auto* field : {"volume", "pitch", "notes", "outputBusVolume",
                              "mainOutputBusRef", "sends", "spatialPanning",
                              "spatialIs3DEnabled", "spatialAttenuationRef",
                              "spatialAttenuationDistanceScaling"}) {
        auto* gutter = page->findChild<QWidget*>(
            QStringLiteral("inspectorFieldGutter_") + QLatin1String(field));
        ASSERT_NE(gutter, nullptr) << field;
        EXPECT_EQ(gutter->width(), inspector.slotWidthFor(2)) << field;
    }

    const char* fields[] = {"volume", "outputBusVolume", "spatialPanning"};
    for (int width : {360, 640, 1280}) {
        page->resize(width, 800);
        page->show();
        QRect reference;
        for (int tab = 0; tab < 3; ++tab) {
            SCOPED_TRACE(::testing::Message() << "width=" << width << " tab=" << tab);
            tabs->setCurrentTab(tab);
            QApplication::processEvents();
            auto* gutter = page->findChild<QWidget*>(
                QStringLiteral("inspectorFieldGutter_") + QLatin1String(fields[tab]));
            auto* row = gutter->parentWidget();
            auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
            ASSERT_NE(layout, nullptr);
            const QRect cell = layout->itemAt(1)->geometry();
            const QRect value(row->mapTo(page.get(), cell.topLeft()), cell.size());
            EXPECT_LT(value.right(), gutter->mapTo(page.get(), QPoint()).x());
            if (tab == 0) reference = value;
            else {
                EXPECT_EQ(value.left(), reference.left());
                EXPECT_EQ(value.right(), reference.right());
            }
        }
    }
}

TEST(InspectorSlotStrip, CustomAndLateRowsUsePageSlotCount)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    QWidget page;
    VNX::setInspectorFormSlotCount(&page, 2);
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        VNX::ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);
    auto* section = provider->build(&page);
    ASSERT_NE(section, nullptr);
    const int width = VNX::ThemeManager::instance().metrics().inspector.slotWidthFor(2);
    auto* modeGutter = section->findChild<QWidget*>(
        QStringLiteral("selectionFormGutter_mode"));
    ASSERT_NE(modeGutter, nullptr);
    EXPECT_EQ(modeGutter->width(), width);

    auto* host = new QWidget(section);
    const auto lateRow = VNX::makeInspectorFormRow(
        host, QStringLiteral("late"), QStringLiteral("Late"), new QLabel(host), {}, 0);
    auto* gutter = lateRow.row->findChild<QWidget*>(
        QStringLiteral("selectionFormGutter_late"));
    ASSERT_NE(gutter, nullptr);
    EXPECT_EQ(gutter->width(), width);

    QWidget standalone;
    EXPECT_EQ(VNX::inspectorFormSlotCount(&standalone, 0), 0);
}

TEST(InspectorSlotStrip, ClosedProductGatesDoNotGrowHangPoints)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::ProjectItem item(QStringLiteral("Bound"), VNX::ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(VNX::ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(VNX::ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(&item);

    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_volume")),
              nullptr);
    EXPECT_NE(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_pitch")),
              nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_makeUpGain")),
              nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_outputBusVolume")),
              nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("bindingPoint_spatialSpread")),
              nullptr);
}

TEST(InspectorSlotStrip, NeutralGlyphsDistinguishKindsAndStatesAtBothDpiScales)
{
    ensureApp();
    const auto& colors = VNX::ThemeManager::instance().colors();
    VNX::InspectorSlotButton binding(VNX::InspectorSlotKind::Binding);
    VNX::InspectorSlotButton attenuation(VNX::InspectorSlotKind::Attenuation);
    const QSize size = binding.size();
    EXPECT_EQ(size, QSize(14, 20));
    for (int scale : {1, 2}) {
        const auto render = [&](VNX::InspectorSlotButton& button) {
            QImage image(size * scale, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(scale);
            image.fill(Qt::transparent);
            button.render(&image);
            return image;
        };
        const QPoint bindingInterior(size.width() * scale / 2, size.height() * scale / 2);
        const QPoint attenuationInterior((size.width() / 2 - 2) * scale,
                                         (size.height() / 2 + 4) * scale);
        for (bool enabled : {true, false}) {
            binding.setEnabled(enabled);
            attenuation.setEnabled(enabled);
            const QColor color = enabled ? colors.textSecondary : colors.textDisabled;
            for (bool bound : {false, true}) {
                SCOPED_TRACE(::testing::Message() << "scale=" << scale
                    << " enabled=" << enabled << " bound=" << bound);
                binding.setBound(bound);
                attenuation.setBound(bound);
                const auto bindingImage = render(binding);
                const auto attenuationImage = render(attenuation);
                EXPECT_NE(bindingImage, attenuationImage);
                if (bound) {
                    EXPECT_EQ(bindingImage.pixelColor(bindingInterior), color);
                    EXPECT_EQ(attenuationImage.pixelColor(attenuationInterior), color);
                } else {
                    EXPECT_NE(bindingImage.pixelColor(bindingInterior), color);
                    EXPECT_NE(attenuationImage.pixelColor(attenuationInterior), color);
                }
                EXPECT_EQ(binding.size(), size);
                EXPECT_EQ(attenuation.size(), size);
            }
        }
    }
}

TEST(InspectorSlotStrip, FieldReadOnlyAndConditionsDisableBindingEntry)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    VNX::ProjectItem item(QStringLiteral("Bound"), VNX::ProjectItem::Type::Sound);
    item.attachDefaultComponents();
    auto volume = *findField(VNX::VMixNode::schema(), QLatin1String("volume"));
    const auto pitch = *findField(VNX::VMixNode::schema(), QLatin1String("pitch"));
    for (bool readOnly : {false, true}) {
        volume.flags.readOnly = readOnly;
        volume.editCondition = QStringLiteral("pitch == 1");
        item.comp<VNX::VMixNode>()->pitch = 0.0f;
        VNX::PropertyEditorEngine engine;
        engine.setSchema({volume, pitch}, VNX::ComponentType::MixNode);
        engine.setItemType(VNX::ProjectItem::Type::Sound);
        std::unique_ptr<QWidget> page(engine.buildPage());
        engine.bindItem(&item);
        auto* button = page->findChild<VNX::BindingFieldDecorator*>("bindingPoint_volume");
        ASSERT_NE(button, nullptr);
        EXPECT_FALSE(button->isEnabled());
        EXPECT_TRUE(button->isReadOnly());
        button->click();
        EXPECT_EQ(button->findChild<VNX::BindingFieldEditor*>(), nullptr);
        item.comp<VNX::VMixNode>()->pitch = 1.0f;
        engine.refreshField(QStringLiteral("pitch"));
        EXPECT_EQ(button->isEnabled(), !readOnly);
        EXPECT_EQ(button->isReadOnly(), readOnly);
        item.comp<VNX::VMixNode>()->pitch = 0.0f;
        engine.refreshField(QStringLiteral("pitch"));
        EXPECT_FALSE(button->isEnabled());
    }
}

TEST(InspectorSlotStrip, HandWrittenRowsShareSlotWidthHelper)
{
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    EXPECT_EQ(inspector.slotWidthFor(1),
              inspector.linkSlotWidth + inspector.slotSpacing);
    EXPECT_EQ(inspector.slotWidthFor(2),
              inspector.bindingSlotWidth * 2 + inspector.slotSpacing * 2);
}
