#include "Core/MemorySettingsStorage.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ComponentRecipeHost.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"

#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <memory>

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

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool writeNinePatch(const QString& path,
                    const QColor& border,
                    const QColor& center)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage image(8, 8, QImage::Format_ARGB32_Premultiplied);
    image.fill(border);
    for (int y = 3; y < 5; ++y)
        for (int x = 3; x < 5; ++x)
            image.setPixelColor(x, y, center);
    return image.save(path);
}

QImage renderSlotSurface(QWidget* container)
{
    QImage image(container->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    container->render(&image, QPoint(), QRegion(),
                      QWidget::RenderFlags(QWidget::DrawWindowBackground));
    return image;
}

bool writeRecipeTheme(const QString& root,
                      const QString& id,
                      const QByteArray& recipes,
                      const QByteArray& assets = R"json({"assets":{}})json")
{
    const QString package = QDir(root).filePath(id);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":3,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"" + id.toUtf8()
        + "\",\"version\":\"1.0.0\","
          "\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\","
          "\"recipes\":\"recipes.json\"}}";
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
                     assets)
        && writeFile(QDir(package).filePath(QStringLiteral("recipes.json")),
                     recipes);
}

std::unique_ptr<VNX::ISettingsStorage> memoryStorage()
{
    return std::make_unique<VNX::MemorySettingsStorage>();
}

}

TEST(ComponentRecipeHost, BuiltinRecipesExposeStableTrackSlots)
{
    ensureApp();
    QTemporaryDir root;
    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost track(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* identity = new QLabel(QStringLiteral("Identity"));
    auto* volume = new VNX::ParameterControlHost(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::InspectorFloat);
    auto* meter = new QLabel(QStringLiteral("Meter"));
    auto* params = new QLabel(QStringLiteral("Params"));
    ASSERT_TRUE(track.registerSlot(VNX::ComponentRecipeSlotId::Identity,
                                  identity));
    ASSERT_TRUE(track.registerSlot(VNX::ComponentRecipeSlotId::Volume,
                                  volume));
    ASSERT_TRUE(track.registerSlot(VNX::ComponentRecipeSlotId::Params, params));
    ASSERT_TRUE(track.registerSlot(VNX::ComponentRecipeSlotId::Meter, meter));

    EXPECT_EQ(track.appliedVariant(),
              VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(track.orderedSlotNames(),
              QStringList({QStringLiteral("identity"),
                           QStringLiteral("params"),
                           QStringLiteral("meter")}));
    EXPECT_EQ(volume->presenterContext(),
              VNX::ParameterPresenterContext::InspectorFloat);
    EXPECT_EQ(volume->presenterId(),
              VNX::ParameterPresenterId::SliderWithValue);
}

TEST(ComponentRecipeHost, AppliesVisibilitySizingAndLogicalAssetMetadata)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.constraints"), R"json({"recipes":[{
            "host":"track.header","variant":"track.compact","slots":[
                {"id":"identity","visible":false},
                {"id":"volume","minimum":40,"preferred":64,"maximum":80,
                 "alignment":"center","asset":"slot.volume.background"}
            ]
        }]})json"));
    const QString assetsPath = QDir(root.path()).filePath(
        QStringLiteral("theme.constraints/assets.json"));
    ASSERT_TRUE(writeFile(
        assetsPath,
        R"json({"assets":{"slot.volume.background":{"type":"image","sources":[{"path":"background.png"}]}}})json"));
    ASSERT_TRUE(writeFile(
        QDir(root.path()).filePath(
            QStringLiteral("theme.constraints/background.png")),
        QByteArrayLiteral("not-decoded-by-this-test")));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* identity = new QLabel(QStringLiteral("Identity"));
    auto* volume = new QLabel(QStringLiteral("Volume"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Identity,
                                  identity));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume,
                                  volume));

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.constraints")).ok());

    EXPECT_TRUE(identity->parentWidget()->isHidden());
    EXPECT_EQ(volume->parentWidget()->minimumWidth(), 64);
    EXPECT_EQ(volume->parentWidget()->maximumWidth(), 64);
    EXPECT_EQ(host.slotAsset(VNX::ComponentRecipeSlotId::Volume),
              QStringLiteral("slot.volume.background"));
}

TEST(ComponentRecipeHost, ThemeSwitchReordersSlotsAndPreservesBindingState)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.compact"), R"json({"recipes":[{
            "host":"track.header","breakpoint":"compact",
            "variant":"track.compact","slots":[
                {"id":"meter"},
                {"id":"volume","presenter":"rotary-knob"},
                {"id":"identity"}
            ]
        }]})json"));
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.expanded"), R"json({"recipes":[{
            "host":"track.header","variant":"track.expanded","slots":[
                {"id":"identity"},
                {"id":"volume","presenter":"vertical-fader"},
                {"id":"meter"}
            ]
        }]})json"));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* identity = new QLabel(QStringLiteral("Identity"));
    auto* volume = new VNX::ParameterControlHost(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::InspectorFloat, nullptr, &manager);
    auto* meter = new QLabel(QStringLiteral("Meter"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Identity,
                                  identity));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume,
                                  volume));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Meter, meter));
    volume->binding()->setRange(-96.0, 24.0);
    volume->binding()->setValue(-18.0);
    volume->binding()->beginEdit();
    volume->binding()->previewEdit(-9.0);

    host.setBreakpoint(VNX::ComponentRecipeBreakpoint::Compact);
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.compact")).ok());
    EXPECT_EQ(host.appliedVariant(),
              VNX::ComponentRecipeVariant::TrackCompact);
    EXPECT_EQ(host.orderedSlotNames(),
              QStringList({QStringLiteral("meter"),
                           QStringLiteral("volume"),
                           QStringLiteral("identity")}));
    EXPECT_EQ(volume->presenterContext(),
              VNX::ParameterPresenterContext::InspectorFloat);
    EXPECT_EQ(volume->presenterId(), VNX::ParameterPresenterId::RotaryKnob);
    EXPECT_TRUE(volume->binding()->isEditing());
    EXPECT_DOUBLE_EQ(volume->binding()->editStartValue(), -18.0);
    EXPECT_DOUBLE_EQ(volume->binding()->value(), -9.0);

    host.setBreakpoint(VNX::ComponentRecipeBreakpoint::Default);
    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.expanded")).ok());
    EXPECT_EQ(host.appliedVariant(),
              VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(volume->presenterContext(),
              VNX::ParameterPresenterContext::InspectorFloat);
    EXPECT_EQ(volume->presenterId(),
              VNX::ParameterPresenterId::VerticalFader);
    EXPECT_TRUE(volume->binding()->isEditing());
    volume->binding()->cancelEdit();
    EXPECT_DOUBLE_EQ(volume->binding()->value(), -18.0);
}

TEST(ComponentRecipeHost, TrackHeaderVolumeAcceptsAllInspectorPresenters)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.track.volume"), R"json({"recipes":[{
            "host":"track.header","variant":"track.expanded","slots":[
                {"id":"volume","presenter":"vertical-fader"},
                {"id":"identity"}
            ]
        }]})json"));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new VNX::ParameterControlHost(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::InspectorFloat, nullptr, &manager);
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Identity,
                                  new QLabel(QStringLiteral("Track"))));

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.track.volume")).ok());

    EXPECT_EQ(host.appliedVariant(),
              VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(host.orderedSlotNames(),
              QStringList({QStringLiteral("volume"),
                           QStringLiteral("identity")}));
    EXPECT_EQ(volume->presenterContext(),
              VNX::ParameterPresenterContext::InspectorFloat);
    EXPECT_EQ(volume->presenterId(),
              VNX::ParameterPresenterId::VerticalFader);
}

namespace {

const QByteArray kSlotBackgroundAssets =
    R"json({"assets":{"slot.strip.background":{"type":"ninePatch",)json"
    R"json("insets":{"left":3,"top":3,"right":3,"bottom":3},)json"
    R"json("sources":[{"path":"slot.png"}]}}})json";

const QByteArray kFixedVolumeSlotRecipe =
    R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
    R"json("slots":[{"id":"volume","preferred":40,)json"
    R"json("asset":"slot.strip.background"}]}]})json";

QWidget* layOutSingleSlot(VNX::ComponentRecipeHost& host)
{
    host.resize(200, 40);
    host.layout()->activate();
    return host.slotWidget(VNX::ComponentRecipeSlotId::Volume)->parentWidget();
}

}

TEST(ComponentRecipeHost, SlotBackgroundPaintsTheThemeNinePatchIntoTheSlotRect)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString id = QStringLiteral("theme.slot.ninepatch");
    ASSERT_TRUE(writeRecipeTheme(root.path(), id, kFixedVolumeSlotRecipe,
                                 kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(id + QStringLiteral("/slot.png")),
        QColor(Qt::red), QColor(Qt::blue)));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new QLabel(QStringLiteral("Volume"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));
    ASSERT_TRUE(manager.selectTheme(id).ok());

    QWidget* container = layOutSingleSlot(host);
    ASSERT_EQ(container->size(), QSize(40, 40));
    EXPECT_EQ(host.slotAsset(VNX::ComponentRecipeSlotId::Volume),
              QStringLiteral("slot.strip.background"));

    const QImage painted = renderSlotSurface(container);
    EXPECT_EQ(painted.pixelColor(1, 1), QColor(Qt::red));
    EXPECT_EQ(painted.pixelColor(38, 38), QColor(Qt::red));
    EXPECT_EQ(painted.pixelColor(20, 1), QColor(Qt::red));
    EXPECT_EQ(painted.pixelColor(20, 20), QColor(Qt::blue));
    EXPECT_EQ(painted.pixelColor(5, 20), QColor(Qt::blue));
    EXPECT_EQ(host.slotWidget(VNX::ComponentRecipeSlotId::Volume), volume);
}

TEST(ComponentRecipeHost, SlotAssetLeavesEverySizeHintToTheRecipe)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QByteArray plainRecipe =
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"volume"}]}]})json";
    const QByteArray assetRecipe =
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"volume","asset":"slot.strip.background"}]}]})json";
    ASSERT_TRUE(writeRecipeTheme(root.path(), QStringLiteral("theme.slot.plain"),
                                 plainRecipe));
    const QString assetId = QStringLiteral("theme.slot.sized");
    ASSERT_TRUE(writeRecipeTheme(root.path(), assetId, assetRecipe,
                                 kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(assetId + QStringLiteral("/slot.png")),
        QColor(Qt::red), QColor(Qt::blue)));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new QLabel(QStringLiteral("Volume"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.slot.plain")).ok());
    QWidget* container = layOutSingleSlot(host);
    const QSize plainHint = container->sizeHint();
    const QSize plainMinimumHint = container->minimumSizeHint();
    const QSize plainSize = container->size();

    ASSERT_TRUE(manager.selectTheme(assetId).ok());
    container = layOutSingleSlot(host);
    ASSERT_EQ(renderSlotSurface(container).pixelColor(1, 1), QColor(Qt::red));

    EXPECT_EQ(container->sizeHint(), plainHint);
    EXPECT_EQ(container->minimumSizeHint(), plainMinimumHint);
    EXPECT_EQ(container->size(), plainSize);
}

TEST(ComponentRecipeHost, UndecodableSlotAssetLeavesTheSlotAndContentIntact)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.slot.absent"),
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"volume","preferred":40}]}]})json"));
    const QString brokenId = QStringLiteral("theme.slot.broken");
    ASSERT_TRUE(writeRecipeTheme(root.path(), brokenId, kFixedVolumeSlotRecipe,
                                 kSlotBackgroundAssets));
    ASSERT_TRUE(writeFile(
        QDir(root.path()).filePath(brokenId + QStringLiteral("/slot.png")),
        QByteArrayLiteral("not-a-png")));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new QLabel(QStringLiteral("Volume"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.slot.absent")).ok());
    QWidget* container = layOutSingleSlot(host);
    const QImage withoutAsset = renderSlotSurface(container);
    const QRect withoutAssetGeometry = container->geometry();

    ASSERT_TRUE(manager.selectTheme(brokenId).ok());
    container = layOutSingleSlot(host);

    EXPECT_EQ(host.slotAsset(VNX::ComponentRecipeSlotId::Volume),
              QStringLiteral("slot.strip.background"));
    EXPECT_EQ(renderSlotSurface(container), withoutAsset);
    EXPECT_EQ(container->geometry(), withoutAssetGeometry);
    EXPECT_EQ(host.slotWidget(VNX::ComponentRecipeSlotId::Volume), volume);
    EXPECT_EQ(volume->parentWidget(), container);
    EXPECT_FALSE(container->isHidden());
    EXPECT_TRUE(volume->isEnabled());
    EXPECT_EQ(volume->geometry(), QRect(0, 0, 40, 40));
}

TEST(ComponentRecipeHost, NinePatchWiderThanTheSlotDegradesToAStretchedFill)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString id = QStringLiteral("theme.slot.tiny");
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), id,
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"volume","preferred":4,)json"
        R"json("asset":"slot.strip.background"}]}]})json",
        kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(id + QStringLiteral("/slot.png")),
        QColor(Qt::red), QColor(Qt::blue)));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new QWidget;
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));
    ASSERT_TRUE(manager.selectTheme(id).ok());

    host.resize(200, 4);
    host.layout()->activate();
    QWidget* container = volume->parentWidget();
    ASSERT_EQ(container->size(), QSize(4, 4));

    const QImage painted = renderSlotSurface(container);
    EXPECT_EQ(painted.pixelColor(0, 0), QColor(Qt::red));
    EXPECT_EQ(painted.pixelColor(2, 2), QColor(Qt::blue));
}

TEST(ComponentRecipeHost, ThemeSwitchInvalidatesTheResolvedSlotBackground)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString redId = QStringLiteral("theme.slot.red");
    const QString greenId = QStringLiteral("theme.slot.green");
    ASSERT_TRUE(writeRecipeTheme(root.path(), redId, kFixedVolumeSlotRecipe,
                                 kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(redId + QStringLiteral("/slot.png")),
        QColor(Qt::red), QColor(Qt::blue)));
    ASSERT_TRUE(writeRecipeTheme(root.path(), greenId, kFixedVolumeSlotRecipe,
                                 kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(greenId + QStringLiteral("/slot.png")),
        QColor(Qt::green), QColor(Qt::magenta)));
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.slot.hidden"),
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"identity"},)json"
        R"json({"id":"volume","visible":false,)json"
        R"json("asset":"slot.strip.background"}]}]})json",
        kSlotBackgroundAssets));
    ASSERT_TRUE(writeNinePatch(
        QDir(root.path()).filePath(
            QStringLiteral("theme.slot.hidden/slot.png")),
        QColor(Qt::red), QColor(Qt::blue)));
    ASSERT_TRUE(writeRecipeTheme(
        root.path(), QStringLiteral("theme.slot.bare"),
        R"json({"recipes":[{"host":"track.header","variant":"track.compact",)json"
        R"json("slots":[{"id":"volume","preferred":40}]}]})json"));

    VNX::ThemeManager manager(root.path(), memoryStorage());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    auto* volume = new QLabel(QStringLiteral("Volume"));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume, volume));

    ASSERT_TRUE(manager.selectTheme(redId).ok());
    QWidget* container = layOutSingleSlot(host);
    const QImage red = renderSlotSurface(container);
    ASSERT_EQ(red.pixelColor(1, 1), QColor(Qt::red));
    ASSERT_EQ(red.pixelColor(20, 20), QColor(Qt::blue));

    ASSERT_TRUE(manager.selectTheme(greenId).ok());
    container = layOutSingleSlot(host);
    const QImage green = renderSlotSurface(container);
    EXPECT_EQ(green.pixelColor(1, 1), QColor(Qt::green));
    EXPECT_EQ(green.pixelColor(20, 20), QColor(Qt::magenta));

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.slot.hidden")).ok());
    EXPECT_TRUE(container->isHidden());
    EXPECT_TRUE(host.slotAsset(VNX::ComponentRecipeSlotId::Volume).isEmpty());

    ASSERT_TRUE(manager.selectTheme(QStringLiteral("theme.slot.bare")).ok());
    container = layOutSingleSlot(host);
    EXPECT_TRUE(host.slotAsset(VNX::ComponentRecipeSlotId::Volume).isEmpty());
    EXPECT_NE(renderSlotSurface(container), green);
}

TEST(ComponentRecipeHost, MissingThemeRecipeUsesBuiltinComposition)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString id = QStringLiteral("theme.no-recipe");
    const QString package = QDir(root.path()).filePath(id);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":2,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"No Recipe\",\"version\":\"1.0.0\","
          "\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\",\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},\"assetMap\":\"assets.json\"}}";
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("manifest.json")),
                          manifest));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("tokens.json")),
                          R"json({"colors":{"accent.primary":"#ffabcdef"}})json"));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("visuals/curve.json")),
                          R"json({"colors":{"curve.line":"#ff112233"}})json"));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("visuals/meter.json")),
                          R"json({"colors":{"meter.green":"#ff22aa44"}})json"));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("visuals/waveform.json")),
                          R"json({"colors":{"waveform.line":"#ff334455"}})json"));
    ASSERT_TRUE(writeFile(QDir(package).filePath(QStringLiteral("assets.json")),
                          R"json({"assets":{}})json"));
    VNX::ThemeManager manager(root.path(), memoryStorage());
    ASSERT_TRUE(manager.selectTheme(id).ok());
    VNX::ComponentRecipeHost host(
        VNX::ComponentRecipeHostId::TrackHeader, nullptr, &manager);
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Identity,
                                  new QLabel(QStringLiteral("Identity"))));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Volume,
                                  new QLabel(QStringLiteral("Volume"))));
    ASSERT_TRUE(host.registerSlot(VNX::ComponentRecipeSlotId::Params,
                                  new QLabel(QStringLiteral("Params"))));

    EXPECT_EQ(host.appliedVariant(),
              VNX::ComponentRecipeVariant::TrackExpanded);
    EXPECT_EQ(host.orderedSlotNames(),
              QStringList({QStringLiteral("identity"),
                           QStringLiteral("params")}));
}
