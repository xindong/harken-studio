#include "Core/ActionManager.h"
#include "Core/AppSettings.h"
#include "Core/AudioEngine.h"
#include "Core/AuditionManager.h"
#include "Core/IPC/IPCServer.h"
#include "Core/LayoutController.h"
#include "Core/LogService.h"
#include "Core/PlatformContextService.h"
#include "Core/PackGenerationRecord.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/SelectionRouter.h"
#include "Core/ShortcutManager.h"
#include "Core/ThemeManager.h"
#include "Core/UndoCommands.h"
#include "Components/VTrack.h"
#include "Dialogs/Settings/SettingsPanel.h"
#include "MainWindow.h"
#include "Monitor/EventCaptureController.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "Widgets/Blueprint/BlueprintCanvasWidget.h"
#include "Widgets/Blueprint/NodeContentEditor.h"
#include "Widgets/Common/ParameterBinding.h"
#include "Widgets/GridEditor/GridEditorPanel.h"
#include "Widgets/GridEditor/DiscreteGridWidget.h"
#include "Widgets/GridEditor/SelectionListWidget.h"
#include "Widgets/ControlPanel/ControlPanel.h"
#include "Widgets/Hierarchy/Hierarchy.h"
#include "Widgets/Inspector/InspectorPanel.h"
#include "Widgets/ProjectExplorer/ProjectExplorer.h"
#include "Widgets/Publisher/PublisherPanel.h"
#include "Widgets/Timeline/TimelineCanvas.h"
#include "Widgets/Timeline/TimelineEditorPanel.h"
#include "Support/AITestSupport.h"
#include "Tooling/StudioCli/Shell.h"
#include "Tooling/StudioCli/StudioSession.h"

#include <gtest/gtest.h>

#include <DockWidget.h>

#include <QApplication>
#include <QEventLoop>
#include <QDateTime>
#include <QDirIterator>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QAbstractSocket>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMenu>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTcpSocket>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void sendShortcut(QWidget* target, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::ControlModifier)
{
    QApplication::setActiveWindow(target->window());
    target->setFocus();
    QApplication::processEvents();
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(target, &release);
    QApplication::processEvents();
}

class EditorDockLayoutTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& engine = harken::studio::AudioEngine::instance();
        engine.shutdown();
        auto& pm = harken::studio::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        harken::studio::bridge::EngineSessionConfig engineConfig;
        engineConfig.deviceMode = harken::studio::bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(engine.initialize(engineConfig));
        ASSERT_FALSE(engine.isSimulationMode());
        ASSERT_TRUE(engine.startOutput());
        harken::studio::ActionManager::instance().clearAllForTesting();
        ASSERT_TRUE(directory_.isValid());
        window_ = std::make_unique<harken::studio::MainWindow>();
        harken::studio::ProjectConfig config;
        config.name = QStringLiteral("EditorDockLayout");
        config.location = directory_.path();
        ASSERT_TRUE(pm.createProject(config));
        controller_ = window_->findChild<harken::studio::LayoutController*>();
        ASSERT_NE(controller_, nullptr);
        controller_->loadProjectLayout();
        QEventLoop startup;
        bool presentationFinished = false;
        bool presentationShown = false;
        QObject::connect(window_.get(), &harken::studio::MainWindow::startupPresentationFinished,
                         &startup, [&](bool shown) {
            presentationFinished = true;
            presentationShown = shown;
            startup.quit();
        });
        window_->show();
        if (!presentationFinished) {
            QTimer::singleShot(3000, &startup, &QEventLoop::quit);
            startup.exec();
        }
        ASSERT_TRUE(presentationFinished);
        ASSERT_TRUE(presentationShown);
        window_->showNormal();
        window_->resize(1800, 1000);
        drainEvents();
    }

    void TearDown() override
    {
        harken::studio::UndoRedoManager::instance().clear();
        if (window_)
            QObject::disconnect(&harken::studio::UndoRedoManager::instance(), nullptr, window_.get(), nullptr);
        window_.reset();
        harken::studio::ActionManager::instance().setTransactionCallbacks({});
        harken::studio::ActionManager::instance().clearAllForTesting();
        auto& pm = harken::studio::ProjectManager::instance();
        if (pm.isProjectOpen()) pm.closeProject();
        drainEvents();
    }

    static void drainEvents()
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
    }

    static void clickWidget(QWidget* widget, const QPoint& position,
                            Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QApplication::setActiveWindow(widget->window());
        QMouseEvent press(QEvent::MouseButtonPress, position, widget->mapToGlobal(position),
                          Qt::LeftButton, Qt::LeftButton, modifiers);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, position, widget->mapToGlobal(position),
                            Qt::LeftButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(widget, &release);
        drainEvents();
    }

    static void selectChildRow(harken::studio::GridEditorPanel* panel, int index)
    {
        auto* list = panel->findChild<harken::studio::SelectionListWidget*>();
        if (list && list->isVisible()) {
            for (auto* row : list->findChildren<QWidget*>(QStringLiteral("selectionChildRow"))) {
                if (row->property("rowIndex").toInt() != index) continue;
                auto* handle = row->findChild<QWidget*>(QStringLiteral("selectionChildDragHandle"));
                ASSERT_NE(handle, nullptr);
                clickWidget(handle, handle->rect().center());
                return;
            }
            FAIL() << "Selection row is missing";
        } else {
            auto* table = panel->discreteGrid()->table();
            ASSERT_NE(table->item(index, 0), nullptr);
            const QRect cell = table->visualItemRect(table->item(index, 0));
            ASSERT_TRUE(table->viewport()->rect().contains(cell.center()));
            clickWidget(table->viewport(), cell.center());
        }
    }

    QTemporaryDir directory_;
    std::unique_ptr<harken::studio::MainWindow> window_;
    harken::studio::LayoutController* controller_ = nullptr;
};

}

TEST_F(EditorDockLayoutTest, PackWarningsKeepTheirOwnershipAcrossBuildAndReuse)
{
    auto& manager = harken::studio::ProjectManager::instance();
    for (const auto& name : {QStringLiteral("FirstEmpty"), QStringLiteral("SecondEmpty")})
        ASSERT_NE(manager.createItem(nullptr, name, harken::studio::ProjectItem::Type::Action), nullptr);
    const auto created = harken::studio::ActionManager::instance().execute(
        QStringLiteral("create_project_platform"),
        {{QStringLiteral("name"), QStringLiteral("OtherPlatform")},
         {QStringLiteral("base_platform"), QStringLiteral("Android")}});
    ASSERT_TRUE(created.success) << created.errorMessage.toStdString();
    const auto& platforms = manager.currentProject()->platforms;
    const QStringList selected{harken::studio::ProjectPlatformSemantics::idString(platforms.front().id),
                              harken::studio::ProjectPlatformSemantics::idString(platforms.back().id)};
    ASSERT_TRUE(window_->setWorkspaceWindowVisible(QStringLiteral("Publisher"), true));
    auto* publisher = window_->findChild<harken::studio::PublisherPanel*>();
    ASSERT_NE(publisher, nullptr);
    auto& store = harken::studio::PackGenerationRecordStore::instance();
    auto* log = harken::studio::LogService::globalModel();
    for (int run = 0; run < 2; ++run) {
        SCOPED_TRACE(run);
        store.clear();
        log->clear();
        QEventLoop completion;
        QObject::connect(&store, &harken::studio::PackGenerationRecordStore::exportActivityChanged,
            &completion, [&](bool active) { if (!active) completion.quit(); });
        emit publisher->exportRequested(selected, {}, true, false, false);
        if (store.isExporting()) {
            QTimer::singleShot(10000, &completion, &QEventLoop::quit);
            completion.exec();
        }
        ASSERT_FALSE(store.isExporting());
        drainEvents();
        const auto entries = log->entries(harken::studio::LogEntry::Warning, QStringLiteral("Pack"));
        int warningCount = 0;
        for (const auto& record : store.records()) {
            EXPECT_TRUE(record.success) << record.errors.toStdString();
            for (const auto& warning : record.warnings.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                ++warningCount;
                EXPECT_EQ(std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
                    return entry.message.contains(warning) && entry.message.contains(record.packName)
                        && entry.message.contains(record.platformName)
                        && entry.message.contains(record.language);
                }), 1) << warning.toStdString();
            }
        }
        EXPECT_EQ(warningCount, 4);
        EXPECT_EQ(entries.size(), static_cast<std::size_t>(warningCount + 1));
        const auto summary = QCoreApplication::translate("MainWindow", ", %1 warning(s)").arg(warningCount);
        EXPECT_EQ(std::count_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.message.endsWith(summary);
        }), 1);
    }
}

TEST_F(EditorDockLayoutTest, ShellKeepsBufferedCommandsWhenClosingIsRejected)
{
    ASSERT_TRUE(harken::studio::ProjectManager::instance().saveProject());
    QFile input(directory_.filePath(QStringLiteral("shell-input.txt")));
    ASSERT_TRUE(input.open(QIODevice::WriteOnly));
    ASSERT_GT(input.write("exit\nget_project_info\nexit\n"), 0);
    input.close();
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
#ifdef Q_OS_WIN
    const int inputDescriptor = _fileno(stdin);
    const int savedInput = _dup(inputDescriptor);
    const auto replaceDescriptor = [](int from, int to) { return _dup2(from, to); };
    const auto closeDescriptor = [](int descriptor) { _close(descriptor); };
#else
    const int inputDescriptor = fileno(stdin);
    const int savedInput = dup(inputDescriptor);
    const auto replaceDescriptor = [](int from, int to) { return dup2(from, to); };
    const auto closeDescriptor = [](int descriptor) { close(descriptor); };
#endif
    ASSERT_GE(savedInput, 0);
    const auto restore = qScopeGuard([&] {
        replaceDescriptor(savedInput, inputDescriptor);
        closeDescriptor(savedInput);
        std::clearerr(stdin);
    });
    ASSERT_GE(replaceDescriptor(input.handle(), inputDescriptor), 0);
    std::clearerr(stdin);

    auto& records = harken::studio::PackGenerationRecordStore::instance();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    const auto runId = records.beginProgress(canceled);
    const auto finishProgress = qScopeGuard([&] { records.finishProgress(runId, true); });
    harken::studio::cli::StudioSession session(*window_->ipcServer());
    volatile std::sig_atomic_t interrupted = 0;
    harken::studio::cli::ShellExitRequest request;
    QVector<QJsonObject> responses;
    int closeAttempts = 0;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, qApp, &QCoreApplication::quit);
    timeout.start(5000);
    const int code = harken::studio::cli::runShell(session, 1000, true, interrupted,
        [&](QJsonObject response) { responses.append(response); }, &request,
        [&](harken::studio::cli::ShellExitRequest) -> QJsonObject {
            ++closeAttempts;
            const bool closed = window_->closeFromShell(false);
            if (!closed) {
                EXPECT_TRUE(canceled->load());
                records.finishProgress(runId, true);
                return harken::studio::cli::failure(QStringLiteral("EXIT_NOT_COMPLETED"), QStringLiteral("Export is stopping."));
            }
            return {{QStringLiteral("status"), QStringLiteral("ok")}};
        });
    EXPECT_TRUE(timeout.isActive());
    timeout.stop();
    EXPECT_EQ(code, 0);
    EXPECT_EQ(closeAttempts, 2);
    EXPECT_EQ(request, harken::studio::cli::ShellExitRequest::Close);
    ASSERT_EQ(responses.size(), 3);
    EXPECT_EQ(responses[0].value(QStringLiteral("code")).toString(), QStringLiteral("EXIT_NOT_COMPLETED"));
    EXPECT_EQ(responses[1].value(QStringLiteral("command")).toString(), QStringLiteral("get_project_info"));
    EXPECT_EQ(responses[1].value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    EXPECT_EQ(responses[2].value(QStringLiteral("command")).toString(), QStringLiteral("exit"));
    EXPECT_EQ(responses[2].value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
}

TEST_F(EditorDockLayoutTest, ColorRefreshPreservesBlueprintEditorsAndStructureRefreshReplacesThem)
{
    auto& project = harken::studio::ProjectManager::instance();
    ASSERT_NE(project.createItem(nullptr, QStringLiteral("ColorPreviewNode"), harken::studio::ProjectItem::Type::Sound), nullptr);
    auto* blueprint = window_->findChild<harken::studio::BlueprintCanvasWidget*>();
    ASSERT_NE(blueprint, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    drainEvents();
    drainEvents();

    QList<QPointer<harken::studio::NodeContentEditor>> editors;
    for (auto* widget : QApplication::allWidgets()) {
        auto* editor = qobject_cast<harken::studio::NodeContentEditor*>(widget);
        if (editor && editor->isHidden() && editor->parentWidget()
            && !editor->parentWidget()->parentWidget()) editors.append(editor);
    }
    ASSERT_FALSE(editors.isEmpty());

    harken::studio::NodeContentRenderer renderer;
    harken::studio::NodeContentInfo content;
    content.volume = -3.0f;
    const auto initialDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    const auto image = [](const harken::studio::NodeContentPresentation& presentation) {
        QImage result(presentation.size, QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.drawPicture(0, 0, presentation.drawing);
        return result;
    };
    const auto initialImage = image(initialDrawing);

    const auto originalPalette = blueprint->viewport()->palette();
    auto palette = originalPalette;
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#326475")));
    blueprint->viewport()->setPalette(palette);
    drainEvents();
    for (const auto& editor : editors) EXPECT_FALSE(editor.isNull());

    auto& theme = harken::studio::ThemeManager::instance();
    const auto restore = qScopeGuard([&] {
        theme.cancelAppearancePreview();
        blueprint->viewport()->setPalette(originalPalette);
        drainEvents();
    });
    auto draft = theme.appearance();
    draft.colors[draft.themeId][draft.paletteId].insert(QStringLiteral("background.primary"), QColor(QStringLiteral("#293746")));
    draft.colors[draft.themeId][draft.paletteId].insert(QStringLiteral("text.primary"), QColor(QStringLiteral("#ff3366")));
    ASSERT_TRUE(theme.previewAppearance(draft).ok());
    drainEvents();
    for (const auto& editor : editors) EXPECT_FALSE(editor.isNull());
    renderer.clearDrawings();
    const auto colorDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    EXPECT_EQ(colorDrawing.size, initialDrawing.size);
    EXPECT_EQ(colorDrawing.fields, initialDrawing.fields);
    EXPECT_NE(image(colorDrawing), initialImage);
    content.volume = -12.0f;
    const auto valueDrawing = renderer.presentation(content, content.fields(), std::nullopt, true);
    EXPECT_NE(image(valueDrawing), image(colorDrawing));

    QEvent styleChange(QEvent::StyleChange);
    QApplication::sendEvent(blueprint->viewport(), &styleChange);
    drainEvents();
    for (const auto& editor : editors) EXPECT_TRUE(editor.isNull());
}

TEST_F(EditorDockLayoutTest, BlueprintEditingKeepsTransactionsIsolatedAcrossNodeActivation)
{
    using Authoring = harken::studio::PlayablePropertyAuthoringV1;
    auto& manager = harken::studio::ProjectManager::instance();
    auto& undo = harken::studio::UndoRedoManager::instance();
    auto& platforms = harken::studio::PlatformContextService::instance();
    const auto originalContext = platforms.context();
    const auto restore = qScopeGuard([&] {
        platforms.setSelected(originalContext.selected, originalContext.primary, originalContext.all);
    });
    platforms.setAll(true);
    auto* first = manager.createItem(nullptr, QStringLiteral("FirstEditTarget"), harken::studio::ProjectItem::Type::Sound);
    auto* second = manager.createItem(nullptr, QStringLiteral("SecondEditTarget"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto firstLocator = Authoring::definitionLocator(*first);
    const auto secondLocator = Authoring::definitionLocator(*second);
    ASSERT_TRUE(firstLocator);
    ASSERT_TRUE(secondLocator);
    const auto* volume = Authoring::fieldByName(QStringLiteral("volume"));
    ASSERT_NE(volume, nullptr);
    const auto firstState = [&] {
        return Authoring::readProperty(*first, *firstLocator, volume->id, std::nullopt);
    };
    const auto secondState = [&] {
        return Authoring::readProperty(*second, *secondLocator, volume->id, std::nullopt);
    };
    const auto initialFirst = firstState();
    const auto initialSecond = secondState();
    ASSERT_TRUE(initialFirst);
    ASSERT_TRUE(initialSecond);
    undo.clearHistory();

    auto canvas = std::make_unique<harken::studio::BlueprintCanvasWidget>();
    canvas->setSoundsFiltered(false);
    canvas->rebuildFromProject();
    drainEvents();
    const auto findNode = [&](const harken::studio::ProjectItem* target) -> harken::studio::NodeItem* {
        for (auto* item : canvas->scene()->items())
            if (auto* node = dynamic_cast<harken::studio::NodeItem*>(item); node && node->objectID() == target->objectID)
                return node;
        return nullptr;
    };
    auto* firstNode = findNode(first);
    auto* secondNode = findNode(second);
    ASSERT_NE(firstNode, nullptr);
    ASSERT_NE(secondNode, nullptr);
    auto* editor = qobject_cast<harken::studio::NodeContentEditor*>(firstNode->ensureContentWidget());
    ASSERT_NE(editor, nullptr);
    auto* binding = editor->binding(harken::studio::NodeContentField::Volume);
    ASSERT_NE(binding, nullptr);
    binding->beginEdit();
    binding->previewEdit(-6.0);
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_EQ(secondState(), initialSecond);
    EXPECT_FALSE(firstNode->releaseContentWidget());
    editor->finishEditing(false);
    EXPECT_EQ(firstState(), initialFirst);
    EXPECT_FALSE(undo.canUndo());

    binding->beginEdit();
    binding->previewEdit(-6.0);
    editor->finishEditing(true);
    ASSERT_TRUE(firstNode->releaseContentWidget());
    drainEvents();
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_EQ(secondState(), initialSecond);

    editor = qobject_cast<harken::studio::NodeContentEditor*>(secondNode->ensureContentWidget());
    ASSERT_NE(editor, nullptr);
    binding = editor->binding(harken::studio::NodeContentField::Volume);
    ASSERT_NE(binding, nullptr);
    EXPECT_DOUBLE_EQ(binding->value(), initialSecond->value.toDouble());
    binding->beginEdit();
    binding->previewEdit(-12.0);
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_DOUBLE_EQ(secondState()->value.toDouble(), -12.0);
    editor->finishEditing(true);
    ASSERT_TRUE(secondNode->releaseContentWidget());
    drainEvents();

    ASSERT_TRUE(undo.undo());
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_EQ(secondState(), initialSecond);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(firstState(), initialFirst);
    EXPECT_EQ(secondState(), initialSecond);
    EXPECT_FALSE(undo.canUndo());
    ASSERT_TRUE(undo.redo());
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_EQ(secondState(), initialSecond);
    ASSERT_TRUE(undo.redo());
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_DOUBLE_EQ(secondState()->value.toDouble(), -12.0);
    drainEvents();

    editor = qobject_cast<harken::studio::NodeContentEditor*>(findNode(first)->ensureContentWidget());
    ASSERT_NE(editor, nullptr);
    binding = editor->binding(harken::studio::NodeContentField::Volume);
    ASSERT_NE(binding, nullptr);
    EXPECT_DOUBLE_EQ(binding->value(), -6.0);
    binding->beginEdit();
    binding->previewEdit(-3.0);
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -3.0);
    EXPECT_DOUBLE_EQ(secondState()->value.toDouble(), -12.0);
    QPointer<harken::studio::NodeContentEditor> activeEditor = editor;
    canvas.reset();
    drainEvents();
    EXPECT_TRUE(activeEditor.isNull());
    EXPECT_DOUBLE_EQ(firstState()->value.toDouble(), -6.0);
    EXPECT_DOUBLE_EQ(secondState()->value.toDouble(), -12.0);
}

TEST_F(EditorDockLayoutTest, InterfaceZoomShortcutsReachMainFloatingAndModalWindows)
{
    auto& settings = harken::studio::AppSettings::instance();
    auto& shortcuts = harken::studio::ShortcutManager::instance();
    const int originalLevel = settings.globalUIZoomLevel();
    const auto originalIn = shortcuts.info(harken::studio::Shortcuts::ZoomIn);
    const auto originalOut = shortcuts.info(harken::studio::Shortcuts::ZoomOut);
    const auto restore = qScopeGuard([&] {
        settings.setGlobalUIZoomLevel(originalLevel);
        for (const auto& info : {originalIn, originalOut}) {
            if (info.isCustom) shortcuts.setShortcut(info.id, info.currentShortcut);
            else shortcuts.resetToDefault(info.id);
        }
    });
    shortcuts.resetToDefault(harken::studio::Shortcuts::ZoomIn);
    shortcuts.resetToDefault(harken::studio::Shortcuts::ZoomOut);
    settings.setGlobalUIZoomLevel(0);
    sendShortcut(window_.get(), Qt::Key_Equal);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(window_.get(), Qt::Key_Plus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    sendShortcut(window_.get(), Qt::Key_Minus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(window_.get(), Qt::Key_Plus, Qt::ControlModifier | Qt::KeypadModifier);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    sendShortcut(window_.get(), Qt::Key_Minus, Qt::ControlModifier | Qt::KeypadModifier);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(window_.get(), Qt::Key_0);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);

    auto* viewMenu = window_->findChild<QMenu*>(QStringLiteral("viewsMenu"));
    ASSERT_NE(viewMenu, nullptr);
    for (int iteration = 0; iteration < 3; ++iteration) {
        viewMenu->aboutToShow();
        sendShortcut(window_.get(), Qt::Key_Equal);
        EXPECT_EQ(settings.globalUIZoomLevel(), 2);
        sendShortcut(window_.get(), Qt::Key_Minus);
        EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    }

    harken::studio::SettingsPanel floating;
    floating.show();
    drainEvents();
    auto* search = floating.findChild<QLineEdit*>();
    ASSERT_NE(search, nullptr);
    search->setText(QStringLiteral("appearance"));
    search->setSelection(2, 4);
    sendShortcut(search, Qt::Key_Equal);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    EXPECT_EQ(search->text(), QStringLiteral("appearance"));
    EXPECT_EQ(search->selectedText(), QStringLiteral("pear"));
    sendShortcut(search, Qt::Key_Minus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);

    QDialog modal(window_.get());
    auto* layout = new QVBoxLayout(&modal);
    auto* edit = new QLineEdit(QStringLiteral("unfinished value"), &modal);
    auto* capture = new QKeySequenceEdit(&modal);
    layout->addWidget(edit);
    layout->addWidget(capture);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    drainEvents();
    ASSERT_EQ(QApplication::activeModalWidget(), &modal);
    edit->setSelection(2, 5);
    sendShortcut(edit, Qt::Key_Plus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    EXPECT_EQ(edit->text(), QStringLiteral("unfinished value"));
    EXPECT_EQ(edit->selectedText(), QStringLiteral("finis"));
    sendShortcut(edit, Qt::Key_Minus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(edit, Qt::Key_0);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(capture, Qt::Key_Equal);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    EXPECT_EQ(capture->keySequence(), QKeySequence(QStringLiteral("Ctrl+=")));

    edit->setFocus();
    edit->setSelection(2, 5);
    shortcuts.setShortcut(harken::studio::Shortcuts::ZoomIn, QKeySequence(QStringLiteral("Ctrl+Alt+=")));
    sendShortcut(edit, Qt::Key_Equal);
    sendShortcut(edit, Qt::Key_Plus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 1);
    sendShortcut(edit, Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    shortcuts.setShortcut(harken::studio::Shortcuts::ZoomIn, {});
    sendShortcut(edit, Qt::Key_Equal, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_EQ(settings.globalUIZoomLevel(), 2);
    shortcuts.resetToDefault(harken::studio::Shortcuts::ZoomIn);
    sendShortcut(edit, Qt::Key_Plus);
    EXPECT_EQ(settings.globalUIZoomLevel(), 3);
    EXPECT_EQ(edit->text(), QStringLiteral("unfinished value"));
    EXPECT_EQ(edit->selectedText(), QStringLiteral("finis"));
    QObject observation;
    int changes = 0;
    QObject::connect(&settings, &harken::studio::AppSettings::globalUIScaleChanged,
                     &observation, [&] { ++changes; });
    settings.setGlobalUIZoomLevel(harken::studio::AppSettings::MAX_GLOBAL_ZOOM_LEVEL);
    changes = 0;
    sendShortcut(edit, Qt::Key_Equal);
    EXPECT_EQ(settings.globalUIZoomLevel(), harken::studio::AppSettings::MAX_GLOBAL_ZOOM_LEVEL);
    EXPECT_EQ(changes, 0);
    settings.setGlobalUIZoomLevel(harken::studio::AppSettings::MIN_GLOBAL_ZOOM_LEVEL);
    changes = 0;
    sendShortcut(edit, Qt::Key_Minus);
    EXPECT_EQ(settings.globalUIZoomLevel(), harken::studio::AppSettings::MIN_GLOBAL_ZOOM_LEVEL);
    EXPECT_EQ(changes, 0);
}

TEST_F(EditorDockLayoutTest, LocalBlueprintLayoutPreservesProjectSavePointsAcrossUndoAndReopen)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto& undo = harken::studio::UndoRedoManager::instance();
    auto* sound = manager.createItem(nullptr, QStringLiteral("LayoutNode"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto guid = sound->guid;
    const auto id = sound->objectID;
    ASSERT_TRUE(manager.saveProject());
    undo.clearHistory();
    const QString projectPath = manager.currentProject()->projectPath;
    const QString projectId = manager.currentProject()->projectId;
    const auto gridSnap = QSettings().value(QStringLiteral("Blueprint/GridSnapEnabled"));
    const auto cleanup = qScopeGuard([&] {
        QSettings settings;
        settings.remove(QStringLiteral("Blueprint/SignalChainLayout/Nodes/") + projectId);
        if (gridSnap.isValid()) settings.setValue(QStringLiteral("Blueprint/GridSnapEnabled"), gridSnap);
        else settings.remove(QStringLiteral("Blueprint/GridSnapEnabled"));
    });
    const auto documents = [&] {
        QMap<QString, QPair<QByteArray, QDateTime>> result;
        QDirIterator files(QFileInfo(projectPath).absolutePath(),
                           {QStringLiteral("*.hsp"), QStringLiteral("*.vp")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            EXPECT_TRUE(file.open(QIODevice::ReadOnly));
            result.insert(file.fileName(), {file.readAll(), QFileInfo(file).lastModified()});
        }
        return result;
    };
    const auto saved = documents();
    auto canvas = std::make_unique<harken::studio::BlueprintCanvasWidget>();
    canvas->setGridSnapEnabled(false);
    canvas->useSignalChain(sound);
    harken::studio::NodeItem* node = nullptr;
    for (auto* item : canvas->scene()->items()) {
        auto* candidate = dynamic_cast<harken::studio::NodeItem*>(item);
        if (candidate && candidate->objectID() == id) node = candidate;
    }
    ASSERT_NE(node, nullptr);
    const auto original = node->pos();
    const auto moved = original + QPointF(96, 64);
    QGraphicsSceneMouseEvent press(QEvent::GraphicsSceneMousePress);
    press.setButton(Qt::LeftButton);
    press.setButtons(Qt::LeftButton);
    canvas->scene()->sendEvent(node, &press);
    node->setPos(moved);
    QGraphicsSceneMouseEvent release(QEvent::GraphicsSceneMouseRelease);
    release.setButton(Qt::LeftButton);
    canvas->scene()->sendEvent(node, &release);
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());
    EXPECT_FALSE(sound->isDirtyObject());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(undo.redo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());

    ASSERT_TRUE(manager.renameItem(sound, QStringLiteral("RenamedNode")));
    EXPECT_FALSE(undo.isClean());
    EXPECT_TRUE(sound->isDirtyObject());
    canvas->resetNodeLayout();
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_FALSE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_TRUE(undo.isClean());
    EXPECT_FALSE(sound->isDirtyObject());
    canvas->resetNodeLayout();
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), original);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(undo.undo());
    drainEvents();
    EXPECT_EQ(canvas->nodePosition(id), moved);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(documents(), saved);

    undo.clearHistory();
    canvas = std::make_unique<harken::studio::BlueprintCanvasWidget>();
    canvas->useSignalChain(sound);
    EXPECT_EQ(canvas->nodePosition(id), moved);
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath));
    ASSERT_TRUE(harken::studio::TestSupport::spinUntil([&] { return manager.findItemByGuid(guid) != nullptr; }));
    sound = manager.findItemByGuid(guid);
    ASSERT_NE(sound, nullptr);
    canvas->useSignalChain(sound);
    EXPECT_EQ(canvas->nodePosition(sound->objectID), moved);
    EXPECT_TRUE(undo.isClean());
    ASSERT_TRUE(manager.saveProject());
    EXPECT_EQ(documents(), saved);
}

TEST_F(EditorDockLayoutTest, AuditionTargetInvalidatesWithDeletedSubtreesAndProjectClosure)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* parent = manager.createItem(nullptr, QStringLiteral("AuditionRoot"), harken::studio::ProjectItem::Type::Container);
    auto* nested = manager.createItem(parent, QStringLiteral("NestedAudition"), harken::studio::ProjectItem::Type::Container);
    auto* child = manager.createItem(nested, QStringLiteral("NestedChild"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(child, nullptr);
    auto* router = window_->findChild<harken::studio::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->onProjectItemDoubleClicked(parent);
    drainEvents();
    auto* panel = qobject_cast<harken::studio::GridEditorPanel*>(
        window_->activeStandaloneEditorPanel(QStringLiteral("GridEditor")));
    ASSERT_NE(panel, nullptr);
    panel->drillInto(nested->guid);
    drainEvents();
    selectChildRow(panel, 0);
    ASSERT_EQ(window_->auditionTarget(), child);
    ASSERT_TRUE(manager.deleteItem(nested));
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);

    router->selectItem(parent, false);
    ASSERT_EQ(window_->auditionTarget(), parent);
    ASSERT_TRUE(manager.closeProject());
    drainEvents();
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, NavigationViewsKeepLocalSelectionsAcrossActiveObjectChanges)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* first = window_->createHierarchyPanel();
    auto* second = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<harken::studio::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    auto* blueprint = window_->findChild<harken::studio::BlueprintCanvasWidget*>();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(blueprint, nullptr);
    blueprint->setFixedSize(640, 480);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* firstItem = manager.createItem(nullptr, QStringLiteral("FirstSelection"), harken::studio::ProjectItem::Type::Sound);
    auto* secondItem = manager.createItem(nullptr, QStringLiteral("SecondSelection"), harken::studio::ProjectItem::Type::Sound);
    auto* explorerItem = manager.createItem(nullptr, QStringLiteral("ExplorerSelection"), harken::studio::ProjectItem::Type::Sound);
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("NavigationBus"), harken::studio::ProjectItem::Type::Bus);
    ASSERT_NE(firstItem, nullptr);
    ASSERT_NE(secondItem, nullptr);
    ASSERT_NE(explorerItem, nullptr);
    ASSERT_NE(bus, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    blueprint->setBusVisible(bus->objectID, true);
    drainEvents();

    ASSERT_TRUE(second->selectProjectItem(secondItem));
    ASSERT_TRUE(explorer->selectProjectItem(explorerItem));
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    drainEvents();
    const QTransform initialViewport = blueprint->viewportTransform();
    ASSERT_TRUE(first->selectProjectItem(firstItem));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), firstItem);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    ASSERT_TRUE(first->selectProjectItem(bus));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{bus});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), bus);
    EXPECT_EQ(window_->auditionTarget(), bus);
    ASSERT_TRUE(first->selectProjectItem(firstItem));

    ASSERT_TRUE(explorer->selectProjectItem(secondItem));
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->viewportTransform(), initialViewport);
    EXPECT_EQ(inspector->currentContextItem(), secondItem);
    EXPECT_EQ(window_->auditionTarget(), secondItem);

    ASSERT_TRUE(blueprint->focusItem(explorerItem));
    ASSERT_FALSE(blueprint->scene()->selectedItems().isEmpty());
    auto* selectedNode = blueprint->scene()->selectedItems().front();
    const QTransform selectedViewport = blueprint->viewportTransform();
    blueprint->scene()->clearSelection();
    selectedNode->setSelected(true);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->scene()->selectedItems(), QList<QGraphicsItem*>{selectedNode});
    EXPECT_EQ(blueprint->viewportTransform(), selectedViewport);
    EXPECT_EQ(inspector->currentContextItem(), explorerItem);
    EXPECT_EQ(window_->auditionTarget(), explorerItem);

    for (auto* panel : window_->allInspectorPanels())
        window_->closeInspectorPanel(panel->panelId());
    drainEvents();
    ASSERT_EQ(window_->activeInspectorPanel(), nullptr);
    ASSERT_EQ(window_->inspectorShowingItem(firstItem, true), nullptr);
    auto* tree = first->treeWidget();
    window_->dockOfPanel(first)->setAsCurrentTab();
    tree->scrollToItem(tree->currentItem());
    drainEvents();
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    const QTransform hierarchyViewport = blueprint->viewportTransform();
    const QPoint position = tree->visualItemRect(tree->currentItem()).center();
    ASSERT_TRUE(tree->viewport()->rect().contains(position));
    clickWidget(tree->viewport(), position);
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, position,
                            tree->viewport()->mapToGlobal(position),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &doubleClick);
    QMouseEvent release(QEvent::MouseButtonRelease, position,
                        tree->viewport()->mapToGlobal(position),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &release);
    drainEvents();
    ASSERT_NE(window_->inspectorShowingItem(firstItem), nullptr);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), hierarchyViewport);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    const auto inspectorsBefore = window_->allInspectorPanels();
    first->openInNewWindowRequested(firstItem);
    drainEvents();
    EXPECT_GT(window_->allInspectorPanels().size(), inspectorsBefore.size());
    ASSERT_NE(window_->activeInspectorPanel(), nullptr);
    EXPECT_EQ(window_->activeInspectorPanel()->currentContextItem(), firstItem);
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_EQ(blueprint->viewportTransform(), hierarchyViewport);
    EXPECT_EQ(window_->auditionTarget(), firstItem);

    auto* explorerTree = explorer->treeWidget();
    window_->dockOfPanel(explorer)->setAsCurrentTab();
    explorerTree->scrollToItem(explorerTree->currentItem());
    drainEvents();
    const QPoint explorerPosition = explorerTree->visualItemRect(explorerTree->currentItem()).center();
    ASSERT_TRUE(explorerTree->viewport()->rect().contains(explorerPosition));
    clickWidget(explorerTree->viewport(), explorerPosition);
    QMouseEvent explorerDoubleClick(QEvent::MouseButtonDblClick, explorerPosition,
                                    explorerTree->viewport()->mapToGlobal(explorerPosition),
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(explorerTree->viewport(), &explorerDoubleClick);
    QMouseEvent explorerRelease(QEvent::MouseButtonRelease, explorerPosition,
                                explorerTree->viewport()->mapToGlobal(explorerPosition),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(explorerTree->viewport(), &explorerRelease);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{explorerItem});
    EXPECT_DOUBLE_EQ(blueprint->transform().m11(), 1.0);
    EXPECT_NE(blueprint->viewportTransform(), hierarchyViewport);

    auto* router = window_->findChild<harken::studio::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    QObject activationObserver;
    int activationCount = 0;
    QObject::connect(router, &harken::studio::SelectionRouter::selectionChanged, &activationObserver,
                     [&](harken::studio::ProjectItem*) { ++activationCount; });
    for (auto* target : {explorerItem, bus}) {
        ASSERT_TRUE(explorer->selectProjectItem(target));
        window_->dockOfPanel(blueprint)->setAsCurrentTab();
        ASSERT_TRUE(blueprint->focusItem(target));
        ASSERT_TRUE(first->selectProjectItem(firstItem));
        drainEvents();
        ASSERT_EQ(router->activeItem(), firstItem);
        auto* currentInspector = window_->activeInspectorPanel();
        ASSERT_NE(currentInspector, nullptr);
        ASSERT_EQ(currentInspector->currentContextItem(), firstItem);
        ASSERT_EQ(blueprint->scene()->selectedItems().size(), 1);
        auto* node = dynamic_cast<harken::studio::NodeItem*>(blueprint->scene()->selectedItems().front());
        ASSERT_NE(node, nullptr);
        ASSERT_EQ(node->objectID(), target->objectID);
        const QPoint nodePosition = blueprint->mapFromScene(node->mapToScene(node->titleRect().center()));
        ASSERT_TRUE(blueprint->viewport()->rect().contains(nodePosition));
        ASSERT_EQ(blueprint->itemAt(nodePosition), node);
        activationCount = 0;

        clickWidget(blueprint->viewport(), nodePosition);

        EXPECT_EQ(activationCount, 1);
        EXPECT_EQ(router->activeItem(), target);
        EXPECT_EQ(currentInspector->currentContextItem(), target);
        EXPECT_EQ(window_->auditionTarget(), target);
        EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{firstItem});
        EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{secondItem});
        EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{target});
        EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{target});
    }
}

TEST_F(EditorDockLayoutTest, BackgroundProjectChangesPreserveActiveContextUntilItsObjectIsDeleted)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* first = window_->createHierarchyPanel();
    auto* second = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<harken::studio::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* parent = manager.createItem(nullptr, QStringLiteral("InactiveBranch"), harken::studio::ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    auto* inactive = manager.createItem(parent, QStringLiteral("InactiveSelection"), harken::studio::ProjectItem::Type::Sound);
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveSelection"), harken::studio::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedExplorerSelection"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(inactive, nullptr);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(parked, nullptr);
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    ASSERT_TRUE(first->selectProjectItem(inactive));
    ASSERT_TRUE(second->selectProjectItem(active));

    ASSERT_NE(manager.createItem(nullptr, QStringLiteral("BackgroundCreation"), harken::studio::ProjectItem::Type::Sound), nullptr);
    drainEvents();
    EXPECT_EQ(first->selectedItems(), QList<harken::studio::ProjectItem*>{inactive});
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{active});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);

    ASSERT_TRUE(manager.deleteItem(parent));
    drainEvents();
    EXPECT_TRUE(first->selectedItems().isEmpty());
    EXPECT_EQ(second->selectedItems(), QList<harken::studio::ProjectItem*>{active});
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);

    ASSERT_TRUE(manager.deleteItem(active));
    drainEvents();
    EXPECT_TRUE(second->selectedItems().isEmpty());
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(inspector->currentContextItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, ActiveObjectChangesFollowExplicitNavigationInput)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* hierarchy = window_->createHierarchyPanel();
    auto* otherHierarchy = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<harken::studio::ProjectExplorer*>();
    auto* blueprint = window_->findChild<harken::studio::BlueprintCanvasWidget*>();
    auto* inspector = window_->createInspectorPanel();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_NE(otherHierarchy, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(blueprint, nullptr);
    ASSERT_NE(inspector, nullptr);
    blueprint->setFixedSize(640, 480);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* parent = manager.createItem(nullptr, QStringLiteral("InputBranch"), harken::studio::ProjectItem::Type::Container);
    ASSERT_NE(parent, nullptr);
    auto* first = manager.createItem(parent, QStringLiteral("FirstInput"), harken::studio::ProjectItem::Type::Sound);
    auto* second = manager.createItem(parent, QStringLiteral("SecondInput"), harken::studio::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedInput"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(parked, nullptr);
    blueprint->setSoundsFiltered(false);
    blueprint->rebuildFromProject();
    drainEvents();
    auto* tree = hierarchy->treeWidget();
    ASSERT_TRUE(hierarchy->selectProjectItem(first));
    auto* firstRow = tree->currentItem();
    ASSERT_TRUE(hierarchy->selectProjectItem(second));
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    window_->dockOfPanel(hierarchy)->setAsCurrentTab();
    drainEvents();
    tree->scrollToItem(firstRow);
    tree->setFocus(Qt::OtherFocusReason);
    drainEvents();
    const QPoint firstPosition = tree->visualItemRect(firstRow).center();
    ASSERT_TRUE(tree->viewport()->rect().contains(firstPosition));
    blueprint->setZoomFactor(0.75);
    blueprint->centerOn(QPointF(1200, 800));
    const QTransform blueprintViewport = blueprint->viewportTransform();

    clickWidget(tree->viewport(), firstPosition);
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    QKeyEvent downPress(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(tree, &downPress);
    QKeyEvent downRelease(QEvent::KeyRelease, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(tree, &downRelease);
    drainEvents();
    ASSERT_EQ(hierarchy->selectedItem(), second);
    EXPECT_EQ(inspector->currentContextItem(), second);
    EXPECT_EQ(window_->auditionTarget(), second);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    const QPoint rightPosition = tree->visualItemRect(firstRow).center();
    QMouseEvent rightPress(QEvent::MouseButtonPress, rightPosition,
                           tree->viewport()->mapToGlobal(rightPosition),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &rightPress);
    QMouseEvent rightRelease(QEvent::MouseButtonRelease, rightPosition,
                             tree->viewport()->mapToGlobal(rightPosition),
                             Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &rightRelease);
    drainEvents();
    ASSERT_EQ(hierarchy->selectedItem(), first);
    EXPECT_EQ(inspector->currentContextItem(), second);
    EXPECT_EQ(window_->auditionTarget(), second);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    clickWidget(tree->viewport(), tree->visualItemRect(firstRow).center());
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    ASSERT_TRUE(otherHierarchy->selectProjectItem(second));
    ASSERT_EQ(hierarchy->selectedItem(), first);
    clickWidget(tree->viewport(), tree->visualItemRect(firstRow).center());
    EXPECT_EQ(inspector->currentContextItem(), first);
    EXPECT_EQ(window_->auditionTarget(), first);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(tree, &escape);
    drainEvents();
    EXPECT_TRUE(hierarchy->selectedItems().isEmpty());
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(otherHierarchy->selectedItems(), QList<harken::studio::ProjectItem*>{second});
    EXPECT_EQ(blueprint->selectedProjectItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(blueprint->viewportTransform(), blueprintViewport);
    EXPECT_EQ(inspector->currentContextItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
}

TEST_F(EditorDockLayoutTest, DeletedActiveObjectDoesNotClearIndependentEditorAndAuditionContexts)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveObject"), harken::studio::ProjectItem::Type::Sound);
    auto* inspected = manager.createItem(nullptr, QStringLiteral("IndependentInspector"), harken::studio::ProjectItem::Type::Sound);
    auto* container = manager.createItem(nullptr, QStringLiteral("IndependentAudition"), harken::studio::ProjectItem::Type::Container);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(inspected, nullptr);
    ASSERT_NE(container, nullptr);
    auto* auditioned = manager.createItem(container, QStringLiteral("AuditionChild"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(auditioned, nullptr);
    auto* router = window_->findChild<harken::studio::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    router->selectItem(active, false);
    window_->openItemInNewInspector(inspected, harken::studio::MainWindow::PreferredOpenArea::CentralTab, false);
    auto* inspector = window_->inspectorShowingItem(inspected);
    ASSERT_NE(inspector, nullptr);
    auto* editor = qobject_cast<harken::studio::GridEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("GridEditor"), container));
    ASSERT_NE(editor, nullptr);
    drainEvents();
    selectChildRow(editor, 0);
    ASSERT_EQ(router->activeItem(), active);
    ASSERT_EQ(inspector->currentContextItem(), inspected);
    ASSERT_EQ(window_->auditionTarget(), auditioned);

    ASSERT_TRUE(manager.deleteItem(active));
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    EXPECT_EQ(editor->boundItem(), container);
    EXPECT_EQ(window_->auditionTarget(), auditioned);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), auditioned);
}

TEST_F(EditorDockLayoutTest, UndoRedoRestoresObjectsWithoutRestoringActiveContexts)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* inspected = manager.createItem(nullptr, QStringLiteral("IndependentInspector"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(inspected, nullptr);
    window_->openItemInNewInspector(inspected, harken::studio::MainWindow::PreferredOpenArea::CentralTab, false);
    auto* inspector = window_->inspectorShowingItem(inspected);
    ASSERT_NE(inspector, nullptr);
    inspector->setPinned(true);
    auto* router = window_->findChild<harken::studio::SelectionRouter*>();
    ASSERT_NE(router, nullptr);
    auto& undo = harken::studio::UndoRedoManager::instance();
    undo.clear();
    const auto created = harken::studio::ActionManager::instance().execute(
        QStringLiteral("create_attenuation"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("ActiveAttenuation")}});
    ASSERT_TRUE(created.success) << created.errorMessage.toStdString();
    const QUuid guid(created.data.value(QStringLiteral("guid")).toString());
    ASSERT_FALSE(guid.isNull());
    auto* active = manager.findItemByGuid(guid);
    ASSERT_NE(active, nullptr);
    router->selectItem(active, false);
    drainEvents();
    ASSERT_EQ(router->activeItem(), active);
    ASSERT_EQ(window_->auditionTarget(), active);
    ASSERT_EQ(inspector->currentContextItem(), inspected);

    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);

    ASSERT_TRUE(undo.redo());
    ASSERT_NE(manager.findItemByGuid(guid), nullptr);
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);

    inspector->setPinned(false);
    drainEvents();
    EXPECT_EQ(router->activeItem(), nullptr);
    EXPECT_EQ(window_->auditionTarget(), nullptr);
    EXPECT_EQ(inspector->currentContextItem(), inspected);
    EXPECT_EQ(window_->findChild<ControlPanel*>()->boundItem(), nullptr);
}

TEST_F(EditorDockLayoutTest, LocalTimelineSelectionPreservesNavigationUntilExplicitInspection)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* segment = manager.createItem(nullptr, QStringLiteral("ClipSegment"), harken::studio::ProjectItem::Type::Segment);
    ASSERT_NE(segment, nullptr);
    auto* track = manager.createItem(segment, QStringLiteral("ClipTrack"), harken::studio::ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* sound = manager.createItem(track, QStringLiteral("ClipTarget"), harken::studio::ProjectItem::Type::Sound);
    auto* active = manager.createItem(nullptr, QStringLiteral("ActiveOutsideTimeline"), harken::studio::ProjectItem::Type::Sound);
    auto* parked = manager.createItem(nullptr, QStringLiteral("ParkedOutsideTimeline"), harken::studio::ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(active, nullptr);
    ASSERT_NE(parked, nullptr);
    track->comp<harken::studio::VTrack>()->clips = {};
    auto* hierarchy = window_->createHierarchyPanel();
    auto* explorer = window_->findChild<harken::studio::ProjectExplorer*>();
    auto* inspector = window_->createInspectorPanel();
    auto* router = window_->findChild<harken::studio::SelectionRouter*>();
    ASSERT_NE(hierarchy, nullptr);
    ASSERT_NE(explorer, nullptr);
    ASSERT_NE(inspector, nullptr);
    ASSERT_NE(router, nullptr);
    window_->setLastFocusedInspectorPanelId(inspector->panelId());
    auto* timeline = qobject_cast<harken::studio::TimelineEditorPanel*>(
        window_->openStandaloneEditor(QStringLiteral("TimelineEditor"), track));
    ASSERT_NE(timeline, nullptr);
    ASSERT_TRUE(timeline->placeItem(sound, 0, 0, true));
    ASSERT_EQ(timeline->clipCount(0), 1);
    const QUuid clip = timeline->clipGuid(0, 0);
    ASSERT_TRUE(timeline->selectLane(track->guid));
    timeline->setEditCursorVisible(false);
    timeline->fitClips();
    ASSERT_TRUE(explorer->selectProjectItem(parked));
    ASSERT_TRUE(hierarchy->selectProjectItem(active));
    window_->dockOfPanel(timeline)->setAsCurrentTab();
    drainEvents();
    auto* canvas = timeline->findChild<harken::studio::TimelineCanvas*>(QStringLiteral("timelineLaneViewport"));
    ASSERT_NE(canvas, nullptr);
    canvas->repaint();
    ASSERT_EQ(canvas->lastRenderData().clipItems.size(), 1);
    const QRect visibleClip = canvas->lastRenderData().clipItems.front().rect.intersected(canvas->rect());
    ASSERT_GT(visibleClip.width(), 10);
    ASSERT_GT(visibleClip.height(), 0);
    ASSERT_TRUE(timeline->selectedClipGuids().isEmpty());

    clickWidget(canvas, visibleClip.center());
    EXPECT_EQ(timeline->selectedClipGuids(), QSet<QUuid>{clip});
    EXPECT_EQ(router->activeItem(), active);
    EXPECT_EQ(inspector->currentContextItem(), active);
    EXPECT_EQ(window_->auditionTarget(), active);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{parked});
    EXPECT_EQ(hierarchy->selectedItems(), QList<harken::studio::ProjectItem*>{active});

    ASSERT_TRUE(timeline->inspectClip(0, clip));
    drainEvents();
    EXPECT_EQ(router->activeItem(), sound);
    EXPECT_EQ(inspector->currentContextItem(), sound);
    EXPECT_EQ(window_->auditionTarget(), sound);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{sound});
    EXPECT_EQ(hierarchy->selectedItems(), QList<harken::studio::ProjectItem*>{active});
    ASSERT_NE(inspector->placementContext(), nullptr);
    ASSERT_FALSE(inspector->placementContext()->empty());
    EXPECT_EQ(inspector->placementContext()->occurrences().front().clipGuid, clip);

    ASSERT_TRUE(hierarchy->selectProjectItem(active));
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, visibleClip.center(),
                            canvas->mapToGlobal(visibleClip.center()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &doubleClick);
    drainEvents();
    EXPECT_EQ(router->activeItem(), sound);
    EXPECT_EQ(inspector->currentContextItem(), sound);
    EXPECT_EQ(window_->auditionTarget(), sound);
    EXPECT_EQ(explorer->selectedItems(), QList<harken::studio::ProjectItem*>{sound});
    EXPECT_EQ(hierarchy->selectedItems(), QList<harken::studio::ProjectItem*>{active});
    ASSERT_NE(inspector->placementContext(), nullptr);
    ASSERT_FALSE(inspector->placementContext()->empty());
    EXPECT_EQ(inspector->placementContext()->occurrences().front().clipGuid, clip);
}

TEST_F(EditorDockLayoutTest, EventIdentityIndexSurvivesObjectEdits)
{
    auto& manager = harken::studio::ProjectManager::instance();
    auto* variable = manager.createItem(nullptr, QStringLiteral("Intensity"), harken::studio::ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    harken::studio::Monitor::CapturedEvent event;
    event.header.stableId = variable->runtimeId();
    event.objectGuid = variable->guid;
    event.projectGuid = QUuid(manager.currentProject()->projectId);
    auto* capture = window_->eventCaptureController();
    ASSERT_NE(capture, nullptr);
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Intensity_Renamed")));
    EXPECT_EQ(capture->matchingProjectObject(event), variable->objectID);
    auto* copy = manager.duplicateItem(variable);
    ASSERT_NE(copy, nullptr);
    EXPECT_NE(copy->runtimeId(), event.header.stableId);
    auto copiedEvent = event;
    copiedEvent.header.stableId = copy->runtimeId();
    copiedEvent.objectGuid = copy->guid;
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    ASSERT_TRUE(manager.deleteItem(variable));
    EXPECT_EQ(capture->matchingProjectObject(event), 0u);
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), copy->objectID);
    copiedEvent.projectGuid = QUuid::createUuid();
    EXPECT_EQ(capture->matchingProjectObject(copiedEvent), 0u);
}

TEST(StandaloneEditorIpcEventTest,
     AuditionSeekRequestIdentitySurvivesIpc)
{
    ensureApp();
    auto& projects = harken::studio::ProjectManager::instance();
    if (projects.isProjectOpen())
        projects.closeProject();
    harken::studio::ActionManager::instance().clearAllForTesting();

    QTcpSocket socket;
    auto window = std::make_unique<harken::studio::MainWindow>();
    const auto cleanup = qScopeGuard([&]() {
        socket.abort();
        harken::studio::UndoRedoManager::instance().clear();
        if (projects.isProjectOpen())
            projects.closeProject();
        QObject::disconnect(&harken::studio::UndoRedoManager::instance(), nullptr, window.get(), nullptr);
        window.reset();
        harken::studio::ActionManager::instance().setTransactionCallbacks({});
        harken::studio::ActionManager::instance().clearAllForTesting();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });

    auto* server = window->ipcServer();
    ASSERT_NE(server, nullptr);
    server->stop();
    ASSERT_TRUE(server->start(0));
    ASSERT_GT(server->port(), 0);

    socket.connectToHost(QHostAddress::LocalHost,
                         static_cast<quint16>(server->port()));
    ASSERT_TRUE(harken::studio::TestSupport::spinUntil([&]() {
        return socket.state() == QAbstractSocket::ConnectedState
            && server->clientCount() == 1;
    }));

    auto* audition = window->auditionManager();
    ASSERT_NE(audition, nullptr);
    for (const quint64 requestId : {9007199254740993ULL, 9007199254740995ULL}) {
        audition->auditionSeekFinished(
            42, requestId, harken::studio::bridge::kEngineOk, true);
        ASSERT_TRUE(harken::studio::TestSupport::spinUntil(
            [&]() { return socket.canReadLine(); }));

        QJsonParseError error;
        const QJsonDocument message = QJsonDocument::fromJson(
            socket.readLine().trimmed(), &error);
        ASSERT_EQ(error.error, QJsonParseError::NoError);
        ASSERT_TRUE(message.isObject());
        const QJsonObject event = message.object();
        const QJsonObject data = event.value(QStringLiteral("data")).toObject();
        EXPECT_EQ(data.value(QStringLiteral("audition_seek_request_id")).toString(),
                  QString::number(requestId));
    }
}
