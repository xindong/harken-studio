#include "Core/MemorySettingsStorage.h"
#include "Core/HarkenStyle.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QIntValidator>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QImage>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QScopeGuard>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QStyleOptionMenuItem>
#include <QStylePainter>
#include <QToolBar>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QTimer>

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

bool writeDenseMetricsPackage(const QString& themesRoot, const QString& id)
{
    const QString root = QDir(themesRoot).filePath(id);
    const QByteArray manifest = QByteArray("{")
        + "\"schemaVersion\":7,\"id\":\"" + id.toUtf8()
        + "\",\"displayName\":\"" + id.toUtf8()
        + "\",\"version\":\"1.0.0\",\"baseThemeId\":\""
        + VNX::builtinThemeId().toUtf8() + "\""
        + ",\"compatibility\":{\"harkenVersion\":\">=0.1.0 <1.0.0\","
          "\"capabilities\":[\"curve-v1\",\"meter-v1\",\"waveform-v1\"]},"
          "\"entries\":{\"tokens\":\"tokens.json\",\"visuals\":{"
          "\"curve\":\"visuals/curve.json\","
          "\"meter\":\"visuals/meter.json\","
          "\"waveform\":\"visuals/waveform.json\"},"
          "\"assetMap\":\"assets.json\"}}";

    const QByteArray tokens = R"json({
        "metrics": {
            "table.headerHeight": 43,
            "table.rowHeight": 49,
            "table.cellPaddingX": 11,
            "table.cellPaddingY": 5
        }
    })json";

    return writeFile(QDir(root).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(root).filePath(QStringLiteral("tokens.json")), tokens)
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/curve.json")),
                     R"json({"metrics":{"curve.lineWidth":2}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.green":"#ff22aa44"}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.line":"#ff334455"}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json");
}

std::unique_ptr<VNX::ThemeManager> makeManager(const QString& root)
{
    return std::make_unique<VNX::ThemeManager>(
        root, std::make_unique<VNX::MemorySettingsStorage>());
}

class StatefulMenuBar final : public QMenuBar
{
public:
    QStyle::State itemState;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOption panel;
        panel.initFrom(this);
        painter.drawControl(QStyle::CE_MenuBarEmptyArea, panel);
        for (auto* action : actions()) {
            QStyleOptionMenuItem item;
            initStyleOption(&item, action);
            item.rect = actionGeometry(action);
            item.state |= itemState;
            painter.drawControl(QStyle::CE_MenuBarItem, item);
        }
    }
};

QImage renderWidget(QWidget& widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

}

TEST(ThemeControlValidation, UnconstrainedEditorsPreserveDomainValidation)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QLineEdit editor;
    QObject::connect(&editor, &QLineEdit::textChanged, &editor, [&] {
        VNX::QtControlSkin::setSemanticProperty(&editor, "inputInvalid", editor.text().isEmpty());
    });
    editor.setText(QStringLiteral("valid"));
    editor.setStyle(&style);
    editor.ensurePolished();
    editor.clear();
    EXPECT_TRUE(editor.property("inputInvalid").toBool());

    QFocusEvent focusIn(QEvent::FocusIn);
    QCoreApplication::sendEvent(&editor, &focusIn);
    EXPECT_TRUE(editor.property("inputInvalid").toBool());
    style.unpolish(&editor);
    style.polish(&editor);
    EXPECT_TRUE(editor.property("inputInvalid").toBool());

    editor.setText(QStringLiteral("valid again"));
    EXPECT_FALSE(editor.property("inputInvalid").toBool());
}

TEST(ThemeControlValidation, ValidatorsAndMasksInstalledAfterPolishKeepAutomaticValidation)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QLineEdit validated;
    validated.setStyle(&style);
    validated.ensurePolished();
    validated.setValidator(new QIntValidator(10, 99, &validated));
    validated.setText(QStringLiteral("1"));
    EXPECT_TRUE(validated.property("inputInvalid").toBool());
    validated.setText(QStringLiteral("42"));
    EXPECT_FALSE(validated.property("inputInvalid").toBool());

    QLineEdit masked;
    masked.setStyle(&style);
    masked.ensurePolished();
    masked.setInputMask(QStringLiteral("99;_"));
    masked.setText(QStringLiteral("1"));
    EXPECT_TRUE(masked.property("inputInvalid").toBool());
    masked.setText(QStringLiteral("42"));
    EXPECT_FALSE(masked.property("inputInvalid").toBool());
}

TEST(ThemeMetricsMigration, ColorPreviewPreservesMetricsAndSynchronizesNativePalette)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QWidget panel;
    panel.setStyle(&style);
    auto* layout = new QVBoxLayout(&panel);
    auto* label = new QLabel(QStringLiteral("Parameter"), &panel);
    label->setProperty("textRole", QStringLiteral("primary"));
    label->setStyle(&style);
    auto* header = new QWidget(&panel);
    header->setObjectName(QStringLiteral("listHead"));
    header->setStyle(&style);
    auto* headerLayout = new QVBoxLayout(header);
    auto* headerLabel = new QLabel(QStringLiteral("Column"), header);
    headerLabel->setProperty("textRole", QStringLiteral("primary"));
    headerLabel->setStyle(&style);
    headerLayout->addWidget(headerLabel);
    auto* editor = new QLineEdit(QStringLiteral("unfinished input"), &panel);
    editor->setStyle(&style);
    auto* localLabel = new QLabel(QStringLiteral("Local appearance"), &panel);
    localLabel->setStyle(&style);
    localLabel->setProperty("textRole", QStringLiteral("primary"));
    localLabel->setStyleSheet(QStringLiteral("color: #ccee77; padding-left: 3px;"));
    auto* fixedCombo = new QComboBox(&panel);
    fixedCombo->setStyle(&style);
    fixedCombo->addItems({QStringLiteral("First"), QStringLiteral("Second")});
    fixedCombo->setCurrentIndex(1);
    fixedCombo->ensurePolished();
    fixedCombo->setFixedSize(173, 37);
    layout->addWidget(label);
    layout->addWidget(header);
    layout->addWidget(editor);
    layout->addWidget(localLabel);
    layout->addWidget(fixedCombo);
    panel.resize(400, 200);
    panel.show();
    QCoreApplication::processEvents();
    editor->setSelection(2, 6);
    const auto original = manager->snapshot();
    const auto originalPalette = qApp->palette();
    const auto geometry = editor->geometry();
    const auto font = editor->font();
    const auto comboGeometry = fixedCombo->geometry();
    const auto comboMinimum = fixedCombo->minimumSize();
    const auto comboMaximum = fixedCombo->maximumSize();
    const QColor primary(QStringLiteral("#ffc8a0e0"));
    const QColor secondary(QStringLiteral("#ffb0c9df"));
    const QColor title(QStringLiteral("#ff73b493"));
    const QColor mainBackground(QStringLiteral("#ff263040"));
    const QColor controlBackground(QStringLiteral("#ff354152"));
    const QColor raisedBackground(QStringLiteral("#ff465668"));
    auto preview = manager->appearance();
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("text.primary"), primary);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("text.secondary"), secondary);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("text.title"), title);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("background.primary"), mainBackground);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("background.secondary"), controlBackground);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("background.tertiary"), raisedBackground);
    ASSERT_TRUE(manager->previewAppearance(preview).ok());
    QCoreApplication::processEvents();
    EXPECT_EQ(label->palette().color(QPalette::WindowText), primary);
    EXPECT_EQ(headerLabel->palette().color(QPalette::WindowText), secondary);
    EXPECT_EQ(localLabel->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ccee77")));
    EXPECT_EQ(qApp->palette().color(QPalette::Window), mainBackground);
    EXPECT_EQ(qApp->palette().color(QPalette::Base), controlBackground);
    EXPECT_EQ(qApp->palette().color(QPalette::ToolTipBase), controlBackground);
    EXPECT_EQ(qApp->palette().color(QPalette::AlternateBase), raisedBackground);
    EXPECT_EQ(qApp->palette().color(QPalette::Button), raisedBackground);
    EXPECT_EQ(qApp->palette().color(QPalette::Text), primary);
    EXPECT_EQ(qApp->palette().color(QPalette::PlaceholderText), secondary);
    EXPECT_EQ(editor->palette().color(QPalette::PlaceholderText), secondary);
    EXPECT_EQ(qApp->palette().color(QPalette::ToolTipText), primary);
    EXPECT_EQ(qApp->palette().color(QPalette::Mid), secondary);
    EXPECT_EQ(qApp->palette().color(QPalette::Dark), secondary);
    EXPECT_EQ(editor->geometry(), geometry);
    EXPECT_EQ(editor->font(), font);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("finish"));
    EXPECT_EQ(fixedCombo->geometry(), comboGeometry);
    EXPECT_EQ(fixedCombo->minimumSize(), comboMinimum);
    EXPECT_EQ(fixedCombo->maximumSize(), comboMaximum);
    EXPECT_EQ(fixedCombo->currentText(), QStringLiteral("Second"));
    VNX::QtControlSkin::setSemanticProperty(label, "textRole", QStringLiteral("title"));
    EXPECT_EQ(label->palette().color(QPalette::WindowText), title);
    manager->cancelAppearancePreview();
    EXPECT_EQ(qApp->palette(), originalPalette);
    EXPECT_EQ(label->palette().color(QPalette::WindowText), original->ui.colors.textTitle);
    EXPECT_EQ(headerLabel->palette().color(QPalette::WindowText), original->ui.colors.textSecondary);
    EXPECT_EQ(editor->palette().color(QPalette::PlaceholderText), original->qtPalette.placeholderText);
    EXPECT_EQ(localLabel->palette().color(QPalette::WindowText), QColor(QStringLiteral("#ccee77")));
    EXPECT_EQ(editor->text(), QStringLiteral("unfinished input"));
    EXPECT_EQ(editor->selectedText(), QStringLiteral("finish"));
    EXPECT_EQ(fixedCombo->geometry(), comboGeometry);
    EXPECT_EQ(fixedCombo->minimumSize(), comboMinimum);
    EXPECT_EQ(fixedCombo->maximumSize(), comboMaximum);
    EXPECT_EQ(fixedCombo->currentText(), QStringLiteral("Second"));
}

TEST(ThemeMetricsMigration, BorderPreviewRepaintsOpaqueDescendantsWithoutRepolish)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QWidget window;
    window.setStyle(&style);
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new QFrame(&window);
    panel->setProperty("surfaceRole", QStringLiteral("panel"));
    panel->setStyle(&style);
    layout->addWidget(panel);
    auto* fields = new QVBoxLayout(panel);
    auto* editor = new QLineEdit(QStringLiteral("unfinished input"), panel);
    editor->setStyle(&style);
    fields->addWidget(editor);
    panel->setAttribute(Qt::WA_OpaquePaintEvent);
    editor->setAttribute(Qt::WA_OpaquePaintEvent);
    class AppearanceEvents final : public QObject {
    public:
        QHash<QObject*, int> paints;
        int styleChanges = 0;
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Paint) ++paints[watched];
            if (event->type() == QEvent::StyleChange) ++styleChanges;
            return false;
        }
    } events;
    panel->installEventFilter(&events);
    editor->installEventFilter(&events);
    const auto settle = [] {
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();
    };
    window.resize(400, 200);
    window.show();
    editor->setFocus();
    editor->setSelection(2, 6);
    settle();
    ASSERT_GT(events.paints.value(panel), 0);
    ASSERT_GT(events.paints.value(editor), 0);
    const auto geometry = editor->geometry();
    const auto palette = editor->palette();
    const auto nativeStyle = qApp->styleSheet();
    const auto committed = manager->appearance();
    const auto check = [&] {
        settle();
        EXPECT_GT(events.paints.value(panel), 0);
        EXPECT_GT(events.paints.value(editor), 0);
        EXPECT_EQ(events.styleChanges, 0);
        EXPECT_EQ(qApp->styleSheet(), nativeStyle);
        EXPECT_EQ(editor->palette(), palette);
        EXPECT_EQ(editor->geometry(), geometry);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("finish"));
        EXPECT_EQ(editor->text(), QStringLiteral("unfinished input"));
        EXPECT_EQ(manager->appearance(), committed);
    };
    for (const QString role : {QStringLiteral("border.primary"), QStringLiteral("border.secondary"),
                               QStringLiteral("timeline.clipBorder")}) {
        SCOPED_TRACE(role.toStdString());
        for (const QColor color : {QColor("#e74f31"), QColor("#358bd4")}) {
            events.paints.clear();
            events.styleChanges = 0;
            auto preview = committed;
            preview.colors[preview.themeId][preview.paletteId].insert(role, color);
            ASSERT_TRUE(manager->previewAppearance(preview).ok());
            check();
        }
    }
    events.paints.clear();
    events.styleChanges = 0;
    manager->cancelAppearancePreview();
    check();
}

TEST(ThemeMetricsMigration, PolishedDescendantsFollowAncestorChangesAcrossMoveDestroyAndShow)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    manager->widgetAppearance().updateStyleSheet(QStringLiteral(R"qss(
        QLabel { color: #111111; }
        #surface QLabel { color: #224466; }
        #surface[active="true"] QLabel { color: #446688; }
        #renamed QLabel { color: #6688aa; }
        #destination[active="false"] QLabel { color: #aaccee; }
        #destination[active="true"] QLabel { color: #88aacc; }
        #destination[windowTitle="Native scope"] QLabel { color: #cc8844; }
    )qss"));
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    auto ancestor = std::make_unique<QWidget>();
    ancestor->setObjectName(QStringLiteral("surface"));
    ancestor->setProperty("active", true);
    ancestor->setStyle(&style);
    ancestor->ensurePolished();
    QWidget destination;
    destination.setObjectName(QStringLiteral("destination"));
    destination.setProperty("active", false);
    destination.setStyle(&style);
    destination.ensurePolished();
    QLabel child(QStringLiteral("Surviving descendant"), ancestor.get());
    child.setStyle(&style);
    child.ensurePolished();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#446688")));

    ancestor->setProperty("active", false);
    QCoreApplication::processEvents();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#224466")));
    ancestor->setObjectName(QStringLiteral("renamed"));
    QCoreApplication::processEvents();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#6688aa")));

    ancestor->setObjectName(QStringLiteral("surface"));
    child.setParent(&destination);
    ancestor.reset();
    QCoreApplication::processEvents();
    EXPECT_EQ(child.parentWidget(), &destination);
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#aaccee")));

    child.hide();
    destination.show();
    QCoreApplication::processEvents();
    ASSERT_FALSE(child.isVisible());
    destination.setProperty("active", true);
    child.show();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#88aacc")));
    QCoreApplication::processEvents();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#88aacc")));

    destination.setWindowTitle(QStringLiteral("Native scope"));
    QEvent styleChanged(QEvent::StyleChange);
    QCoreApplication::sendEvent(&destination, &styleChanged);
    QCoreApplication::processEvents();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#cc8844")));
    destination.setWindowTitle(QString());
    QCoreApplication::sendEvent(&destination, &styleChanged);
    QCoreApplication::processEvents();
    EXPECT_EQ(child.palette().color(QPalette::WindowText), QColor(QStringLiteral("#88aacc")));
}

TEST(ThemeMetricsMigration, CheckedIndicatorsRemainDistinguishableAcrossPalettesAndEnabledStates)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QCheckBox check(QStringLiteral("Enabled setting"));
    check.setStyle(&style);
    check.resize(180, 28);
    check.show();
    for (const auto& palette : manager->availablePalettes())
    {
        SCOPED_TRACE(palette.id.toStdString());
        auto appearance = manager->appearance();
        appearance.paletteId = palette.id;
        ASSERT_TRUE(manager->applyAppearance(appearance).ok());
        for (const bool enabled : {true, false})
        {
            SCOPED_TRACE(enabled);
            check.setEnabled(enabled);
            check.setChecked(false);
            QCoreApplication::processEvents();
            const auto unchecked = renderWidget(check);
            QStyleOptionButton option;
            option.initFrom(&check);
            option.text = check.text();
            const QRect indicator = check.style()->subElementRect(
                QStyle::SE_CheckBoxIndicator, &option, &check).intersected(unchecked.rect());
            ASSERT_FALSE(indicator.isEmpty());
            check.setChecked(true);
            QCoreApplication::processEvents();
            const auto checked = renderWidget(check);
            int visiblePixels = 0;
            for (int y = indicator.top(); y <= indicator.bottom(); ++y)
            {
                for (int x = indicator.left(); x <= indicator.right(); ++x)
                {
                    const QColor before = unchecked.pixelColor(x, y);
                    const QColor after = checked.pixelColor(x, y);
                    const int difference = qMax(qAbs(after.red() - before.red()),
                        qMax(qAbs(after.green() - before.green()), qAbs(after.blue() - before.blue())));
                    if (difference >= 24) ++visiblePixels;
                }
            }
            EXPECT_GE(visiblePixels, 8);
        }
    }
}

TEST(ThemeMetricsMigration, MenuStateColorsCoverPaddingWithoutChangingGeometry)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    StatefulMenuBar bar;
    bar.setObjectName(QStringLiteral("titleBarMenuBar"));
    bar.setStyle(&style);
    bar.setStyleSheet(QStringLiteral(
        "#titleBarMenuBar::item { padding-top: 11px; padding-bottom: 11px; }"));
    auto* action = bar.addAction(QStringLiteral("Menu action"));
    bar.resize(400, bar.sizeHint().height());
    bar.show();
    QCoreApplication::processEvents();
    const QRect actionRect = bar.actionGeometry(action);
    const QSize sizeHint = bar.sizeHint();
    ASSERT_FALSE(actionRect.isEmpty());
    const auto checkStates = [&] {
        for (const bool pressed : {false, true}) {
            bar.itemState = QStyle::State_Selected;
            if (pressed) bar.itemState |= QStyle::State_Sunken;
            const auto image = renderWidget(bar);
            const auto expected = pressed ? manager->colors().accent : manager->colors().bgHover;
            EXPECT_EQ(image.pixelColor(actionRect.left() + 1, actionRect.center().y()), expected);
            EXPECT_EQ(image.pixelColor(actionRect.center().x(), actionRect.top() + 1), expected);
            EXPECT_EQ(image.pixelColor(actionRect.center().x(), actionRect.bottom() - 1), expected);
        }
    };
    checkStates();
    auto preview = manager->appearance();
    const QColor hover(QStringLiteral("#ff6d3a91"));
    const QColor accent(QStringLiteral("#ff217d9b"));
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("background.hover"), hover);
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("accent.primary"), accent);
    ASSERT_TRUE(manager->previewAppearance(preview).ok());
    QCoreApplication::processEvents();
    EXPECT_EQ(manager->colors().bgHover, hover);
    EXPECT_EQ(manager->colors().accent, accent);
    EXPECT_EQ(bar.actionGeometry(action), actionRect);
    EXPECT_EQ(bar.sizeHint(), sizeHint);
    checkStates();
    manager->cancelAppearancePreview();
    EXPECT_EQ(bar.actionGeometry(action), actionRect);
    EXPECT_EQ(bar.sizeHint(), sizeHint);
    checkStates();
}

TEST(ThemeMetricsMigration, ButtonTextAlignmentMatchesNativeStyleWrapping)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    std::unique_ptr<QStyle> nativeStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPushButton reference(QStringLiteral("Action label"));
    reference.setStyle(nativeStyle.get());
    reference.setObjectName(QStringLiteral("alignmentButton"));
    reference.resize(280, 36);
    QPushButton actual(reference.text());
    actual.setStyle(&style);
    actual.setObjectName(reference.objectName());
    actual.resize(reference.size());
    const auto textBounds = [](const QImage& image) {
        QRect result;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixelColor(x, y);
                if (pixel.red() > 180 && pixel.green() > 180 && pixel.blue() > 180)
                    result = result.united(QRect(x, y, 1, 1));
            }
        return result;
    };
    for (const auto& alignment : {QStringLiteral("left"), QStringLiteral("center"), QStringLiteral("right")}) {
        SCOPED_TRACE(alignment.toStdString());
        const QString source = QStringLiteral(
            "#alignmentButton { color: #ffffff; background: #112233; border: 0px;"
            "padding: 4px 8px; text-align: %1; }").arg(alignment);
        reference.setStyleSheet(source);
        actual.setStyleSheet(manager->widgetAppearance().updateWidgetStyle(&actual, source));
        reference.ensurePolished();
        actual.ensurePolished();
        const auto expected = textBounds(renderWidget(reference));
        ASSERT_FALSE(expected.isEmpty());
        EXPECT_EQ(textBounds(renderWidget(actual)), expected);
    }
}

TEST(ThemeMetricsMigration, ToolbarSurfaceAndBorderSurviveNativeStyleWrapping)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    for (const bool floating : {false, true}) {
        SCOPED_TRACE(floating ? "floating" : "docked");
        QMainWindow window;
        window.setStyle(&style);
        QToolBar toolbar;
        toolbar.setStyle(&style);
        toolbar.addAction(QStringLiteral("Action"));
        if (floating) {
            toolbar.resize(400, toolbar.sizeHint().height());
            toolbar.show();
        } else {
            window.addToolBar(&toolbar);
            window.resize(400, 120);
            window.show();
        }
        QCoreApplication::processEvents();
        const auto image = renderWidget(toolbar);
        const int x = toolbar.width() - toolbar.contentsMargins().right() - 1;
        EXPECT_EQ(image.pixelColor(x, toolbar.height() / 2), manager->snapshot()->qtPalette.window);
        EXPECT_EQ(image.pixelColor(x, 0), manager->colors().sectionSeparator);
    }
}

TEST(ThemeMetricsMigration, OwnedPanelSurfaceSurvivesVisibilityAndColorPreview)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QWidget window;
    window.setStyle(&style);
    auto* layout = new QVBoxLayout(&window);
    ControlPanel panel(&window);
    panel.setStyle(&style);
    layout->addWidget(&panel);
    window.resize(1000, 220);
    window.show();
    QCoreApplication::processEvents();
    const QRect geometry = panel.geometry();
    const auto check = [&](const char* stage) {
        SCOPED_TRACE(stage);
        ASSERT_TRUE(panel.isVisible());
        const auto image = renderWidget(window);
        const auto origin = panel.mapTo(&window, QPoint());
        EXPECT_EQ(image.pixelColor(origin + QPoint(1, panel.height() / 2)),
                  manager->colors().auditionBg);
        EXPECT_EQ(image.pixelColor(origin + QPoint(panel.width() / 2, 0)),
                  manager->colors().borderPrimary);
        EXPECT_EQ(panel.geometry(), geometry);
    };
    check("initial show");
    panel.hide();
    auto preview = manager->appearance();
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("border.primary"),
                                             QColor(QStringLiteral("#945bd1")));
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("background.primary"),
                                             QColor(QStringLiteral("#1a2836")));
    ASSERT_TRUE(manager->previewAppearance(preview).ok());
    panel.show();
    QCoreApplication::processEvents();
    check("preview after show");
    manager->cancelAppearancePreview();
    QCoreApplication::processEvents();
    check("cancel preview");
}

TEST(ThemeMetricsMigration, ProgressContentsPreserveOuterBorderAcrossDirectionsAndPreview)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QProgressBar progress;
    progress.setProperty("shellRole", QStringLiteral("statusProgress"));
    progress.setStyle(&style);
    progress.setRange(0, 100);
    progress.setTextVisible(false);
    progress.resize(320, 30);
    progress.show();
    QCoreApplication::processEvents();
    const QRect geometry = progress.geometry();
    const auto checkBoundary = [&] {
        for (const auto direction : {Qt::LeftToRight, Qt::RightToLeft}) {
            progress.setLayoutDirection(direction);
            for (const bool inverted : {false, true}) {
                progress.setInvertedAppearance(inverted);
                for (const int value : {0, 37, 100}) {
                    SCOPED_TRACE(::testing::Message() << "direction=" << direction
                        << " inverted=" << inverted << " value=" << value);
                    progress.setValue(value);
                    const auto image = renderWidget(progress);
                    const QPoint center = image.rect().center();
                    const auto border = manager->widgetAppearance().visual(&progress,
                        QStyle::State_Enabled).borderColors[0];
                    EXPECT_EQ(image.pixelColor(center.x(), 0), border);
                    EXPECT_EQ(image.pixelColor(center.x(), image.height() - 1), border);
                    EXPECT_EQ(image.pixelColor(0, center.y()), border);
                    EXPECT_EQ(image.pixelColor(image.width() - 1, center.y()), border);
                    if (value == 0)
                        EXPECT_EQ(image.pixelColor(center), manager->colors().bgPrimary);
                    if (value == 100)
                        EXPECT_EQ(image.pixelColor(center), manager->colors().accent);
                    EXPECT_EQ(progress.geometry(), geometry);
                }
            }
        }
    };
    checkBoundary();
    auto preview = manager->appearance();
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("border.secondary"), QColor(QStringLiteral("#ffb68442")));
    preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("accent.primary"), QColor(QStringLiteral("#ff327f95")));
    ASSERT_TRUE(manager->previewAppearance(preview).ok());
    QCoreApplication::processEvents();
    checkBoundary();
    manager->cancelAppearancePreview();
    QCoreApplication::processEvents();
    checkBoundary();
}

TEST(ThemeMetricsMigration, MatrixThemeChangesPreserveEditingState)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(writeDenseMetricsPackage(root.path(), QStringLiteral("matrix-dense")));
    auto manager = makeManager(root.path());
    const QString style = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(style); });
    VNX::MatrixTableWidget matrix(nullptr, manager.get());
    auto* view = matrix.table();
    view->setRowCount(2);
    view->setColumnCount(2);
    auto* header = new QWidget;
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* axis = new QLineEdit("axis", header);
    auto* input = new QLineEdit("unfinished", header);
    layout->addWidget(axis);
    layout->addWidget(input);
    matrix.setHeaderWidget(0, header);
    auto* row = new QLineEdit("row");
    view->setCellWidget(0, 1, row);
    view->setCurrentCell(0, 0);
    matrix.resize(640, 300);
    matrix.show();
    input->setCursorPosition(4);
    for (const auto& theme : {QStringLiteral("matrix-dense"), VNX::builtinThemeId()}) {
        ASSERT_TRUE(manager->selectTheme(theme).ok());
        QCoreApplication::processEvents();
        matrix.refreshLayout();
        EXPECT_EQ(view->currentRow(), 0);
        EXPECT_EQ(view->currentColumn(), 0);
        EXPECT_EQ(input->text(), "unfinished");
        EXPECT_EQ(input->cursorPosition(), 4);
    }
}

TEST(ThemeMetricsMigration, TableThemeChangesPreserveEditingStateAcrossBodies)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeDenseMetricsPackage(directory.path(), QStringLiteral("table-dense")));
    auto manager = makeManager(directory.path());
    const auto restore = qScopeGuard([&] { manager->selectTheme(VNX::builtinThemeId()); });
    VNX::TableWidget table(nullptr, manager.get());
    table.setColumns(QStringLiteral("ThemeTable"), {
        {QStringLiteral("Name"), QStringLiteral("Name"), 500},
        {QStringLiteral("Value"), QStringLiteral("Value"), 500}
    });
    auto* row = new QWidget(table.rowsWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* name = new QLabel(QStringLiteral("Parameter"), row);
    auto* cell = table.createCell(row);
    auto* edit = new QLineEdit(QStringLiteral("unfinished input"), cell);
    auto* otherEdit = new QLineEdit(QStringLiteral("second input"), cell);
    edit->setCursorPosition(5);
    int changes = 0;
    QObject::connect(edit, &QLineEdit::textChanged, &table, [&] { ++changes; });
    layout->addWidget(name);
    cell->addWidget(edit);
    cell->addWidget(otherEdit);
    layout->addWidget(cell);
    table.registerRow(row, {name, cell});
    table.rowsLayout()->addWidget(row);
    VNX::ProportionalTreeWidget native({500, 500}, nullptr, manager.get());
    native.setHeaderLabels({"Name", "Value"});
    auto* item = new QTreeWidgetItem(&native, {"Parameter", "value"});
    native.setCurrentItem(item);
    VNX::TableWidget listTable(nullptr, manager.get());
    auto* list = listTable.createCustomView(QStringLiteral("ThemeList"),
        [](QWidget* parent, VNX::ThemeManager*) { return new QListWidget(parent); });
    auto* listItem = new QListWidgetItem(QStringLiteral("Shelf item"), list);
    list->setCurrentItem(listItem);
    table.resize(600, 180);
    table.show();
    native.resize(600, 180);
    native.show();
    listTable.resize(600, 180);
    listTable.show();
    for (const auto& theme : {VNX::builtinThemeId(), QStringLiteral("table-dense"),
                              VNX::builtinThemeId()}) {
        ASSERT_TRUE(manager->selectTheme(theme).ok());
        QCoreApplication::processEvents();
        table.refreshLayout();
        EXPECT_EQ(edit->text(), QStringLiteral("unfinished input"));
        EXPECT_EQ(edit->cursorPosition(), 5);
        EXPECT_EQ(changes, 0);
        EXPECT_EQ(native.currentItem(), item);
        EXPECT_EQ(list->currentItem(), listItem);
    }
}
