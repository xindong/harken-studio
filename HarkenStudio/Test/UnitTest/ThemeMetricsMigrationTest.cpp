#include "Core/MemorySettingsStorage.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/HarkenStyle.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterPresenter.h"
#include "Widgets/Common/RangeField.h"
#include "Widgets/Common/ListFooter.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/Inspector/BindingFieldDecorator.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QScopeGuard>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QStyleFactory>
#include <QStyleOption>
#include <QToolButton>

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
        + "\"schemaVersion\":1,\"id\":\"" + id.toUtf8()
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
            "navigation.chevronExtent": 24,
            "object.iconExtent": 28,
            "table.headerHeight": 43,
            "table.rowHeight": 49,
            "table.footerHeight": 47,
            "table.cellPaddingX": 11,
            "table.cellPaddingY": 5,
            "table.buttonWidth": 39,
            "table.buttonHeight": 31,
            "table.buttonSpacing": 7,
            "table.compactColumnWidth": 33,
            "table.minimumColumnWidth": 61,
            "table.resizeHandleRadius": 8,
            "inspector.pageMargin": 21,
            "inspector.sectionSpacing": 17,
            "inspector.rowSpacing": 19,
            "inspector.columnPadding": 23,
            "inspector.rowHeight": 41,
            "inspector.labelColumnShare": 450,
            "inspector.valueColumnShare": 550,
            "inspector.formLabelWidth": 137,
            "inspector.valueColumnWidth": 143,
            "inspector.linkSlotWidth": 33,
            "inspector.sourceSlotWidth": 29,
            "inspector.bindingSlotWidth": 37,
            "inspector.slotHeight": 35,
            "inspector.slotSpacing": 9,
            "inspector.separatorHeight": 5,
            "inspector.rangeSeparatorWidth": 27,
            "parameter.knobExtent": 111,
            "parameter.faderMinimumHeight": 173,
            "parameter.labelMinimumWidth": 151,
            "parameter.valueMinimumWidth": 129,
            "parameter.compactValueMinimumWidth": 121,
            "parameter.sliderGrooveThickness": 10,
            "parameter.sliderHandleThickness": 16,
            "parameter.sliderHandleLength": 30,
            "timeline.laneHeaderWidth": 321,
            "timeline.laneHeaderMinimumWidth": 171,
            "timeline.laneHeight": 91,
            "timeline.blankRowHeight": 51,
            "timeline.laneSpacing": 7,
            "timeline.rulerHeight": 37,
            "timeline.markerLaneHeight": 19,
            "timeline.tickLength": 9,
            "timeline.majorTickLength": 15,
            "timeline.minLabelSpacing": 61,
            "timeline.minTickSpacing": 11,
            "timeline.clipVerticalInset": 8,
            "timeline.clipRadius": 5,
            "timeline.clipMinimumWidth": 17,
            "timeline.resizeHandleWidth": 9,
            "timeline.markerHitRadius": 11,
            "timeline.statusBarHeight": 25,
            "timeline.compactHeaderWidth": 241,
            "timeline.queueLaneLabelWidth": 91,
            "timeline.headerGripWidth": 6
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

}

TEST(ThemeMetricsMigration, ThemePackageOverridesEveryMigratedMetric)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeDenseMetricsPackage(root.path(),
                                         QStringLiteral("dense")));

    auto manager = makeManager(root.path());
    manager->refreshThemes();
    const auto result = manager->selectTheme(QStringLiteral("dense"));
    ASSERT_TRUE(result.ok()) << result.diagnostics.size();

    const auto& inspector = manager->metrics().inspector;
    const auto& parameter = manager->metrics().parameter;
    const auto& timeline = manager->metrics().timeline;
    EXPECT_EQ(manager->metrics().navigationChevronExtent, 24);
    EXPECT_EQ(manager->metrics().objectIconExtent, 28);

    EXPECT_EQ(inspector.pageMargin, 21);
    EXPECT_EQ(inspector.sectionSpacing, 17);
    EXPECT_EQ(inspector.rowSpacing, 19);
    EXPECT_EQ(inspector.columnPadding, 23);
    EXPECT_EQ(inspector.rowHeight, 41);
    EXPECT_EQ(inspector.labelColumnShare, 450);
    EXPECT_EQ(inspector.valueColumnShare, 550);
    EXPECT_EQ(inspector.formLabelWidth, 137);
    EXPECT_EQ(inspector.valueColumnWidth, 143);
    EXPECT_EQ(inspector.linkSlotWidth, 33);
    EXPECT_EQ(inspector.sourceSlotWidth, 29);
    EXPECT_EQ(inspector.bindingSlotWidth, 37);
    EXPECT_EQ(inspector.slotHeight, 35);
    EXPECT_EQ(inspector.slotSpacing, 9);
    EXPECT_EQ(inspector.slotWidthFor(1), 42);
    EXPECT_EQ(inspector.slotWidthFor(2), 92);
    EXPECT_EQ(inspector.separatorHeight, 5);
    EXPECT_EQ(inspector.rangeSeparatorWidth, 27);

    EXPECT_EQ(parameter.knobExtent, 111);
    EXPECT_EQ(parameter.faderMinimumHeight, 173);
    EXPECT_EQ(parameter.labelMinimumWidth, 151);
    EXPECT_EQ(parameter.valueMinimumWidth, 129);
    EXPECT_EQ(parameter.compactValueMinimumWidth, 121);
    EXPECT_EQ(parameter.sliderGrooveThickness, 10);
    EXPECT_EQ(parameter.sliderHandleThickness, 16);
    EXPECT_EQ(parameter.sliderHandleLength, 30);

    EXPECT_EQ(timeline.laneHeaderWidth, 321);
    EXPECT_EQ(timeline.laneHeaderMinimumWidth, 171);
    EXPECT_EQ(timeline.laneHeight, 91);
    EXPECT_EQ(timeline.blankRowHeight, 51);
    EXPECT_EQ(timeline.laneSpacing, 7);
    EXPECT_EQ(timeline.rulerHeight, 37);
    EXPECT_EQ(timeline.markerLaneHeight, 19);
    EXPECT_EQ(timeline.tickLength, 9);
    EXPECT_EQ(timeline.majorTickLength, 15);
    EXPECT_EQ(timeline.minLabelSpacing, 61);
    EXPECT_EQ(timeline.minTickSpacing, 11);
    EXPECT_EQ(timeline.clipVerticalInset, 8);
    EXPECT_EQ(timeline.clipRadius, 5);
    EXPECT_EQ(timeline.clipMinimumWidth, 17);
    EXPECT_EQ(timeline.resizeHandleWidth, 9);
    EXPECT_EQ(timeline.markerHitRadius, 11);
    EXPECT_EQ(timeline.statusBarHeight, 25);
    EXPECT_EQ(timeline.compactHeaderWidth, 241);
    EXPECT_EQ(timeline.queueLaneLabelWidth, 91);
    EXPECT_EQ(timeline.headerGripWidth, 6);

    manager->selectTheme(VNX::builtinThemeId());
}

TEST(ThemeMetricsMigration, ChevronGeometryFollowsThemeSwitchAcrossQtControls)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeDenseMetricsPackage(root.path(), QStringLiteral("dense")));
    auto manager = makeManager(root.path());
    QComboBox combo;
    QToolButton expander;
    expander.setProperty("navigationRole", QStringLiteral("chevron"));
    expander.setArrowType(Qt::RightArrow);
    for (QWidget* widget : {static_cast<QWidget*>(&combo), static_cast<QWidget*>(&expander)}) {
        auto* style = new VNX::HarkenStyle(QStyleFactory::create(QStringLiteral("Fusion")),
                                         manager.get());
        style->setParent(widget);
        widget->setStyle(style);
    }
    const auto verify = [&](int extent) {
        combo.ensurePolished();
        expander.ensurePolished();
        combo.resize(180, extent + 2);
        expander.resize(expander.sizeHint());
        QStyleOptionComboBox option;
        option.initFrom(&combo);
        option.rect = combo.rect();
        const QRect arrow = combo.style()->subControlRect(
            QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, &combo);
        EXPECT_EQ(arrow.width(), extent);
        EXPECT_GE(arrow.height(), extent);
        EXPECT_EQ(expander.size(), QSize(extent, extent));
        EXPECT_EQ(expander.iconSize(), QSize(extent, extent));
        EXPECT_EQ(expander.style()->pixelMetric(QStyle::PM_HeaderMarkSize), extent);
        EXPECT_EQ(expander.style()->pixelMetric(QStyle::PM_TreeViewIndentation), extent);
    };
    verify(20);
    manager->refreshThemes();
    ASSERT_TRUE(manager->selectTheme(QStringLiteral("dense")).ok());
    verify(24);
    ASSERT_TRUE(manager->selectTheme(VNX::builtinThemeId()).ok());
    verify(20);
}

TEST(ThemeMetricsMigration, PresenterGeometryFollowsThemeSwitchAtRuntime)
{
    ensureApp();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(writeDenseMetricsPackage(root.path(),
                                         QStringLiteral("dense")));

    auto manager = makeManager(root.path());
    manager->refreshThemes();

    VNX::ParameterControlHost knobHost(
        QStringLiteral("Cutoff"),
        VNX::ParameterPresenterContext::EffectParameter, nullptr,
        manager.get());
    VNX::ParameterControlHost faderHost(
        QStringLiteral("Volume"),
        VNX::ParameterPresenterContext::InspectorFloat, nullptr,
        manager.get());
    faderHost.setPresenterOverride(VNX::ParameterPresenterId::VerticalFader);

    ASSERT_NE(knobHost.presenter(), nullptr);
    ASSERT_NE(faderHost.presenter(), nullptr);
    EXPECT_EQ(knobHost.presenter()->primaryEditor()->minimumWidth(), 64);
    EXPECT_EQ(faderHost.presenter()->primaryEditor()->minimumHeight(), 96);

    const auto result = manager->selectTheme(QStringLiteral("dense"));
    ASSERT_TRUE(result.ok());

    ASSERT_NE(knobHost.presenter(), nullptr);
    ASSERT_NE(faderHost.presenter(), nullptr);
    EXPECT_EQ(knobHost.presenter()->primaryEditor()->minimumWidth(), 111);
    EXPECT_EQ(faderHost.presenter()->primaryEditor()->minimumHeight(), 173);

    manager->selectTheme(VNX::builtinThemeId());
    ASSERT_NE(knobHost.presenter(), nullptr);
    EXPECT_EQ(knobHost.presenter()->primaryEditor()->minimumWidth(), 64);
}

TEST(ThemeMetricsMigration, MatrixHeadersResizeForThemeWithoutReplacingEditingState)
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
        EXPECT_EQ(matrix.headerWidget(0), header);
        EXPECT_EQ(view->currentRow(), 0);
        EXPECT_EQ(view->currentColumn(), 0);
        EXPECT_EQ(input->text(), "unfinished");
        EXPECT_EQ(input->cursorPosition(), 4);
    }
}

TEST(ThemeMetricsMigration, InspectorGeometryReadsTheSnapshotAtBuildTime)
{
    ensureApp();
    const auto& inspector =
        VNX::ThemeManager::instance().metrics().inspector;

    VNX::RangeField range;
    VNX::BindingFieldDecorator binding(1);

    auto* separator = range.findChild<QLabel*>(
        QStringLiteral("rangeFieldSeparator"));
    ASSERT_NE(separator, nullptr);
    EXPECT_EQ(separator->minimumWidth(), inspector.rangeSeparatorWidth);
    EXPECT_EQ(separator->maximumWidth(), inspector.rangeSeparatorWidth);
    EXPECT_EQ(binding.minimumWidth(), inspector.bindingSlotWidth);
    EXPECT_EQ(binding.maximumWidth(), inspector.bindingSlotWidth);
    EXPECT_EQ(binding.minimumHeight(), inspector.slotHeight);
    EXPECT_EQ(binding.maximumHeight(), inspector.slotHeight);
}

TEST(ThemeMetricsMigration, InspectorGeometryIgnoresThemeRepublish)
{
    ensureApp();
    constexpr int kSentinel = 4321;

    VNX::RangeField range;
    VNX::BindingFieldDecorator binding(1);

    auto* separator = range.findChild<QLabel*>(
        QStringLiteral("rangeFieldSeparator"));
    ASSERT_NE(separator, nullptr);
    ASSERT_NE(separator->minimumWidth(), kSentinel);
    ASSERT_NE(binding.minimumWidth(), kSentinel);

    separator->setFixedWidth(kSentinel);
    binding.setFixedWidth(kSentinel);

    Q_EMIT VNX::ThemeManager::instance().themeChanged(
        VNX::ThemeManager::instance().snapshot());

    EXPECT_EQ(separator->minimumWidth(), kSentinel);
    EXPECT_EQ(binding.minimumWidth(), kSentinel);
}

TEST(ThemeMetricsMigration, TableGeometryUpdatesInPlaceAcrossWidgetAndNativeBodies)
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
    auto* widgetGroup = table.createGroupHeader(QStringLiteral("Parameters"));
    auto* row = new QWidget(table.rowsWidget());
    row->setProperty("selected", true);
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
    auto* footer = table.footer();
    VNX::ProportionalTreeWidget native({500, 500}, nullptr, manager.get());
    native.setHeaderLabels({"Name", "Value"});
    auto* nativeGroup = native.createGroup(QStringLiteral("Parameters"));
    auto* item = new QTreeWidgetItem(&native, {"Parameter", "value"});
    native.setCurrentItem(item);
    VNX::TableWidget listTable(nullptr, manager.get());
    auto* list = listTable.createCustomView(QStringLiteral("ThemeList"),
        [&](QWidget* parent, VNX::ThemeManager* themeManager) {
            EXPECT_EQ(parent, &listTable);
            EXPECT_EQ(themeManager, manager.get());
            return new QListWidget(parent);
        });
    auto* listItem = new QListWidgetItem(QStringLiteral("Shelf item"), list);
    list->setCurrentItem(listItem);
    VNX::TableWidget modelTable(nullptr, manager.get());
    auto* modelView = modelTable.createCustomView(QStringLiteral("ThemeCells"),
        [](QWidget* parent, VNX::ThemeManager*) { return new QTableWidget(1, 2, parent); });
    modelView->setItem(0, 0, new QTableWidgetItem(QStringLiteral("Parameter")));
    modelTable.resize(600, 180);
    modelTable.show();
    table.resize(600, 180);
    table.show();
    native.resize(600, 180);
    native.show();
    listTable.resize(600, 180);
    listTable.show();
    QCoreApplication::processEvents();
    table.refreshLayout();
    const auto checkCell = [&] {
        const auto& metrics = manager->metrics().table;
        EXPECT_EQ(edit->x(), metrics.cellPaddingX);
        EXPECT_EQ(cell->width() - otherEdit->geometry().right() - 1, metrics.cellPaddingX);
        EXPECT_EQ(otherEdit->x() - edit->geometry().right() - 1, 2 * metrics.cellPaddingX);
        EXPECT_NEAR(edit->width(), otherEdit->width(), 1);
        EXPECT_GE(edit->y(), metrics.cellPaddingY);
        EXPECT_GE(cell->height() - edit->geometry().bottom() - 1, metrics.cellPaddingY);
        EXPECT_EQ(edit->text(), QStringLiteral("unfinished input"));
        EXPECT_EQ(edit->cursorPosition(), 5);
        EXPECT_EQ(changes, 0);
    };
    checkCell();
    EXPECT_EQ(widgetGroup->height(), native.visualItemRect(nativeGroup).height());
    EXPECT_EQ(table.headerCell(0)->font().pointSize(), name->font().pointSize());
    EXPECT_EQ(native.header()->font().pointSize(), name->font().pointSize());
    EXPECT_EQ(table.headerCell(0)->font().pointSize(), 10);
    const int oldHeight = row->height();
    const int oldGap = otherEdit->x() - edit->geometry().right() - 1;
    ASSERT_TRUE(manager->selectTheme(QStringLiteral("table-dense")).ok());
    QCoreApplication::processEvents();
    table.refreshLayout();
    const auto& metrics = manager->metrics().table;
    checkCell();
    EXPECT_EQ(widgetGroup->height(), native.visualItemRect(nativeGroup).height());
    EXPECT_GE(widgetGroup->height(), metrics.headerHeight);
    EXPECT_GT(otherEdit->x() - edit->geometry().right() - 1, oldGap);
    EXPECT_EQ(table.header()->height(), native.header()->height());
    EXPECT_EQ(table.headerCell(0)->font().pointSize(), name->font().pointSize());
    EXPECT_EQ(native.header()->font().pointSize(), name->font().pointSize());
    EXPECT_GE(table.header()->height(), metrics.headerHeight);
    EXPECT_GE(row->height(), metrics.rowHeight);
    EXPECT_GT(row->height(), oldHeight);
    EXPECT_GE(footer->height(), metrics.footerHeight);
    EXPECT_GE(footer->addButton()->width(), metrics.buttonWidth);
    EXPECT_GE(footer->addButton()->height(), metrics.buttonHeight);
    const int iconExtent = manager->metrics().objectIconExtent;
    EXPECT_EQ(footer->addButton()->iconSize(), QSize(iconExtent, iconExtent));
    EXPECT_EQ(footer->removeButton()->iconSize(), QSize(iconExtent, iconExtent));
    EXPECT_EQ(modelView->rowHeight(0), row->height());
    EXPECT_EQ(modelView->iconSize(), QSize(iconExtent, iconExtent));
    EXPECT_EQ(name->contentsMargins().left(), metrics.cellPaddingX);
    EXPECT_TRUE(row->property("selected").toBool());
    EXPECT_EQ(native.currentItem(), item);
    EXPECT_EQ(table.rowsWidget()->findChild<QLineEdit*>(), edit);
    EXPECT_EQ(native.visualItemRect(item).height(), row->height());
    EXPECT_EQ(list->visualItemRect(listItem).height(), row->height());
    EXPECT_EQ(list->currentItem(), listItem);
    ASSERT_TRUE(manager->selectTheme(VNX::builtinThemeId()).ok());
    QCoreApplication::processEvents();
    table.refreshLayout();
    checkCell();
    EXPECT_EQ(otherEdit->x() - edit->geometry().right() - 1, oldGap);
    EXPECT_EQ(row->height(), oldHeight);
    EXPECT_EQ(table.header()->height(), native.header()->height());
    EXPECT_EQ(table.rowsWidget()->findChild<QLineEdit*>(), edit);
}
