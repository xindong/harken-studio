#include "Core/MemorySettingsStorage.h"
#include "Core/HarkenStyle.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Core/ThemeLayout.h"
#include "Core/FontManager.h"
#include "Dialogs/StartupLoadingWindow.h"
#include "Widgets/Assistant/AssistantPanel.h"
#include "Widgets/Common/LinearSliderPresenter.h"
#include "Widgets/Common/VerticalFaderPresenter.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/TimeAxis.h"
#include "Widgets/Timeline/TimelineRenderer.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/MatrixTableWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Widgets/Shared/TreeExpansionWidget.h"

#include <gtest/gtest.h>

#include <QAccessible>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
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
#include <QPointer>
#include <QScreen>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextCursor>
#include <QSlider>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QStyleOptionMenuItem>
#include <QStylePainter>
#include <QToolBar>
#include <QVBoxLayout>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

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

bool writeMetricsPackage(const QString& themesRoot, const QString& id, const QByteArray& tokens)
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

    return writeFile(QDir(root).filePath(QStringLiteral("manifest.json")),
                     manifest)
        && writeFile(QDir(root).filePath(QStringLiteral("tokens.json")), tokens)
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/curve.json")),
                     R"json({"metrics":{"curve.lineWidth":2}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/meter.json")),
                     R"json({"colors":{"meter.spectrum.fillTop":"#ff22aa44"}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("visuals/waveform.json")),
                     R"json({"colors":{"waveform.rawPeaks.centerLine":"#ff334455"}})json")
        && writeFile(QDir(root).filePath(QStringLiteral("assets.json")),
                     R"json({"assets":{}})json");
}

bool writeDenseMetricsPackage(const QString& themesRoot, const QString& id)
{
    return writeMetricsPackage(themesRoot, id, R"json({
        "metrics": {
            "table.headerHeight": 43,
            "table.rowHeight": 49,
            "table.cellPaddingX": 11,
            "table.cellPaddingY": 5
        }
    })json");
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

TEST(ThemeControlValidation, CollapsedTreesPreserveAccessibleIdentityAcrossPolishCycles)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    static QTreeView* observedTree = nullptr;
    static int resetCount = 0;
    const auto previousHandler = QAccessible::installUpdateHandler([](QAccessibleEvent* event) {
        if (event->type() != QAccessible::TableModelChanged) return;
        auto* change = static_cast<QAccessibleTableModelChangeEvent*>(event);
        if (event->object() == observedTree
            && change->modelChangeType() == QAccessibleTableModelChangeEvent::ModelReset)
            ++resetCount;
        if (!QAccessible::isActive()) {
            auto* interface = event->accessibleInterface();
            if (auto* table = interface ? interface->tableInterface() : nullptr)
                table->modelChange(change);
        }
    });
    const auto restoreHandler = qScopeGuard([previousHandler] {
        observedTree = nullptr;
        QAccessible::installUpdateHandler(previousHandler);
    });
    for (int kind = 0; kind < 4; ++kind) {
        SCOPED_TRACE(kind);
        QStandardItemModel model;
        std::unique_ptr<QTreeView> tree;
        switch (kind) {
        case 0: tree = std::make_unique<QTreeWidget>(); break;
        case 1: tree = std::make_unique<VNX::ProportionalTreeWidget>(QList<int>{1, 1}, nullptr, manager.get()); break;
        case 2: tree = std::make_unique<VNX::TreeExpansionWidget>(); break;
        case 3: tree = std::make_unique<QTreeView>(); break;
        }
        if (auto* items = qobject_cast<QTreeWidget*>(tree.get())) {
            items->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
            auto* group = new QTreeWidgetItem(items, {QStringLiteral("Group"), QStringLiteral("group")});
            new QTreeWidgetItem(group, {QStringLiteral("Child"), QStringLiteral("child")});
            new QTreeWidgetItem(items, {QStringLiteral("Following"), QStringLiteral("following")});
            new QTreeWidgetItem(items, {QStringLiteral("Last"), QStringLiteral("last")});
        } else {
            model.setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
            auto* group = new QStandardItem(QStringLiteral("Group"));
            group->appendRow({new QStandardItem(QStringLiteral("Child")), new QStandardItem(QStringLiteral("child"))});
            model.appendRow({group, new QStandardItem(QStringLiteral("group"))});
            model.appendRow({new QStandardItem(QStringLiteral("Following")), new QStandardItem(QStringLiteral("following"))});
            model.appendRow({new QStandardItem(QStringLiteral("Last")), new QStandardItem(QStringLiteral("last"))});
            tree->setModel(&model);
        }
        tree->setStyle(&style);
        tree->resize(400, 300);
        tree->show();
        QCoreApplication::processEvents();
        observedTree = tree.get();
        auto* interface = QAccessible::queryAccessibleInterface(tree.get());
        ASSERT_NE(interface, nullptr);
        ASSERT_NE(interface->tableInterface(), nullptr);
        const auto verifyChildren = [&] {
            QSet<QAccessible::Id> identities;
            for (int index = 0; index < interface->childCount(); ++index) {
                auto* child = interface->child(index);
                ASSERT_NE(child, nullptr);
                ASSERT_TRUE(child->isValid());
                EXPECT_EQ(child->parent(), interface);
                EXPECT_EQ(interface->indexOfChild(child), index);
                const auto identity = QAccessible::uniqueId(child);
                EXPECT_FALSE(identities.contains(identity));
                identities.insert(identity);
                if (index + 1 < interface->childCount())
                    EXPECT_NE(interface->child(interface->indexOfChild(child) + 1), child);
            }
        };
        const QModelIndex group = tree->model()->index(0, 0);
        for (int cycle = 0; cycle < 3; ++cycle) {
            SCOPED_TRACE(cycle);
            if (cycle > 0) {
                style.unpolish(tree.get());
                style.polish(tree.get());
            }
            tree->expand(group);
            tree->doItemsLayout();
            QAccessibleTableModelChangeEvent reset(tree.get(), QAccessibleTableModelChangeEvent::ModelReset);
            interface->tableInterface()->modelChange(&reset);
            ASSERT_EQ(interface->tableInterface()->rowCount(), 4);
            verifyChildren();
            resetCount = 0;
            tree->collapse(group);
            ASSERT_FALSE(tree->isExpanded(group));
            EXPECT_EQ(resetCount, 1);
            EXPECT_EQ(interface->tableInterface()->rowCount(), 3);
            verifyChildren();
        }
        observedTree = nullptr;
    }
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
    layout->activate();
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

TEST(ThemeMetricsMigration, MessagePresentationChangesPreserveReadingAndPendingInput)
{
    ensureApp();
    auto& theme = VNX::ThemeManager::instance();
    const qreal originalScale = theme.snapshot()->uiScale * 100.0;
    const auto restore = qScopeGuard([&] { theme.setUIScale(originalScale); });
    theme.setUIScale(100);
    const auto settle = [] {
        QEventLoop loop;
        QTimer::singleShot(80, &loop, &QEventLoop::quit);
        loop.exec();
    };
    VNX::AssistantPanel panel;
    panel.resize(640, 640);
    panel.show();
    const QString message = QStringLiteral("Response **with emphasis** and `inline_code`. ").repeated(20);
    QJsonArray history;
    for (int index = 0; index < 24; ++index)
        history.append(QJsonObject{
            {QStringLiteral("role"), index % 2 != 0 ? QStringLiteral("user") : QStringLiteral("assistant")},
            {QStringLiteral("content"), message}});
    panel.onConversationRestored(history);
    panel.onConfirmationRequired(QStringLiteral("pending"), QJsonObject{
        {QStringLiteral("cmd"), QStringLiteral("inspect")},
        {QStringLiteral("args"), QJsonObject{}}});
    settle();
    auto* list = panel.findChild<QListWidget*>(QStringLiteral("aiChatList"));
    auto* input = panel.findChild<QLineEdit*>(QStringLiteral("assistantInput"));
    ASSERT_NE(list, nullptr);
    ASSERT_NE(input, nullptr);
    input->setFocus();
    QCoreApplication::processEvents();
    auto* anchor = list->item(8);
    QPointer<QWidget> messageWidget = list->itemWidget(anchor);
    QPointer<QLabel> label = messageWidget->findChild<QLabel*>(QStringLiteral("aiMessageContent"));
    QPointer<QWidget> confirmation = list->itemWidget(list->item(24));
    ASSERT_NE(label, nullptr);
    list->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
    label->setSelection(3, 11);
    const QString selected = label->selectedText();
    input->setText(QStringLiteral("unfinished input"));
    input->setSelection(2, 6);
    for (int percent : {150, 70, 300, 100}) {
        SCOPED_TRACE(percent);
        theme.setUIScale(percent);
        settle();
        ASSERT_FALSE(messageWidget.isNull());
        ASSERT_FALSE(label.isNull());
        EXPECT_EQ(list->itemWidget(anchor), messageWidget);
        EXPECT_EQ(list->count(), 25);
        EXPECT_EQ(list->itemWidget(list->item(24)), confirmation);
        EXPECT_EQ(label->selectedText(), selected);
        EXPECT_EQ(input->selectedText(), QStringLiteral("finish"));
        EXPECT_LE(qAbs(list->visualItemRect(anchor).top()), 2);
        QTextDocument document;
        document.setDefaultFont(label->font());
        document.setHtml(label->text());
        const auto code = document.find(QStringLiteral("inline_code"));
        ASSERT_FALSE(code.isNull());
        EXPECT_NEAR(code.charFormat().fontPointSize(),
            VNX::FontManager::instance().getFont(VNX::FontRole::Mono).pointSizeF(), 0.01);
    }
    list->verticalScrollBar()->setValue(list->verticalScrollBar()->value()
        + list->visualItemRect(anchor).height() / 3);
    const auto before = list->visualItemRect(anchor);
    theme.setUIScale(150);
    settle();
    const auto after = list->visualItemRect(anchor);
    EXPECT_NEAR(-qreal(after.top()) / after.height(), -qreal(before.top()) / before.height(), 0.02);
    list->scrollToBottom();
    theme.setUIScale(125);
    settle();
    EXPECT_EQ(list->verticalScrollBar()->value(), list->verticalScrollBar()->maximum());
}

TEST(ThemeMetricsMigration, StartupPresentationKeepsControlsInsideAvailableScreen)
{
    ensureApp();
    auto& theme = VNX::ThemeManager::instance();
    const qreal originalScale = theme.snapshot()->uiScale * 100.0;
    const auto restore = qScopeGuard([&] { theme.setUIScale(originalScale); });
    for (int percent : VNX::AppSettings::GLOBAL_SCALE_PRESETS) {
        SCOPED_TRACE(percent);
        theme.setUIScale(percent);
        VNX::StartupLoadingWindow window;
        window.setProjectName(QStringLiteral("A project with a long name ").repeated(12));
        window.show();
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();
        ASSERT_NE(window.screen(), nullptr);
        EXPECT_TRUE(window.screen()->availableGeometry().contains(window.frameGeometry()));
        for (const auto* name : {"startupLoadingProject", "startupLoadingStatus",
                                "startupLoadingPercent", "startupLoadingProgress",
                                "startupLoadingCloseButton"}) {
            auto* control = window.findChild<QWidget*>(QLatin1String(name));
            ASSERT_NE(control, nullptr);
            EXPECT_TRUE(control->isVisible());
            EXPECT_TRUE(window.rect().contains(QRect(control->mapTo(&window, QPoint()), control->size())));
        }
        window.dismiss();
    }
}

TEST(ThemeMetricsMigration, DisabledAndDimmedLabelsFollowColorPreviewAndRestore)
{
    ensureApp();
    QTemporaryDir root;
    auto manager = makeManager(root.path());
    VNX::HarkenStyle style(QStyleFactory::create(QStringLiteral("Fusion")), manager.get());
    QWidget panel;
    panel.setStyle(&style);
    auto* layout = new QVBoxLayout(&panel);
    const auto addLabel = [&](const QString& text) {
        auto* label = new QLabel(text, &panel);
        label->setStyle(&style);
        layout->addWidget(label);
        return label;
    };
    auto* plain = addLabel(QStringLiteral("Plain"));
    auto* primary = addLabel(QStringLiteral("Primary"));
    primary->setProperty("textRole", QStringLiteral("primary"));
    auto* readOnly = addLabel(QStringLiteral("Read only"));
    readOnly->setProperty("roValue", true);
    auto* decibels = addLabel(QStringLiteral("dB"));
    decibels->setProperty("bindingUnit", QStringLiteral("dB"));
    auto* cents = addLabel(QStringLiteral("cent"));
    cents->setProperty("bindingUnit", QStringLiteral("cent"));
    auto* dimmed = addLabel(QStringLiteral("Navigable"));
    dimmed->setProperty("textRole", QStringLiteral("primary"));
    dimmed->setProperty("dimmed", true);
    auto* table = new VNX::TableWidget(&panel, manager.get());
    table->setStyle(&style);
    table->setColumns(QStringLiteral("DisabledLabelTable"), {
        {QStringLiteral("index"), QStringLiteral("#"), 1}
    });
    auto* row = new QWidget(table->rowsWidget());
    auto* cells = new QHBoxLayout(row);
    auto* number = table->createRowNumber(row, 0);
    number->setStyle(&style);
    cells->addWidget(number);
    table->registerRow(row, {number});
    table->rowsLayout()->addWidget(row);
    auto* header = table->headerCell(0);
    header->setStyle(&style);
    layout->addWidget(table);
    const QList<QLabel*> disabled{plain, primary, readOnly, decibels, cents, number, header};
    for (auto* label : disabled) label->setEnabled(false);
    panel.resize(400, 300);
    panel.show();
    QCoreApplication::processEvents();
    const auto original = manager->snapshot();
    const auto verify = [&](const QColor& color) {
        QCoreApplication::processEvents();
        for (const auto* label : disabled) {
            SCOPED_TRACE(label->text().toStdString());
            EXPECT_FALSE(label->isEnabled());
            EXPECT_EQ(label->palette().color(QPalette::Disabled, QPalette::WindowText), color);
        }
        EXPECT_TRUE(dimmed->isEnabled());
        EXPECT_EQ(dimmed->palette().color(QPalette::WindowText), color);
        EXPECT_EQ(qApp->palette().color(QPalette::Disabled, QPalette::WindowText), color);
    };
    verify(original->ui.colors.textDisabled);
    for (const QColor color : {QColor(QStringLiteral("#7c618f")), QColor(QStringLiteral("#52796f"))}) {
        auto preview = manager->appearance();
        preview.colors[preview.themeId][preview.paletteId].insert(QStringLiteral("text.disabled"), color);
        ASSERT_TRUE(manager->previewAppearance(preview).ok());
        verify(color);
    }
    manager->cancelAppearancePreview();
    verify(original->ui.colors.textDisabled);
    for (auto* label : disabled) label->setEnabled(true);
    VNX::QtControlSkin::setSemanticProperty(dimmed, "dimmed", false);
    QCoreApplication::processEvents();
    for (const auto* label : {plain, primary, readOnly, number, dimmed})
        EXPECT_EQ(label->palette().color(QPalette::WindowText), original->ui.colors.textPrimary);
    for (const auto* label : {decibels, cents})
        EXPECT_EQ(label->palette().color(QPalette::WindowText), original->ui.colors.textSecondary);
    EXPECT_EQ(header->palette().color(QPalette::WindowText), original->ui.colors.textSecondary);
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
                               QStringLiteral("border.tick")}) {
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
        EXPECT_EQ(image.pixelColor(x, 0), manager->colors().borderPrimary);
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
                  manager->colors().panelBackground);
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

TEST(ThemeMetricsMigration, ItemViewPaintedRowsRemainDisjointAfterPolishAndScaleChanges)
{
    class RecordingDelegate final : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        mutable QVector<QRect> rectangles = QVector<QRect>(4);

        void paint(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
        {
            rectangles[index.row()] = option.rect;
            QStyledItemDelegate::paint(painter, option, index);
        }
    };
    ensureApp();
    QTemporaryDir directory;
    auto manager = makeManager(directory.path());
    qApp->setStyle(new VNX::HarkenStyle(QStyleFactory::create(QStringLiteral("Fusion")), manager.get()));
    const auto restore = qScopeGuard([] { qApp->setStyle(QStyleFactory::create(QStringLiteral("Fusion"))); });
    for (const auto& name : {QString(), QStringLiteral("settingsCategoryList")}) {
        SCOPED_TRACE(name.toStdString());
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        const auto createList = [&] {
            auto* list = new QListWidget;
            list->setObjectName(name);
            list->setItemDelegate(new RecordingDelegate(list));
            list->addItems({"First", "Second", "Third", "Fourth"});
            list->setCurrentRow(2);
            layout->addWidget(list);
            return list;
        };
        auto* existing = createList();
        window.resize(800, 1400);
        window.show();
        for (const int percent : {100, 300, 50, 125, 100}) {
            SCOPED_TRACE(percent);
            manager->setUIScale(percent);
            auto* fresh = createList();
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
            renderWidget(window);
            for (auto* list : {existing, fresh}) {
                const auto* delegate = static_cast<RecordingDelegate*>(list->itemDelegate());
                for (int row = 0; row < list->count(); ++row) {
                    const auto rect = delegate->rectangles[row];
                    EXPECT_GE(rect.height(), list->fontMetrics().height());
                    if (row > 0) EXPECT_GT(rect.top(), delegate->rectangles[row - 1].bottom());
                }
                EXPECT_EQ(list->currentRow(), 2);
            }
            delete fresh;
        }
    }
}

TEST(ThemeMetricsMigration, ExistingLayoutsFollowMetricsWithoutReplacingEditorsOrCommittingGestures)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(writeMetricsPackage(directory.path(), QStringLiteral("geometry"), R"json({
        "metrics": {
            "spacing.small": 16,
            "spacing.medium": 24,
            "spacing.xSmall": 8,
            "control.height": 40,
            "parameter.labelMinimumWidth": 180,
            "parameter.faderMinimumHeight": 180,
            "parameter.valueMinimumWidth": 120
        }
    })json"));
    auto manager = makeManager(directory.path());
    const auto restore = qScopeGuard([&] { manager->selectTheme(VNX::builtinThemeId()); });
    VNX::ParameterBinding binding;
    binding.setName(QStringLiteral("Gain"));
    binding.setValue(0.25);
    QWidget panel;
    auto* layout = new QVBoxLayout(&panel);
    VNX::ThemeLayout::margins(layout, VNX::ThemeLayout::Space::Medium, *manager);
    VNX::ThemeLayout::spacing(layout, VNX::ThemeLayout::Space::Small, *manager);
    auto* editor = new QLineEdit(QStringLiteral("unfinished input"), &panel);
    layout->addWidget(editor);
    VNX::ThemeLayout::addSpacing(layout, VNX::ThemeLayout::Space::Small, *manager);
    const int spacerIndex = layout->count() - 1;
    auto* slider = new VNX::LinearSliderPresenter(&binding, true, manager.get(), &panel);
    auto* fader = new VNX::VerticalFaderPresenter(&binding, manager.get(), &panel);
    layout->addWidget(slider);
    layout->addWidget(fader);
    panel.resize(700, 700);
    panel.show();
    QCoreApplication::processEvents();
    auto* valueInput = slider->valueEditor()->findChild<QLineEdit*>();
    auto* faderInput = fader->valueEditor()->findChild<QLineEdit*>();
    ASSERT_NE(valueInput, nullptr);
    ASSERT_NE(faderInput, nullptr);
    editor->setSelection(2, 5);
    const auto originalMargins = layout->contentsMargins();
    const int originalFaderHeight = fader->slider()->minimumHeight();
    binding.beginEdit();
    binding.previewEdit(0.75);
    ASSERT_TRUE(manager->selectTheme(QStringLiteral("geometry")).ok());
    QCoreApplication::processEvents();
    EXPECT_GT(layout->contentsMargins().left(), originalMargins.left());
    EXPECT_GT(fader->slider()->minimumHeight(), originalFaderHeight);
    EXPECT_EQ(editor->selectedText(), QStringLiteral("finis"));
    EXPECT_TRUE(binding.isEditing());
    EXPECT_DOUBLE_EQ(binding.value(), 0.75);

    valueInput->selectAll();
    const QString selectedValue = valueInput->selectedText();
    for (int percent : {125, 150, 200, 100}) {
        manager->setUIScale(percent);
        QCoreApplication::processEvents();
        EXPECT_EQ(layout->contentsMargins().left(), manager->metrics().spacingMedium);
        EXPECT_EQ(fader->slider()->minimumHeight(), manager->metrics().parameter.faderMinimumHeight);
        EXPECT_EQ(editor->selectedText(), QStringLiteral("finis"));
        EXPECT_TRUE(binding.isEditing());
        EXPECT_DOUBLE_EQ(binding.value(), 0.75);
        EXPECT_EQ(valueInput->font(), slider->valueEditor()->font());
        EXPECT_EQ(faderInput->font(), fader->valueEditor()->font());
        EXPECT_EQ(valueInput->selectedText(), selectedValue);
    }

    QWidget fresh;
    auto* freshLayout = new QVBoxLayout(&fresh);
    VNX::ThemeLayout::margins(freshLayout, VNX::ThemeLayout::Space::Medium, *manager);
    VNX::ThemeLayout::spacing(freshLayout, VNX::ThemeLayout::Space::Small, *manager);
    VNX::ThemeLayout::addSpacing(freshLayout, VNX::ThemeLayout::Space::Small, *manager);
    EXPECT_EQ(layout->contentsMargins(), freshLayout->contentsMargins());
    EXPECT_EQ(layout->spacing(), freshLayout->spacing());
    EXPECT_EQ(layout->itemAt(spacerIndex)->sizeHint(), freshLayout->itemAt(0)->sizeHint());
    delete layout->takeAt(spacerIndex);
    delete freshLayout->takeAt(0);
    ASSERT_TRUE(manager->reloadTheme().ok());
    binding.cancelEdit();
    EXPECT_DOUBLE_EQ(binding.value(), 0.25);
    ASSERT_TRUE(manager->selectTheme(VNX::builtinThemeId()).ok());
    QCoreApplication::processEvents();
    EXPECT_EQ(layout->contentsMargins(), originalMargins);
    EXPECT_EQ(fader->slider()->minimumHeight(), originalFaderHeight);
    EXPECT_EQ(editor->text(), QStringLiteral("unfinished input"));
    EXPECT_EQ(editor->selectedText(), QStringLiteral("finis"));
}

TEST(ThemeMetricsMigration, InconsistentGeometryCannotReplaceTheActiveTheme)
{
    ensureApp();
    QTemporaryDir directory;
    const QList<QByteArray> variants{
        R"json({"metrics":{"toggle.height":8,"toggle.thumbPadding":4}})json",
        R"json({"metrics":{"node.pinRadius":12,"node.pinHitRadius":4}})json",
        R"json({"metrics":{"timeAxis.handleWidth":0}})json",
        R"json({"metrics":{"toast.minimumWidth":400,"toast.maximumWidth":200}})json",
        R"json({"metrics":{"connection.lineWidth":20,"connection.hitWidth":8}})json",
        R"json({"metrics":{"form.labelColumnShare":0,"form.valueColumnShare":0}})json"
    };
    for (int index = 0; index < variants.size(); ++index)
        ASSERT_TRUE(writeMetricsPackage(directory.path(), QStringLiteral("invalid-%1").arg(index), variants[index]));
    auto manager = makeManager(directory.path());
    const auto original = manager->snapshot();
    const auto appearance = manager->appearance();
    for (int index = 0; index < variants.size(); ++index) {
        SCOPED_TRACE(index);
        const auto result = manager->selectTheme(QStringLiteral("invalid-%1").arg(index));
        EXPECT_FALSE(result.ok());
        ASSERT_FALSE(result.diagnostics.isEmpty());
        EXPECT_EQ(result.diagnostics.first().code, VNX::ThemePackageErrorCode::MetricOutOfRange);
        EXPECT_EQ(manager->snapshot(), original);
        EXPECT_EQ(manager->appearance(), appearance);
    }
}

TEST(ThemeMetricsMigration, TimeHandlePaintAndHitGeometryShareThemeDimensions)
{
    ensureApp();
    QTemporaryDir directory;
    ASSERT_TRUE(writeMetricsPackage(directory.path(), QStringLiteral("handles"), R"json({
        "metrics": {"timeAxis.handleWidth": 28, "timeAxis.handleBandHeight": 24}
    })json"));
    auto manager = makeManager(directory.path());
    const auto restore = qScopeGuard([&] { manager->selectTheme(VNX::builtinThemeId()); });
    for (const auto& theme : {VNX::builtinThemeId(), QStringLiteral("handles")}) {
        ASSERT_TRUE(manager->selectTheme(theme).ok());
        const auto& geometry = manager->metrics().timeAxis;
        for (bool start : {false, true}) {
            const auto shape = start ? VNX::TimelineMarkerShape::EntryHandle : VNX::TimelineMarkerShape::ExitHandle;
            const QRect hit = VNX::TimelineRenderer::markerHandleRect(50, 100, shape, geometry);
            EXPECT_EQ(hit, VNX::timeAxisHandleRect(50, 0, start, geometry));
            QImage image(100, 100, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            VNX::drawTimeAxisHandle(painter, Qt::red, Qt::white, hit);
            painter.end();
            QRect painted;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) != 0)
                        painted = painted.united(QRect(x, y, 1, 1));
            EXPECT_EQ(painted, hit);
        }
    }
}
