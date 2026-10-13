#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ItemTypeCatalog.h"
#include "ActionManager.h"
#include "ContextInfo.h"
#include "HelpCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlatformContextService.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VAudio.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VSpatial.h"
#include "Components/VVariable.h"
#include "Components/VPiece.h"
#include "Components/VTrack.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"
#include "Widgets/Inspector/InspectorLayout.h"
#include "Widgets/Inspector/ContentObjectSchemaRows.h"
#include "Widgets/Inspector/InspectorCustomizationBootstrap.h"
#include "Widgets/Inspector/InspectorCustomizationRegistry.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/Inspector/InspectorLaunchEntry.h"
#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Shared/InspectorFormRow.h"
#include "Widgets/Shared/ProjectItemMime.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/Common/ParameterControlHost.h"
#include "Widgets/Common/ParameterSlider.h"
#include "Widgets/Common/RangeField.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Widgets/Selector/ObjectRefWidget.h"
#include "Widgets/Selector/SelectorWindow.h"
#include "ThemeManager.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QJsonArray>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QScopeGuard>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <array>
#include <memory>

using VNX::ProjectItem;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void typeDigits(QWidget* target, const QString& digits)
{
    for (const QChar digit : digits) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_0 + digit.digitValue(),
                        Qt::NoModifier, QString(digit));
        QApplication::sendEvent(target, &press);
    }
}

void acceptEdit(QWidget* target)
{
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier,
                     QStringLiteral("\r"));
    QApplication::sendEvent(target, &accept);
}

bool isWithin(const QWidget* widget, const QWidget* ancestor)
{
    for (const QWidget* current = widget; current;
         current = current->parentWidget()) {
        if (current == ancestor)
            return true;
    }
    return false;
}

QWidget* selectionChildRow(QWidget* root, const QUuid& childGuid)
{
    if (!root) return nullptr;
    const QString guid = childGuid.toString(QUuid::WithoutBraces);
    for (QWidget* row : root->findChildren<QWidget*>(
             QStringLiteral("selectionChildRow"))) {
        if (row->property("childGuid").toString() == guid)
            return row;
    }
    return nullptr;
}

QWidget* selectionChildRow(QWidget* root, uint32_t stableOccurrenceOrdinal)
{
    if (!root) return nullptr;
    for (QWidget* row : root->findChildren<QWidget*>(
             QStringLiteral("selectionChildRow"))) {
        if (row->property("stableOccurrenceOrdinal").toUInt()
            == stableOccurrenceOrdinal)
            return row;
    }
    return nullptr;
}

QWidget* selectionChildDragHandle(QWidget* row)
{
    if (!row) return nullptr;
    const auto descendants = row->findChildren<QWidget*>();
    for (QWidget* widget : descendants)
        if (widget->property("dragHandleRow").isValid())
            return widget;
    return nullptr;
}

void dragSelectionChild(QWidget* fromRow, QWidget* toRow)
{
    QWidget* handle = selectionChildDragHandle(fromRow);
    QWidget* list = fromRow;
    while (list && list->objectName() != QLatin1String("selectionChildrenList"))
        list = list->parentWidget();
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(toRow, nullptr);
    ASSERT_NE(list, nullptr);
    const QPointF pressLocal = handle->rect().center();
    const QPointF pressGlobal = handle->mapToGlobal(pressLocal.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressGlobal,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    EXPECT_TRUE(fromRow->property("selected").toBool());

    const QPointF releaseGlobal = toRow->mapToGlobal(toRow->rect().center());
    const QPointF releaseLocal = list->mapFromGlobal(releaseGlobal.toPoint());
    QMouseEvent release(QEvent::MouseButtonRelease, releaseLocal, releaseGlobal,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list, &release);
    QCoreApplication::processEvents();
}

QTreeWidgetItem* findTreeItemByText(QTreeWidgetItem* item,
                                    const QString& text)
{
    if (!item) return nullptr;
    if (item->text(0) == text) return item;
    for (int index = 0; index < item->childCount(); ++index)
        if (QTreeWidgetItem* match = findTreeItemByText(
                item->child(index), text))
            return match;
    return nullptr;
}

bool chooseOpenSelectorItem(const QString& text)
{
    VNX::SelectorWindow* selector = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* candidate = qobject_cast<VNX::SelectorWindow*>(widget);
        if (candidate && candidate->isVisible()) {
            selector = candidate;
            break;
        }
    }
    if (!selector) return false;
    auto* tree = selector->findChild<QTreeWidget*>(
        QStringLiteral("SelectorTreeWidget"));
    if (!tree) return false;
    QTreeWidgetItem* item = nullptr;
    for (int index = 0; index < tree->topLevelItemCount() && !item; ++index)
        item = findTreeItemByText(tree->topLevelItem(index), text);
    if (!item) return false;
    if (!(item->flags() & Qt::ItemIsEnabled)
        || !(item->flags() & Qt::ItemIsSelectable))
        return false;
    tree->setCurrentItem(item);
    selector->accept();
    QCoreApplication::processEvents();
    return true;
}

VNX::ClipLedger makeClipLedger(std::initializer_list<VNX::Clip> clips)
{
    VNX::ClipLedger ledger;
    for (VNX::Clip clip : clips) {
        EXPECT_TRUE(ledger.mintOccurrenceOrdinal(&clip));
        ledger.items.push_back(std::move(clip));
    }
    return ledger;
}

class ProjectActionScope {
public:
    explicit ProjectActionScope(VNX::ProjectManager& projectManager)
    {
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerProjectActions(
            VNX::ActionManager::instance(), &projectManager, nullptr, {});
        VNX::registerContainerActions(
            VNX::ActionManager::instance(), &projectManager);
        VNX::UndoRedoManager::instance().clear();
    }

    ~ProjectActionScope()
    {
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
    }
};

} // namespace

TEST(ProjectItemNotesField, RebindPreservesFocusedUncommittedEdit)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::containerInspectorSchema(), VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Container);
    QWidget host;
    std::unique_ptr<QWidget> page(engine.buildPage(&host));
    host.show();

    ProjectItem item(QStringLiteral("MyContainer"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    item.notes = QStringLiteral("saved notes");
    engine.bindItem(&item);

    auto* notesEdit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    ASSERT_NE(notesEdit, nullptr);
    notesEdit->show();
    notesEdit->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(notesEdit->hasFocus());
    notesEdit->setPlainText(QStringLiteral("unsaved notes"));

    engine.bindItem(&item);

    EXPECT_EQ(notesEdit->toPlainText(), QStringLiteral("unsaved notes"));
    EXPECT_EQ(item.notes, QStringLiteral("saved notes"));
}

TEST(ProjectItemNotesField, RefreshPreservesFocusedUncommittedEdit)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::containerInspectorSchema(), VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Container);
    QWidget host;
    std::unique_ptr<QWidget> page(engine.buildPage(&host));
    host.show();

    ProjectItem item(QStringLiteral("MyContainer"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    item.notes = QStringLiteral("saved notes");
    engine.bindItem(&item);

    auto* notesEdit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    ASSERT_NE(notesEdit, nullptr);
    notesEdit->show();
    notesEdit->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(notesEdit->hasFocus());
    notesEdit->setPlainText(QStringLiteral("unsaved notes"));
    item.notes = QStringLiteral("external notes");

    ASSERT_TRUE(engine.refreshField(QStringLiteral("notes")));

    EXPECT_EQ(notesEdit->toPlainText(), QStringLiteral("unsaved notes"));
    EXPECT_EQ(item.notes, QStringLiteral("external notes"));
}

TEST(ProjectItemNotesField, BindsAsMultilineCustomRow)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::InspectorCustomization customization;
    customization.tabbedLayout = true;
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::containerInspectorSchema(), VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Container);
    engine.setCustomization(&customization);
    std::unique_ptr<QWidget> page(engine.buildPage());

    ProjectItem item(QStringLiteral("MyContainer"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    item.notes = QStringLiteral("multi\nline notes");
    item.color = 0xFF336699u;

    engine.bindItem(&item);

    auto* notesEdit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    ASSERT_NE(notesEdit, nullptr);
    EXPECT_EQ(notesEdit->toPlainText(), QStringLiteral("multi\nline notes"));
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("prop_color")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("identity_name")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("identity_type")), nullptr);
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("section_Identity")), nullptr);

    auto* basic = page->findChild<QWidget*>(QStringLiteral("inspectorTabBasic"));
    ASSERT_NE(basic, nullptr);
    auto* layout = basic->layout();
    ASSERT_NE(layout, nullptr);
    QStringList sectionTitles;
    for (int i = 0; i < layout->count(); ++i) {
        if (auto* section = qobject_cast<VNX::CollapsibleSection*>(layout->itemAt(i)->widget()))
            sectionTitles.push_back(section->title());
    }
    EXPECT_EQ(sectionTitles, (QStringList{QStringLiteral("Playback"),
                                         QStringLiteral("Selection"),
                                         QStringLiteral("Custom")}));
}

TEST(ProjectItemNotesField, CommitsAndUndoesFromInspector)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("InspectorNotesProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* action = pm.createItem(
        nullptr, QStringLiteral("NotesAction"), ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    VNX::UndoRedoManager::instance().clear();

    VNX::InspectorPanel panel;
    panel.showItemInspector(action);
    auto* notesEdit = panel.findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    ASSERT_NE(notesEdit, nullptr);
    notesEdit->setFocus();
    notesEdit->setPlainText(QStringLiteral("first line\nsecond line"));
    QFocusEvent focusOut(QEvent::FocusOut);
    QApplication::sendEvent(notesEdit, &focusOut);

    EXPECT_EQ(action->notes, QStringLiteral("first line\nsecond line"));
    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(action->notes.isEmpty());

    pm.closeProject();
}

TEST(InspectorSectionWidget, EffectsSurfaceIsRemovedAndBindingsUseFieldDecorators)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& reg = VNX::InspectorCustomizationRegistry::instance();

    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes())
    {
        EXPECT_EQ(reg.sectionProvider(type, QStringLiteral("effects")), nullptr)
            << VNX::ItemTypeCatalog::canonicalName(type).toStdString();
        EXPECT_EQ(reg.sectionProvider(type, QStringLiteral("bindings")), nullptr)
            << VNX::ItemTypeCatalog::canonicalName(type).toStdString();
    }

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    EXPECT_EQ(page->findChild<QWidget*>(QStringLiteral("prop_signalGraph")), nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("listAdd_signalGraph")), nullptr);
    EXPECT_EQ(page->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_signalGraph")), nullptr);

    const auto& schema = VNX::VMixNode::schema();
    const auto volume = std::find_if(schema.begin(), schema.end(),
        [](const VNX::PropertyDescriptor& descriptor) {
            return descriptor.field == QLatin1String("volume");
        });
    ASSERT_NE(volume, schema.end());
    std::unique_ptr<QWidget> decorator(reg.makeBindingDecorator(*volume));
    ASSERT_NE(decorator, nullptr);
    EXPECT_EQ(decorator->objectName(), QStringLiteral("bindingPoint_volume"));
}

TEST(InspectorContentLayout, CustomSectionsMatchContentInsetsAndControlEdgeAcrossPageLayouts)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    struct ProviderCase {
        ProjectItem::Type type;
        const char* slot;
        const char* list;
        const VNX::PropertySchema& schema;
    };
    const ProviderCase cases[] = {
        {ProjectItem::Type::Attenuation, "dimensions", "dimensionsListWidget", VNX::attenuationInspectorSchema()},
        {ProjectItem::Type::Action, "steps", "stepsListWidget", VNX::actionInspectorSchema()},
        {ProjectItem::Type::Sound, "sources", "sourcesListWidget", VNX::soundInspectorSchema()},
        {ProjectItem::Type::Track, "clips", "clipsList", VNX::trackInspectorSchema()},
        {ProjectItem::Type::Container, "selection", "selectionChildrenList", VNX::containerInspectorSchema()}
    };
    for (const auto& entry : cases) {
        for (int mode = 0; mode < 2; ++mode) {
            SCOPED_TRACE(::testing::Message() << entry.slot << " mode=" << mode);
            ProjectItem item(QStringLiteral("LayoutItem"), entry.type);
            item.attachDefaultComponents();
            if (auto* selection = item.comp<VNX::VSelection>())
                selection->mode = VNX::SelectionMode::Random;
            VNX::InspectorCustomization customization;
            customization.tabbedLayout = mode != 0;
            VNX::PropertyEditorEngine engine;
            engine.setSchema(entry.schema, VNX::ComponentType::Audio);
            engine.setItemType(entry.type);
            engine.setCustomization(&customization);
            std::unique_ptr<QWidget> page(engine.buildPage());
            engine.bindItem(&item);
            auto* control = page->findChild<QWidget*>(QStringLiteral("prop_notes"));
            auto* notesLabel = page->findChild<QLabel*>(QStringLiteral("label_notes"));
            auto* list = page->findChild<QWidget*>(QLatin1String(entry.list));
            ASSERT_NE(control, nullptr);
            ASSERT_NE(notesLabel, nullptr);
            ASSERT_NE(list, nullptr);
            auto* table = list->findChild<QFrame*>(QStringLiteral("listBox"));
            ASSERT_NE(table, nullptr);
            auto* section = page->findChild<VNX::CollapsibleSection*>(
                QStringLiteral("section_") + QLatin1String(entry.slot));
            auto* custom = page->findChild<VNX::CollapsibleSection*>(QStringLiteral("section_custom"));
            ASSERT_NE(section, nullptr);
            ASSERT_NE(custom, nullptr);
            auto* content = section->findChild<QWidget*>(
                QStringLiteral("CollapsibleContent"), Qt::FindDirectChildrenOnly);
            auto* customContent = custom->findChild<QWidget*>(
                QStringLiteral("CollapsibleContent"), Qt::FindDirectChildrenOnly);
            ASSERT_NE(content, nullptr);
            ASSERT_NE(customContent, nullptr);
            QWidget* first = table;
            QWidget* last = table;
            if (entry.type == ProjectItem::Type::Container) {
                first = section->findChild<QWidget*>(QStringLiteral("selectionFormRow_mode"));
                last = section->findChild<QWidget*>(QStringLiteral("selectionFormRow_memoryScope"));
            }
            ASSERT_NE(first, nullptr);
            ASSERT_NE(last, nullptr);
            auto* notesRow = notesLabel->parentWidget();
            page->show();
            for (auto* animation : page->findChildren<QParallelAnimationGroup*>()) {
                if (animation->state() == QAbstractAnimation::Stopped)
                    continue;
                QEventLoop settle;
                QObject::connect(animation, &QAbstractAnimation::finished,
                                 &settle, &QEventLoop::quit);
                QTimer::singleShot(1000, &settle, &QEventLoop::quit);
                settle.exec();
                ASSERT_EQ(animation->state(), QAbstractAnimation::Stopped);
            }
            for (int width : {360, 800, 500}) {
                SCOPED_TRACE(width);
                page->resize(width, 1000);
                Q_EMIT VNX::ThemeManager::instance().themeChanged(
                    VNX::ThemeManager::instance().snapshot());
                QApplication::processEvents(QEventLoop::AllEvents, 100);
                EXPECT_EQ(page->width(), width);
                EXPECT_GT(table->width(), 0);
                EXPECT_EQ(table->mapTo(page.get(), QPoint(table->width(), 0)).x(),
                          control->mapTo(page.get(), QPoint(control->width(), 0)).x());
                EXPECT_EQ(list->width() - table->geometry().right() - 1,
                          inspector.slotWidthFor(2) + inspector.columnPadding);
                const int topInset = notesRow->mapTo(customContent, QPoint()).y();
                const int bottomInset = customContent->height()
                    - notesRow->mapTo(customContent, QPoint(0, notesRow->height())).y();
                EXPECT_GT(topInset, 0);
                EXPECT_GT(bottomInset, 0);
                EXPECT_EQ(first->mapTo(content, QPoint()).y(), topInset);
                EXPECT_EQ(content->height()
                              - last->mapTo(content, QPoint(0, last->height())).y(),
                          bottomInset);
                if (auto* modeControl = page->findChild<QComboBox*>(QStringLiteral("prop_mode")))
                    EXPECT_EQ(table->mapTo(page.get(), QPoint(table->width(), 0)).x(),
                              modeControl->mapTo(page.get(), QPoint(modeControl->width(), 0)).x());
            }
        }
    }
}

TEST(InspectorContentLayout, TableFieldsRetainIdentityAndFitTheAvailableWidth)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    const std::pair<ProjectItem::Type, const VNX::PropertySchema*> cases[] = {
        {ProjectItem::Type::Sound, &VNX::soundInspectorSchema()},
        {ProjectItem::Type::Container, &VNX::containerInspectorSchema()},
        {ProjectItem::Type::Track, &VNX::trackInspectorSchema()},
        {ProjectItem::Type::Layer, &VNX::layerInspectorSchema()},
        {ProjectItem::Type::Piece, &VNX::pieceInspectorSchema()},
        {ProjectItem::Type::Bus, &VNX::busInspectorSchema()},
        {ProjectItem::Type::Variable, &VNX::variableInspectorSchema()},
        {ProjectItem::Type::Action, &VNX::actionInspectorSchema()},
        {ProjectItem::Type::Attenuation, &VNX::attenuationInspectorSchema()},
    };
    for (const auto& [type, schema] : cases) {
        VNX::PropertyEditorEngine engine;
        engine.setSchema(*schema, VNX::ComponentType::Audio);
        engine.setItemType(type);
        std::unique_ptr<QWidget> page(engine.buildPage());
        for (const auto& field : *schema) {
            if (field.type != VNX::PropType::List) continue;
            SCOPED_TRACE(field.field.toStdString());
            EXPECT_EQ(page->findChild<QLabel*>(QStringLiteral("label_") + field.field), nullptr);
            VNX::TableWidget* table = nullptr;
            for (auto* candidate : page->findChildren<VNX::TableWidget*>())
                if (candidate->property("inspectorField").toString() == field.field) {
                    EXPECT_EQ(table, nullptr);
                    table = candidate;
                }
            ASSERT_NE(table, nullptr);
            EXPECT_EQ(table->accessibleName(), field.label);
            ASSERT_GT(table->columnCount(), 0);
            const bool display = field.listPresentation == VNX::ListPresentation::Display;
            EXPECT_EQ(table->options().kind, display ? VNX::TableKind::Display : VNX::TableKind::Config);
            EXPECT_EQ(table->headerCell(0)->property("inspectorState").toBool(), !display);
            auto* section = table->parentWidget();
            while (section && !qobject_cast<VNX::CollapsibleSection*>(section))
                section = section->parentWidget();
            ASSERT_NE(section, nullptr);
            EXPECT_EQ(qobject_cast<VNX::CollapsibleSection*>(section)->title(),
                      VNX::inspectorSectionInfo(field.section).title);
            for (size_t i = 0; i < field.elementSchema.size(); ++i) {
                const auto* column = table->headerCell(static_cast<int>(i));
                ASSERT_NE(column, nullptr);
                EXPECT_EQ(column->text(), field.elementSchema[i].label);
                EXPECT_EQ(column->property("inspectorColumn").toString(), field.elementSchema[i].field);
            }
        }
        auto* control = page->findChild<QWidget*>(QStringLiteral("prop_notes"));
        ASSERT_NE(control, nullptr);
        page->show();
        for (int width : {720, 280, 360, 720}) {
            SCOPED_TRACE(width);
            page->resize(width, 1000);
            QCoreApplication::processEvents();
            for (auto* table : page->findChildren<VNX::TableWidget*>()) {
                SCOPED_TRACE(table->property("inspectorField").toString().toStdString());
                table->refreshLayout();
                QCoreApplication::processEvents();
                auto* scroll = table->scrollArea();
                ASSERT_NE(scroll, nullptr);
                EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
                EXPECT_FALSE(scroll->horizontalScrollBar()->isVisible());
                EXPECT_EQ(table->rowsWidget()->width(), scroll->viewport()->width());
                EXPECT_EQ(table->mapTo(page.get(), QPoint(table->width(), 0)).x(),
                          control->mapTo(page.get(), QPoint(control->width(), 0)).x());
                int totalWidth = 0;
                for (int i = 0; i < table->columnCount(); ++i)
                    if (!table->headerCell(i)->isHidden()) totalWidth += table->headerCell(i)->width();
                EXPECT_EQ(totalWidth, scroll->viewport()->width());
            }
        }
    }
}

TEST(InspectorContentLayout, NumberedTablesReorderDataAndRoundtripUndo)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir directory;
    VNX::ProjectConfig config;
    config.name = QStringLiteral("TableOrder");
    config.location = directory.path();
    ASSERT_TRUE(pm.createProject(config));
    const auto cleanup = qScopeGuard([&] { pm.closeProject(); });
    ProjectActionScope actions(pm);
    VNX::PlatformContextService::instance().setAll(true);
    auto* layer = pm.createItem(nullptr, "Layer", ProjectItem::Type::Layer);
    auto* track = pm.createItem(layer, "Track", ProjectItem::Type::Track);
    auto* first = pm.createItem(track, "First", ProjectItem::Type::Sound);
    auto* second = pm.createItem(track, "Second", ProjectItem::Type::Sound);
    auto* action = pm.createItem(nullptr, "Action", ProjectItem::Type::Action);
    auto* attenuation = pm.createItem(nullptr, "Attenuation", ProjectItem::Type::Attenuation);
    auto* variable = pm.createItem(nullptr, "Variable", ProjectItem::Type::Variable);
    auto* master = pm.getMasterBusItem();
    auto* bus = pm.createItem(master, "Bus", ProjectItem::Type::Bus);
    auto* targetA = pm.createItem(master, "TargetA", ProjectItem::Type::Bus);
    auto* targetB = pm.createItem(master, "TargetB", ProjectItem::Type::Bus);
    ASSERT_TRUE(layer && first && second && track && action && attenuation && variable && bus && targetA && targetB);
    track->comp<VNX::VTrack>()->clips = makeClipLedger({
        {QUuid::createUuid(), first->guid, 0, 1000000000LL},
        {QUuid::createUuid(), second->guid, 1000000000LL, 2000000000LL}});
    VNX::VActionStep step;
    step.targetObjectID = first->objectID;
    action->comp<VNX::VAction>()->steps.push_back(step);
    step.targetObjectID = second->objectID;
    action->comp<VNX::VAction>()->steps.push_back(step);
    attenuation->comp<VNX::VAttenuation>()->dimensions = {
        VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Volume},
        VNX::AttenuationDimensionDef{VNX::AttenuationDimension::Pitch}};
    for (auto& dimension : attenuation->comp<VNX::VAttenuation>()->dimensions)
        dimension.curve = {{0.0f, 0.0f}, {100.0f, 0.0f}};
    ASSERT_TRUE(attenuation->comp<VNX::VAttenuation>()->setDimensions(
        attenuation->comp<VNX::VAttenuation>()->toJson()["dimensions"].toArray()));
    variable->comp<VNX::VVariable>()->kind = VNX::VariableKind::Step;
    variable->comp<VNX::VVariable>()->rangeMax = 10;
    variable->comp<VNX::VVariable>()->labels = {{2, "First"}, {5, "Second"}};
    ASSERT_TRUE(VNX::ActionManager::instance().execute("set_item_property",
        {{"authoring_handle", static_cast<qint64>(bus->objectID)}, {"property", "sends"},
         {"value", QJsonArray{
             QJsonObject{{"targetBusID", static_cast<qint64>(targetA->objectID)}, {"sendLevel", -6.0}},
             QJsonObject{{"targetBusID", static_cast<qint64>(targetB->objectID)}, {"sendLevel", -12.0}}}}}).success);

    const struct {
        ProjectItem* item;
        const char* field;
        std::function<QJsonArray()> data;
    } cases[] = {
        {track, "clips", [track] { return track->comp<VNX::VTrack>()->clips.toJson(); }},
        {action, "steps", [action] { return action->comp<VNX::VAction>()->toJson()["steps"].toArray(); }},
        {attenuation, "dimensions", [attenuation] { return attenuation->comp<VNX::VAttenuation>()->toJson()["dimensions"].toArray(); }},
        {variable, "labels", [variable] { return variable->comp<VNX::VVariable>()->toJson()["labels"].toArray(); }},
        {bus, "sends", [bus] { return bus->comp<VNX::VMixNode>()->toJson()["sends"].toArray(); }},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.field);
        VNX::InspectorPanel panel;
        panel.showItemInspector(test.item);
        panel.resize(720, 1000);
        panel.show();
        QCoreApplication::processEvents();
        VNX::TableWidget* table = nullptr;
        for (auto* candidate : panel.findChildren<VNX::TableWidget*>())
            if (candidate->property("inspectorField").toString() == QLatin1String(test.field)) table = candidate;
        ASSERT_NE(table, nullptr);
        const auto before = test.data();
        ASSERT_TRUE(table->canMoveRow(0, 1));
        ASSERT_EQ(before.size(), 2);
        auto* stack = VNX::UndoRedoManager::instance().undoStack();
        stack->clear();
        table->moveRow(0, 1);
        QCoreApplication::processEvents();
        const auto after = test.data();
        const auto expected = test.item == variable
            ? QJsonArray{
                QJsonObject{{"anchor", 2}, {"name", "Second"}, {"id", before[1].toObject()["id"]}},
                QJsonObject{{"anchor", 5}, {"name", "First"}, {"id", before[0].toObject()["id"]}}}
            : QJsonArray{before[1], before[0]};
        EXPECT_EQ(after, expected);
        ASSERT_EQ(stack->count(), 1);
        stack->undo();
        QCoreApplication::processEvents();
        EXPECT_EQ(test.data(), before);
        stack->redo();
        QCoreApplication::processEvents();
        EXPECT_EQ(test.data(), after);
    }
}

TEST(InspectorContentLayout, DerivedTablesDoNotExposeConfigurationRowMovement)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir directory;
    VNX::ProjectConfig config;
    config.name = QStringLiteral("DerivedTableOrder");
    config.location = directory.path();
    ASSERT_TRUE(pm.createProject(config));
    const auto cleanup = qScopeGuard([&] { pm.closeProject(); });
    ProjectActionScope actions(pm);
    auto* parent = pm.createItem(pm.getMasterBusItem(), "Parent", ProjectItem::Type::Bus);
    auto* first = pm.createItem(parent, "First", ProjectItem::Type::Bus);
    auto* second = pm.createItem(parent, "Second", ProjectItem::Type::Bus);
    ASSERT_TRUE(parent && first && second);
    first->comp<VNX::VMixNode>()->sends = {{parent->objectID, 0.5f}};
    second->comp<VNX::VMixNode>()->sends = {{parent->objectID, 0.25f}};
    const auto firstBefore = first->comp<VNX::VMixNode>()->toJson();
    const auto secondBefore = second->comp<VNX::VMixNode>()->toJson();
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::busInspectorSchema(), VNX::ComponentType::Bus);
    engine.setItemType(ProjectItem::Type::Bus);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(parent);
    page->resize(720, 1000);
    page->show();
    QCoreApplication::processEvents();
    VNX::UndoRedoManager::instance().clear();
    for (const char* field : {"inputs", "receives"}) {
        SCOPED_TRACE(field);
        VNX::TableWidget* table = nullptr;
        for (auto* candidate : page->findChildren<VNX::TableWidget*>())
            if (candidate->property("inspectorField").toString() == QLatin1String(field)) table = candidate;
        ASSERT_NE(table, nullptr);
        const auto rowIds = [table] {
            QList<uint32_t> ids;
            for (int i = 0; i < table->rowsLayout()->count(); ++i) {
                auto* row = table->rowsLayout()->itemAt(i)->widget();
                if (auto* ref = row->findChild<VNX::ObjectRefWidget*>()) ids.append(ref->currentObjectID());
            }
            return ids;
        };
        const auto before = rowIds();
        EXPECT_FALSE(table->canMoveRow(0, 1));
        EXPECT_EQ(table->options().kind, VNX::TableKind::Display);
        EXPECT_EQ(table->columnCount(), 2);
        ASSERT_EQ(before.size(), 2);
        table->moveRow(0, 1);
        QCoreApplication::processEvents();
        EXPECT_EQ(rowIds(), before);
        engine.bindItem(parent);
        QCoreApplication::processEvents();
        EXPECT_EQ(rowIds(), before);
    }
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(first->comp<VNX::VMixNode>()->toJson(), firstBefore);
    EXPECT_EQ(second->comp<VNX::VMixNode>()->toJson(), secondBefore);
}

TEST(InspectorContentLayout, LaunchEntryReservesPageSlots)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    for (int count : {1, 2}) {
        QWidget page;
        VNX::setInspectorFormSlotCount(&page, count);
        auto* layout = new QVBoxLayout(&page);
        auto* entry = new VNX::InspectorLaunchEntry(QStringLiteral("grid"), &page);
        layout->addWidget(entry);
        auto* button = entry->findChild<QToolButton*>(QStringLiteral("launchEntryButton_grid"));
        ASSERT_NE(button, nullptr);
        page.show();
        for (int width : {360, 800, 500}) {
            page.resize(width, 300);
            QApplication::processEvents();
            EXPECT_EQ(entry->width() - button->geometry().right() - 1,
                      inspector.slotWidthFor(count) + inspector.pageMargin);
        }
    }
}

TEST(SourcesSectionWidget, DeepEditProviderExposesAssetActions)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Sound, QStringLiteral("sources"));
    ASSERT_NE(provider, nullptr);
    std::unique_ptr<QWidget> widget(provider->build(nullptr));
    ASSERT_NE(widget, nullptr);
    EXPECT_NE(widget->findChild<QWidget*>(QStringLiteral("sourcesListWidget")), nullptr);
    EXPECT_NE(widget->findChild<QToolButton*>(QStringLiteral("listAdd_sources")), nullptr);
    EXPECT_NE(widget->findChild<QToolButton*>(QStringLiteral("listRemoveSelected_sources")), nullptr);
    EXPECT_EQ(widget->findChild<QPushButton*>(QStringLiteral("sourcesAddExisting")), nullptr);
    EXPECT_EQ(widget->findChild<QPushButton*>(QStringLiteral("sourcesImport")), nullptr);
    EXPECT_EQ(widget->findChild<QLabel*>(QStringLiteral("placeholderMessage_sources")), nullptr);
    provider->bind(widget.get(), nullptr);
}

TEST(TimelineSectionWidget, InspectorHasNoTimelineLaunchSection)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& registry = VNX::InspectorCustomizationRegistry::instance();
    for (auto type : {ProjectItem::Type::Track, ProjectItem::Type::Piece}) {
        ProjectItem item(QStringLiteral("Content"), type);
        item.attachDefaultComponents();
        auto* provider = registry.sectionProvider(type, QStringLiteral("timeline"));
        EXPECT_EQ(provider, nullptr);
        VNX::InspectorPanel inspector;
        inspector.showItemInspector(&item);
        EXPECT_EQ(inspector.findChild<QLabel*>("timelineSectionSummary"), nullptr);
        EXPECT_EQ(inspector.findChild<QToolButton*>("launchEntryButton_timeline"), nullptr);
    }
}

TEST(SelectionSectionWidget, ModePagesFollowSelectionWithoutLaunchEntries)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);

    std::unique_ptr<QWidget> w(provider->build(nullptr));
    ASSERT_NE(w, nullptr);
    auto* placeholder = w->findChild<QWidget*>(QStringLiteral("selectionSectionPlaceholder"));
    auto* behavior = w->findChild<QWidget*>(QStringLiteral("selectionBehaviorPage"));
    ASSERT_NE(placeholder, nullptr);
    ASSERT_NE(behavior, nullptr);
    EXPECT_EQ(w->findChild<QToolButton*>(QStringLiteral("launchEntryButton_selection")),
              nullptr);
    EXPECT_EQ(w->findChild<QToolButton*>(QStringLiteral("launchEntryButton_transition")),
              nullptr);
    EXPECT_EQ(w->findChild<QToolButton*>(QStringLiteral("launchEntryButton_timeline")),
              nullptr);

    provider->bind(w.get(), nullptr);
    EXPECT_FALSE(placeholder->isHidden());

    ProjectItem item(QStringLiteral("VCContainer"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    auto* sel = item.compOrCreate<VNX::VSelection>();

    sel->mode = VNX::SelectionMode::Random;
    provider->bind(w.get(), &item);
    auto* playType = w->findChild<QComboBox*>(QStringLiteral("prop_playType"));
    ASSERT_NE(playType, nullptr);
    EXPECT_TRUE(playType->isVisibleTo(w.get()));
    const auto playTypeTopic = VNX::ContextInfo::tagOf(w->findChild<QWidget*>(
        QStringLiteral("selectionFormRow_playType")));
    ASSERT_TRUE(playTypeTopic.has_value());
    EXPECT_EQ(playTypeTopic->field, QStringLiteral("playType"));
    EXPECT_NE(VNX::HelpCatalog::entryFor(*playTypeTopic), nullptr);
    EXPECT_NE(w->findChild<QComboBox*>(QStringLiteral("selectionBehaviorDiscipline")), nullptr);
    EXPECT_FALSE(w->findChild<QComboBox*>(QStringLiteral("selectionBehaviorDiscipline"))->isHidden());

    sel->mode = VNX::SelectionMode::Match;
    provider->bind(w.get(), &item);
    EXPECT_TRUE(behavior->isHidden());
    EXPECT_FALSE(playType->isVisibleTo(w.get()));
    auto* response = w->findChild<QComboBox*>(QStringLiteral("prop_variableResponse"));
    ASSERT_NE(response, nullptr);
    EXPECT_TRUE(response->isVisibleTo(w.get()));
    const auto responseTopic = VNX::ContextInfo::tagOf(w->findChild<QWidget*>(
        QStringLiteral("selectionFormRow_variableResponse")));
    ASSERT_TRUE(responseTopic.has_value());
    EXPECT_EQ(responseTopic->field, QStringLiteral("variableResponse"));
    EXPECT_NE(VNX::HelpCatalog::entryFor(*responseTopic), nullptr);
    sel->mode = VNX::SelectionMode::Blend;
    sel->blendResponse = VNX::VariableResponse::Live;
    provider->bind(w.get(), &item);
    EXPECT_TRUE(response->isVisibleTo(w.get()));
    EXPECT_EQ(w->findChild<QWidget*>(QStringLiteral("selectionVariableRehearsalGroup")),
              nullptr);
    EXPECT_EQ(w->findChild<QToolButton*>(QStringLiteral("gridAddAxisBtn")), nullptr);

    sel->mode = VNX::SelectionMode::Random;
    sel->playType = VNX::SelectionPlayType::Queue;
    sel->spliceByMarker = true;
    provider->bind(w.get(), &item);
    auto* splice = w->findChild<QCheckBox*>(QStringLiteral("selectionSpliceByMarker"));
    EXPECT_EQ(splice, nullptr);
    EXPECT_EQ(w->findChild<QWidget*>(QStringLiteral("selectionQueueGroup")), nullptr);
    EXPECT_TRUE(sel->spliceByMarker);
    EXPECT_FALSE(response->isVisibleTo(w.get()));
    EXPECT_TRUE(playType->isVisibleTo(w.get()));
    EXPECT_EQ(w->findChild<QPushButton*>(
                  QStringLiteral("selectionBehaviorResetEmitter")),
              nullptr);
    EXPECT_EQ(w->findChild<QPushButton*>(
                  QStringLiteral("selectionBehaviorResetGlobal")),
              nullptr);
}

TEST(SelectionSectionWidget, KeepsFieldsFlatAndSeparatesRehearsalTools)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);

    std::unique_ptr<QWidget> widget(provider->build(nullptr));
    auto* section = widget.get();
    EXPECT_EQ(qobject_cast<VNX::CollapsibleSection*>(section), nullptr);

    ProjectItem item(QStringLiteral("StructuredContainer"),
                     ProjectItem::Type::Container);
    item.attachDefaultComponents();
    provider->bind(widget.get(), &item);

    auto* mode = section->findChild<QComboBox*>(QStringLiteral("prop_mode"));
    auto* modeRow = section->findChild<QWidget*>(
        QStringLiteral("selectionFormRow_mode"));
    auto* modeLabel = section->findChild<QLabel*>(
        QStringLiteral("selectionFormLabel_mode"));
    auto* behaviorPage = section->findChild<QWidget*>(
        QStringLiteral("selectionBehaviorPage"));
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(modeRow, nullptr);
    ASSERT_NE(modeLabel, nullptr);
    ASSERT_NE(behaviorPage, nullptr);
    EXPECT_EQ(section->findChild<QWidget*>(
                  QStringLiteral("selectionVariableRehearsalGroup")),
              nullptr);
    EXPECT_EQ(section->findChild<QWidget*>(
                  QStringLiteral("selectionRehearsalGroup")),
              nullptr);
    EXPECT_EQ(section->findChild<QWidget*>(
                  QStringLiteral("selectionAuthoredGroup")),
              nullptr);
    EXPECT_EQ(section->findChild<QWidget*>(
                  QStringLiteral("selectionVariableAuthoredGroup")),
              nullptr);
    EXPECT_EQ(section->findChild<QLabel*>(
                  QStringLiteral("selectionBehaviorWeightLabel")),
              nullptr);
    EXPECT_EQ(section->findChild<QLabel*>(
                  QStringLiteral("selectionSequenceOrderLabel")),
              nullptr);
    EXPECT_EQ(section->findChildren<QComboBox*>(QStringLiteral("prop_mode")).size(), 1);
    EXPECT_TRUE(isWithin(mode, section));
    EXPECT_FALSE(isWithin(mode, behaviorPage));

    const VNX::PropertyDescriptor* modeDescriptor = nullptr;
    for (const auto& descriptor : VNX::VSelection::schema()) {
        if (descriptor.field == QLatin1String("mode")) {
            modeDescriptor = &descriptor;
            break;
        }
    }
    ASSERT_NE(modeDescriptor, nullptr);
    EXPECT_EQ(modeLabel->text(), modeDescriptor->label);
    EXPECT_EQ(mode->accessibleName(), modeDescriptor->label);
    const auto modeTopic = VNX::ContextInfo::tagOf(modeRow);
    ASSERT_TRUE(modeTopic.has_value());
    EXPECT_EQ(modeTopic->ownerComponent, VNX::ComponentType::Selection);
    EXPECT_EQ(modeTopic->field, QStringLiteral("mode"));
    EXPECT_EQ(modeTopic->fieldLabel, modeDescriptor->label);
    EXPECT_EQ(modeTopic->objectGuid, item.guid);
    EXPECT_EQ(modeTopic->objectType, item.type);
    EXPECT_EQ(modeTopic->objectName, item.name);
    EXPECT_EQ(VNX::HelpCatalog::keyFor(*modeTopic),
              QStringLiteral("fields.selection.mode"));
    const VNX::HelpEntry* modeHelp = VNX::HelpCatalog::entryFor(*modeTopic);
    ASSERT_NE(modeHelp, nullptr);
    EXPECT_EQ(modeHelp->title, modeDescriptor->label);

    const std::array<const char*, 4> behaviorControls = {
        "selectionBehaviorDiscipline",
        "selectionBehaviorAvoidRepeat",
        "selectionBehaviorMemoryScope",
        "selectionBehaviorResume"};
    for (const char* objectName : behaviorControls) {
        auto* control = section->findChild<QWidget*>(
            QString::fromLatin1(objectName));
        ASSERT_NE(control, nullptr) << objectName;
        EXPECT_TRUE(isWithin(control, behaviorPage)) << objectName;
    }
    const std::array<const char*, 4> directBehaviorContainers = {
        "selectionFormRow_discipline",
        "selectionFormRow_avoidRepeat",
        "selectionFormRow_memoryScope",
        "selectionFormRow_resumeFromLast"};
    for (const char* objectName : directBehaviorContainers) {
        auto* container = section->findChild<QWidget*>(
            QString::fromLatin1(objectName));
        ASSERT_NE(container, nullptr) << objectName;
        EXPECT_EQ(container->parentWidget(), behaviorPage) << objectName;
    }
    auto* childrenList = section->findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(childrenList, nullptr);
    EXPECT_FALSE(isWithin(childrenList, behaviorPage));
    EXPECT_EQ(behaviorPage->findChildren<QFrame*>(
                  QStringLiteral("listBox"), Qt::FindDirectChildrenOnly).size(),
              0);
    EXPECT_EQ(childrenList->findChildren<QFrame*>(
                  QStringLiteral("listBox"), Qt::FindDirectChildrenOnly).size(),
              1);

    EXPECT_EQ(section->findChild<QToolButton*>(
                  QStringLiteral("launchEntryButton_selection")),
              nullptr);
    EXPECT_EQ(section->findChild<QToolButton*>(
                  QStringLiteral("launchEntryButton_transition")),
              nullptr);
    EXPECT_EQ(section->findChild<QToolButton*>(
                  QStringLiteral("launchEntryButton_timeline")),
              nullptr);
    EXPECT_EQ(section->findChild<QPushButton*>(
                  QStringLiteral("selectionBehaviorResetEmitter")),
              nullptr);
    EXPECT_EQ(section->findChild<QPushButton*>(
                  QStringLiteral("selectionBehaviorResetGlobal")),
              nullptr);
    EXPECT_EQ(section->findChild<QPushButton*>(
                  QStringLiteral("selectionBehaviorNext")),
              nullptr);
    EXPECT_EQ(section->findChild<QLineEdit*>(
                  QStringLiteral("selectionBehaviorEmitterKey")),
              nullptr);

    ProjectItem reboundItem(QStringLiteral("ReboundContainer"),
                            ProjectItem::Type::Container);
    reboundItem.attachDefaultComponents();
    provider->bind(section, &reboundItem);
    const auto reboundTopic = VNX::ContextInfo::tagOf(modeRow);
    ASSERT_TRUE(reboundTopic.has_value());
    EXPECT_EQ(reboundTopic->objectGuid, reboundItem.guid);
    EXPECT_EQ(reboundTopic->objectType, reboundItem.type);
    EXPECT_EQ(reboundTopic->objectName, reboundItem.name);
}

TEST(SelectionSectionWidget, ModeChangesKeepVisibleRowsCompact)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);

    ProjectItem item(QStringLiteral("CompactSelection"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    auto* selection = item.comp<VNX::VSelection>();
    for (int i = 0; i < 8; ++i)
        selection->selections.shared.matchAxes.push_back(QUuid::createUuid());

    QWidget host;
    auto* hostLayout = new QVBoxLayout(&host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    auto* section = provider->build(&host);
    hostLayout->addWidget(section);
    hostLayout->addStretch(1);
    auto* children = section->findChild<QWidget*>(QStringLiteral("selectionChildrenList"));
    auto* content = section->findChild<QWidget*>(QStringLiteral("selectionSectionContent"));
    ASSERT_NE(children, nullptr);
    ASSERT_NE(content, nullptr);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;
    host.show();

    for (int width : {360, 640}) {
        host.resize(width, 1000);
        for (const auto& [mode, playType] : {
                 std::pair{VNX::SelectionMode::Match, VNX::SelectionPlayType::Single},
                 std::pair{VNX::SelectionMode::Blend, VNX::SelectionPlayType::Single},
                 std::pair{VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Single},
                 std::pair{VNX::SelectionMode::Random, VNX::SelectionPlayType::Single},
                 std::pair{VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Queue},
                 std::pair{VNX::SelectionMode::Random, VNX::SelectionPlayType::Queue}}) {
            SCOPED_TRACE(::testing::Message() << "width=" << width
                         << " mode=" << static_cast<int>(mode));
            selection->mode = mode;
            selection->playType = playType;
            provider->bind(section, &item);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

            QList<QWidget*> rows;
            if (children->isVisible()) rows.push_back(children);
            for (auto* row : content->findChildren<QWidget*>()) {
                if (!row->isVisible()
                    || !row->objectName().startsWith(QStringLiteral("selectionFormRow_")))
                    continue;
                rows.push_back(row);
                EXPECT_EQ(row->height(), std::max(inspector.rowHeight, row->sizeHint().height()))
                    << row->objectName().toStdString();
            }
            ASSERT_GT(rows.size(), 1);
            std::sort(rows.begin(), rows.end(), [content](QWidget* left, QWidget* right) {
                return left->mapTo(content, QPoint()).y() < right->mapTo(content, QPoint()).y();
            });
            int bottom = rows.front()->mapTo(content, QPoint()).y() + rows.front()->height();
            for (int i = 1; i < rows.size(); ++i) {
                auto* row = rows[i];
                EXPECT_EQ(row->mapTo(content, QPoint()).y() - bottom, inspector.rowSpacing)
                    << row->objectName().toStdString();
                bottom = row->mapTo(content, QPoint()).y() + row->height();
            }
            EXPECT_EQ(content->height() - bottom, 0);
        }
    }
}

TEST(SelectionSectionWidget, SelectionRowsUseInspectorFormContract)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);

    std::unique_ptr<QWidget> section(provider->build(nullptr));
    ProjectItem item(QStringLiteral("FormContainer"),
                     ProjectItem::Type::Container);
    item.attachDefaultComponents();
    provider->bind(section.get(), &item);
    const auto& inspector = VNX::ThemeManager::instance().metrics().inspector;

    const std::array<const char*, 6> rowKeys = {
        "mode", "playType", "discipline", "avoidRepeat", "memoryScope", "resumeFromLast"};
    for (const char* key : rowKeys) {
        const QString suffix = QString::fromLatin1(key);
        auto* row = section->findChild<QWidget*>(
            QStringLiteral("selectionFormRow_") + suffix);
        auto* label = section->findChild<QLabel*>(
            QStringLiteral("selectionFormLabel_") + suffix);
        auto* value = section->findChild<QWidget*>(
            QStringLiteral("selectionFormValue_") + suffix);
        auto* gutter = section->findChild<QWidget*>(
            QStringLiteral("selectionFormGutter_") + suffix);
        ASSERT_NE(row, nullptr) << key;
        ASSERT_NE(label, nullptr) << key;
        ASSERT_NE(value, nullptr) << key;
        ASSERT_NE(gutter, nullptr) << key;
        auto* layout = qobject_cast<QHBoxLayout*>(row->layout());
        ASSERT_NE(layout, nullptr) << key;
        auto* valueLayout = qobject_cast<QHBoxLayout*>(value->layout());
        ASSERT_NE(valueLayout, nullptr) << key;

        EXPECT_EQ(row->minimumHeight(), inspector.rowHeight) << key;
        EXPECT_EQ(layout->contentsMargins(), QMargins()) << key;
        EXPECT_EQ(layout->spacing(), 0) << key;
        EXPECT_EQ(label->property("textRole").toString(),
                  QStringLiteral("formLabel")) << key;
        EXPECT_EQ(label->contentsMargins(),
                  QMargins(0, 0, inspector.columnPadding, 0)) << key;
        EXPECT_EQ(value->property("columnRole").toString(),
                  QStringLiteral("value")) << key;
        EXPECT_EQ(valueLayout->contentsMargins(),
                  QMargins(inspector.columnPadding, 0,
                           inspector.columnPadding, 0)) << key;
        EXPECT_EQ(layout->stretch(layout->indexOf(label)),
                  inspector.labelColumnShare) << key;
        EXPECT_EQ(layout->stretch(layout->indexOf(value)),
                  inspector.valueColumnShare) << key;
        EXPECT_EQ(gutter->minimumWidth(), inspector.slotWidthFor(2)) << key;
        EXPECT_EQ(gutter->maximumWidth(), inspector.slotWidthFor(2)) << key;
    }

    VNX::PropertyEditorEngine schemaEngine;
    schemaEngine.setSchema(VNX::containerInspectorSchema(), VNX::ComponentType::Audio);
    schemaEngine.setItemType(VNX::ProjectItem::Type::Container);
    std::unique_ptr<QWidget> schemaPage(schemaEngine.buildPage());
    auto* schemaGutter = schemaPage->findChild<QWidget*>(
        QStringLiteral("inspectorFieldGutter_volume"));
    auto* formGutter = section->findChild<QWidget*>(
        QStringLiteral("selectionFormGutter_mode"));
    ASSERT_NE(schemaGutter, nullptr);
    ASSERT_NE(formGutter, nullptr);
    EXPECT_EQ(formGutter->minimumWidth(), schemaGutter->minimumWidth());
    EXPECT_EQ(formGutter->maximumWidth(), schemaGutter->maximumWidth());
    EXPECT_EQ(formGutter->minimumWidth(), inspector.slotWidthFor(2));

    auto* avoidRepeat = section->findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorAvoidRepeat"));
    ASSERT_NE(avoidRepeat, nullptr);
    EXPECT_EQ(avoidRepeat->buttonSymbols(), QAbstractSpinBox::NoButtons);
    EXPECT_TRUE(avoidRepeat->alignment().testFlag(Qt::AlignLeft));
    EXPECT_EQ(avoidRepeat->property("fieldRole").toString(),
              QStringLiteral("inspectorValue"));

    auto* resume = section->findChild<QCheckBox*>(
        QStringLiteral("selectionBehaviorResume"));
    auto* resumeRow = section->findChild<QWidget*>(
        QStringLiteral("selectionFormRow_resumeFromLast"));
    auto* memoryScope = section->findChild<QComboBox*>(
        QStringLiteral("selectionBehaviorMemoryScope"));
    ASSERT_NE(resume, nullptr);
    ASSERT_NE(resumeRow, nullptr);
    ASSERT_NE(memoryScope, nullptr);
    EXPECT_TRUE(resumeRow->isHidden());
    EXPECT_TRUE(memoryScope->isEnabled());

    auto* selection = item.comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Random;
    selection->playType = VNX::SelectionPlayType::Queue;
    provider->bind(section.get(), &item);
    EXPECT_FALSE(resumeRow->isHidden());
    EXPECT_FALSE(resume->isChecked());
    EXPECT_FALSE(memoryScope->isVisibleTo(section.get()));
}

TEST(SelectionSectionWidget, StandaloneOccurrenceRowsResolveBoundChildren)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto* provider = VNX::InspectorCustomizationRegistry::instance().sectionProvider(
        ProjectItem::Type::Container, QStringLiteral("selection"));
    ASSERT_NE(provider, nullptr);

    std::unique_ptr<QWidget> section(provider->build(nullptr));
    ProjectItem item(QStringLiteral("StandaloneContainer"),
                     ProjectItem::Type::Container);
    item.guid = QUuid::createUuid();
    item.attachDefaultComponents();
    auto child = std::make_shared<ProjectItem>(
        QStringLiteral("StandaloneChild"), ProjectItem::Type::Sound);
    child->guid = QUuid::createUuid();
    child->parent = &item;
    child->attachDefaultComponents();
    item.children.push_back(child);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        item, child->guid));

    provider->bind(section.get(), &item);
    auto* list = section->findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    QWidget* row = selectionChildRow(list, child->guid);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->property("stableOccurrenceOrdinal").toUInt(), 0u);
}

TEST(SelectionSectionWidget, InspectorSearchFindsVisiblePropertyLabels)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    ProjectItem item(QStringLiteral("SearchableContainer"),
                     ProjectItem::Type::Container);
    item.attachDefaultComponents();
    auto child = std::make_shared<ProjectItem>(
        QStringLiteral("SearchChild"), ProjectItem::Type::Sound);
    child->attachDefaultComponents();
    child->guid = QUuid::createUuid();
    child->parent = &item;
    item.children.push_back(child);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        item, child->guid));

    VNX::InspectorPanel panel;
    panel.showItemInspector(&item);
    auto* section = panel.findChild<VNX::CollapsibleSection*>(
        QStringLiteral("section_selection"));
    auto* search = panel.findChild<VNX::SearchField*>();
    auto* mode = panel.findChild<QComboBox*>(QStringLiteral("prop_mode"));
    auto* discipline = panel.findChild<QWidget*>(
        QStringLiteral("selectionFormRow_discipline"));
    auto* childrenList = panel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    auto* weightHeader = panel.findChild<QLabel*>(
        QStringLiteral("selectionChildWeightHeader"));
    ASSERT_NE(section, nullptr);
    ASSERT_NE(search, nullptr);
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(discipline, nullptr);
    ASSERT_NE(childrenList, nullptr);
    ASSERT_NE(weightHeader, nullptr);
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("selectionQueueGroup")), nullptr);
    EXPECT_FALSE(discipline->isHidden());
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_FALSE(weightHeader->isHidden());

    search->setText(QStringLiteral("Selection Mode"));
    EXPECT_FALSE(section->isHidden());
    EXPECT_TRUE(section->isExpanded());

    search->clear();
    QCoreApplication::processEvents();
    EXPECT_FALSE(discipline->isHidden());
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_FALSE(weightHeader->isHidden());

    search->setText(QStringLiteral("Splice by Marker"));
    EXPECT_TRUE(section->isHidden());

    auto* selection = item.comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = VNX::SelectionMode::Random;
    selection->playType = VNX::SelectionPlayType::Queue;
    selection->spliceByMarker = true;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        &item, QStringLiteral("mode"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(section->isHidden());
    EXPECT_EQ(panel.findChild<QWidget*>(QStringLiteral("selectionQueueGroup")), nullptr);
    EXPECT_TRUE(selection->spliceByMarker);

    selection->mode = VNX::SelectionMode::Random;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        &item, QStringLiteral("mode"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(section->isHidden());

    selection->mode = VNX::SelectionMode::Sequence;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        &item, QStringLiteral("mode"));
    QCoreApplication::processEvents();
    EXPECT_EQ(mode->currentData().toInt(),
              static_cast<int>(VNX::SelectionMode::Sequence));

    search->setText(QStringLiteral("Selection Mode"));
    EXPECT_FALSE(section->isHidden());
    search->clear();
    QCoreApplication::processEvents();
    EXPECT_TRUE(discipline->isHidden());
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_TRUE(weightHeader->isHidden());

    selection->mode = VNX::SelectionMode::Match;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        &item, QStringLiteral("mode"));
    QCoreApplication::processEvents();
    auto* childRow = selectionChildRow(childrenList, child->guid);
    ASSERT_NE(childRow, nullptr);
    EXPECT_EQ(childRow->findChild<QAbstractButton*>(QStringLiteral("selectionChildExpander")), nullptr);
    EXPECT_TRUE(childrenList->isHidden());
    selection->mode = VNX::SelectionMode::Sequence;
    emit VNX::ProjectManager::instance().itemPropertyChanged(
        &item, QStringLiteral("mode"));
    QCoreApplication::processEvents();
    childRow = selectionChildRow(childrenList, child->guid);
    ASSERT_NE(childRow, nullptr);
    EXPECT_FALSE(childrenList->isHidden());
    search->setText(QStringLiteral("Volume"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(section->isHidden());
    EXPECT_TRUE(childRow->isHidden());
    search->setText(QStringLiteral("Pitch"));
    QCoreApplication::processEvents();
    EXPECT_TRUE(section->isHidden());
    search->clear();
    QCoreApplication::processEvents();
    EXPECT_FALSE(childRow->isHidden());

    search->setText(QStringLiteral("RenamedSearchChild"));
    EXPECT_TRUE(section->isHidden());
    child->name = QStringLiteral("RenamedSearchChild");
    emit VNX::ProjectManager::instance().itemRenamed(
        child.get(), QStringLiteral("SearchChild"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(section->isHidden());
    QWidget* renamedRow = selectionChildRow(childrenList, child->guid);
    ASSERT_NE(renamedRow, nullptr);
    EXPECT_FALSE(renamedRow->isHidden());
}

TEST(TimelineInspectorEntry, TrackAndPieceExposeInspectorContext)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    ProjectItem track(QStringLiteral("Track"), ProjectItem::Type::Track);
    ProjectItem piece(QStringLiteral("Piece"), ProjectItem::Type::Piece);
    track.attachDefaultComponents();
    piece.attachDefaultComponents();
    for (ProjectItem* item : {&track, &piece}) {
        VNX::InspectorPanel panel;
        panel.showItemInspector(item);
        EXPECT_EQ(panel.currentContextItem(), item);
        EXPECT_NE(panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_volume")), nullptr);
        EXPECT_NE(panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_pitch")), nullptr);
        EXPECT_NE(panel.findChild<VNX::ValueSegmentedControl*>(QStringLiteral("prop_loopCount")), nullptr);
    }
}

TEST(AudioFeedChassisRows, ResolveThroughMixNodeFallback)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::containerInspectorSchema(), VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Container);
    std::unique_ptr<QWidget> page(engine.buildPage());

    ProjectItem item(QStringLiteral("MyContainer"), ProjectItem::Type::Container);
    item.attachDefaultComponents();
    item.comp<VNX::VMixNode>()->volume = -7.5f;
    item.comp<VNX::VMixNode>()->pitch = 3.0f;

    engine.bindItem(&item);

    auto* volume = page->findChild<VNX::ParameterSlider*>(QStringLiteral("prop_volume"));
    auto* pitch = page->findChild<VNX::ParameterSlider*>(QStringLiteral("prop_pitch"));
    ASSERT_NE(volume, nullptr);
    ASSERT_NE(pitch, nullptr);
    EXPECT_FLOAT_EQ(static_cast<float>(volume->value()), -7.5f);
    EXPECT_FLOAT_EQ(static_cast<float>(pitch->value()), 3.0f);
}

TEST(ContainerInspectorMode, ComboCommitsSelectionMode)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("ContainerInspectorModeProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("ModeLayer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("ModeContainer"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(container->comp<VNX::VSelection>(), nullptr);
    ASSERT_NE(container->objectID, 0u);

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    auto* section = panel.findChild<VNX::CollapsibleSection*>(
        QStringLiteral("section_selection"));
    auto* behaviorPage = panel.findChild<QWidget*>(
        QStringLiteral("selectionBehaviorPage"));
    const auto modeEditors = panel.findChildren<QComboBox*>(
        QStringLiteral("prop_mode"));
    ASSERT_NE(section, nullptr);
    ASSERT_NE(behaviorPage, nullptr);
    ASSERT_EQ(modeEditors.size(), 1);
    auto* combo = modeEditors.front();
    ASSERT_NE(combo, nullptr);
    EXPECT_TRUE(isWithin(combo, section));
    EXPECT_FALSE(isWithin(combo, behaviorPage));

    const VNX::PropertyDescriptor* modeDescriptor = nullptr;
    for (const auto& descriptor : VNX::VSelection::schema()) {
        if (descriptor.field == QLatin1String("mode")) {
            modeDescriptor = &descriptor;
            break;
        }
    }
    ASSERT_NE(modeDescriptor, nullptr);
    ASSERT_EQ(combo->count(), static_cast<int>(modeDescriptor->options.size()));
    for (int index = 0; index < combo->count(); ++index)
        EXPECT_EQ(combo->itemData(index), modeDescriptor->options[static_cast<size_t>(index)].value);

    const int sequenceIndex = combo->findData(
        static_cast<int>(VNX::SelectionMode::Sequence));
    ASSERT_GE(sequenceIndex, 0);
    auto* childrenList = panel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    auto* weightHeader = panel.findChild<QLabel*>(
        QStringLiteral("selectionChildWeightHeader"));
    auto* discipline = panel.findChild<QWidget*>(
        QStringLiteral("selectionFormRow_discipline"));
    ASSERT_NE(childrenList, nullptr);
    ASSERT_NE(weightHeader, nullptr);
    ASSERT_NE(discipline, nullptr);
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_FALSE(weightHeader->isHidden());
    EXPECT_FALSE(discipline->isHidden());

    VNX::UndoRedoManager::instance().clear();
    const int before = VNX::UndoRedoManager::instance().undoStack()->count();
    combo->setCurrentIndex(sequenceIndex);
    QCoreApplication::processEvents();

    EXPECT_EQ(container->comp<VNX::VSelection>()->mode,
              VNX::SelectionMode::Sequence);
    EXPECT_EQ(combo->currentData().toInt(),
              static_cast<int>(VNX::SelectionMode::Sequence));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), before + 1);
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_TRUE(weightHeader->isHidden());
    EXPECT_TRUE(discipline->isHidden());

    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->comp<VNX::VSelection>()->mode,
              VNX::SelectionMode::Random);
    EXPECT_EQ(combo->currentData().toInt(),
              static_cast<int>(VNX::SelectionMode::Random));
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_FALSE(weightHeader->isHidden());
    EXPECT_FALSE(discipline->isHidden());

    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->comp<VNX::VSelection>()->mode,
              VNX::SelectionMode::Sequence);
    EXPECT_EQ(combo->currentData().toInt(),
              static_cast<int>(VNX::SelectionMode::Sequence));
    EXPECT_FALSE(childrenList->isHidden());
    EXPECT_TRUE(weightHeader->isHidden());
    EXPECT_TRUE(discipline->isHidden());

    auto* playType = panel.findChild<QComboBox*>(QStringLiteral("prop_playType"));
    auto* memoryScope = panel.findChild<QWidget*>(QStringLiteral("selectionFormRow_memoryScope"));
    auto* resume = panel.findChild<QWidget*>(QStringLiteral("selectionFormRow_resumeFromLast"));
    ASSERT_NE(playType, nullptr);
    ASSERT_NE(memoryScope, nullptr);
    ASSERT_NE(resume, nullptr);
    EXPECT_EQ(combo->count(), 4);
    playType->setCurrentIndex(playType->findData(static_cast<int>(VNX::SelectionPlayType::Queue)));
    QCoreApplication::processEvents();
    EXPECT_TRUE(container->comp<VNX::VSelection>()->isQueue());
    EXPECT_TRUE(memoryScope->isHidden());
    EXPECT_FALSE(resume->isHidden());
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_FALSE(container->comp<VNX::VSelection>()->isQueue());
    EXPECT_EQ(playType->currentData().toInt(), static_cast<int>(VNX::SelectionPlayType::Single));
    EXPECT_FALSE(memoryScope->isHidden());
    EXPECT_TRUE(resume->isHidden());
    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(playType->currentData().toInt(), static_cast<int>(VNX::SelectionPlayType::Queue));

    combo->setCurrentIndex(combo->findData(static_cast<int>(VNX::SelectionMode::Blend)));
    QCoreApplication::processEvents();
    auto* response = panel.findChild<QComboBox*>(QStringLiteral("prop_variableResponse"));
    ASSERT_NE(response, nullptr);
    EXPECT_FALSE(playType->isVisibleTo(section));
    const int responseBase = VNX::UndoRedoManager::instance().undoStack()->count();
    response->setCurrentIndex(response->findData(static_cast<int>(VNX::VariableResponse::AtStart)));
    QCoreApplication::processEvents();
    EXPECT_EQ(container->comp<VNX::VSelection>()->variableResponse(), VNX::VariableResponse::AtStart);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), responseBase + 1);
    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->comp<VNX::VSelection>()->variableResponse(), VNX::VariableResponse::Live);
    EXPECT_EQ(response->currentData().toInt(), static_cast<int>(VNX::VariableResponse::Live));

    pm.closeProject();
}

TEST(VariableInspectorRange, InvertedCommitReloadsNormalizedBounds)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("VariableInspectorRangeProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* variable = pm.createItem(
        nullptr, QStringLiteral("RangeVariable"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);

    VNX::InspectorPanel panel;
    panel.showItemInspector(variable);
    auto* range = panel.findChild<VNX::RangeField*>(QStringLiteral("prop_range"));
    ASSERT_NE(range, nullptr);

    emit range->valuesCommitted(10.0, 3.0);
    QCoreApplication::processEvents();

    EXPECT_DOUBLE_EQ(range->lowValue(), 3.0);
    EXPECT_DOUBLE_EQ(range->highValue(), 10.0);

    pm.closeProject();
}

TEST(VariableInspectorRange, ConflictDialogListsEveryCellAndCanProceed)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("VariableRangeImpactProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* variable = pm.createItem(
        nullptr, QStringLiteral("RangeDrive"), ProjectItem::Type::Variable);
    ProjectItem* container = pm.createItem(
        nullptr, QStringLiteral("RangeGrid"), ProjectItem::Type::Container);
    ProjectItem* first = pm.createItem(
        container, QStringLiteral("RangeFirst"), ProjectItem::Type::Sound);
    ProjectItem* second = pm.createItem(
        container, QStringLiteral("RangeSecond"), ProjectItem::Type::Sound);
    ASSERT_NE(variable, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* declaration = variable->compOrCreate<VNX::VVariable>();
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 2.0f;
    auto* selection = container->compOrCreate<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Match;
    auto matches = selection->matchGrid();
    ASSERT_TRUE(matches.addAxis(variable->guid));
    ASSERT_TRUE(matches.addRow(first->guid));
    ASSERT_TRUE(matches.addRow(second->guid));
    ASSERT_TRUE(matches.setCell(0, 0, 2));
    ASSERT_TRUE(matches.setCell(1, 0, 2));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(matches));

    VNX::InspectorPanel panel;
    panel.showItemInspector(variable);
    auto* range = panel.findChild<VNX::RangeField*>(QStringLiteral("prop_range"));
    ASSERT_NE(range, nullptr);

    QString details;
    QTimer confirmer;
    QObject::connect(&confirmer, &QTimer::timeout, [&details]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName() != QLatin1String("variableRangeImpactDialog"))
            return;
        if (auto* edit = modal->findChild<QPlainTextEdit*>(
                QStringLiteral("referenceImpactDetails"))) {
            details = edit->toPlainText();
        }
        if (auto* button = modal->findChild<QPushButton*>(
                QStringLiteral("referenceImpactConfirmButton"))) {
            button->click();
        }
    });
    confirmer.start(0);
    emit range->valuesCommitted(0.0, 1.0);
    confirmer.stop();
    QCoreApplication::processEvents();

    EXPECT_TRUE(details.contains(first->name));
    EXPECT_TRUE(details.contains(second->name));
    EXPECT_FLOAT_EQ(declaration->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(declaration->rangeMax, 1.0f);

    pm.closeProject();
}

TEST(SelectionSectionWidget, UnifiedChildrenListDragReordersAndUndo)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SequenceOrderProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("SequenceLayer"), ProjectItem::Type::Layer);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("SequenceContainer"), ProjectItem::Type::Container);
    ProjectItem* first = pm.createItem(
        container, QStringLiteral("SequenceFirst"), ProjectItem::Type::Sound);
    ProjectItem* second = pm.createItem(
        container, QStringLiteral("SequenceSecond"), ProjectItem::Type::Sound);
    ProjectItem* otherContainer = pm.createItem(
        layer, QStringLiteral("OtherContainer"), ProjectItem::Type::Container);
    ProjectItem* otherFirst = pm.createItem(
        otherContainer, QStringLiteral("OtherFirst"), ProjectItem::Type::Sound);
    ProjectItem* otherSecond = pm.createItem(
        otherContainer, QStringLiteral("OtherSecond"), ProjectItem::Type::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(otherContainer, nullptr);
    ASSERT_NE(otherFirst, nullptr);
    ASSERT_NE(otherSecond, nullptr);
    container->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Sequence;
    otherContainer->comp<VNX::VSelection>()->mode =
        VNX::SelectionMode::Sequence;
    VNX::UndoRedoManager::instance().clear();

    const auto firstOccurrence =
        VNX::PlayablePropertyAuthoringV1::containerChildLocator(
            *container, first->guid);
    const auto secondOccurrence =
        VNX::PlayablePropertyAuthoringV1::containerChildLocator(
            *container, second->guid);
    ASSERT_TRUE(firstOccurrence.has_value());
    ASSERT_TRUE(secondOccurrence.has_value());

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    panel.show();
    VNX::InspectorPanel otherPanel;
    otherPanel.showItemInspector(otherContainer);
    otherPanel.setGeometry(panel.geometry());
    otherPanel.show();
    QCoreApplication::processEvents();

    auto* list = panel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    auto* add = list->findChild<QToolButton*>(
        QStringLiteral("listAdd_selectionChildren"));
    auto* remove = list->findChild<QToolButton*>(
        QStringLiteral("listRemoveSelected_selectionChildren"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);
    auto* otherList = otherPanel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(otherList, nullptr);
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_TRUE(panel.findChildren<QPushButton*>(
                    QStringLiteral("selectionSequenceOrderUp")).isEmpty());
    EXPECT_TRUE(panel.findChildren<QPushButton*>(
                    QStringLiteral("selectionSequenceOrderDown")).isEmpty());

    QWidget* firstRow = selectionChildRow(list, first->guid);
    QWidget* secondRow = selectionChildRow(list, second->guid);
    ASSERT_NE(firstRow, nullptr);
    ASSERT_NE(secondRow, nullptr);
    EXPECT_EQ(firstRow->property("stableOccurrenceOrdinal").toUInt(),
              firstOccurrence->stableOccurrenceOrdinal);
    EXPECT_EQ(secondRow->property("stableOccurrenceOrdinal").toUInt(),
              secondOccurrence->stableOccurrenceOrdinal);

    dragSelectionChild(firstRow, secondRow);

    ASSERT_EQ(container->children.size(), 2u);
    EXPECT_EQ(container->children[0].get(), first);
    EXPECT_EQ(container->children[1].get(), second);
    ASSERT_EQ(otherContainer->children.size(), 2u);
    EXPECT_EQ(otherContainer->children[0].get(), otherFirst);
    EXPECT_EQ(otherContainer->children[1].get(), otherSecond);
    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal,
              secondOccurrence->stableOccurrenceOrdinal);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal,
              firstOccurrence->stableOccurrenceOrdinal);
    ASSERT_EQ(otherContainer->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(otherContainer->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal,
              0u);
    EXPECT_EQ(otherContainer->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal,
              1u);

    const auto rows = list->findChildren<QWidget*>(
        QStringLiteral("selectionChildRow"));
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0]->property("childGuid").toString(),
              second->guid.toString(QUuid::WithoutBraces));
    EXPECT_EQ(rows[1]->property("childGuid").toString(),
              first->guid.toString(QUuid::WithoutBraces));

    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children[0].get(), first);
    EXPECT_EQ(container->children[1].get(), second);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal,
              firstOccurrence->stableOccurrenceOrdinal);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal,
              secondOccurrence->stableOccurrenceOrdinal);

    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children[0].get(), first);
    EXPECT_EQ(container->children[1].get(), second);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal,
              secondOccurrence->stableOccurrenceOrdinal);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal,
              firstOccurrence->stableOccurrenceOrdinal);

    pm.closeProject();
}

TEST(SelectionSectionWidget, DroppedChildrenPreserveOwnershipAndPlatformScopedUndo)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SelectionDropProject");
    config.location = tempDir.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    ASSERT_TRUE(pm.createProject(config));
    const auto cleanup = qScopeGuard([&] { pm.closeProject(); });
    ProjectActionScope actions(pm);
    auto* container = pm.createItem(nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    auto* first = pm.createItem(container, QStringLiteral("First"), ProjectItem::Type::Sound);
    auto* second = pm.createItem(container, QStringLiteral("Second"), ProjectItem::Type::Sound);
    auto* outsider = pm.createItem(nullptr, QStringLiteral("Outsider"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(outsider, nullptr);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(*container, first->guid));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(*container, second->guid));
    auto* selection = container->comp<VNX::VSelection>();
    const auto before = selection->selections;
    const auto behavior = selection->behavior;
    const auto* outsiderParent = outsider->parent;
    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    const auto restoreContext = qScopeGuard([&] { context.setAll(true); });
    VNX::UndoRedoManager::instance().clear();
    VNX::InspectorPanel panel;
    panel.resize(480, 720);
    panel.showItemInspector(container);
    panel.show();
    QCoreApplication::processEvents();
    auto* list = panel.findChild<QWidget*>(QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    auto* table = list->findChild<VNX::TableWidget*>();
    ASSERT_NE(table, nullptr);
    auto* footer = table->findChild<QWidget*>(QStringLiteral("listFoot"));
    ASSERT_NE(footer, nullptr);
    const auto drop = [&](QWidget* target, const QList<uint32_t>& ids) {
        QMimeData mime;
        VNX::ProjectItemMime::writeObjectIDs(&mime, ids);
        QDragEnterEvent enter(target->rect().center(), Qt::CopyAction | Qt::MoveAction,
                              &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        if (!enter.isAccepted()) return false;
        QDropEvent event(target->rect().center(), Qt::CopyAction | Qt::MoveAction,
                         &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &event);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (event.isAccepted()) EXPECT_EQ(event.dropAction(), Qt::CopyAction);
        return event.isAccepted();
    };
    EXPECT_FALSE(drop(footer, {first->objectID, outsider->objectID}));
    EXPECT_EQ(selection->selections, before);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
    ASSERT_TRUE(drop(footer, {first->objectID, second->objectID}));
    const auto after = selection->selections;
    const auto rows = after.table(HarkenTest::PlatformIds::Android).occurrenceRows();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].childGuid, first->guid);
    EXPECT_EQ(rows[1].childGuid, second->guid);
    EXPECT_NE(rows[0].stableOccurrenceOrdinal, rows[1].stableOccurrenceOrdinal);
    EXPECT_EQ(after.shared, before.shared);
    EXPECT_EQ(after.table(HarkenTest::PlatformIds::Windows), before.table(HarkenTest::PlatformIds::Windows));
    EXPECT_EQ(selection->behavior, behavior);
    EXPECT_EQ(first->parent, container);
    EXPECT_EQ(second->parent, container);
    EXPECT_EQ(outsider->parent, outsiderParent);
    EXPECT_EQ(container->children.size(), 2u);
    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    QCoreApplication::processEvents();
    EXPECT_EQ(selection->selections, before);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    EXPECT_EQ(selection->selections, after);
    auto* row = selectionChildRow(list, rows[0].stableOccurrenceOrdinal);
    ASSERT_NE(row, nullptr);
    auto* reference = row->findChild<VNX::ObjectRefWidget*>();
    ASSERT_NE(reference, nullptr);
    ASSERT_TRUE(reference->isVisible());
    ASSERT_TRUE(reference->canAcceptObject(second->objectID));
    EXPECT_FALSE(drop(reference, {first->objectID, second->objectID}));
    EXPECT_EQ(selection->selections, after);
    ASSERT_TRUE(drop(reference, {second->objectID}));
    const auto replaced = selection->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows();
    ASSERT_EQ(replaced.size(), rows.size());
    EXPECT_EQ(replaced[0].childGuid, second->guid);
    EXPECT_EQ(replaced[0].stableOccurrenceOrdinal, rows[0].stableOccurrenceOrdinal);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    QCoreApplication::processEvents();
    EXPECT_EQ(selection->selections, after);
    ASSERT_TRUE(drop(footer, {first->objectID}));
    EXPECT_EQ(selection->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().size(), 3u);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    QCoreApplication::processEvents();
    EXPECT_EQ(selection->selections, after);
}

TEST(SelectionSectionWidget, ChildrenFooterAddsAndUndoes)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SelectionChildrenFooterProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("FooterLayer"), ProjectItem::Type::Layer);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("FooterContainer"), ProjectItem::Type::Container);
    ProjectItem* child = pm.createItem(
        container, QStringLiteral("FooterCandidate"), ProjectItem::Type::Sound);
    ProjectItem* remaining = pm.createItem(
        container, QStringLiteral("FooterRemaining"), ProjectItem::Type::Sound);
    ProjectItem* outsider = pm.createItem(
        layer, QStringLiteral("FooterOutsider"), ProjectItem::Type::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(remaining, nullptr);
    ASSERT_NE(outsider, nullptr);
    ASSERT_TRUE(
        VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
            *container, child->guid)
            .has_value());
    ASSERT_TRUE(
        VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
            *container, remaining->guid)
            .has_value());
    EXPECT_TRUE(container->children.size() == 2u);
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_EQ(child->parent, container);
    EXPECT_EQ(remaining->parent, container);
    VNX::UndoRedoManager::instance().clear();

    VNX::InspectorPanel panel;
    panel.resize(480, 720);
    panel.showItemInspector(container);
    panel.show();
    QCoreApplication::processEvents();

    auto* list = panel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    auto* add = list->findChild<QToolButton*>(
        QStringLiteral("listAdd_selectionChildren"));
    auto* remove = list->findChild<QToolButton*>(
        QStringLiteral("listRemoveSelected_selectionChildren"));
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);
    const auto addImage = add->icon().pixmap(add->iconSize(), 1.0).toImage();
    const auto removeImage = remove->icon().pixmap(remove->iconSize(), 1.0, QIcon::Disabled).toImage();
    ASSERT_EQ(addImage.size(), QSize(20, 20));
    ASSERT_EQ(removeImage.size(), QSize(20, 20));
    EXPECT_EQ(add->size(), remove->size());
    EXPECT_EQ(add->iconSize(), QSize(20, 20));
    EXPECT_EQ(remove->iconSize(), QSize(20, 20));
    EXPECT_EQ(add->geometry().center().y(), remove->geometry().center().y());
    EXPECT_EQ(addImage.pixelColor(10, 6).alpha(), 255);
    EXPECT_EQ(addImage.pixelColor(9, 6).alpha(), 0);
    EXPECT_EQ(addImage.pixelColor(11, 6).alpha(), 0);
    EXPECT_EQ(addImage.pixelColor(6, 10).alpha(), 255);
    EXPECT_EQ(removeImage.pixelColor(6, 10).alpha(), 255);
    EXPECT_EQ(removeImage.pixelColor(6, 9).alpha(), 0);
    EXPECT_EQ(removeImage.pixelColor(6, 11).alpha(), 0);
    EXPECT_TRUE(list->findChildren<QWidget*>(
                    QStringLiteral("selectionChildRow")).isEmpty());
    EXPECT_FALSE(remove->isEnabled());

    add->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(chooseOpenSelectorItem(outsider->name));
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    VNX::SelectorWindow::dismiss();
    QCoreApplication::processEvents();

    add->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(chooseOpenSelectorItem(child->name));

    EXPECT_EQ(container->children.size(), 2u);
    EXPECT_EQ(child->parent, container);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().childGuid,
              child->guid);
    ASSERT_NE(selectionChildRow(list, child->guid), nullptr);
    EXPECT_EQ(list->findChildren<QWidget*>(
                  QStringLiteral("selectionChildRow")).size(), 1);
    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children.size(), 2u);
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_TRUE(list->findChildren<QWidget*>(
                    QStringLiteral("selectionChildRow")).isEmpty());

    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children.size(), 2u);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().childGuid,
              child->guid);
    EXPECT_EQ(child->parent, container);
    ASSERT_EQ(container->parent, layer);
    EXPECT_EQ(outsider->parent, layer);
    EXPECT_EQ(layer->children.size(), 2u);
    QWidget* row = selectionChildRow(list, child->guid);
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->property("listRow").toBool());
    auto* reference = row->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("selectionChildObject"));
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->currentObjectID(), child->objectID);

    auto* header = list->findChild<QWidget*>(QStringLiteral("listHead"));
    auto* weightHeader = list->findChild<QLabel*>(
        QStringLiteral("selectionChildWeightHeader"));
    ASSERT_NE(header, nullptr);
    ASSERT_NE(weightHeader, nullptr);
    ASSERT_FALSE(weightHeader->isHidden());
    const int initialWeightWidth = weightHeader->width();
    const QPointF resizeStart(
        weightHeader->geometry().left(), header->rect().center().y());
    const QPointF resizeGlobal = header->mapToGlobal(resizeStart.toPoint());
    QMouseEvent resizePress(QEvent::MouseButtonPress, resizeStart, resizeGlobal,
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &resizePress);
    const QPointF resizeEnd = resizeStart - QPointF(24.0, 0.0);
    const QPointF resizeEndGlobal = header->mapToGlobal(resizeEnd.toPoint());
    QMouseEvent resizeMove(QEvent::MouseMove, resizeEnd, resizeEndGlobal,
                           Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &resizeMove);
    QMouseEvent resizeRelease(QEvent::MouseButtonRelease, resizeEnd,
                              resizeEndGlobal, Qt::LeftButton, Qt::NoButton,
                              Qt::NoModifier);
    QApplication::sendEvent(header, &resizeRelease);
    QCoreApplication::processEvents();
    EXPECT_GT(weightHeader->width(), initialWeightWidth);

    QWidget* handle = selectionChildDragHandle(row);
    ASSERT_NE(handle, nullptr);
    auto* rowsHost = list->findChild<QWidget*>(
        QStringLiteral("selectionChildrenRows"));
    ASSERT_NE(rowsHost, nullptr);
    ASSERT_TRUE(rowsHost->isAncestorOf(handle));
    const QPointF pressLocal = handle->rect().center();
    const QPointF pressGlobal = handle->mapToGlobal(pressLocal.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressGlobal,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(handle, &press);
    ASSERT_TRUE(row->property("selected").toBool());
    ASSERT_TRUE(remove->isEnabled());
    const QPointF releaseGlobal = row->mapToGlobal(row->rect().center());
    const QPointF releaseLocal = list->mapFromGlobal(releaseGlobal.toPoint());
    QMouseEvent release(QEvent::MouseButtonRelease, releaseLocal, releaseGlobal,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(list, &release);
    QCoreApplication::processEvents();
    ASSERT_TRUE(remove->isEnabled());
    remove->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children.size(), 2u);
    EXPECT_EQ(child->parent, container);
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_EQ(selectionChildRow(list, child->guid), nullptr);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children.size(), 2u);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().childGuid,
              child->guid);
    EXPECT_EQ(child->parent, container);
    EXPECT_NE(selectionChildRow(list, child->guid), nullptr);

    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    EXPECT_EQ(container->children.size(), 2u);
    EXPECT_EQ(child->parent, container);
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_EQ(selectionChildRow(list, child->guid), nullptr);

    pm.closeProject();
}

TEST(SelectionSectionWidget,
     DuplicateOccurrencesReplaceExactlyAndPreserveIdentityOrderAndWeightsThroughUndo)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("DuplicateOccurrenceInspectorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("DuplicateLayer"), ProjectItem::Type::Layer);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("DuplicateContainer"), ProjectItem::Type::Container);
    ProjectItem* shared = pm.createItem(
        container, QStringLiteral("SharedSound"), ProjectItem::Type::Sound);
    ProjectItem* replacement = pm.createItem(
        container, QStringLiteral("ReplacementSound"), ProjectItem::Type::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(shared, nullptr);
    ASSERT_NE(replacement, nullptr);
    ASSERT_TRUE(
        VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
            *container, shared->guid)
            .has_value());
    ASSERT_TRUE(
        VNX::PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
            *container, replacement->guid)
            .has_value());
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    VNX::UndoRedoManager::instance().clear();
    EXPECT_EQ(shared->parent, container);
    EXPECT_EQ(replacement->parent, container);

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    panel.show();
    QCoreApplication::processEvents();
    auto* list = panel.findChild<QWidget*>(
        QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    auto* add = list->findChild<QToolButton*>(
        QStringLiteral("listAdd_selectionChildren"));
    ASSERT_NE(add, nullptr);

    add->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(chooseOpenSelectorItem(shared->name));
    add->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(chooseOpenSelectorItem(shared->name));

    ASSERT_EQ(container->children.size(), 2u);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    const uint32_t replacedOrdinal =
        container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].stableOccurrenceOrdinal;
    const uint32_t siblingOrdinal =
        container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal;
    EXPECT_NE(replacedOrdinal, siblingOrdinal);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].childGuid, shared->guid);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].childGuid, shared->guid);
    ASSERT_NE(selectionChildRow(list, replacedOrdinal), nullptr);
    ASSERT_NE(selectionChildRow(list, siblingOrdinal), nullptr);

    auto replacedLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, replacedOrdinal);
    auto siblingLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, siblingOrdinal);
    ASSERT_TRUE(replacedLocator.has_value());
    ASSERT_TRUE(siblingLocator.has_value());
    auto* replacedWeight = selectionChildRow(list, replacedOrdinal)->findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorWeight"));
    auto* siblingWeight = selectionChildRow(list, siblingOrdinal)->findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorWeight"));
    ASSERT_NE(replacedWeight, nullptr);
    ASSERT_NE(siblingWeight, nullptr);
    replacedWeight->setValue(3);
    siblingWeight->setValue(7);
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto originalBehavior = container->comp<VNX::VSelection>()->behavior;
    auto replacedRows = originalRows;
    auto replacements = replacedRows.occurrenceRows();
    replacements[0].childGuid = replacement->guid;
    ASSERT_TRUE(replacedRows.setOccurrenceRows(replacements));
    VNX::UndoRedoManager::instance().clear();

    QWidget* replacedRow = selectionChildRow(list, replacedOrdinal);
    ASSERT_NE(replacedRow, nullptr);
    auto* reference = replacedRow->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("selectionChildObject"));
    ASSERT_NE(reference, nullptr);
    auto* pick = reference->findChild<QToolButton*>(QStringLiteral("refPick"));
    ASSERT_NE(pick, nullptr);
    EXPECT_TRUE(pick->isVisible());
    pick->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(chooseOpenSelectorItem(shared->name));
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    pick->click();
    QCoreApplication::processEvents();
    ASSERT_TRUE(chooseOpenSelectorItem(replacement->name));

    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    replacedLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, replacedOrdinal);
    siblingLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, siblingOrdinal);
    ASSERT_TRUE(replacedLocator.has_value());
    ASSERT_TRUE(siblingLocator.has_value());
    EXPECT_EQ(replacedLocator->occurrenceGuid, replacement->guid);
    EXPECT_EQ(replacedLocator->stableOccurrenceOrdinal, replacedOrdinal);
    EXPECT_EQ(siblingLocator->occurrenceGuid, shared->guid);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, replacedRows);
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior, originalBehavior);
    replacedRow = selectionChildRow(list, replacedOrdinal);
    ASSERT_NE(replacedRow, nullptr);
    reference = replacedRow->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("selectionChildObject"));
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->currentObjectID(), replacement->objectID);

    VNX::UndoRedoManager::instance().undoStack()->undo();
    QCoreApplication::processEvents();
    replacedLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, replacedOrdinal);
    siblingLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, siblingOrdinal);
    ASSERT_TRUE(replacedLocator.has_value());
    ASSERT_TRUE(siblingLocator.has_value());
    EXPECT_EQ(replacedLocator->occurrenceGuid, shared->guid);
    EXPECT_EQ(siblingLocator->occurrenceGuid, shared->guid);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior, originalBehavior);

    VNX::UndoRedoManager::instance().undoStack()->redo();
    QCoreApplication::processEvents();
    replacedLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, replacedOrdinal);
    siblingLocator = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, siblingOrdinal);
    ASSERT_TRUE(replacedLocator.has_value());
    ASSERT_TRUE(siblingLocator.has_value());
    EXPECT_EQ(replacedLocator->occurrenceGuid, replacement->guid);
    EXPECT_EQ(siblingLocator->occurrenceGuid, shared->guid);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, replacedRows);
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior, originalBehavior);
    EXPECT_EQ(shared->parent, container);
    EXPECT_EQ(replacement->parent, container);
    EXPECT_EQ(container->children.size(), 2u);

    pm.closeProject();
}

TEST(SelectionSectionWidget, RandomWeightEditKeepsFocusedEditorAlive)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("RandomWeightProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("RandomLayer"), ProjectItem::Type::Layer);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("RandomContainer"), ProjectItem::Type::Container);
    ProjectItem* child = pm.createItem(
        container, QStringLiteral("RandomChild"), ProjectItem::Type::Sound);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(child, nullptr);
    const auto childOccurrence =
        VNX::PlayablePropertyAuthoringV1::containerChildLocator(
            *container, child->guid);
    ASSERT_TRUE(childOccurrence.has_value());

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    auto* weight = panel.findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorWeight"));
    ASSERT_NE(weight, nullptr);
    QPointer<QSpinBox> weightGuard(weight);
    weight->setValue(1);
    QCoreApplication::processEvents();

    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.find(
                  childOccurrence->stableOccurrenceOrdinal)->weight,
              1);
    EXPECT_FALSE(weightGuard.isNull());
    EXPECT_EQ(panel.findChild<QSpinBox*>(
                  QStringLiteral("selectionBehaviorWeight")),
              weight);
    pm.closeProject();
}

TEST(SelectionSectionWidget, TypedBehaviorNumbersCommitOnceOnAccept)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("TypedBehaviorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("TypedLayer"), ProjectItem::Type::Layer);
    ProjectItem* container = pm.createItem(
        layer, QStringLiteral("TypedContainer"), ProjectItem::Type::Container);
    ProjectItem* child = pm.createItem(
        container, QStringLiteral("TypedChild"), ProjectItem::Type::Sound);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(pm.createItem(container, QStringLiteral("TypedSibling"),
                            ProjectItem::Type::Sound), nullptr);
    ASSERT_NE(pm.createItem(container, QStringLiteral("TypedThird"),
                            ProjectItem::Type::Sound), nullptr);
    const auto childOccurrence =
        VNX::PlayablePropertyAuthoringV1::containerChildLocator(
            *container, child->guid);
    ASSERT_TRUE(childOccurrence.has_value());

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    panel.show();

    auto* weight = panel.findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorWeight"));
    ASSERT_NE(weight, nullptr);
    ASSERT_FALSE(weight->keyboardTracking());
    weight->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    weight->selectAll();

    auto undoDepth = [] {
        return VNX::UndoRedoManager::instance().undoStack()->count();
    };
    const int beforeWeight = undoDepth();
    typeDigits(weight, QStringLiteral("42"));
    EXPECT_EQ(undoDepth(), beforeWeight);
    acceptEdit(weight);
    QCoreApplication::processEvents();
    EXPECT_EQ(undoDepth(), beforeWeight + 1);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.find(
                  childOccurrence->stableOccurrenceOrdinal)->weight,
              42);

    auto* avoidRepeat = panel.findChild<QSpinBox*>(
        QStringLiteral("selectionBehaviorAvoidRepeat"));
    ASSERT_NE(avoidRepeat, nullptr);
    ASSERT_FALSE(avoidRepeat->keyboardTracking());
    avoidRepeat->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    avoidRepeat->selectAll();

    const int beforeAvoid = undoDepth();
    typeDigits(avoidRepeat, QStringLiteral("1"));
    EXPECT_EQ(undoDepth(), beforeAvoid);
    acceptEdit(avoidRepeat);
    QCoreApplication::processEvents();
    EXPECT_EQ(undoDepth(), beforeAvoid + 1);
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior.avoidRepeat, 1u);

    pm.closeProject();
}

TEST(SelectionSectionWidget, PlatformSwitchUsesIndependentRowsAndKeepsAuthoredBehavior)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlatformSelectionInspector");
    config.location = temp.path();
    config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope actions(pm);
    auto* container = pm.createItem(nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    for (int i = 0; i < 10; ++i)
        ASSERT_NE(pm.createItem(container, QStringLiteral("Sound%1").arg(i), ProjectItem::Type::Sound), nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    selection->behavior.avoidRepeat = 9;
    auto& android = selection->selections.editTable(HarkenTest::PlatformIds::Android);
    auto androidRows = android.occurrenceRows();
    androidRows.erase(androidRows.begin(), androidRows.end() - 1);
    androidRows.front().weight = 23;
    ASSERT_TRUE(android.setOccurrenceRows(androidRows));
    auto* variable = pm.createItem(nullptr, QStringLiteral("BlendVariable"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    VNX::MixingChildDef sharedCurve{container->children.back()->guid, {{0.0f, 0.3f}, {100.0f, 0.3f}}, {}};
    VNX::MixingChildDef androidCurve{container->children.back()->guid, {{0.0f, 0.7f}, {100.0f, 0.7f}}, {}};
    sharedCurve.normalize();
    androidCurve.normalize();
    ASSERT_TRUE(selection->selections.shared.setMixing(VNX::MixingDef{{{variable->guid, {sharedCurve}}}}));
    ASSERT_TRUE(android.setMixing(VNX::MixingDef{{{variable->guid, {androidCurve}}}}));
    const auto before = selection->selections;
    auto& context = VNX::PlatformContextService::instance();
    context.setSelected({HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    auto weights = panel.findChildren<QSpinBox*>(QStringLiteral("selectionBehaviorWeight"));
    ASSERT_EQ(weights.size(), 1);
    EXPECT_EQ(weights.front()->value(), 23);
    auto* avoid = panel.findChild<QSpinBox*>(QStringLiteral("selectionBehaviorAvoidRepeat"));
    ASSERT_NE(avoid, nullptr);
    EXPECT_EQ(avoid->maximum(), 0);
    auto* memory = panel.findChild<QComboBox*>(QStringLiteral("selectionBehaviorMemoryScope"));
    ASSERT_NE(memory, nullptr);
    memory->setCurrentIndex(memory->findData(static_cast<int>(VNX::SelectionMemoryScope::Global)));
    QCoreApplication::processEvents();
    EXPECT_EQ(selection->behavior.avoidRepeat, 9);
    weights = panel.findChildren<QSpinBox*>(QStringLiteral("selectionBehaviorWeight"));
    weights.front()->setValue(71);
    QCoreApplication::processEvents();
    EXPECT_EQ(selection->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().front().weight, 71);
    EXPECT_EQ(selection->selections.shared, before.shared);
    context.setSelected({HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    QCoreApplication::processEvents();
    weights = panel.findChildren<QSpinBox*>(QStringLiteral("selectionBehaviorWeight"));
    ASSERT_EQ(weights.size(), 10);
    for (const auto* weight : weights) EXPECT_EQ(weight->value(), 50);
    avoid = panel.findChild<QSpinBox*>(QStringLiteral("selectionBehaviorAvoidRepeat"));
    ASSERT_NE(avoid, nullptr);
    EXPECT_EQ(avoid->maximum(), 8);
    EXPECT_EQ(avoid->value(), 8);
    EXPECT_EQ(selection->behavior.avoidRepeat, 9);
    context.setAll(true);
    QCoreApplication::processEvents();
    weights = panel.findChildren<QSpinBox*>(QStringLiteral("selectionBehaviorWeight"));
    weights.front()->setValue(31);
    QCoreApplication::processEvents();
    EXPECT_FALSE(selection->selections.platforms.isEmpty());
    EXPECT_EQ(selection->selections.shared.occurrenceRows().front().weight, 31);
    const auto sharedRows = selection->selections.shared.occurrenceRows();
    const auto platformRows = selection->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows();
    ASSERT_EQ(platformRows.size(), sharedRows.size());
    for (size_t index = 0; index < sharedRows.size(); ++index) {
        EXPECT_EQ(platformRows[index].childGuid, sharedRows[index].childGuid);
        EXPECT_EQ(platformRows[index].weight, sharedRows[index].weight);
    }
    EXPECT_EQ(platformRows.back().stableOccurrenceOrdinal, androidRows.front().stableOccurrenceOrdinal);
    EXPECT_GE(platformRows.front().stableOccurrenceOrdinal, before.table(HarkenTest::PlatformIds::Android).occurrenceOrdinalHighWater);
    EXPECT_EQ(selection->mixing().toJson(), before.shared.mixing().toJson());
    EXPECT_EQ(selection->mixing(HarkenTest::PlatformIds::Android).toJson(),
              before.table(HarkenTest::PlatformIds::Android).mixing().toJson());
    pm.closeProject();
}

TEST(PlayablePropertyInspector, GroupHeadersReplaceFieldSourcesAndPreserveLocalMixing)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("LocalValueIndicatorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);
    VNX::PlatformContextService::instance().setAll(true);
    auto* parent = pm.createItem(
        nullptr, QStringLiteral("Parent"), ProjectItem::Type::Container);
    auto* child = pm.createItem(
        parent, QStringLiteral("Child"), ProjectItem::Type::Sound);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    const auto parentLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*parent);
    const auto childLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*child);
    ASSERT_TRUE(parentLocator && childLocator);
    for (const uint8_t id : {1, 2, 4}) {
        const auto* field = VNX::PlayablePropertyAuthoringV1::fieldById(id);
        ASSERT_NE(field, nullptr);
        SCOPED_TRACE(field->fieldName);
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
            *parent, *parentLocator, id, QJsonValue(2), std::nullopt));
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
            *child, *childLocator, id, QJsonValue(3), std::nullopt));
    }
    const auto parentPresence = parent->playablePropertyPresence.bits;
    const auto childPresence = child->playablePropertyPresence.bits;
    VNX::InspectorPanel panel;
    panel.showItemInspector(child);
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields())
        EXPECT_EQ(panel.findChild<QLabel*>(
            QStringLiteral("source_") + QLatin1String(field.fieldName)), nullptr);
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        auto* inherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_") + QLatin1String(group));
        ASSERT_NE(inherit, nullptr);
        EXPECT_TRUE(inherit->isChecked());
        EXPECT_TRUE(inherit->isEnabled());
    }
    auto* sendInherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_send"));
    sendInherit->click();
    EXPECT_EQ(child->fieldGroups.send.shared.mode, VNX::FieldGroupMode::Local);
    ASSERT_TRUE(child->fieldGroups.send.shared.localHistory);
    EXPECT_TRUE(child->fieldGroups.send.shared.localHistory->sends.empty());
    sendInherit->click();
    EXPECT_EQ(child->fieldGroups.send.shared.mode, VNX::FieldGroupMode::Inherit);
    ASSERT_TRUE(child->fieldGroups.send.shared.localHistory);
    EXPECT_TRUE(child->fieldGroups.send.shared.localHistory->sends.empty());
    EXPECT_EQ(parent->playablePropertyPresence.bits, parentPresence);
    EXPECT_EQ(child->playablePropertyPresence.bits, childPresence);
    for (const uint8_t id : {1, 2, 4}) {
        const auto parentValue = VNX::PlayablePropertyAuthoringV1::readProperty(
            *parent, *parentLocator, id, std::nullopt);
        const auto childValue = VNX::PlayablePropertyAuthoringV1::readProperty(
            *child, *childLocator, id, std::nullopt);
        ASSERT_TRUE(parentValue && childValue);
        EXPECT_TRUE(parentValue->present);
        EXPECT_TRUE(childValue->present);
        EXPECT_EQ(parentValue->value, QJsonValue(2));
        EXPECT_EQ(childValue->value, QJsonValue(3));
    }
    pm.closeProject();
}

TEST(PlayablePropertyInspector, ClipInspectionEditsSharedDefinitionPlaybackAndIgnoresOccurrenceValues)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("PlayablePropertyInspectorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);
    VNX::PlatformContextService::instance().setAll(true);

    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("SourceLayer"), ProjectItem::Type::Layer);
    ProjectItem* sound = pm.createItem(
        layer, QStringLiteral("SourceSound"), ProjectItem::Type::Sound);
    ProjectItem* piece = pm.createItem(
        nullptr, QStringLiteral("SourcePiece"), ProjectItem::Type::Piece);
    ProjectItem* track = pm.createItem(
        piece, QStringLiteral("SourceTrack"), ProjectItem::Type::Track);
    ASSERT_NE(layer, nullptr);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(piece, nullptr);
    ASSERT_NE(track, nullptr);

    const auto soundLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(
        *sound);
    ASSERT_TRUE(soundLocator.has_value());
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.duration = 48000;
    clip.playableProperties.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    ASSERT_TRUE(clip.playableProperties.setValue(harkenplayableproperties::PlayablePropertyIdV1::Volume, -6.0f));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto clipLocator = VNX::PlayablePropertyAuthoringV1::trackClipLocator(
        *track, clip.clipGuid);
    ASSERT_TRUE(clipLocator.has_value());
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *track, *clipLocator, 1, -6.0, std::nullopt));
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *track, *clipLocator, 9, 0.75, std::nullopt));
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sound, *soundLocator, 9, 0.75, std::nullopt));
    const auto groupArgs = [](ProjectItem* target, const QString& group) {
        return QJsonObject{{"definition_guid", target->guid.toString(QUuid::WithoutBraces)},
                           {"group", group}, {"scope", "Shared"}};
    };
    for (ProjectItem* target : {layer, track}) {
        auto args = groupArgs(target, QStringLiteral("spatialRating"));
        if (target == track) {
            args.insert(QStringLiteral("mode"), QStringLiteral("Local"));
            ASSERT_TRUE(VNX::ActionManager::instance().execute("set_field_group_mode", args).success);
            args.remove(QStringLiteral("mode"));
        }
        args.insert(QStringLiteral("field"), QStringLiteral("spatial.panning"));
        args.insert(QStringLiteral("value"), target == layer ? 0.25 : 0.75);
        ASSERT_TRUE(VNX::ActionManager::instance().execute("set_field_group_field", args).success);
    }
    auto outputArgs = groupArgs(layer, QStringLiteral("mainOutput"));
    outputArgs.insert(QStringLiteral("field"), QStringLiteral("outputBusVolume"));
    outputArgs.insert(QStringLiteral("value"), -8.0);
    ASSERT_TRUE(VNX::ActionManager::instance().execute("set_field_group_field", outputArgs).success);

    VNX::InspectorPanel panel;
    panel.showItemInspector(
        sound, VNX::PlacementContext::fromOccurrence(
            {track->guid, clip.clipGuid, sound->guid}));
    VNX::InspectorPanel linkedInspector;
    linkedInspector.showItemInspector(sound);
    auto* linkedVolume = linkedInspector.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(linkedVolume, nullptr);
    auto* panning = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_spatialPanning"));
    auto* inherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_spatialRating"));
    auto* volume = panel.findChild<VNX::ParameterSlider*>(QStringLiteral("prop_volume"));
    ASSERT_NE(panning, nullptr);
    ASSERT_NE(inherit, nullptr);
    ASSERT_NE(volume, nullptr);
    EXPECT_TRUE(inherit->isChecked());
    EXPECT_DOUBLE_EQ(panning->value(), 0.25);
    EXPECT_FALSE(panning->isEnabled());
    EXPECT_DOUBLE_EQ(volume->value(), 0.0);
    inherit->click();
    panning->binding()->beginEdit();
    panning->binding()->previewEdit(-0.6);
    panning->binding()->commitEdit();
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_FLOAT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialPanning, -0.6f);
    EXPECT_FLOAT_EQ(track->fieldGroups.spatialRating.shared.localHistory->spatialPanning, 0.75f);
    volume->binding()->beginEdit();
    volume->binding()->previewEdit(-9.0);
    volume->binding()->commitEdit();
    const auto clipVolume = VNX::PlayablePropertyAuthoringV1::readProperty(
        *track, *clipLocator, 1, std::nullopt);
    ASSERT_TRUE(clipVolume);
    EXPECT_FALSE(clipVolume->present);
    EXPECT_EQ(clipVolume->value, QJsonValue(0.0));
    const auto definitionVolume = VNX::PlayablePropertyAuthoringV1::readProperty(
        *sound, *soundLocator, 1, std::nullopt);
    ASSERT_TRUE(definitionVolume);
    EXPECT_TRUE(definitionVolume->present);
    EXPECT_EQ(definitionVolume->value, QJsonValue(-9.0));
    EXPECT_DOUBLE_EQ(linkedVolume->value(), -9.0);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_DOUBLE_EQ(volume->value(), 0.0);
    EXPECT_DOUBLE_EQ(linkedVolume->value(), 0.0);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_DOUBLE_EQ(volume->value(), -9.0);
    EXPECT_DOUBLE_EQ(linkedVolume->value(), -9.0);
    inherit->click();
    EXPECT_DOUBLE_EQ(panning->value(), 0.25);
    EXPECT_FALSE(panning->isEnabled());

    auto* outputVolume = panel.findChild<VNX::ParameterSlider*>(
        QStringLiteral("prop_outputBusVolume"));
    auto* outputInherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_mainOutput"));
    ASSERT_NE(outputVolume, nullptr);
    ASSERT_NE(outputInherit, nullptr);
    EXPECT_DOUBLE_EQ(-8.0, outputVolume->value());
    outputInherit->click();
    outputVolume->binding()->beginEdit();
    outputVolume->binding()->previewEdit(-12.0);
    outputVolume->binding()->commitEdit();
    ASSERT_TRUE(sound->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_FLOAT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -12.0f);
    outputInherit->click();
    EXPECT_DOUBLE_EQ(-8.0, outputVolume->value());
    EXPECT_EQ(timeline->clips.items.front().playableProperties.presence.bits & VNX::kFieldGroupPropertyMask, 0u);

    pm.closeProject();
}

namespace {

QToolButton* segmentAt(VNX::ValueSegmentedControl* control, const char* position)
{
    for (auto* button : control->findChildren<QToolButton*>())
        if (button->property("segmentPosition").toString() == QLatin1String(position))
            return button;
    return nullptr;
}

std::vector<VNX::SegmentSpec> loopSegmentSpecs()
{
    return {
        { .value = 1, .numeric = true, .numericMin = 1, .numericMax = 999 },
        { .text = QStringLiteral("Infinite"), .value = 0 },
    };
}

}

TEST(PlayablePropertyInspector, LoopRowHasEditableCountAndInfiniteAndTracksValue)
{
    ensureApp();
    VNX::ValueSegmentedControl::clearSessionMemory();
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());

    auto* loop = page->findChild<VNX::ValueSegmentedControl*>(
        QStringLiteral("prop_loopCount"));
    ASSERT_NE(loop, nullptr);
    EXPECT_EQ(page->findChild<QSpinBox*>(QStringLiteral("prop_loopCount")), nullptr);

    auto* finite = segmentAt(loop, "first");
    auto* infinity = segmentAt(loop, "last");
    ASSERT_NE(finite, nullptr);
    ASSERT_NE(infinity, nullptr);
    EXPECT_EQ(loop->findChildren<QToolButton*>().size(), 2);

    ProjectItem sound(QStringLiteral("LoopStates"), ProjectItem::Type::Sound);
    sound.guid = QUuid::createUuid();
    sound.attachDefaultComponents();
    sound.comp<VNX::VAudio>()->loopCount = 1;
    engine.bindItem(&sound);
    EXPECT_TRUE(finite->isChecked());
    EXPECT_FALSE(infinity->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("1"));

    sound.comp<VNX::VAudio>()->loopCount = 7;
    engine.refreshField(QStringLiteral("loopCount"));
    EXPECT_TRUE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("7"));

    sound.comp<VNX::VAudio>()->loopCount = 0;
    engine.refreshField(QStringLiteral("loopCount"));
    EXPECT_TRUE(infinity->isChecked());
    EXPECT_FALSE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("7"));

    sound.comp<VNX::VAudio>()->loopCount = std::numeric_limits<int>::max();
    engine.refreshField(QStringLiteral("loopCount"));
    EXPECT_TRUE(finite->isChecked());
}

TEST(PlayablePropertyInspector, LoopFiniteSegmentClampsInsteadOfChangingSegment)
{
    ensureApp();
    VNX::ValueSegmentedControl::clearSessionMemory();
    VNX::ValueSegmentedControl control;
    control.setSegments(loopSegmentSpecs());
    control.setContext(QStringLiteral("clamp/loopCount"));
    control.setValue(QVariant(5));

    auto* finite = segmentAt(&control, "first");
    ASSERT_NE(finite, nullptr);
    auto* editor = control.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);

    QList<QVariant> previews;
    QObject::connect(&control, &VNX::ValueSegmentedControl::numericPreviewed,
                     [&previews, &control](const QVariant& value) {
        previews.push_back(value);
        control.setValue(value);
    });

    for (const auto& [typed, expected] : std::vector<std::pair<QString, qlonglong>>{
             {QStringLiteral("1"), 1}, {QStringLiteral("0"), 1},
             {QStringLiteral("-5"), 1}, {QString(), 1},
             {QStringLiteral("4"), 4}, {QStringLiteral("100000"), 999}})
    {
        QMouseEvent open(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(finite, &open);
        ASSERT_TRUE(control.isEditing()) << typed.toStdString();

        previews.clear();
        editor->setText(QString());
        if (!typed.isEmpty())
        {
            editor->insert(typed);
            ASSERT_FALSE(previews.isEmpty()) << typed.toStdString();
            EXPECT_EQ(previews.last().toLongLong(), expected) << typed.toStdString();
        }

        QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(editor, &commit);
        EXPECT_FALSE(control.isEditing());
        // An out-of-range count never rehomes the row to infinity: the
        // user stays on the segment they were editing.
        EXPECT_TRUE(finite->isChecked()) << typed.toStdString();
        EXPECT_FALSE(segmentAt(&control, "last")->isChecked());
        EXPECT_EQ(finite->text(), QString::number(expected)) << typed.toStdString();
    }
}

TEST(PlayablePropertyInspector, LoopSegmentClickRestoresRememberedCount)
{
    ensureApp();
    VNX::ValueSegmentedControl::clearSessionMemory();
    VNX::ValueSegmentedControl control;
    control.setSegments(loopSegmentSpecs());
    control.setContext(QStringLiteral("click/loopCount"));

    QList<QVariant> picked;
    QObject::connect(&control, &VNX::ValueSegmentedControl::segmentPicked,
                     [&picked](const QVariant& value) { picked.push_back(value); });

    auto* finite = segmentAt(&control, "first");
    auto* infinity = segmentAt(&control, "last");
    ASSERT_NE(finite, nullptr);
    ASSERT_NE(infinity, nullptr);

    // With nothing authored yet the finite segment offers the schema fallback.
    control.setValue(QVariant(0));
    finite->click();
    ASSERT_EQ(picked.size(), 1);
    EXPECT_EQ(picked.last().toLongLong(), 1);
    control.setValue(picked.last());
    EXPECT_TRUE(finite->isChecked());
    EXPECT_FALSE(infinity->isChecked());

    picked.clear();
    finite->click();
    EXPECT_TRUE(picked.isEmpty());
    EXPECT_TRUE(finite->isChecked());
    EXPECT_FALSE(infinity->isChecked());

    // Once a count has been seen, leaving the segment and coming back offers
    // that count again rather than the schema fallback.
    control.setValue(QVariant(7));
    picked.clear();
    infinity->click();
    ASSERT_EQ(picked.size(), 1);
    EXPECT_EQ(picked.last().toLongLong(), 0);

    control.setValue(QVariant(0));
    picked.clear();
    finite->click();
    ASSERT_EQ(picked.size(), 1);
    EXPECT_EQ(picked.last().toLongLong(), 7);

    // Clicking the segment the field already sits on is not an edit.
    control.setValue(QVariant(7));
    picked.clear();
    finite->click();
    EXPECT_TRUE(picked.isEmpty());

    control.setValue(QVariant(1));
    picked.clear();
    finite->click();
    EXPECT_TRUE(picked.isEmpty());
}

TEST(PlayablePropertyInspector, LoopAbandonedEditLeavesCountAndMemoryIntact)
{
    ensureApp();
    VNX::ValueSegmentedControl::clearSessionMemory();
    VNX::ValueSegmentedControl control;
    control.setSegments(loopSegmentSpecs());
    control.setContext(QStringLiteral("escape/loopCount"));
    control.setValue(QVariant(7));

    auto* finite = segmentAt(&control, "first");
    ASSERT_NE(finite, nullptr);
    auto* editor = control.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);

    int committedCount = 0;
    int abandonedCount = 0;
    QObject::connect(&control, &VNX::ValueSegmentedControl::numericEditEnded,
                     [&](bool committed) { committed ? ++committedCount : ++abandonedCount; });

    QMouseEvent open(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(finite, &open);
    ASSERT_TRUE(control.isEditing());
    editor->setText(QString());
    editor->insert(QStringLiteral("99"));

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escape);
    EXPECT_FALSE(control.isEditing());
    EXPECT_EQ(abandonedCount, 1);
    EXPECT_EQ(committedCount, 0);
    EXPECT_EQ(finite->text(), QStringLiteral("7"));

    // The abandoned 99 previewed through the field and flowed back in, but it
    // must not outlive its own Escape as the remembered fallback.
    control.setValue(QVariant(0));
    EXPECT_TRUE(segmentAt(&control, "last")->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("7"));
}

TEST(PlayablePropertyInspector, LoopConfirmingUneditedFiniteCountFromFixedSegmentPreviewsIt)
{
    ensureApp();
    VNX::ValueSegmentedControl::clearSessionMemory();
    VNX::ValueSegmentedControl control;
    control.setSegments(loopSegmentSpecs());
    control.setContext(QStringLiteral("confirm/loopCount"));

    auto* finite = segmentAt(&control, "first");
    ASSERT_NE(finite, nullptr);
    auto* editor = control.findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);

    QList<QVariant> previews;
    int committedCount = 0;
    QObject::connect(&control, &VNX::ValueSegmentedControl::numericPreviewed,
                     [&previews](const QVariant& value) { previews.push_back(value); });
    QObject::connect(&control, &VNX::ValueSegmentedControl::numericEditEnded,
                     [&](bool committed) { if (committed) ++committedCount; });

    control.setValue(QVariant(0));
    QMouseEvent openDefault(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(finite, &openDefault);
    ASSERT_TRUE(control.isEditing());
    EXPECT_EQ(editor->text(), QStringLiteral("1"));
    EXPECT_TRUE(previews.isEmpty());

    QKeyEvent commitDefault(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &commitDefault);
    EXPECT_FALSE(control.isEditing());
    ASSERT_EQ(previews.size(), 1);
    EXPECT_EQ(previews.last().toLongLong(), 1);
    EXPECT_EQ(committedCount, 1);
    EXPECT_TRUE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("1"));

    control.setValue(QVariant(7));
    control.setValue(QVariant(0));
    previews.clear();
    committedCount = 0;
    QMouseEvent openInfinity(QEvent::MouseButtonDblClick, QPointF(1, 1), QPointF(1, 1),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(finite, &openInfinity);
    ASSERT_TRUE(control.isEditing());
    EXPECT_EQ(editor->text(), QStringLiteral("7"));
    EXPECT_TRUE(previews.isEmpty());

    QKeyEvent commitInfinity(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &commitInfinity);
    EXPECT_FALSE(control.isEditing());
    ASSERT_EQ(previews.size(), 1);
    EXPECT_EQ(previews.last().toLongLong(), 7);
    EXPECT_EQ(committedCount, 1);
    EXPECT_TRUE(finite->isChecked());
    EXPECT_EQ(finite->text(), QStringLiteral("7"));
}

TEST(PlayablePropertyInspector, NonSliderRowsShareGeneratedOverrideActionShape)
{
    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    ProjectItem sound(QStringLiteral("ActionShape"), ProjectItem::Type::Sound);
    sound.guid = QUuid::createUuid();
    sound.objectID = 42;
    sound.attachDefaultComponents();
    engine.bindItem(&sound);

    const auto loop = engine.buildSetItemPropertyArgs(
        QStringLiteral("loopCount"), QVariant::fromValue(uint32_t{3}));
    EXPECT_EQ(loop.value(QStringLiteral("override_operation")).toString(),
              QStringLiteral("set_override"));
    EXPECT_EQ(loop.value(QStringLiteral("value")).toInt(), 3);

    const auto streaming = engine.buildSetItemPropertyArgs(
        QStringLiteral("streamingMode"), QVariant::fromValue(uint32_t{1}));
    EXPECT_EQ(streaming.value(QStringLiteral("value")).toInt(), 1);

    const auto reference = engine.buildSetItemPropertyArgs(
        QStringLiteral("mainOutputBusRef"),
        QVariant::fromValue(static_cast<qulonglong>(123)));
    EXPECT_FALSE(reference.contains(QStringLiteral("value")));

    const QVariantList sends{QVariantMap{
        {QStringLiteral("targetBusID"), 7},
        {QStringLiteral("sendLevel"), -3.0}}};
    const auto list = engine.buildSetItemPropertyArgs(
        QStringLiteral("sends"), sends);
    EXPECT_TRUE(list.value(QStringLiteral("value")).isArray());
}

TEST(PlayablePropertyInspector, GroupReferencesUseGuidsAndRejectInvalidActiveTargets)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("StableReferenceInspectorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);
    VNX::PlatformContextService::instance().setAll(true);
    ProjectItem* bus = pm.createItem(
        pm.getMasterBusItem(), QStringLiteral("WideBus"), ProjectItem::Type::Bus);
    ProjectItem* attenuation = pm.createItem(
        nullptr, QStringLiteral("WideAttenuation"), ProjectItem::Type::Attenuation);
    ProjectItem* sound = pm.createItem(
        nullptr, QStringLiteral("ReferenceSound"), ProjectItem::Type::Sound);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(attenuation, nullptr);
    ASSERT_NE(sound, nullptr);
    bus->guid = VNX::authoringUuid(harkentest::StudioObjectId(UINT64_C(0x100000011)));
    attenuation->guid = VNX::authoringUuid(harkentest::StudioObjectId(UINT64_C(0x100000022)));
    ASSERT_NE(harkentest::StudioObjectId(bus->objectID), bus->runtimeId());

    VNX::PropertyEditorEngine engine;
    engine.setSchema(VNX::contentObjectInspectorRows(ProjectItem::Type::Sound),
                     VNX::ComponentType::Audio);
    engine.setItemType(ProjectItem::Type::Sound);
    std::unique_ptr<QWidget> page(engine.buildPage());
    engine.bindItem(sound);

    const auto soundLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(soundLocator.has_value());
    auto* outputInherit = page->findChild<QCheckBox*>(QStringLiteral("inherit_mainOutput"));
    auto* ratingInherit = page->findChild<QCheckBox*>(QStringLiteral("inherit_spatialRating"));
    ASSERT_NE(outputInherit, nullptr);
    ASSERT_NE(ratingInherit, nullptr);
    EXPECT_TRUE(outputInherit->isChecked());
    EXPECT_FALSE(outputInherit->isEnabled());
    EXPECT_TRUE(ratingInherit->isChecked());
    EXPECT_FALSE(ratingInherit->isEnabled());
    auto* enabled3D = page->findChild<QCheckBox*>(QStringLiteral("prop_spatialIs3DEnabled"));
    ASSERT_NE(enabled3D, nullptr);
    enabled3D->click();
    auto* busRef = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_mainOutputBusRef"));
    ASSERT_NE(busRef, nullptr);
    busRef->objectChanged(bus->objectID);
    ASSERT_TRUE(sound->fieldGroups.mainOutput.shared.localHistory);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              VNX::toFieldGroupGuid(bus->guid));
    EXPECT_EQ(busRef->currentObjectID(), bus->objectID);
    EXPECT_FALSE(busRef->isInherited());

    auto* attenuationRef = page->findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_spatialAttenuationRef"));
    ASSERT_NE(attenuationRef, nullptr);
    attenuationRef->objectChanged(attenuation->objectID);
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef,
              VNX::toFieldGroupGuid(attenuation->guid));
    attenuation->name = QStringLiteral("RenamedWideAttenuation");
    engine.refreshField(QStringLiteral("spatialAttenuationRef"));
    EXPECT_EQ(attenuationRef->currentObjectID(), attenuation->objectID);
    auto* attenuationLabel = attenuationRef->findChild<QLabel*>(QStringLiteral("refName"));
    ASSERT_NE(attenuationLabel, nullptr);
    EXPECT_EQ(attenuationLabel->text(), attenuation->name);

    auto* outputVolume = page->findChild<VNX::ParameterSlider*>(QStringLiteral("prop_outputBusVolume"));
    ASSERT_NE(outputVolume, nullptr);
    outputVolume->binding()->beginEdit();
    outputVolume->binding()->previewEdit(-5.0);
    outputVolume->binding()->commitEdit();
    busRef->objectChanged(0);
    EXPECT_EQ(busRef->currentObjectID(), pm.getMasterBusItem()->objectID);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, VNX::FieldGroupMode::Inherit);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              VNX::toFieldGroupGuid(pm.getMasterBusItem()->guid));
    EXPECT_FLOAT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -5.0f);
    EXPECT_EQ(page->findChild<QLabel*>(QStringLiteral("source_mainOutputBusRef")), nullptr);

    const auto before = sound->fieldGroups;
    const int undoDepth = VNX::UndoRedoManager::instance().undoStack()->count();
    for (const auto& invalid : {QUuid::createUuid().toString(QUuid::WithoutBraces),
                                attenuation->guid.toString(QUuid::WithoutBraces),
                                QStringLiteral("18446744073709551614")}) {
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("set_field_group_field"), QJsonObject{
                {"definition_guid", sound->guid.toString(QUuid::WithoutBraces)},
                {"group", "mainOutput"}, {"scope", "Shared"},
                {"field", "mainOutputBusRef"}, {"value", invalid}});
        EXPECT_FALSE(result.success);
        EXPECT_EQ(sound->fieldGroups, before);
        EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoDepth);
        EXPECT_EQ(busRef->currentObjectID(), pm.getMasterBusItem()->objectID);
    }
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sound, *soundLocator, 7, VNX::objectIdString(bus->runtimeId()), std::nullopt));
    EXPECT_FALSE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *sound, *soundLocator, 15, QStringLiteral("18446744073709551613"), std::nullopt));
    EXPECT_FALSE(engine.refreshField(QStringLiteral("bindingAttachmentRef")));
    EXPECT_EQ(page->findChild<QLineEdit*>(
                  QStringLiteral("prop_bindingAttachmentRef")), nullptr);
    EXPECT_EQ(page->findChild<VNX::ObjectRefWidget*>(
                  QStringLiteral("prop_bindingAttachmentRef")), nullptr);
    const auto attachment = VNX::PlayablePropertyAuthoringV1::readProperty(
        *sound, *soundLocator, 15, std::nullopt);
    EXPECT_FALSE(attachment.has_value());
    pm.closeProject();
}

TEST(PlayablePropertyInspector, RatingGroupDistinguishesInheritedAttenuationFromLocalNone)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("AttenuationReferenceInspectorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);
    VNX::PlatformContextService::instance().setAll(true);
    ProjectItem* layer = pm.createItem(
        nullptr, QStringLiteral("ParentLayer"), ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ProjectItem* sound = pm.createItem(
        layer, QStringLiteral("ChildSound"), ProjectItem::Type::Sound);
    ProjectItem* inherited = pm.createItem(
        nullptr, QStringLiteral("ParentAttenuation"), ProjectItem::Type::Attenuation);
    ProjectItem* local = pm.createItem(
        nullptr, QStringLiteral("LocalAttenuation"), ProjectItem::Type::Attenuation);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(inherited, nullptr);
    ASSERT_NE(local, nullptr);
    QJsonObject parentArgs{{"definition_guid", layer->guid.toString(QUuid::WithoutBraces)},
                          {"group", "spatialRating"}, {"scope", "Shared"}};
    parentArgs.insert(QStringLiteral("field"), QStringLiteral("spatial.is3DEnabled"));
    parentArgs.insert(QStringLiteral("value"), true);
    ASSERT_TRUE(VNX::ActionManager::instance().execute("set_field_group_field", parentArgs).success);
    parentArgs.insert(QStringLiteral("field"), QStringLiteral("spatial.attenuationRef"));
    parentArgs.insert(QStringLiteral("value"), inherited->guid.toString(QUuid::WithoutBraces));
    ASSERT_TRUE(VNX::ActionManager::instance().execute("set_field_group_field", parentArgs).success);

    VNX::InspectorPanel panel;
    panel.showItemInspector(sound);
    auto* reference = panel.findChild<VNX::ObjectRefWidget*>(
        QStringLiteral("prop_spatialAttenuationRef"));
    auto* inherit = panel.findChild<QCheckBox*>(QStringLiteral("inherit_spatialRating"));
    ASSERT_NE(reference, nullptr);
    ASSERT_NE(inherit, nullptr);
    EXPECT_EQ(panel.findChild<QToolButton*>(QStringLiteral("fieldGroupSettings_spatialRating")), nullptr);
    size_t referenceCount = 0;
    for (auto* widget : panel.findChildren<VNX::ObjectRefWidget*>())
        if (widget->filterType() == ProjectItem::Type::Attenuation)
            ++referenceCount;
    EXPECT_EQ(referenceCount, 1u);
    EXPECT_EQ(panel.findChild<QWidget*>(
                  QStringLiteral("prop_spatialAttenuationCurveType")), nullptr);
    EXPECT_EQ(panel.findChild<QLabel*>(QStringLiteral("source_spatialAttenuationRef")), nullptr);
    EXPECT_EQ(reference->currentObjectID(), inherited->objectID);
    EXPECT_TRUE(inherit->isChecked());
    EXPECT_TRUE(reference->isReadOnly());
    EXPECT_TRUE(inherit->toolTip().contains(layer->name));
    const auto original = sound->fieldGroups;
    const int undoDepth = VNX::UndoRedoManager::instance().undoStack()->count();
    reference->objectChanged(local->objectID);
    EXPECT_EQ(sound->fieldGroups, original);
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), undoDepth);
    inherit->click();
    EXPECT_FALSE(reference->isReadOnly());
    reference->objectChanged(local->objectID);
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory);
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef,
              VNX::toFieldGroupGuid(local->guid));
    EXPECT_EQ(reference->currentObjectID(), local->objectID);

    reference->objectChanged(0);
    EXPECT_TRUE(harkenprojectschema::IsNull(sound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef));
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, VNX::FieldGroupMode::Local);
    EXPECT_EQ(reference->currentObjectID(), 0u);
    EXPECT_FALSE(inherit->isChecked());
    inherit->click();
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, VNX::FieldGroupMode::Inherit);
    EXPECT_TRUE(harkenprojectschema::IsNull(sound->fieldGroups.spatialRating.shared.localHistory->spatialAttenuationRef));
    EXPECT_EQ(reference->currentObjectID(), inherited->objectID);
    EXPECT_TRUE(reference->isReadOnly());

    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(sound->fieldGroups.spatialRating.shared.mode, VNX::FieldGroupMode::Local);
    EXPECT_EQ(reference->currentObjectID(), 0u);
    EXPECT_FALSE(reference->isReadOnly());
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(reference->currentObjectID(), local->objectID);
    VNX::UndoRedoManager::instance().undoStack()->redo();
    VNX::UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(reference->currentObjectID(), inherited->objectID);
    parentArgs.insert(QStringLiteral("value"), local->guid.toString(QUuid::WithoutBraces));
    const auto parentChange = VNX::ActionManager::instance().execute(
        QStringLiteral("set_field_group_field"), parentArgs);
    ASSERT_TRUE(parentChange.success) << parentChange.errorMessage.toStdString();
    EXPECT_EQ(reference->currentObjectID(), local->objectID);
    EXPECT_TRUE(inherit->isChecked());
    EXPECT_TRUE(reference->isReadOnly());
    VNX::UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(reference->currentObjectID(), inherited->objectID);
    EXPECT_EQ(sound->playablePropertyPresence.bits & VNX::kFieldGroupPropertyMask, 0u);
    pm.closeProject();
}

TEST(PlayablePropertyInspector, ContainerChildRowsKeepWeightWithoutPlaybackEditorsOrExpansion)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();
    auto& pm = VNX::ProjectManager::instance();
    if (pm.isProjectOpen()) pm.closeProject();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    VNX::ProjectConfig config;
    config.name = QStringLiteral("ChildOccurrenceInspectorProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    ProjectActionScope projectActions(pm);
    ProjectItem* container = pm.createItem(
        nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    ProjectItem* first = pm.createItem(
        container, QStringLiteral("FirstChild"), ProjectItem::Type::Sound);
    ProjectItem* second = pm.createItem(
        container, QStringLiteral("SecondChild"), ProjectItem::Type::Sound);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto occurrence = VNX::PlayablePropertyAuthoringV1::containerChildLocator(
        *container, first->guid);
    ASSERT_TRUE(occurrence);
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto originalBehavior = container->comp<VNX::VSelection>()->behavior;
    VNX::UndoRedoManager::instance().clear();

    VNX::InspectorPanel panel;
    panel.showItemInspector(container);
    auto* list = panel.findChild<QWidget*>(QStringLiteral("selectionChildrenList"));
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->findChildren<QWidget*>(QStringLiteral("selectionChildRow")).size(), 2);
    EXPECT_TRUE(list->findChildren<QWidget*>(QStringLiteral("selectionChildOccurrence")).isEmpty());
    EXPECT_TRUE(list->findChildren<QToolButton*>(QStringLiteral("selectionChildExpander")).isEmpty());
    EXPECT_TRUE(list->findChildren<VNX::ParameterControlHost*>().isEmpty());
    EXPECT_NE(panel.findChild<VNX::ParameterControlHost*>(QStringLiteral("prop_volume")), nullptr);
    auto* firstRow = selectionChildRow(list, first->guid);
    ASSERT_NE(firstRow, nullptr);
    auto* weight = firstRow->findChild<QSpinBox*>(QStringLiteral("selectionBehaviorWeight"));
    ASSERT_NE(weight, nullptr);
    weight->setValue(7);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.find(
        occurrence->stableOccurrenceOrdinal)->weight, 7);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().childGuid, originalRows.occurrenceRows().front().childGuid);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater, originalRows.occurrenceOrdinalHighWater);
    EXPECT_EQ(first->parent, container);
    EXPECT_EQ(second->parent, container);
    ASSERT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(container->comp<VNX::VSelection>()->behavior, originalBehavior);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    pm.closeProject();
}

TEST(InspectorLifecycle, EmptyStateClearsEveryObjectType)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    const std::array<ProjectItem::Type, 8> types = {
        ProjectItem::Type::Sound,
        ProjectItem::Type::Container,
        ProjectItem::Type::Track,
        ProjectItem::Type::Piece,
        ProjectItem::Type::Layer,
        ProjectItem::Type::Action,
        ProjectItem::Type::Variable,
        ProjectItem::Type::Attenuation};

    VNX::InspectorPanel panel;
    for (ProjectItem::Type type : types) {
        ProjectItem item(QStringLiteral("Item"), type);
        item.attachDefaultComponents();
        panel.showItemInspector(&item);
        if (VNX::ItemTypeCatalog::isUserFacingType(type))
            EXPECT_EQ(panel.currentContextItem(), &item);
        else
            EXPECT_EQ(panel.currentContextItem(), nullptr);
        panel.showEmpty();
        EXPECT_EQ(panel.currentContextItem(), nullptr);
        EXPECT_TRUE(panel.pinnedObjectGuid().isEmpty());
    }
}

TEST(InspectorLifecycle, DeletedAncestorInvalidatesCurrentContext)
{
    ensureApp();
    VNX::registerBuiltinInspectorCustomizations();

    ProjectItem parent(QStringLiteral("Parent"), ProjectItem::Type::Container);
    parent.attachDefaultComponents();
    auto child = std::make_shared<ProjectItem>(
        QStringLiteral("Child"), ProjectItem::Type::Sound);
    child->attachDefaultComponents();
    child->parent = &parent;
    parent.children.push_back(child);

    VNX::InspectorPanel panel;
    panel.showItemInspector(child.get());
    panel.setPinned(true);
    emit VNX::ProjectManager::instance().itemDeleted(&parent);

    EXPECT_EQ(panel.currentContextItem(), nullptr);
    EXPECT_TRUE(panel.pinnedObjectGuid().isEmpty());
    EXPECT_EQ(panel.contextName(), QStringLiteral("Nothing selected"));
    EXPECT_TRUE(panel.isPinned());
}
