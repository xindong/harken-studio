#include "Core/MemorySettingsStorage.h"
#include "Core/HarkenStyle.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/MatrixTableWidget.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFocusEvent>
#include <QLabel>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QIntValidator>
#include <QListWidget>
#include <QTableWidget>
#include <QScopeGuard>
#include <QStyleFactory>
#include <QVBoxLayout>
#include <QTemporaryDir>

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
