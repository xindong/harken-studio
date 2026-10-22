#include "UI/EngineIntegrationsPage.h"
#include "UI/ProjectsPage.h"
#include "UI/VersionsPage.h"
#include "UI/IntegrationTaskDialog.h"

#include "Integrations/EngineTargetStore.h"
#include "Integrations/EngineTargetProbe.h"
#include "Integrations/IntegrationStore.h"
#include "UnityIntegrationService.h"
#include "UnityInstallation.h"
#include "Platform/PlatformPaths.h"
#include "Projects/ProjectStore.h"
#include "Settings/HubSettings.h"
#include "Support/StudioProtocolTestUtils.h"
#include "Support/UnityProjectLock.h"
#include "Transactions/InstallIndex.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QAction>
#include <QAbstractItemView>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QThreadPool>
#include <QThread>
#include <QTimer>
#include <QTranslator>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>

#include <memory>

namespace {

QDialog* activeActionDialog()
{
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    return dialog && dialog->objectName() != QStringLiteral("sdkSelectionDialog")
        && dialog->objectName() != QStringLiteral("integrationTaskProgress") ? dialog : nullptr;
}

void writeFile(const QString& path, const QByteArray& contents)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(contents), contents.size());
}

class EngineIntegrationsPageTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(dir.isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir.filePath("user").toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", dir.filePath("machine").toUtf8());
        qputenv("HARKEN_HUB_VERSIONS_ROOT", dir.filePath("versions").toUtf8());
        ASSERT_TRUE(integrations.open(dir.filePath("integrations.db")));
        ASSERT_TRUE(targets.open(dir.filePath("targets.db")));
        ASSERT_TRUE(projects.open(dir.filePath("projects.db")));
        harkenPath = dir.filePath("Demo.hsp");
        executablePath = dir.filePath("studio/HarkenStudio.exe");
        unityPath = dir.filePath("UnityGame");
        sdkPath = dir.filePath("studio/HarkenSDK/Unity/Harken");
        writeFile(harkenPath, "{}");
        writeFile(executablePath, "studio");
        for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
            ASSERT_TRUE(QDir().mkpath(QDir(unityPath).filePath(folder)));
        page = std::make_unique<VNH::EngineIntegrationsPage>(
            &integrations, &targets, &projects);
        page->resize(900, 700);
        page->show();
    }

    void TearDown() override
    {
        page.reset();
        EXPECT_TRUE(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_VERSIONS_ROOT");
    }

    QPushButton* button(const QString& text) const
    {
        for (auto* candidate : page->findChildren<QPushButton*>()) {
            if (candidate->text() == text)
                return candidate;
        }
        return nullptr;
    }

    QLineEdit* field(const QString& placeholder) const
    {
        for (auto* candidate : page->findChildren<QLineEdit*>()) {
            if (candidate->placeholderText() == placeholder)
                return candidate;
        }
        return nullptr;
    }

    QPushButton* primary() const
    {
        return page->findChild<QPushButton*>(QStringLiteral("sdkPrimaryAction"));
    }

    QPushButton* checkButton() const
    {
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        if (dialog && dialog->isVisible())
            return dialog->findChild<QPushButton*>(QStringLiteral("sdkChangeCheck"));
        return page->findChild<QPushButton*>(QStringLiteral("verifySdk"));
    }

    QAction* action(const QString& name) const
    {
        return page->findChild<QAction*>(name);
    }

    void applySdkAndClose()
    {
        checkButton()->click();
        primary()->click();
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        EXPECT_TRUE(dialog->isVisible());
        page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    }

    void startSdkSelection()
    {
        auto* browse = page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"));
        if (!browse->isVisible()) {
            auto* change = page->findChild<QPushButton*>(QStringLiteral("changeSdk"));
            ASSERT_TRUE(QTest::qWaitFor([&]() { return change->isEnabled(); }, 5000));
            change->click();
        }
        ASSERT_TRUE(browse->isVisible());
    }

    void selectSdkFolder(const QString& path)
    {
        startSdkSelection();
        page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"))->click();
        auto* dialog = page->findChild<QFileDialog*>();
        ASSERT_NE(dialog, nullptr);
        emit dialog->fileSelected(path);
        dialog->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    bool waitForVersion() const
    {
        return QTest::qWaitFor([&]() {
            return checkButton()->isEnabled();
        }, 5000);
    }

    QTableWidget* steps() const
    {
        for (auto* candidate : page->findChildren<QTableWidget*>()) {
            if (candidate->columnCount() == 3)
                return candidate;
        }
        return nullptr;
    }

    void makeSdk()
    {
        writeFile(QDir(sdkPath).filePath("package.json"),
                  R"({"name":"com.harken.core","version":"1.0.0"})");
        QJsonArray plugins;
        QJsonObject expected;
        for (const auto* platform : {"Windows", "macOS", "Linux"}) {
            const QString runtime = QString("Runtime/Plugins/%1/runtime.bin").arg(platform);
            writeFile(QDir(sdkPath).filePath(runtime), "runtime");
            plugins.append(QJsonObject{{"platform", platform},
                                       {"alternatives", QJsonArray{runtime}}});
            expected.insert(platform, QJsonArray{platform});
        }
        writeFile(QDir(sdkPath).filePath("harken-integration.json"),
                  QJsonDocument(QJsonObject{
                      {"schema_version", 3}, {"package_name", "com.harken.core"},
                      {"supports_custom_install_path", true},
                      {"required_files", QJsonArray{"package.json"}},
                      {"expected_platforms_by_editor_host", expected},
                      {"native_plugins", plugins}}).toJson());
    }

    QTemporaryDir dir{[]() {
        const QString root = QDir::current().filePath(".temp");
        QDir().mkpath(root);
        return QDir(root).filePath("hub-integration-XXXXXX");
    }()};
    VNH::IntegrationStore integrations;
    VNH::EngineTargetStore targets;
    VNH::ProjectStore projects;
    std::unique_ptr<VNH::EngineIntegrationsPage> page;
    QString harkenPath;
    QString executablePath;
    QString unityPath;
    QString sdkPath;
};

TEST_F(EngineIntegrationsPageTest, LongSdkTasksKeepEventsAndProgressResponsiveUntilCompletion)
{
    QSemaphore release;
    bool background = false;
    bool sawProgress = false;
    int ticks = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, page.get(), [&]() {
        ++ticks;
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("integrationTaskProgress"));
        if (!dialog || !dialog->isVisible()) return;
        const auto* label = dialog->findChild<QLabel*>(QStringLiteral("integrationTaskStatus"));
        sawProgress = label && label->text().contains(QStringLiteral("7"));
        EXPECT_FALSE(page->isEnabled());
        dialog->reject();
        dialog->close();
        EXPECT_TRUE(dialog->isVisible());
        release.release();
        timer.stop();
    });
    timer.start(50);
    const auto* guiThread = QThread::currentThread();
    const auto result = VNH::runIntegrationTask(page.get(), [&]() {
        background = QThread::currentThread() != guiThread;
        for (int index = 0; index < 7; ++index) VNH::IntegrationProgress::inspectedEntry();
        return release.tryAcquire(1, 2500) ? 42 : -1;
    });
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 42);
    EXPECT_TRUE(background);
    EXPECT_TRUE(sawProgress);
    EXPECT_GE(ticks, 2);
    EXPECT_TRUE(page->isEnabled());
    EXPECT_EQ(page->findChild<QDialog*>(QStringLiteral("integrationTaskProgress")), nullptr);
}

TEST_F(EngineIntegrationsPageTest, ReadOnlySdkInspectionCanBeCancelledWithoutClosingItsWorker)
{
    VNH::EngineTargetRecord unreal;
    unreal.engineKind = VNH::EngineKind::Unreal;
    unreal.canonicalRoot = dir.filePath("UnrealGame");
    unreal.descriptorPath = QDir(unreal.canonicalRoot).filePath("Game.uproject");
    writeFile(unreal.descriptorPath, "{}");
    const QString plugin = QDir(unreal.canonicalRoot).filePath("Plugins/Harken/Harken.uplugin");
    writeFile(plugin, R"({"VersionName":"1.0.0"})");
    bool chinese = false;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("integrationTaskProgress"));
        auto* cancel = dialog ? dialog->findChild<QPushButton*>(QStringLiteral("cancelIntegrationInspection")) : nullptr;
        if (cancel && cancel->isVisible()) {
            const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath(chinese ? "scan-progress-zh.png" : "scan-progress-en.png")));
            cancel->click();
            timer.stop();
        }
    });
    for (bool unity : {true, false}) {
        chinese = !unity;
        QTranslator translator;
        if (chinese) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto removeTranslator = qScopeGuard([&]() { QCoreApplication::removeTranslator(&translator); });
        timer.start(50);
        const auto result = VNH::runIntegrationTask(page.get(), [&]() {
            QElapsedTimer elapsed;
            elapsed.start();
            while (elapsed.elapsed() < 2500) {
                if (unity) VNX::UnityInstallation::discover(unityPath);
                else VNH::ProjectIntegrationExecutor::inspectUnreal(unreal);
            }
            return true;
        }, true);
        EXPECT_FALSE(result);
        EXPECT_TRUE(page->isEnabled());
    }
    QFile original(plugin);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), R"({"VersionName":"1.0.0"})");
}

TEST_F(EngineIntegrationsPageTest, UnrealUsesTheSameProjectAndSdkWorkflowInBothLanguages)
{
    const QString originalStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&]() { qApp->setStyleSheet(originalStyle); });
    qApp->setStyleSheet(QStringLiteral(
        "QWidget { background-color: #0D111A; color: #E0E0E0; font-family: 'Segoe UI'; font-size: 12px; }"
        "QPushButton { background-color: #2A3140; border: 1px solid #3D4554; padding: 8px 16px; }"
        "QPushButton:disabled { background-color: #171C28; color: #555555; border-color: #272D3A; }"));
    for (const bool chinese : {false, true}) {
        const QString language = chinese ? "zh" : "en";
        const QString root = dir.filePath(language + "/UnrealGame");
        const QString project = dir.filePath(language + "/Audio.hsp");
        const QString source = dir.filePath(language + "/IndependentSDK");
        QString installed = QDir(root).filePath("Plugins/Harken");
        writeFile(project, "{}");
        writeFile(QDir(root).filePath("Game.uproject"), R"({"EngineAssociation":"5.6"})");
        writeFile(QDir(source).filePath("Harken.uplugin"), R"({"VersionName":"1.0.0"})");
        writeFile(QDir(source).filePath("Runtime.bin"), "runtime");
        writeFile(QDir(source).filePath("Source/HarkenCore/Include/harken/Harken.h"), "#include <harken/ObjectId.h>\n");
        writeFile(QDir(source).filePath("Source/HarkenCore/Include/harken/ObjectId.h"), "header");
        writeFile(source + "/Source/HarkenCore/Lib/Win64/HarkenCoreImport.lib", "import");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll", "runtime");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib", "runtime");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so", "runtime");
        page.reset();
        QTranslator translator;
        if (chinese) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto cleanup = qScopeGuard([&]() {
            page.reset();
            QCoreApplication::removeTranslator(&translator);
        });
        page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
        page->resize(760, 600);
        page->show();
        page->beginEditForProject(project);
        ASSERT_TRUE(waitForVersion());
        auto* browse = button(QCoreApplication::translate("HarkenHub", "Select Game Project"));
        ASSERT_NE(browse, nullptr);
        browse->click();
        auto* picker = page->findChild<QFileDialog*>();
        ASSERT_NE(picker, nullptr);
        emit picker->fileSelected(root);
        picker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* move = page->findChild<QPushButton*>("changeSdkLocation");
        EXPECT_TRUE(move->isVisible());
        EXPECT_FALSE(move->isEnabled());
        EXPECT_FALSE(page->findChild<QWidget*>("sdkSelectionForm")->isVisible());
        const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        auto capture = [&](const QString& stage) {
            QCoreApplication::processEvents();
            if (!screenshots.isEmpty())
                EXPECT_TRUE(page->grab().save(QDir(screenshots).filePath("unreal-" + language + "-" + stage + ".png")));
        };
        capture("unlinked");
        bool cancel = false;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
            auto* dialog = activeActionDialog();
            if (!dialog) return;
            if (auto* path = dialog->findChild<QLineEdit*>("sdkMigrationDestination")) {
                responder.stop();
                const auto resume = qScopeGuard([&]() { responder.start(10); });
                auto* safety = dialog->findChild<QPushButton*>("sdkMigrationSafetyCheck");
                auto* next = dialog->findChild<QPushButton*>("sdkMigrationContinue");
                EXPECT_FALSE(next->isEnabled());
                path->setText("Plugins/Harken/Nested");
                safety->click();
                EXPECT_FALSE(next->isEnabled());
                path->setText("Plugins/Audio/Harken");
                safety->click();
                EXPECT_TRUE(next->isEnabled());
                path->setText("Plugins/Changed");
                EXPECT_FALSE(next->isEnabled());
                path->setText("Plugins/Audio/Harken");
                safety->click();
                if (!screenshots.isEmpty())
                    EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath("unreal-" + language + "-migration.png")));
                if (next->isEnabled()) next->click();
                else dialog->reject();
                return;
            }
            if (cancel) dialog->reject();
            else dialog->accept();
        });
        responder.start(10);
        page->findChild<QPushButton*>("linkProjects")->click();
        const auto count = integrations.list().size();
        ASSERT_EQ(count, chinese ? 2u : 1u);
        selectSdkFolder(source);
        capture("selection");
        checkButton()->click();
        EXPECT_FALSE(QDir(installed).exists());
        applySdkAndClose();
        ASSERT_TRUE(QFileInfo::exists(QDir(installed).filePath("Harken.uplugin")));
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLineEdit*>("installedSdkPath")->text() == QDir::toNativeSeparators(installed);
        }, 5000));
        EXPECT_FALSE(primary()->isVisible());
        EXPECT_EQ(page->findChild<QLineEdit*>("installedSdkPath")->text(), QDir::toNativeSeparators(installed));
        EXPECT_TRUE(page->findChild<QLabel*>("installedSdkVersion")->text().contains("1.0.0"));
        EXPECT_TRUE(QDir(source).removeRecursively());
        checkButton()->click();
        ASSERT_EQ(steps()->rowCount(), 1);
        capture("installed");
        ASSERT_TRUE(move->isEnabled());
        cancel = true;
        move->click();
        EXPECT_TRUE(QFileInfo::exists(QDir(installed).filePath("Harken.uplugin")));
        EXPECT_FALSE(QDir(QDir(root).filePath("Plugins/Audio/Harken")).exists());
        cancel = false;
        move->click();
        installed = QDir(root).filePath("Plugins/Audio/Harken");
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLineEdit*>("installedSdkPath")->text() == QDir::toNativeSeparators(installed);
        }, 5000));
        EXPECT_EQ(integrations.list().size(), count);
        checkButton()->click();
        capture("moved");
        const QDir backups(QDir(root).filePath("Saved/HarkenHub/Backups"));
        const auto folders = backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        ASSERT_EQ(folders.size(), 1);
        QFile journal(backups.filePath(folders.front() + "/migration.json"));
        ASSERT_TRUE(journal.open(QIODevice::ReadOnly));
        VNH::EngineTargetRecord target;
        ASSERT_TRUE(VNH::EngineTargetProbe::resolve(root, &target));
        writeFile(VNH::ProjectIntegrationExecutor::unrealMigrationJournalPath(target), journal.readAll());
        page->showList();
        page->beginEditForProject(project, {}, root);
        ASSERT_TRUE(waitForVersion());
        EXPECT_TRUE(move->isEnabled());
        EXPECT_FALSE(page->findChild<QPushButton*>("uninstallSdk")->isEnabled());
        move->click();
        EXPECT_FALSE(VNH::ProjectIntegrationExecutor::hasPendingUnrealMigration(target));
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLineEdit*>("sdkInstallPath")->text() == "Plugins/Harken";
        }, 5000));
        EXPECT_EQ(integrations.list().size(), count);
        move->click();
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLineEdit*>("sdkInstallPath")->text() == "Plugins/Audio/Harken";
        }, 5000));
        writeFile(QDir(source).filePath("Harken.uplugin"), R"({"VersionName":"2.0.0"})");
        writeFile(QDir(source).filePath("Runtime.bin"), "updated");
        writeFile(QDir(source).filePath("Source/HarkenCore/Include/harken/Harken.h"), "#include <harken/ObjectId.h>\n");
        writeFile(QDir(source).filePath("Source/HarkenCore/Include/harken/ObjectId.h"), "header");
        writeFile(source + "/Source/HarkenCore/Lib/Win64/HarkenCoreImport.lib", "import");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll", "runtime");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib", "runtime");
        writeFile(source + "/Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so", "runtime");
        writeFile(QDir(installed).filePath("Config/Custom.ini"), "custom");
        selectSdkFolder(source);
        applySdkAndClose();
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLabel*>("installedSdkVersion")->text().contains("2.0.0");
        }, 5000));
        auto* uninstall = page->findChild<QPushButton*>("uninstallSdk");
        ASSERT_TRUE(uninstall->isEnabled());
        cancel = true;
        uninstall->click();
        EXPECT_TRUE(QFileInfo::exists(QDir(installed).filePath("Harken.uplugin")));
        cancel = false;
        uninstall->click();
        EXPECT_FALSE(QFileInfo::exists(QDir(installed).filePath("Harken.uplugin")));
        EXPECT_TRUE(QFileInfo::exists(QDir(installed).filePath("Config/Custom.ini")));
        EXPECT_EQ(integrations.list().size(), count);
        ASSERT_TRUE(QTest::qWaitFor([&]() {
            return page->findChild<QLineEdit*>("installedSdkPath")->text().isEmpty();
        }, 5000));
        capture("removed");
    }
}

TEST_F(EngineIntegrationsPageTest, SortedListKeepsSelectionAndActionTargets)
{
    for (const auto* number : {"10", "2"}) {
        const QString suffix = QString::fromLatin1(number);
        VNH::ProjectRecord project;
        project.canonicalPath = dir.filePath("Audio #" + suffix + "/Project" + suffix + ".hsp");
        project.displayName = "Project" + suffix;
        writeFile(project.canonicalPath, "{}");
        ASSERT_TRUE(projects.addProject(project));
        VNH::EngineTargetRecord target;
        target.targetId = suffix;
        target.displayName = "Game" + (suffix == "10" ? QString("2") : QString("10"));
        target.canonicalRoot = dir.filePath(target.displayName);
        ASSERT_TRUE(QDir().mkpath(target.canonicalRoot));
        target.addedAt = "2026-10-21T00:00:00Z";
        ASSERT_TRUE(targets.add(target));
        VNH::IntegrationRecord integration;
        integration.integrationId = suffix;
        integration.harkenProjectPath = project.canonicalPath;
        integration.engineTargetId = suffix;
        integration.createdAt = target.addedAt;
        integration.updatedAt = target.addedAt;
        ASSERT_TRUE(integrations.add(integration, false));
    }
    page->refresh();
    QTableWidget* table = nullptr;
    for (auto* candidate : page->findChildren<QTableWidget*>())
        if (candidate->objectName() == QStringLiteral("engineIntegrationsTable")) table = candidate;
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 2);
    EXPECT_EQ(table->item(0, 0)->text(), "Project2");
    table->selectRow(0);
    const auto clickHeader = [&](int column) {
        auto* header = table->horizontalHeader();
        QTest::mouseClick(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                         QPoint(header->sectionViewportPosition(column) + header->sectionSize(column) / 2,
                                header->height() / 2));
        QCoreApplication::processEvents();
        const auto selected = table->selectionModel()->selectedRows(0);
        ASSERT_EQ(selected.size(), 1);
        EXPECT_EQ(selected.front().data(Qt::UserRole).toString(), "2");
    };
    clickHeader(0);
    EXPECT_EQ(table->item(0, 0)->text(), "Project10");
    clickHeader(1);
    EXPECT_EQ(table->item(0, 1)->text(), "Game2");
    clickHeader(1);
    EXPECT_EQ(table->item(0, 1)->text(), "Game10");
    clickHeader(2);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorSection(), 1);
    EXPECT_EQ(table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
    page->refresh();
    EXPECT_EQ(table->item(0, 1)->text(), "Game10");
    ASSERT_EQ(table->selectionModel()->selectedRows(0).size(), 1);
    EXPECT_EQ(table->selectionModel()->selectedRows(0).front().data(Qt::UserRole).toString(), "2");

    ASSERT_TRUE(QTest::qWaitFor([]() {
        const auto sort = VNH::HubSettings::tableSort(QStringLiteral("engineIntegrations"));
        return sort.column == 1 && sort.order == Qt::DescendingOrder;
    }, 5000));
    VNH::EngineIntegrationsPage reopened(&integrations, &targets, &projects);
    for (auto* candidate : reopened.findChildren<QTableWidget*>()) {
        if (candidate->objectName() != QStringLiteral("engineIntegrationsTable")) continue;
        EXPECT_EQ(candidate->horizontalHeader()->sortIndicatorSection(), 1);
        EXPECT_EQ(candidate->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);
        EXPECT_EQ(candidate->item(0, 0)->data(Qt::UserRole).toString(), "2");
    }
    VNH::ProjectsPage projectsPage(&projects);
    const auto* projectsTable = projectsPage.findChild<QTableWidget*>();
    EXPECT_EQ(projectsTable->horizontalHeader()->sortIndicatorSection(), 0);
    EXPECT_EQ(projectsTable->horizontalHeader()->sortIndicatorOrder(), Qt::AscendingOrder);

    QTextBrowser folderReceiver;
    QList<QUrl> openedFolders;
    QObject::connect(&folderReceiver, &QTextBrowser::anchorClicked, &folderReceiver,
                     [&](const QUrl& url) { openedFolders.append(url); });
    QDesktopServices::setUrlHandler(QStringLiteral("file"), &folderReceiver, "anchorClicked");
    const auto restoreUrlHandler = qScopeGuard([]() {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    });
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString suffix = table->item(row, 0)->data(Qt::UserRole).toString();
        for (int column = 0; column < 2; ++column) {
            const QString folder = column == 0 ? dir.filePath("Audio #" + suffix)
                : dir.filePath(suffix == "2" ? "Game10" : "Game2");
            const QString actionText = column == 0 ? QStringLiteral("Open Harken Project Folder")
                : QStringLiteral("Open Game Project Folder");
            for (const bool contextMenu : {true, false}) {
                const int openedCount = openedFolders.size();
                QTimer::singleShot(0, page.get(), [&]() {
                    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                    ASSERT_NE(menu, nullptr);
                    if (contextMenu) page->refresh();
                    for (auto* action : menu->actions()) {
                        if (action->text() != actionText) continue;
                        EXPECT_TRUE(action->isEnabled());
                        menu->setActiveAction(action);
                        QTest::keyClick(menu, Qt::Key_Return);
                        return;
                    }
                    ADD_FAILURE() << "Project folder action is missing";
                    menu->close();
                });
                if (contextMenu) {
                    const QPoint position = table->visualItemRect(table->item(row, column)).center();
                    QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                                            table->viewport()->mapToGlobal(position));
                    QApplication::sendEvent(table->viewport(), &event);
                } else {
                    table->cellWidget(row, table->columnCount() - 1)->findChild<QToolButton*>()->click();
                }
                ASSERT_EQ(openedFolders.size(), openedCount + 1);
                EXPECT_EQ(openedFolders.last().toLocalFile(), folder);
            }
        }
    }

    QTimer responder;
    bool unlink = false;
    QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu->setActiveAction(unlink ? menu->actions().last() : menu->actions().first());
            QTest::keyClick(menu, Qt::Key_Return);
        } else if (auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            message->button(QMessageBox::Yes)->click();
        }
    });
    responder.start(10);
    table->cellWidget(0, table->columnCount() - 1)->findChild<QToolButton*>()->click();
    EXPECT_EQ(field("Select a .hsp project file")->text(), dir.filePath("Audio #2/Project2.hsp"));
    page->showList();
    unlink = true;
    table->cellWidget(0, table->columnCount() - 1)->findChild<QToolButton*>()->click();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().integrationId, "10");
    EXPECT_TRUE(QFileInfo::exists(dir.filePath("Audio #2/Project2.hsp")));
    EXPECT_EQ(projects.listProjects().size(), 2);
    EXPECT_EQ(targets.list().size(), 2);
}

TEST_F(EngineIntegrationsPageTest, ExistingEditorReturnsBeforeVersionCheckAndKeepsStoredSdk)
{
    VNH::ProjectRecord project;
    project.canonicalPath = harkenPath;
    project.displayName = "Demo";
    ASSERT_TRUE(projects.addProject(project));
    VNH::EngineTargetRecord target;
    target.targetId = "unity";
    target.canonicalRoot = unityPath;
    target.displayName = "UnityGame";
    target.addedAt = "2026-09-20T00:00:00Z";
    ASSERT_TRUE(targets.add(target));
    VNH::IntegrationRecord integration;
    integration.integrationId = "integration";
    integration.harkenProjectPath = harkenPath;
    integration.engineTargetId = target.targetId;
    integration.sdkSourcePath = dir.filePath("custom-sdk");
    integration.createdAt = target.addedAt;
    integration.updatedAt = target.addedAt;
    ASSERT_TRUE(integrations.add(integration, false));

    auto* pool = QThreadPool::globalInstance();
    ASSERT_TRUE(pool->waitForDone(5000));
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore entered;
    QSemaphore release;
    pool->start([&]() { entered.release(); release.acquire(); });
    bool released = false;
    const auto unblock = qScopeGuard([&]() {
        if (!released)
            release.release();
        pool->waitForDone(5000);
        pool->setMaxThreadCount(previousLimit);
    });
    ASSERT_TRUE(entered.tryAcquire(1, 5000));

    page.reset();
    page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
    page->show();
    page->refresh();
    page->refreshStudioVersion();
    QTest::qWait(400);
    EXPECT_TRUE(page->isEnabled());
    EXPECT_EQ(page->findChild<QDialog*>(QStringLiteral("integrationTaskProgress")), nullptr);
    page.reset();
    page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
    page->show();
    page->beginEditForProject(harkenPath, executablePath);
    EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 1);
    EXPECT_FALSE(checkButton()->isEnabled());
    EXPECT_EQ(button("Select Unreal Project"), nullptr);
    EXPECT_EQ(button("Select Unity Project"), nullptr);
    EXPECT_FALSE(page->findChild<QPlainTextEdit*>(QStringLiteral("integrationDetails"))->isVisible());
    EXPECT_EQ(field("Runtime/SDK source path")->text(), integration.sdkSourcePath);

    release.release();
    released = true;
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(field("Runtime/SDK source path")->text(), integration.sdkSourcePath);
}

TEST_F(EngineIntegrationsPageTest, IgnoresVersionResultAfterChangingProjectAndLeavingEditor)
{
    auto* pool = QThreadPool::globalInstance();
    ASSERT_TRUE(pool->waitForDone(5000));
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    const auto restoreLimit = qScopeGuard([&]() { pool->setMaxThreadCount(previousLimit); });
    for (int scenario = 0; scenario < 3; ++scenario) {
        QSemaphore entered;
        QSemaphore release;
        pool->start([&]() { entered.release(); release.acquire(); });
        const auto unblock = qScopeGuard([&]() { release.release(); pool->waitForDone(5000); });
        ASSERT_TRUE(entered.tryAcquire(1, 5000));
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        if (scenario == 0) page->beginEditForProject();
        else if (scenario == 1) page->showList();
        else page.reset();
        release.release();
        ASSERT_TRUE(pool->waitForDone(5000));
        QCoreApplication::processEvents();
        if (!page) continue;
        EXPECT_TRUE(field("Runtime/SDK source path")->text().isEmpty());
        if (scenario != 0)
            EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 0);
    }
}

TEST_F(EngineIntegrationsPageTest, InvalidOrClearedProjectDiscardsPendingVersionAndCanRecover)
{
    auto* pool = QThreadPool::globalInstance();
    ASSERT_TRUE(pool->waitForDone(5000));
    const int previousLimit = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    const auto restoreLimit = qScopeGuard([&]() {
        pool->setMaxThreadCount(previousLimit);
    });
    for (const bool browse : {false, true}) {
        QSemaphore entered;
        QSemaphore release;
        pool->start([&]() { entered.release(); release.acquire(); });
        bool released = false;
        const auto unblock = qScopeGuard([&]() {
            if (!released)
                release.release();
            pool->waitForDone(5000);
        });
        ASSERT_TRUE(entered.tryAcquire(1, 5000));
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        auto* harkenField = field("Select a .hsp project file");
        auto* sdkField = field("Runtime/SDK source path");
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, page.get(), [&]() {
            auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (warning)
                warning->accept();
        });
        dismiss.start(10);
        const QString invalidPath = dir.filePath("Missing.hsp");
        if (browse) {
            harkenField->parentWidget()->findChild<QPushButton*>()->click();
            auto* dialog = page->findChild<QFileDialog*>();
            ASSERT_NE(dialog, nullptr);
            emit dialog->fileSelected(invalidPath);
            dialog->reject();
        } else {
            harkenField->selectAll();
            QTest::keyClicks(harkenField, invalidPath);
            EXPECT_TRUE(sdkField->text().isEmpty());
            ASSERT_TRUE(QMetaObject::invokeMethod(harkenField, "editingFinished"));
        }
        EXPECT_TRUE(sdkField->text().isEmpty());
        release.release();
        released = true;
        ASSERT_TRUE(pool->waitForDone(5000));
        QCoreApplication::processEvents();
        EXPECT_TRUE(sdkField->text().isEmpty());
        EXPECT_TRUE(checkButton()->isEnabled());

        harkenField->setText(harkenPath);
        ASSERT_TRUE(QMetaObject::invokeMethod(harkenField, "editingFinished"));
        ASSERT_TRUE(waitForVersion());
        EXPECT_EQ(sdkField->text(), sdkPath);
        harkenField->selectAll();
        QTest::keyClick(harkenField, Qt::Key_Backspace);
        EXPECT_TRUE(sdkField->text().isEmpty());
    }
}

TEST_F(EngineIntegrationsPageTest, ManagedVersionStillChecksIntegrityAndPreservesSdkEditedWhileLoading)
{
    VNH::InstalledVersionRecord installed;
    installed.buildId = "test-build";
    installed.semanticVersion = "1.0.0";
    installed.healthState = "Installed";
    installed.installPath = dir.filePath("versions/test-build");
    const QString binary = QDir(installed.installPath).filePath(VNH::studioBinaryRelativePath());
    const QByteArray contents("studio");
    writeFile(binary, contents);
    writeFile(QDir(installed.installPath).filePath("package-manifest.json"),
              QJsonDocument(QJsonObject{
                  {"product", "HarkenStudio"}, {"buildId", installed.buildId},
                  {"version", installed.semanticVersion},
                  {"files", QJsonArray{QJsonObject{
                      {"path", VNH::studioBinaryRelativePath()}, {"size", contents.size()},
                      {"sha256", QString::fromLatin1(QCryptographicHash::hash(
                          contents, QCryptographicHash::Sha256).toHex())}}}}}).toJson());
    writeFile(QDir(installed.installPath).filePath("capabilities.json"),
              QJsonDocument(HarkenHubTest::studioCapabilities(
                  installed.buildId, installed.semanticVersion)).toJson());
    writeFile(harkenPath,
              R"({"harkenDocument":{"type":"project.root","savedStudioVersion":"1.0.0"}})");
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(installed, 1, &error));
    sdkPath = QDir(QFileInfo(binary).absolutePath()).filePath("HarkenSDK/Unity/Harken");
    makeSdk();

    page->beginEditForProject(harkenPath, {}, unityPath);
    auto* sdkField = field("Runtime/SDK source path");
    const QString customSdk = dir.filePath("custom-sdk");
    selectSdkFolder(customSdk);
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(sdkField->text(), customSdk);
    selectSdkFolder(sdkPath);
    EXPECT_EQ(sdkField->text(), sdkPath);
    selectSdkFolder(customSdk);

    writeFile(binary, "broken");
    page->refreshStudioVersion();
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(sdkField->text(), customSdk);
}

TEST_F(EngineIntegrationsPageTest, BrowseOpensWithoutRepeatingUnchangedProjectValidation)
{
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    auto* harkenField = field("Select a .hsp project file");
    ASSERT_NE(harkenField, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(harkenField, "editingFinished"));
    EXPECT_TRUE(checkButton()->isEnabled());
    const auto browseButtons = harkenField->parentWidget()->findChildren<QPushButton*>();
    ASSERT_EQ(browseButtons.size(), 1);
    browseButtons.front()->click();
    auto* dialog = page->findChild<QFileDialog*>();
    ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(dialog->isVisible());
    EXPECT_TRUE(dialog->testOption(QFileDialog::DontUseCustomDirectoryIcons));
    dialog->reject();
}

TEST_F(EngineIntegrationsPageTest, ExternalSdkInstallsWithoutStudioAndFolderCancelKeepsSelection)
{
    sdkPath = dir.filePath("standalone-sdk");
    makeSdk();
    ASSERT_TRUE(QFile::remove(executablePath));
    page->beginEditForProject(harkenPath, {}, unityPath);
    ASSERT_TRUE(waitForVersion());
    selectSdkFolder(sdkPath);
    auto* browse = page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"));
    ASSERT_NE(browse, nullptr);
    auto* source = page->findChild<QLineEdit*>(QStringLiteral("sdkSourcePath"));
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->text(), sdkPath);
    EXPECT_TRUE(source->isReadOnly());
    EXPECT_TRUE(primary()->isEnabled());
    checkButton()->click();
    const int rows = steps()->rowCount();
    ASSERT_GT(rows, 0);
    browse->click();
    auto* dialog = page->findChild<QFileDialog*>();
    ASSERT_NE(dialog, nullptr);
    dialog->reject();
    EXPECT_EQ(field("Runtime/SDK source path")->text(), sdkPath);
    EXPECT_EQ(steps()->rowCount(), rows);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
        auto* confirmation = activeActionDialog();
        if (confirmation) confirmation->accept();
    });
    responder.start(10);
    applySdkAndClose();
    EXPECT_TRUE(integrations.list().empty());
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().sdkSourcePath, sdkPath);
    EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core/package.json")));
    EXPECT_FALSE(primary()->isVisible());
    page->showList();
    page->beginEditForProject(harkenPath, {}, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(field("Runtime/SDK source path")->text(), sdkPath);
    writeFile(QDir(sdkPath).filePath("Runtime/Added.cs"), "new SDK component");
    startSdkSelection();
    applySdkAndClose();
    EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core/Runtime/Added.cs")));
}

TEST_F(EngineIntegrationsPageTest, RequiresClosedUnityBeforeCheckingAndRechecksAfterConfirmation)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
    auto* engineHint = page->findChild<QLabel*>(QStringLiteral("sdkEngineRunningHint"));
    ASSERT_NE(engineHint, nullptr);
    EXPECT_FALSE(engineHint->isVisible());
    const QString installed = QDir(unityPath).filePath("Packages/com.harken.core");
    HarkenHubTest::UnityProjectLock unityLock;
    ASSERT_TRUE(unityLock.lock(unityPath));
    ASSERT_TRUE(QTest::qWaitFor([&]() { return engineHint->isVisible(); }, 5000));
    int extraDialogs = 0;
    bool inlineProgressSeen = false;
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, page.get(), [&]() {
        if (auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget()); active && active != dialog) {
            ++extraDialogs;
            active->reject();
        }
        if (page->findChild<QWidget*>(QStringLiteral("sdkInlineProgress"))->isVisible()) {
            inlineProgressSeen = true;
            dialog->close();
            EXPECT_TRUE(dialog->isVisible());
        }
    });
    inspect.start(0);
    primary()->click();
    EXPECT_FALSE(primary()->isEnabled());
    EXPECT_TRUE(checkButton()->isEnabled());
    EXPECT_EQ(steps()->rowCount(), 1);
    EXPECT_FALSE(QDir(installed).exists());
    ASSERT_TRUE(unityLock.unlock());
    ASSERT_TRUE(QTest::qWaitFor([&]() { return !engineHint->isVisible(); }, 5000));
    QTimer stopScan;
    bool stopped = false;
    QObject::connect(&stopScan, &QTimer::timeout, page.get(), [&]() {
        auto* stop = page->findChild<QWidget*>(QStringLiteral("sdkInlineProgress"))
            ->findChild<QPushButton*>(QStringLiteral("cancelIntegrationInspection"));
        if (!stop->isVisible()) return;
        stopScan.stop();
        stop->click();
        stopped = true;
    });
    stopScan.start(0);
    checkButton()->click();
    EXPECT_TRUE(stopped);
    EXPECT_FALSE(QDir(installed).exists());
    checkButton()->click();
    EXPECT_TRUE(primary()->isEnabled());
    EXPECT_FALSE(QDir(installed).exists());
    writeFile(QDir(sdkPath).filePath("Runtime/Added.cs"), "new content after review");
    primary()->click();
    EXPECT_FALSE(QDir(installed).exists());
    EXPECT_TRUE(primary()->isEnabled());
    EXPECT_TRUE(page->findChild<QLabel*>(QStringLiteral("sdkSelectionSummary"))->text().contains("confirm again"));
    ASSERT_TRUE(unityLock.lock(unityPath));
    primary()->click();
    EXPECT_FALSE(primary()->isEnabled());
    EXPECT_FALSE(QDir(installed).exists());
    ASSERT_TRUE(unityLock.unlock());
    checkButton()->click();
    primary()->click();
    EXPECT_TRUE(QFileInfo::exists(installed + "/Runtime/Added.cs"));
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_TRUE(dialog->isVisible());
    EXPECT_TRUE(page->findChild<QPushButton*>(QStringLiteral("sdkOpenReport"))->isVisible());
    EXPECT_FALSE(engineHint->isVisible());
    EXPECT_EQ(extraDialogs, 0);
    EXPECT_TRUE(inlineProgressSeen);
}

TEST_F(EngineIntegrationsPageTest, QuickSdkChangePreservesSafetyAndHandlesSameVersionContent)
{
    const bool chinese = qEnvironmentVariable("HARKEN_HUB_TEST_LANGUAGE") == QStringLiteral("zh");
    const QString language = chinese ? QStringLiteral("zh") : QStringLiteral("en");
    QTranslator translator;
    if (chinese) {
        ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
        QCoreApplication::installTranslator(&translator);
        page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
        page->resize(900, 700);
        page->show();
    }
    const auto removeTranslator = qScopeGuard([&]() {
        page.reset();
        QCoreApplication::removeTranslator(&translator);
    });
    makeSdk();
    const QString originalSource = sdkPath;
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityPath;
    request.sdkSourcePath = originalSource;
    ASSERT_TRUE(service.run(request).success);
    const QString installed = QDir(unityPath).filePath("Packages/com.harken.core");
    writeFile(installed + "/package.json.meta", "guid: keep-this-guid");
    writeFile(installed + "/game-settings.txt", "keep-game-settings");
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    QTimer approveLink;
    QObject::connect(&approveLink, &QTimer::timeout, page.get(), []() {
        if (auto* dialog = activeActionDialog()) dialog->accept();
    });
    approveLink.start(10);
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    approveLink.stop();
    ASSERT_EQ(integrations.list().size(), 1);
    const auto saved = integrations.list().front();
    page->showList();

    VNH::InstalledVersionRecord studio;
    studio.buildId = "same-sdk-version-new-runtime";
    studio.semanticVersion = "2.0.0";
    studio.healthState = "Installed";
    studio.installPath = dir.filePath("versions/new-runtime");
    const QString binary = QDir(studio.installPath).filePath(VNH::studioBinaryRelativePath());
    writeFile(binary, "studio");
    writeFile(QDir(studio.installPath).filePath("package-manifest.json"),
              QJsonDocument(QJsonObject{{"product", "HarkenStudio"},
                  {"buildId", studio.buildId}, {"version", studio.semanticVersion}}).toJson());
    writeFile(QDir(studio.installPath).filePath("capabilities.json"),
              QJsonDocument(HarkenHubTest::studioCapabilities(
                  studio.buildId, studio.semanticVersion)).toJson());
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(studio, 1, &error));
    sdkPath = QDir(QFileInfo(binary).absolutePath()).filePath("HarkenSDK/Unity/Harken");
    makeSdk();
    const QString runtime = "Runtime/Plugins/Windows/runtime.bin";
    writeFile(QDir(sdkPath).filePath(runtime), "updated-runtime");
    page->refreshStudioVersion();
    auto* table = page->findChild<QTableWidget*>(QStringLiteral("engineIntegrationsTable"));
    ASSERT_NE(table, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&]() { return table->item(0, 4)->text().contains("1.0.0"); }, 5000));
    EXPECT_TRUE(table->item(0, 4)->toolTip().contains(studio.buildId));
    const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
    if (!screenshots.isEmpty())
        EXPECT_TRUE(page->grab().save(QDir(screenshots).filePath("sdk-quick-list-" + language + ".png")));

    int confirmations = 0;
    const auto chooseSource = [&](bool approve) {
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                if (menu->objectName() == QStringLiteral("quickSdkSources")) {
                    for (auto* action : menu->actions()) {
                        if (action->data().toString() != sdkPath) continue;
                        EXPECT_TRUE(action->isEnabled());
                        EXPECT_FALSE(action->isChecked());
                        EXPECT_TRUE(action->text().contains(QCoreApplication::translate(
                            "HarkenHub", " (Same version number as installed)")));
                        menu->setActiveAction(action);
                        if (!screenshots.isEmpty())
                            EXPECT_TRUE(menu->grab().save(QDir(screenshots).filePath("sdk-quick-menu-" + language + ".png")));
                        QTest::keyClick(menu, Qt::Key_Return);
                        break;
                    }
                } else {
                    for (auto* action : menu->actions()) {
                        if (!action->menu()) continue;
                        menu->setActiveAction(action);
                        QTest::keyClick(menu, Qt::Key_Right);
                        break;
                    }
                }
            } else if (auto* dialog = activeActionDialog()) {
                ADD_FAILURE() << "Unexpected extra SDK dialog";
                dialog->reject();
            }
        });
        responder.start(10);
        table->cellWidget(0, table->columnCount() - 1)->findChild<QToolButton*>()->click();
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        EXPECT_TRUE(dialog->isVisible());
        if (primary()->isEnabled()) {
            ++confirmations;
            if (!screenshots.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath("sdk-quick-review-" + language + ".png")));
            if (approve) primary()->click();
        }
    };
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    HarkenHubTest::UnityProjectLock lock;
    ASSERT_TRUE(lock.lock(unityPath));
    chooseSource(true);
    EXPECT_EQ(confirmations, 0);
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "runtime");
    EXPECT_EQ(integrations.list().front().updatedAt, saved.updatedAt);
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    ASSERT_TRUE(lock.unlock());

    chooseSource(false);
    EXPECT_EQ(confirmations, 1);
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "runtime");
    EXPECT_EQ(integrations.list().front().sdkSourcePath, saved.sdkSourcePath);
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();

    chooseSource(true);
    EXPECT_EQ(confirmations, 2);
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "updated-runtime");
    EXPECT_EQ(read(installed + "/package.json.meta"), "guid: keep-this-guid");
    EXPECT_EQ(read(installed + "/game-settings.txt"), "keep-game-settings");
    EXPECT_EQ(integrations.list().front().integrationId, saved.integrationId);
    EXPECT_EQ(integrations.list().front().sdkSourcePath, sdkPath);
    auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
    EXPECT_TRUE(dialog->isVisible());
    EXPECT_TRUE(table->isVisible());
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_FALSE(page->findChild<QWidget*>(QStringLiteral("sdkSelectionForm"))->isVisible());
    const auto* log = page->findChild<QTextBrowser*>(QStringLiteral("sdkSelectionDetails"));
    ASSERT_NE(log, nullptr);
    EXPECT_TRUE(log->toPlainText().contains(".json"));
    if (!screenshots.isEmpty())
        EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath("sdk-quick-result-" + language + ".png")));
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    EXPECT_FALSE(dialog->isVisible());

    const auto inspectCurrentSource = [&](const char* state, const QString& stage) {
        page->refresh();
        const QString stateText = QCoreApplication::translate("HarkenHub", state);
        ASSERT_TRUE(QTest::qWaitFor([&]() { return table->item(0, 3)->text().contains(stateText); }, 5000));
        EXPECT_TRUE(table->item(0, 4)->text().contains(QCoreApplication::translate(
            "HarkenHub", "Installed: %1").arg("1.0.0")));
        bool inspected = false;
        QMenu* rootMenu = nullptr;
        QTimer responder;
        QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            if (menu->objectName() != QStringLiteral("quickSdkSources")) {
                rootMenu = menu;
                for (auto* action : menu->actions()) {
                    if (!action->menu()) continue;
                    menu->setActiveAction(action);
                    QTest::keyClick(menu, Qt::Key_Right);
                    break;
                }
                return;
            }
            EXPECT_TRUE(menu->actions().front()->text().contains(stateText));
            for (auto* action : menu->actions()) {
                if (action->data().toString() != sdkPath) continue;
                inspected = true;
                EXPECT_TRUE(action->isChecked());
                EXPECT_TRUE(action->text().contains(QCoreApplication::translate(
                    "HarkenHub", " (Installed source)")));
                menu->setActiveAction(action);
            }
            if (!screenshots.isEmpty()) {
                EXPECT_TRUE(page->grab().save(QDir(screenshots).filePath(
                    "sdk-status-list-" + stage + "-" + language + ".png")));
                EXPECT_TRUE(menu->grab().save(QDir(screenshots).filePath(
                    "sdk-status-menu-" + stage + "-" + language + ".png")));
                EXPECT_TRUE(rootMenu->grab().save(QDir(screenshots).filePath(
                    "sdk-status-actions-" + language + ".png")));
            }
            responder.stop();
            menu->close();
            if (rootMenu) rootMenu->close();
        });
        responder.start(10);
        table->cellWidget(0, table->columnCount() - 1)->findChild<QToolButton*>()->click();
        EXPECT_TRUE(inspected);
    };
    inspectCurrentSource("Files match installation", "unchanged");
    writeFile(QDir(installed).filePath(runtime), "customized-runtime");
    inspectCurrentSource("Files modified", "modified");
    EXPECT_TRUE(table->item(0, 3)->toolTip().contains(runtime));
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "customized-runtime");
    const QString manifestPath = installed + "/harken-integration.json";
    const QByteArray manifest = read(manifestPath);
    ASSERT_TRUE(QFile::remove(manifestPath));
    inspectCurrentSource("Files modified and missing", "modified-and-missing");
    EXPECT_TRUE(table->item(0, 3)->toolTip().contains("harken-integration.json"));
    EXPECT_TRUE(table->item(0, 3)->toolTip().contains(runtime));
    EXPECT_FALSE(QFileInfo::exists(manifestPath));
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "customized-runtime");
    writeFile(QDir(installed).filePath(runtime), "updated-runtime");
    inspectCurrentSource("Files missing", "missing");
    EXPECT_FALSE(QFileInfo::exists(manifestPath));
    writeFile(manifestPath, manifest);
    inspectCurrentSource("Files match installation", "restored");
    writeFile(QDir(installed).filePath(runtime), "customized-runtime");
    const QString receipt = installed + "/.harken-integration-state.json";
    const QByteArray installedReceipt = read(receipt);
    writeFile(receipt, "{}");
    inspectCurrentSource("File verification incomplete", "unverified");
    EXPECT_EQ(read(QDir(installed).filePath(runtime)), "customized-runtime");
    writeFile(receipt, installedReceipt);

    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    EXPECT_TRUE(primary()->isVisible());
    EXPECT_TRUE(page->findChild<QWidget*>(QStringLiteral("sdkSelectionForm"))->isVisible());
}

TEST_F(EngineIntegrationsPageTest, SdkSelectionCancelKeepsSavedStateAndApplyUpdatesIt)
{
    makeSdk();
    const QString originalSource = sdkPath;
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityPath;
    request.sdkSourcePath = originalSource;
    ASSERT_TRUE(service.run(request).success);
    const QString installed = QDir(unityPath).filePath("Packages/com.harken.core");
    writeFile(installed + "/package.json.meta", "guid: stable-package-guid");
    VNH::InstalledVersionRecord studio;
    studio.buildId = "sdk-update-build";
    studio.semanticVersion = "2.0.0";
    studio.healthState = "Installed";
    studio.installPath = dir.filePath("versions/sdk-update-build");
    const QString binary = QDir(studio.installPath).filePath(VNH::studioBinaryRelativePath());
    writeFile(binary, "studio");
    writeFile(QDir(studio.installPath).filePath("package-manifest.json"),
              QJsonDocument(QJsonObject{{"product", "HarkenStudio"},
                  {"buildId", studio.buildId}, {"version", studio.semanticVersion}}).toJson());
    writeFile(QDir(studio.installPath).filePath("capabilities.json"),
              QJsonDocument(HarkenHubTest::studioCapabilities(
                  studio.buildId, studio.semanticVersion)).toJson());
    QString error;
    ASSERT_TRUE(VNH::InstallIndex::commitInstallation(studio, 1, &error));
    sdkPath = QDir(QFileInfo(binary).absolutePath()).filePath("HarkenSDK/Unity/Harken");
    makeSdk();
    writeFile(QDir(sdkPath).filePath("package.json"),
              R"({"name":"com.harken.core","version":"2.0.0"})");
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    QTimer approve;
    QObject::connect(&approve, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (dialog) dialog->accept();
    });
    approve.start(10);
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    const auto saved = integrations.list().front();
    auto* browse = page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"));
    auto* version = page->findChild<QLabel*>(QStringLiteral("installedSdkVersion"));
    auto* savedPath = page->findChild<QLineEdit*>(QStringLiteral("installedSdkPath"));
    EXPECT_FALSE(browse->isVisible());
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_TRUE(savedPath->isReadOnly());
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());

    const auto selectInstalledSdk = [&](bool clickArrow) {
        startSdkSelection();
        auto* sources = page->findChild<QComboBox*>(QStringLiteral("sdkSourceSelector"));
        ASSERT_NE(sources, nullptr);
        const int index = sources->findData(sdkPath);
        ASSERT_GE(index, 0);
        if (clickArrow) {
            QStyleOptionComboBox option;
            option.initFrom(sources);
            option.editable = sources->isEditable();
            const QRect arrow = sources->style()->subControlRect(
                QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, sources);
            QTest::mouseClick(sources, Qt::LeftButton, Qt::NoModifier, arrow.center());
        } else {
            QTest::mouseClick(sources->lineEdit(), Qt::LeftButton);
        }
        QCoreApplication::processEvents();
        ASSERT_TRUE(sources->view()->isVisible());
        const auto item = sources->model()->index(index, 0);
        sources->view()->scrollTo(item);
        const QRect itemRect = sources->view()->visualRect(item).intersected(
            sources->view()->viewport()->rect());
        ASSERT_FALSE(itemRect.isEmpty());
        QTest::mouseMove(sources->view()->viewport(), itemRect.center());
        QTest::mouseClick(sources->view()->viewport(), Qt::LeftButton, Qt::NoModifier,
                          itemRect.center());
        QCoreApplication::processEvents();
        EXPECT_FALSE(sources->view()->isVisible());
        EXPECT_EQ(sources->lineEdit()->text(), sdkPath);
        const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            EXPECT_TRUE(page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"))->grab().save(
                QDir(screenshots).filePath("sdk-installed-source-en.png")));
    };
    selectInstalledSdk(false);
    EXPECT_TRUE(version->text().contains(QStringLiteral("1.0.0")));
    EXPECT_EQ(savedPath->text(), QDir::toNativeSeparators(installed));
    EXPECT_EQ(integrations.list().front().sdkSourcePath, originalSource);
    EXPECT_EQ(integrations.list().front().updatedAt, saved.updatedAt);
    EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("changeSdkLocation"))->isEnabled());
    EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->isEnabled());
    checkButton()->click();
    EXPECT_TRUE(version->text().contains(QStringLiteral("1.0.0")));
    EXPECT_EQ(integrations.list().front().sdkSourcePath, originalSource);
    const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
    if (!screenshots.isEmpty()) {
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        auto* tabs = dialog->findChild<QTabWidget*>(QStringLiteral("sdkCheckResults"));
        QCoreApplication::processEvents();
        EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath("sdk-changes-en.png")));
        tabs->setCurrentIndex(1);
        EXPECT_TRUE(dialog->grab().save(QDir(screenshots).filePath("sdk-check-log-en.png")));
    }
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    EXPECT_FALSE(browse->isVisible());
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_EQ(field("Runtime/SDK source path")->text(), originalSource);
    EXPECT_EQ(steps()->rowCount(), 0);
    EXPECT_TRUE(version->text().contains(QStringLiteral("1.0.0")));

    selectSdkFolder(sdkPath);
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_FALSE(browse->isVisible());
    EXPECT_EQ(field("Runtime/SDK source path")->text(), originalSource);
    selectInstalledSdk(true);
    checkButton()->click();
    const QString changed = QStringLiteral("Runtime/Plugins/Windows/runtime.bin");
    const QString missing = QStringLiteral("Runtime/Plugins/Linux/runtime.bin");
    writeFile(QDir(installed).filePath(changed), "local customization after review");
    ASSERT_TRUE(QFile::remove(QDir(installed).filePath(missing)));
    writeFile(QDir(sdkPath).filePath("package.json"),
              R"({"name":"com.harken.core","version":"0.9.0"})");
    primary()->click();
    QFile customized(QDir(installed).filePath(changed));
    ASSERT_TRUE(customized.open(QIODevice::ReadOnly));
    EXPECT_EQ(customized.readAll(), "local customization after review");
    customized.close();
    EXPECT_FALSE(QFileInfo::exists(QDir(installed).filePath(missing)));
    EXPECT_TRUE(version->text().contains(QStringLiteral("1.0.0")));
    EXPECT_TRUE(page->findChild<QLabel*>(QStringLiteral("sdkSelectionContext"))->text().contains("Downgrade"));
    const auto* difference = page->findChild<QLabel*>(QStringLiteral("sdkDiffSummary"));
    EXPECT_TRUE(difference->text().contains(QCoreApplication::translate(
        "HarkenHub", "Locally modified files to overwrite or remove: %1").arg(1)));
    EXPECT_TRUE(difference->text().contains(QCoreApplication::translate(
        "HarkenHub", "Missing installed SDK files: %1").arg(1)));
    if (!screenshots.isEmpty()) {
        QCoreApplication::processEvents();
        EXPECT_TRUE(page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"))->grab().save(
            QDir(screenshots).filePath("sdk-review-warning-en.png")));
    }
    writeFile(QDir(sdkPath).filePath("package.json"),
              R"({"name":"com.harken.core","version":"2.0.0"})");
    applySdkAndClose();
    EXPECT_TRUE(version->text().contains(QStringLiteral("2.0.0")));
    EXPECT_EQ(savedPath->text(), QDir::toNativeSeparators(installed));
    EXPECT_EQ(integrations.list().front().integrationId, saved.integrationId);
    EXPECT_EQ(integrations.list().front().sdkSourcePath, sdkPath);
    EXPECT_FALSE(browse->isVisible());
    EXPECT_FALSE(primary()->isVisible());
    QFile meta(installed + "/package.json.meta");
    ASSERT_TRUE(meta.open(QIODevice::ReadOnly));
    EXPECT_EQ(meta.readAll(), "guid: stable-package-guid");
}

TEST_F(EngineIntegrationsPageTest, SourceRefreshPreservesSelectionAndCheckedResults)
{
    makeSdk();
    const QString selectedSource = sdkPath;
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    selectSdkFolder(selectedSource);
    checkButton()->click();
    auto* sources = page->findChild<QComboBox*>(QStringLiteral("sdkSourceSelector"));
    auto* summary = page->findChild<QLabel*>(QStringLiteral("sdkSelectionSummary"));
    ASSERT_NE(sources, nullptr);
    ASSERT_NE(summary, nullptr);
    ASSERT_GT(steps()->rowCount(), 0);
    const QString checkedSummary = summary->text();
    QStringList checkedSteps;
    for (int row = 0; row < steps()->rowCount(); ++row)
        for (int column = 0; column < steps()->columnCount(); ++column)
            checkedSteps << steps()->item(row, column)->text();

    for (int trigger = 0; trigger < 3; ++trigger) {
        VNH::InstalledVersionRecord studio;
        studio.buildId = QStringLiteral("added-studio-%1").arg(trigger);
        studio.semanticVersion = QStringLiteral("2.0.%1").arg(trigger);
        studio.healthState = "Installed";
        studio.installPath = dir.filePath("versions/" + studio.buildId);
        const QString binary = QDir(studio.installPath).filePath(VNH::studioBinaryRelativePath());
        writeFile(binary, "studio");
        writeFile(QDir(studio.installPath).filePath("package-manifest.json"),
                  QJsonDocument(QJsonObject{{"product", "HarkenStudio"},
                      {"buildId", studio.buildId}, {"version", studio.semanticVersion}}).toJson());
        writeFile(QDir(studio.installPath).filePath("capabilities.json"),
                  QJsonDocument(HarkenHubTest::studioCapabilities(
                      studio.buildId, studio.semanticVersion)).toJson());
        sdkPath = QDir(QFileInfo(binary).absolutePath()).filePath("HarkenSDK/Unity/Harken");
        makeSdk();
        QString error;
        ASSERT_TRUE(VNH::InstallIndex::commitInstallation(studio, trigger + 1, &error));
        EXPECT_LT(sources->findData(sdkPath), 0);
        if (trigger == 0) {
            VNH::VersionsPage versions(&projects);
            QObject::connect(&versions, &VNH::VersionsPage::versionsRefreshed,
                             page.get(), &VNH::EngineIntegrationsPage::refreshStudioVersion);
            versions.refresh();
        } else {
            if (trigger == 1)
                QTest::mouseClick(sources->lineEdit(), Qt::LeftButton);
            else
                QTest::keyClick(sources, Qt::Key_F4);
            ASSERT_TRUE(sources->view()->isVisible());
            sources->hidePopup();
        }
        EXPECT_GE(sources->findData(sdkPath), 0);
        EXPECT_EQ(sources->currentText(), selectedSource);
        EXPECT_EQ(summary->text(), checkedSummary);
        QStringList refreshedSteps;
        for (int row = 0; row < steps()->rowCount(); ++row)
            for (int column = 0; column < steps()->columnCount(); ++column)
                refreshedSteps << steps()->item(row, column)->text();
        EXPECT_EQ(refreshedSteps, checkedSteps);
        EXPECT_TRUE(integrations.list().empty());
        EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core")));
    }
}

TEST_F(EngineIntegrationsPageTest, ProjectAssociationDoesNotRequireSdkAndInstallDraftCanBeCancelled)
{
    makeSdk();
    ASSERT_TRUE(QFile::remove(executablePath));
    page->beginEditForProject(harkenPath, {}, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_TRUE(field("Runtime/SDK source path")->text().isEmpty());
    auto* link = page->findChild<QPushButton*>(QStringLiteral("linkProjects"));
    ASSERT_TRUE(link->isEnabled());
    bool accepted = false;
    QTimer respond;
    QObject::connect(&respond, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog) return;
        if (accepted) dialog->accept();
        else dialog->reject();
    });
    respond.start(10);
    link->click();
    EXPECT_TRUE(integrations.list().empty());
    accepted = true;
    link->click();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());
    EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core")));
    selectSdkFolder(sdkPath);
    auto* installPath = page->findChild<QLineEdit*>(QStringLiteral("sdkInstallPath"));
    EXPECT_TRUE(installPath->isVisible());
    EXPECT_FALSE(installPath->isReadOnly());
    installPath->setText("Assets/DraftSDK");
    EXPECT_TRUE(page->findChild<QLineEdit*>(QStringLiteral("installedSdkPath"))->text().isEmpty());
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    EXPECT_FALSE(installPath->isVisible());
    EXPECT_EQ(installPath->text(), "Packages/com.harken.core");
    EXPECT_TRUE(field("Runtime/SDK source path")->text().isEmpty());
    EXPECT_TRUE(targets.list().front().sdkInstallPath.isEmpty());
    EXPECT_TRUE(integrations.list().front().sdkSourcePath.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath("Assets/DraftSDK")));
}

TEST_F(EngineIntegrationsPageTest, SdkDialogCancelEscapeAndCloseDiscardDraftWithoutExpandingMainPanel)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    QCoreApplication::processEvents();
    auto* mainCheck = page->findChild<QPushButton*>(QStringLiteral("verifySdk"));
    auto* savedPath = page->findChild<QLineEdit*>(QStringLiteral("installedSdkPath"));
    for (int closeMode = 0; closeMode < 3; ++closeMode) {
        selectSdkFolder(dir.filePath("draft-sdk"));
        auto* dialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        ASSERT_NE(dialog, nullptr);
        ASSERT_TRUE(dialog->isVisible());
        EXPECT_TRUE(dialog->isModal());
        EXPECT_EQ(primary()->window(), dialog);
        EXPECT_EQ(field("Runtime/SDK source path")->window(), dialog);
        auto* location = page->findChild<QLineEdit*>(QStringLiteral("sdkInstallPath"));
        location->setText("Assets/DraftSDK");
        if (closeMode == 0) page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
        else if (closeMode == 1) QTest::keyClick(dialog, Qt::Key_Escape);
        else dialog->close();
        EXPECT_FALSE(dialog->isVisible());
        EXPECT_FALSE(primary()->isVisible());
        EXPECT_TRUE(mainCheck->isEnabled());
        EXPECT_EQ(field("Runtime/SDK source path")->text(), sdkPath);
        EXPECT_EQ(location->text(), "Packages/com.harken.core");
        EXPECT_TRUE(integrations.list().empty());
        EXPECT_TRUE(targets.list().front().sdkInstallPath.isEmpty());
        EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath("Assets/DraftSDK")));
    }
}

TEST_F(EngineIntegrationsPageTest, ShowsOperationSpecificDetailsAndRefreshesRepeatedChecks)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    ASSERT_EQ(field("Runtime/SDK source path")->text(), sdkPath);
    auto* details = page->findChild<QPlainTextEdit*>(QStringLiteral("integrationDetails"));
    checkButton()->click();
    ASSERT_GE(steps()->currentRow(), 0);
    EXPECT_TRUE(details->toPlainText().contains(steps()->item(steps()->currentRow(), 2)->text()));
    checkButton()->click();
    steps()->setCurrentCell(0, 0);

    QTimer approvals;
    QObject::connect(&approvals, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog)
            return;
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!buttons)
            return;
        for (auto* candidate : buttons->buttons()) {
            const auto role = buttons->buttonRole(candidate);
            if (role == QDialogButtonBox::AcceptRole
                || role == QDialogButtonBox::DestructiveRole) {
                candidate->click();
                return;
            }
        }
        dialog->reject();
    });
    approvals.start(10);
    startSdkSelection();
    applySdkAndClose();
    EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core/package.json")));
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1)
        << details->toPlainText().toStdString();

    page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->click();
    EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath("Packages/com.harken.core/package.json")));
    EXPECT_EQ(integrations.list().size(), 1);
}

TEST_F(EngineIntegrationsPageTest, CustomLocationPersistsAfterInstallAndSurvivesUnlinkAndRemoval)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    auto* location = page->findChild<QLineEdit*>(QStringLiteral("sdkInstallPath"));
    ASSERT_NE(location, nullptr);
    EXPECT_TRUE(location->isReadOnly());
    startSdkSelection();
    EXPECT_FALSE(location->isReadOnly());
    EXPECT_EQ(location->text(), "Packages/com.harken.core");
    const QString custom = QStringLiteral("Assets/ThirdParty/HarkenSDK");
    location->setText(custom);
    EXPECT_EQ(steps()->rowCount(), 0);
    checkButton()->click();
    ASSERT_EQ(targets.list().size(), 1);
    EXPECT_TRUE(targets.list().front().sdkInstallPath.isEmpty());
    EXPECT_EQ(location->text(), custom);
    QTimer approvals;
    QObject::connect(&approvals, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog) return;
        EXPECT_FALSE(location->isEnabled());
        page->beginEditForProject();
        page->showList();
        EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 1);
        EXPECT_EQ(location->text(), custom);
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (!buttons) { dialog->reject(); return; }
        for (auto* candidate : buttons->buttons()) {
            const auto role = buttons->buttonRole(candidate);
            if (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::DestructiveRole) {
                candidate->click();
                return;
            }
        }
        dialog->reject();
    });
    approvals.start(10);
    applySdkAndClose();
    ASSERT_EQ(targets.list().front().sdkInstallPath, custom);
    EXPECT_TRUE(location->isReadOnly());
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(location->text(), custom);
    EXPECT_TRUE(location->isReadOnly());
    const QString screenshot = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) EXPECT_TRUE(page->grab().save(screenshot));
    ASSERT_TRUE(integrations.remove(integrations.list().front().integrationId));
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_EQ(location->text(), custom);
    page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->click();
    EXPECT_EQ(targets.list().front().sdkInstallPath, custom);
    EXPECT_TRUE(location->isReadOnly());
    EXPECT_TRUE(integrations.list().empty());
}

TEST_F(EngineIntegrationsPageTest, AmbiguousGameFolderPreservesSelectionAndCanRecover)
{
    for (const bool mixedEngines : {false, true}) {
        const QString root = dir.filePath(mixedEngines ? "MixedProjects" : "MultipleUnrealProjects");
        const QString descriptor = QDir(root).filePath("Chosen.uproject");
        writeFile(descriptor, R"({"EngineAssociation":"5.6"})");
        if (!mixedEngines) writeFile(QDir(root).filePath("Other.uproject"), "{}");
        else {
            for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
                ASSERT_TRUE(QDir().mkpath(QDir(root).filePath(folder)));
        }
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        ASSERT_TRUE(waitForVersion());
        const auto targetCount = targets.list().size();
        const QString selectedPath = page->findChild<QLineEdit*>("engineProjectPath")->text();
        int warnings = 0;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, page.get(), [&]() {
            auto* warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!warning) return;
            ++warnings;
            warning->accept();
        });
        dismiss.start(10);
        button("Select Game Project")->click();
        auto* folderPicker = page->findChild<QFileDialog*>();
        ASSERT_NE(folderPicker, nullptr);
        emit folderPicker->fileSelected(root);
        folderPicker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        EXPECT_EQ(warnings, 1);
        EXPECT_EQ(targets.list().size(), targetCount);
        EXPECT_EQ(page->findChild<QLineEdit*>("engineProjectPath")->text(), selectedPath);
        ASSERT_TRUE(QFile::remove(descriptor));
        if (!mixedEngines) {
            ASSERT_TRUE(QFile::remove(QDir(root).filePath("Other.uproject")));
            for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
                ASSERT_TRUE(QDir().mkpath(QDir(root).filePath(folder)));
        }
        button("Select Game Project")->click();
        folderPicker = page->findChild<QFileDialog*>();
        ASSERT_NE(folderPicker, nullptr);
        emit folderPicker->fileSelected(root);
        folderPicker->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ASSERT_TRUE(waitForVersion());
        EXPECT_EQ(warnings, 1);
        EXPECT_EQ(targets.list().size(), targetCount + 1);
        EXPECT_EQ(page->findChild<QLineEdit*>("engineProjectPath")->text(), root);
    }
}

TEST_F(EngineIntegrationsPageTest, LocalizedGuidanceUsesSummaryUntilChecking)
{
    const QString originalStyle = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&]() { qApp->setStyleSheet(originalStyle); });
    qApp->setStyleSheet(QStringLiteral(
        "QWidget { background-color: #0D111A; color: #E0E0E0; font-family: 'Segoe UI'; font-size: 12px; }"
        "QPushButton { background-color: #2A3140; border: 1px solid #3D4554; padding: 8px 16px; }"
        "QPushButton:disabled { background-color: #171C28; color: #555555; border-color: #272D3A; }"));
    makeSdk();
    writeFile(QDir(sdkPath).filePath("Runtime/LegacySupport.cs"), "legacy");
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityPath;
    request.sdkSourcePath = sdkPath;
    request.sdkInstallPath = QStringLiteral("Assets/ThirdParty/HarkenSDK");
    ASSERT_TRUE(service.run(request).success);
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    VNH::IntegrationRecord integration;
    integration.integrationId = "integration";
    integration.harkenProjectPath = harkenPath;
    integration.engineTargetId = targets.list().front().targetId;
    integration.sdkSourcePath = sdkPath;
    integration.createdAt = "2026-09-21T00:00:00Z";
    integration.updatedAt = integration.createdAt;
    ASSERT_TRUE(integrations.add(integration, false));
    for (const bool chinese : {false, true}) {
        page.reset();
        QTranslator translator;
        if (chinese) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto cleanup = qScopeGuard([&]() {
            page.reset();
            QCoreApplication::removeTranslator(&translator);
        });
        page = std::make_unique<VNH::EngineIntegrationsPage>(
            &integrations, &targets, &projects);
        page->resize(900, 700);
        page->show();
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        auto* check = button(QCoreApplication::translate("HarkenHub", "Check Current SDK Status"));
        ASSERT_NE(check, nullptr);
        ASSERT_TRUE(QTest::qWaitFor([&]() { return check->isEnabled(); }, 5000));
        auto* source = field(QCoreApplication::translate("HarkenHub", "Runtime/SDK source path"));
        ASSERT_NE(source, nullptr);
        EXPECT_FALSE(source->isVisible());
        EXPECT_TRUE(source->isReadOnly());
        EXPECT_TRUE(page->findChild<QPushButton*>(QStringLiteral("unlinkProjectsButton"))->isVisible());
        EXPECT_FALSE(source->isVisible());
        auto* installedPath = page->findChild<QLineEdit*>(QStringLiteral("installedSdkPath"));
        auto* installedVersion = page->findChild<QLabel*>(QStringLiteral("installedSdkVersion"));
        EXPECT_TRUE(installedPath->isVisible());
        EXPECT_TRUE(installedPath->isReadOnly());
        EXPECT_EQ(installedPath->text(), QDir::toNativeSeparators(QDir(unityPath).filePath(request.sdkInstallPath)));
        EXPECT_EQ(installedPath->toolTip(), installedPath->text());
        EXPECT_TRUE(installedVersion->text().contains(QStringLiteral("1.0.0")));
        auto* summary = page->findChild<QLabel*>(QStringLiteral("integrationSummary"));
        ASSERT_NE(summary, nullptr);
        EXPECT_TRUE(summary->isVisible());
        EXPECT_EQ(page->findChild<QLabel*>(QStringLiteral("integrationRelationshipHint")), nullptr);
        EXPECT_EQ(page->findChild<QLabel*>(QStringLiteral("sdkLocationHint")), nullptr);
        for (const auto* name : {"changeSdk", "changeSdkLocation", "uninstallSdk"}) {
            auto* maintenance = page->findChild<QPushButton*>(QLatin1String(name));
            ASSERT_NE(maintenance, nullptr);
            EXPECT_TRUE(maintenance->isVisible());
            EXPECT_TRUE(maintenance->isEnabled());
        }
        EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("sdkInstallBrowse"))->isVisible());
        EXPECT_FALSE(primary()->isVisible());
        EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"))->isVisible());
        const QString screenshotRoot = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        if (!screenshotRoot.isEmpty()) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-location-zh.png") : QStringLiteral("sdk-location-en.png"))));
        }
        QTimer::singleShot(0, page.get(), [&]() {
            auto* dialog = activeActionDialog();
            ASSERT_NE(dialog, nullptr);
            EXPECT_NE(dialog->findChild<QLineEdit*>(QStringLiteral("sdkMigrationDestination")), nullptr);
            auto* safetyCheck = dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationSafetyCheck"));
            auto* next = dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationContinue"));
            ASSERT_NE(safetyCheck, nullptr);
            ASSERT_NE(next, nullptr);
            EXPECT_FALSE(next->isEnabled());
            if (!screenshotRoot.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshotRoot).filePath(
                    chinese ? QStringLiteral("sdk-migration-dialog-zh.png") : QStringLiteral("sdk-migration-dialog-en.png"))));
            dialog->reject();
        });
        page->findChild<QPushButton*>(QStringLiteral("changeSdkLocation"))->click();
        check->click();
        EXPECT_TRUE(summary->isVisible());
        EXPECT_FALSE(primary()->isVisible());
        EXPECT_FALSE(primary()->isEnabled());
        bool sawConfirmation = false;
        QTimer cancelRemoval;
        QObject::connect(&cancelRemoval, &QTimer::timeout, page.get(), [&]() {
            auto* dialog = activeActionDialog();
            if (!dialog) return;
            sawConfirmation = true;
            if (!screenshotRoot.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshotRoot).filePath(
                    chinese ? QStringLiteral("sdk-uninstall-dialog-zh.png") : QStringLiteral("sdk-uninstall-dialog-en.png"))));
            dialog->reject();
        });
        cancelRemoval.start(10);
        page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->click();
        cancelRemoval.stop();
        EXPECT_TRUE(sawConfirmation);
        EXPECT_EQ(integrations.list().size(), 1);
        EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath(request.sdkInstallPath + "/package.json")));
        if (!screenshotRoot.isEmpty())
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-cancelled-zh.png") : QStringLiteral("sdk-cancelled-en.png"))));
        startSdkSelection();
        auto* sdkDialog = page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"));
        ASSERT_NE(sdkDialog, nullptr);
        ASSERT_TRUE(sdkDialog->isVisible());
        if (!screenshotRoot.isEmpty())
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-selection-zh.png") : QStringLiteral("sdk-selection-en.png"))));
        check = checkButton();
        EXPECT_TRUE(source->isVisible());
        auto* sourceBrowse = page->findChild<QPushButton*>(QStringLiteral("sdkSourceBrowse"));
        EXPECT_TRUE(sourceBrowse->isVisible());
        const QString missingSdk = dir.filePath("missing-sdk");
        selectSdkFolder(missingSdk);
        EXPECT_EQ(source->text(), missingSdk);
        EXPECT_EQ(source->toolTip(), missingSdk);
        EXPECT_FALSE(primary()->isEnabled());
        QCoreApplication::processEvents();
        if (!screenshotRoot.isEmpty())
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-source-unavailable-zh.png") : QStringLiteral("sdk-source-unavailable-en.png"))));
        selectSdkFolder(sdkPath);
        EXPECT_EQ(source->text(), sdkPath);
        EXPECT_TRUE(primary()->isVisible());
        EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("changeSdkLocation"))->isEnabled());
        EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->isEnabled());
        EXPECT_FALSE(action("unlinkProjects")->isEnabled());
        HarkenHubTest::UnityProjectLock unityLock;
        ASSERT_TRUE(unityLock.lock(unityPath));
        check->click();
        EXPECT_FALSE(primary()->isEnabled());
        EXPECT_EQ(steps()->rowCount(), 1);
        if (!screenshotRoot.isEmpty()) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-unity-open-zh.png") : QStringLiteral("sdk-unity-open-en.png"))));
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-check-failed-zh.png") : QStringLiteral("sdk-check-failed-en.png"))));
        }
        ASSERT_TRUE(unityLock.unlock());
        check->click();
        if (!screenshotRoot.isEmpty()) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-current-zh.png") : QStringLiteral("sdk-current-en.png"))));
            const QString projectFile = QDir(unityPath).filePath(
                request.sdkInstallPath + "/Runtime/ProjectOnly.cs");
            writeFile(projectFile, "project-only");
            check->click();
            QCoreApplication::processEvents();
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-attention-zh.png") : QStringLiteral("sdk-attention-en.png"))));
            auto* tabs = sdkDialog->findChild<QTabWidget*>(QStringLiteral("sdkCheckResults"));
            tabs->setCurrentIndex(1);
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-attention-log-zh.png") : QStringLiteral("sdk-attention-log-en.png"))));
            writeFile(QDir(sdkPath).filePath("package.json"),
                      R"({"name":"com.harken.core","version":"2.0.0"})");
            writeFile(QDir(sdkPath).filePath("Runtime/AdditionalSupport.cs"), "additional");
            ASSERT_TRUE(QFile::remove(QDir(sdkPath).filePath("Runtime/LegacySupport.cs")));
            check->click();
            QCoreApplication::processEvents();
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-changes-zh.png") : QStringLiteral("sdk-changes-en.png"))));
            tabs->setCurrentIndex(1);
            EXPECT_TRUE(sdkDialog->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-check-log-zh.png") : QStringLiteral("sdk-check-log-en.png"))));
            writeFile(QDir(screenshotRoot).filePath(chinese ? "sdk-check-log-zh.txt" : "sdk-check-log-en.txt"),
                      sdkDialog->findChild<QTextBrowser*>(QStringLiteral("sdkSelectionDetails"))->toPlainText().toUtf8());
            makeSdk();
            writeFile(QDir(sdkPath).filePath("Runtime/LegacySupport.cs"), "legacy");
            ASSERT_TRUE(QFile::remove(QDir(sdkPath).filePath("Runtime/AdditionalSupport.cs")));
            ASSERT_TRUE(QFile::remove(projectFile));
        }
        page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
        EXPECT_FALSE(source->isVisible());
        EXPECT_FALSE(primary()->isVisible());
        check = checkButton();
        page->showList();
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        ASSERT_TRUE(QTest::qWaitFor([&]() { return check->isEnabled(); }, 5000));
        page->beginEditForProject();
        if (!screenshotRoot.isEmpty()) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-unlinked-zh.png") : QStringLiteral("sdk-unlinked-en.png"))));
        }
    }
}

TEST_F(EngineIntegrationsPageTest, VerifiesInstalledSdkAndDiagnosesUnmanagedWithoutSourceInBothLanguages)
{
    makeSdk();
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityPath;
    request.sdkSourcePath = sdkPath;
    ASSERT_TRUE(service.run(request).success);
    const QString installedRuntime = QDir(unityPath).filePath("Packages/com.harken.core/Runtime/Plugins/Windows/runtime.bin");
    const QString unmanagedProject = dir.filePath("UnmanagedGame");
    for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
        ASSERT_TRUE(QDir().mkpath(QDir(unmanagedProject).filePath(folder)));
    const QString unmanagedPackage = QDir(unmanagedProject).filePath("Packages/com.harken.core/package.json");
    const QByteArray unmanagedContents = R"({"name":"com.harken.core","version":"0.1.0"})";
    writeFile(unmanagedPackage, unmanagedContents);
    ASSERT_TRUE(QDir(sdkPath).removeRecursively());
    for (const bool chinese : {false, true}) {
        page.reset();
        QTranslator translator;
        if (chinese) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto cleanup = qScopeGuard([&]() {
            page.reset();
            QCoreApplication::removeTranslator(&translator);
        });
        page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
        page->resize(900, 700);
        page->show();
        bool unexpectedDialog = false;
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, page.get(), [&]() {
            auto* dialog = activeActionDialog();
            if (dialog) {
                unexpectedDialog = true;
                dialog->reject();
            }
        });
        dismiss.start(10);
        page->beginEditForProject({}, {}, unityPath);
        ASSERT_TRUE(waitForVersion());
        auto* source = field(QCoreApplication::translate("HarkenHub", "Runtime/SDK source path"));
        ASSERT_NE(source, nullptr);
        EXPECT_TRUE(source->text().isEmpty());
        EXPECT_FALSE(source->isVisible());
        auto* installedPath = page->findChild<QLineEdit*>(QStringLiteral("installedSdkPath"));
        EXPECT_TRUE(installedPath->isVisible());
        EXPECT_TRUE(installedPath->isReadOnly());
        auto* summary = page->findChild<QLabel*>(QStringLiteral("integrationSummary"));
        checkButton()->click();
        ASSERT_EQ(steps()->rowCount(), 3);
        writeFile(installedRuntime, "modified runtime");
        checkButton()->click();
        writeFile(installedRuntime, "runtime");

        page->beginEditForProject({}, {}, unmanagedProject);
        ASSERT_TRUE(waitForVersion());
        EXPECT_TRUE(installedPath->isVisible());
        EXPECT_EQ(installedPath->text(), QDir::toNativeSeparators(QDir(unmanagedProject).filePath("Packages/com.harken.core")));
        EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("changeSdk"))->isEnabled());
        checkButton()->click();
        ASSERT_EQ(steps()->rowCount(), 3);
        EXPECT_FALSE(unexpectedDialog);
        EXPECT_TRUE(source->text().isEmpty());
        EXPECT_TRUE(integrations.list().empty());
        QFile unchanged(unmanagedPackage);
        ASSERT_TRUE(unchanged.open(QIODevice::ReadOnly));
        EXPECT_EQ(unchanged.readAll(), unmanagedContents);
        const QString screenshotRoot = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        if (!screenshotRoot.isEmpty()) {
            QCoreApplication::processEvents();
            EXPECT_TRUE(page->grab().save(QDir(screenshotRoot).filePath(
                chinese ? QStringLiteral("sdk-unmanaged-zh.png") : QStringLiteral("sdk-unmanaged-en.png"))));
        }
    }
}

TEST_F(EngineIntegrationsPageTest, MigrationSafetyCheckIsReadOnlyAndInvalidatesChangedDestination)
{
    makeSdk();
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request install;
    install.unityProjectPath = unityPath;
    install.sdkSourcePath = sdkPath;
    ASSERT_TRUE(service.run(install).success);
    const QString original = QDir(unityPath).filePath("Packages/com.harken.core");
    writeFile(original + ".meta", "guid: keep-sdk-guid");
    writeFile(QDir(unityPath).filePath("Assets/Occupied/Custom.txt"), "existing file");
    const QString destination = QStringLiteral("Assets/ThirdParty/MovedSDK");
    for (const bool chinese : {false, true}) {
        page.reset();
        QTranslator translator;
        if (chinese) {
            ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto cleanup = qScopeGuard([&]() {
            page.reset();
            QCoreApplication::removeTranslator(&translator);
        });
        page = std::make_unique<VNH::EngineIntegrationsPage>(&integrations, &targets, &projects);
        page->resize(900, 700);
        page->show();
        page->beginEditForProject(harkenPath, executablePath, unityPath);
        ASSERT_TRUE(waitForVersion());
        auto* summary = page->findChild<QLabel*>(QStringLiteral("integrationSummary"));
        bool checked = false;
        QTimer::singleShot(0, page.get(), [&]() {
            auto* dialog = activeActionDialog();
            ASSERT_NE(dialog, nullptr);
            const auto closeDialog = qScopeGuard([&]() { dialog->reject(); });
            auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("sdkMigrationDestination"));
            auto* safetyCheck = dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationSafetyCheck"));
            auto* next = dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationContinue"));
            auto* status = dialog->findChild<QLabel*>(QStringLiteral("sdkMigrationSafetyStatus"));
            auto* details = dialog->findChild<QPlainTextEdit*>(QStringLiteral("sdkMigrationSafetyDetails"));
            ASSERT_NE(input, nullptr);
            ASSERT_NE(safetyCheck, nullptr);
            ASSERT_NE(next, nullptr);
            ASSERT_NE(status, nullptr);
            ASSERT_NE(details, nullptr);
            EXPECT_FALSE(next->isEnabled());
            EXPECT_TRUE(details->isReadOnly());
            EXPECT_TRUE(details->toPlainText().isEmpty());
            input->setText(QStringLiteral("Assets/Occupied"));
            safetyCheck->click();
            EXPECT_FALSE(next->isEnabled());
            const QString screenshotRoot = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
            if (!screenshotRoot.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshotRoot).filePath(
                    chinese ? QStringLiteral("sdk-migration-failed-zh.png") : QStringLiteral("sdk-migration-failed-en.png"))));
            input->setText(destination);
            EXPECT_TRUE(details->toPlainText().isEmpty());
            safetyCheck->click();
            EXPECT_TRUE(next->isEnabled());
            EXPECT_EQ(summary->text(), status->text());
            EXPECT_TRUE(details->toPlainText().contains(destination));
            EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath(destination)));
            EXPECT_TRUE(QFileInfo::exists(original + "/package.json"));
            EXPECT_FALSE(QDir(QDir(VNH::userDataDir()).filePath("UnityIntegration/Backups")).exists());
            EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(unityPath));
            if (!screenshotRoot.isEmpty())
                EXPECT_TRUE(dialog->grab().save(QDir(screenshotRoot).filePath(
                    chinese ? QStringLiteral("sdk-migration-passed-zh.png") : QStringLiteral("sdk-migration-passed-en.png"))));
            input->setText(QStringLiteral("Assets/AnotherLocation"));
            EXPECT_FALSE(next->isEnabled());
            EXPECT_TRUE(details->toPlainText().isEmpty());
            EXPECT_EQ(steps()->rowCount(), 0);
            input->setText(destination);
            EXPECT_FALSE(next->isEnabled());
            safetyCheck->click();
            EXPECT_TRUE(next->isEnabled());
            checked = true;
        });
        page->findChild<QPushButton*>(QStringLiteral("changeSdkLocation"))->click();
        EXPECT_TRUE(checked);
        EXPECT_EQ(targets.list().front().sdkInstallPath, "Packages/com.harken.core");
        EXPECT_TRUE(integrations.list().empty());
        EXPECT_FALSE(QFileInfo::exists(QDir(unityPath).filePath(destination)));
        EXPECT_FALSE(QDir(QDir(VNH::userDataDir()).filePath("UnityIntegration/Backups")).exists());
        QFile metadata(original + ".meta");
        ASSERT_TRUE(metadata.open(QIODevice::ReadOnly));
        EXPECT_EQ(metadata.readAll(), "guid: keep-sdk-guid");
    }
}

TEST_F(EngineIntegrationsPageTest, MigrationCancelCommitAndRecoveryPreserveTheRelationship)
{
    makeSdk();
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request install;
    install.unityProjectPath = unityPath;
    install.sdkSourcePath = sdkPath;
    ASSERT_TRUE(service.run(install).success);
    const QString original = QDir(unityPath).filePath("Packages/com.harken.core");
    writeFile(QDir(original).filePath("Custom.txt"), "user changes");
    writeFile(original + ".meta", "guid: keep-sdk-guid");
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    VNH::IntegrationRecord relationship;
    relationship.integrationId = "unchanged-relationship";
    relationship.harkenProjectPath = harkenPath;
    relationship.engineTargetId = targets.list().front().targetId;
    relationship.sdkSourcePath = sdkPath;
    relationship.createdAt = "2026-09-21T00:00:00Z";
    relationship.updatedAt = relationship.createdAt;
    ASSERT_TRUE(integrations.add(relationship, false));
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    ASSERT_TRUE(QDir(sdkPath).removeRecursively());
    auto* migrate = page->findChild<QPushButton*>(QStringLiteral("changeSdkLocation"));
    auto* location = page->findChild<QLineEdit*>(QStringLiteral("sdkInstallPath"));
    ASSERT_NE(migrate, nullptr);
    const QString destination = QStringLiteral("Assets/ThirdParty/MovedSDK");
    bool approve = false;
    bool modifyAfterReview = false;
    QTimer approvals;
    QObject::connect(&approvals, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog) return;
        approvals.stop();
        const auto resume = qScopeGuard([&]() { approvals.start(10); });
        EXPECT_FALSE(location->isEnabled());
        page->showList();
        EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 1);
        auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("sdkMigrationDestination"));
        if (input) {
            input->setText(destination);
            dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationSafetyCheck"))->click();
            auto* next = dialog->findChild<QPushButton*>(QStringLiteral("sdkMigrationContinue"));
            EXPECT_TRUE(next->isEnabled());
            if (modifyAfterReview)
                writeFile(QDir(original).filePath("Custom.txt"), "changed after review");
            if (next->isEnabled()) next->click();
            else dialog->reject();
        } else if (approve) {
            dialog->accept();
        } else {
            dialog->reject();
        }
    });
    approvals.start(10);
    migrate->click();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return location->text() == "Packages/com.harken.core"; }, 5000));
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(QDir(unityPath).filePath(destination)).exists());
    approve = true;
    modifyAfterReview = true;
    migrate->click();
    EXPECT_EQ(location->text(), "Packages/com.harken.core");
    EXPECT_TRUE(QDir(original).exists());
    EXPECT_FALSE(QDir(QDir(unityPath).filePath(destination)).exists());
    EXPECT_FALSE(QDir(QDir(VNH::userDataDir()).filePath("UnityIntegration/Backups")).exists());
    EXPECT_EQ(integrations.list().front().integrationId, relationship.integrationId);
    modifyAfterReview = false;
    migrate->click();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return location->text() == destination; }, 5000));
    EXPECT_EQ(targets.list().front().sdkInstallPath, destination);
    EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath(destination + "/Custom.txt")));
    EXPECT_TRUE(QFileInfo::exists(QDir(unityPath).filePath(destination + ".meta")));
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().integrationId, relationship.integrationId);
    EXPECT_EQ(integrations.list().front().updatedAt, relationship.updatedAt);
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());

    const QDir backups(QDir(VNH::userDataDir()).filePath("UnityIntegration/Backups"));
    const QStringList folders = backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    ASSERT_EQ(folders.size(), 1);
    QFile record(backups.filePath(folders.front() + "/migration.json"));
    ASSERT_TRUE(record.open(QIODevice::ReadOnly));
    writeFile(VNX::UnityInstallation::migrationJournalPath(unityPath), record.readAll());
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    EXPECT_TRUE(migrate->isEnabled());
    EXPECT_FALSE(page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->isEnabled());
    migrate->click();
    ASSERT_TRUE(QTest::qWaitFor([&]() { return location->text() == "Packages/com.harken.core"; }, 5000));
    EXPECT_EQ(targets.list().front().sdkInstallPath, "Packages/com.harken.core");
    EXPECT_FALSE(VNX::UnityInstallation::hasPendingMigration(unityPath));
    EXPECT_EQ(integrations.list().front().integrationId, relationship.integrationId);
    EXPECT_EQ(integrations.list().front().updatedAt, relationship.updatedAt);
}

TEST_F(EngineIntegrationsPageTest, RecoversMovedLocationAndDoesNotSaveFailedCandidate)
{
    makeSdk();
    VNX::UnityIntegrationService service;
    VNX::UnityIntegrationService::Request request;
    request.unityProjectPath = unityPath;
    request.sdkSourcePath = sdkPath;
    ASSERT_TRUE(service.run(request).success);
    const QString moved = QStringLiteral("Assets/RelocatedSDK");
    ASSERT_TRUE(QDir().rename(QDir(unityPath).filePath(QStringLiteral("Packages/com.harken.core")),
                             QDir(unityPath).filePath(moved)));
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    auto* location = page->findChild<QLineEdit*>(QStringLiteral("sdkInstallPath"));
    EXPECT_EQ(location->text(), moved);
    EXPECT_TRUE(location->isReadOnly());
    ASSERT_EQ(targets.list().size(), 1);
    EXPECT_EQ(targets.list().front().sdkInstallPath, moved);
    const QString preserved = dir.filePath(QStringLiteral("RemovedSDK"));
    ASSERT_TRUE(QDir().rename(QDir(unityPath).filePath(moved), preserved));
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    EXPECT_FALSE(location->isReadOnly());
    location->setText(QStringLiteral("../Outside"));
    primary()->click();
    EXPECT_EQ(targets.list().front().sdkInstallPath, moved);
    EXPECT_TRUE(integrations.list().empty());
    EXPECT_EQ(location->text(), "../Outside");
}

TEST_F(EngineIntegrationsPageTest, ReportsUnreadableDatabaseInsteadOfAnEmptySuccessfulList)
{
    targets.close();
    writeFile(dir.filePath("targets.db"), "corrupt database");
    ASSERT_FALSE(targets.open(dir.filePath("targets.db")));
    page->showList();
    auto* error = page->findChild<QLabel*>(QStringLiteral("integrationStorageError"));
    ASSERT_NE(error, nullptr);
    EXPECT_TRUE(error->isVisible());
    EXPECT_FALSE(targets.lastError().isEmpty());
}

TEST_F(EngineIntegrationsPageTest, InstallUpdateUnlinkAndRelinkKeepSdkIdentity)
{
    makeSdk();
    writeFile(QDir(sdkPath).filePath("Runtime/Component.cs"), "original component");
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    const QString installed = QDir(unityPath).filePath("Packages/com.harken.core");
    bool approve = false;
    int confirmations = 0;
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog) return;
        ++confirmations;
        if (auto* message = qobject_cast<QMessageBox*>(dialog)) {
            message->button(approve ? QMessageBox::Yes : QMessageBox::No)->click();
        } else if (approve) {
            dialog->accept();
        } else {
            dialog->reject();
        }
    });
    responder.start(10);
    primary()->click();
    EXPECT_EQ(confirmations, 0);
    EXPECT_FALSE(QFileInfo::exists(installed + "/package.json"));
    EXPECT_TRUE(integrations.list().empty());

    approve = true;
    primary()->click();
    EXPECT_TRUE(page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"))->isVisible());
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    EXPECT_TRUE(integrations.list().empty());
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    const QString originalId = integrations.list().front().integrationId;
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_FALSE(primary()->isEnabled());
    writeFile(installed + "/Runtime/Component.cs.meta", "guid: stable-component-guid");
    writeFile(installed + "/Runtime.meta", "guid: stable-runtime-folder-guid");
    writeFile(installed + ".meta", "guid: stable-sdk-root-guid");

    approve = false;
    action("unlinkProjects")->trigger();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().integrationId, originalId);
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());
    approve = true;
    action("unlinkProjects")->trigger();
    EXPECT_TRUE(integrations.list().empty());
    EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 1);
    EXPECT_FALSE(field("Select a .hsp project file")->isReadOnly());
    EXPECT_FALSE(field("Select a Unity project")->isReadOnly());
    EXPECT_EQ(field("Select a .hsp project file")->text(), harkenPath);
    EXPECT_FALSE(primary()->isVisible());
    EXPECT_TRUE(page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->isEnabled());
    EXPECT_TRUE(page->findChild<QLabel*>("installedSdkVersion")->text().contains(QStringLiteral("1.0.0")));
    EXPECT_TRUE(QFileInfo::exists(installed + "/Runtime/Component.cs.meta"));
    page->showList();
    for (auto* table : page->findChildren<QTableWidget*>())
        if (table->columnCount() == 5) EXPECT_EQ(table->rowCount(), 0);

    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    checkButton()->click();
    ASSERT_TRUE(page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->isVisible());
    const auto modifiedAt = QFileInfo(installed + "/Runtime/Component.cs").lastModified();
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_NE(integrations.list().front().integrationId, originalId);
    EXPECT_EQ(QFileInfo(installed + "/Runtime/Component.cs").lastModified(), modifiedAt);
    EXPECT_FALSE(primary()->isVisible());

    writeFile(QDir(sdkPath).filePath("Runtime/Component.cs"), "updated component");
    checkButton()->click();
    EXPECT_FALSE(primary()->isVisible());
    startSdkSelection();
    EXPECT_TRUE(primary()->isEnabled());
    approve = false;
    primary()->click();
    QFile unchanged(installed + "/Runtime/Component.cs");
    ASSERT_TRUE(unchanged.open(QIODevice::ReadOnly));
    EXPECT_EQ(unchanged.readAll(), "original component");
    unchanged.close();
    approve = true;
    primary()->click();
    EXPECT_TRUE(page->findChild<QDialog*>(QStringLiteral("sdkSelectionDialog"))->isVisible());
    page->findChild<QPushButton*>(QStringLiteral("cancelSdkSelection"))->click();
    QFile updated(installed + "/Runtime/Component.cs");
    ASSERT_TRUE(updated.open(QIODevice::ReadOnly));
    EXPECT_EQ(updated.readAll(), "updated component");
    updated.close();
    for (const auto& entry : {std::pair{QString("/Runtime/Component.cs.meta"), QByteArray("guid: stable-component-guid")},
                              std::pair{QString("/Runtime.meta"), QByteArray("guid: stable-runtime-folder-guid")},
                              std::pair{QString(".meta"), QByteArray("guid: stable-sdk-root-guid")}}) {
        QFile meta(installed + entry.first);
        ASSERT_TRUE(meta.open(QIODevice::ReadOnly));
        EXPECT_EQ(meta.readAll(), entry.second);
    }
    EXPECT_FALSE(primary()->isEnabled());
    page->findChild<QPushButton*>(QStringLiteral("uninstallSdk"))->click();
    EXPECT_FALSE(primary()->isVisible());
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());
    EXPECT_EQ(page->findChild<QStackedWidget*>()->currentIndex(), 1);
    const QString linkedId = integrations.list().front().integrationId;
    page->showList();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    applySdkAndClose();
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().integrationId, linkedId);
    EXPECT_TRUE(QFileInfo::exists(installed + "/package.json"));
    EXPECT_FALSE(primary()->isVisible());
}

TEST_F(EngineIntegrationsPageTest, FailedUnlinkKeepsRelationshipAndSourceChangeInvalidatesCheck)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    QTimer responder;
    QObject::connect(&responder, &QTimer::timeout, page.get(), [&]() {
        auto* dialog = activeActionDialog();
        if (!dialog) return;
        if (auto* message = qobject_cast<QMessageBox*>(dialog))
            message->button(message->standardButtons().testFlag(QMessageBox::Yes)
                ? QMessageBox::Yes : QMessageBox::Ok)->click();
        else dialog->accept();
    });
    responder.start(10);
    startSdkSelection();
    applySdkAndClose();
    page->findChild<QPushButton*>(QStringLiteral("linkProjects"))->click();
    ASSERT_EQ(integrations.list().size(), 1);
    const QString id = integrations.list().front().integrationId;
    integrations.close();
    action("unlinkProjects")->trigger();
    EXPECT_TRUE(field("Select a .hsp project file")->isReadOnly());
    ASSERT_TRUE(integrations.open(dir.filePath("integrations.db")));
    ASSERT_EQ(integrations.list().size(), 1);
    EXPECT_EQ(integrations.list().front().integrationId, id);
    startSdkSelection();
    writeFile(QDir(sdkPath).filePath("package.json"),
              R"({"name":"com.harken.core","version":"2.0.0"})");
    checkButton()->click();
    auto* changes = page->findChild<QTreeWidget*>(QStringLiteral("sdkFileChanges"));
    auto* log = page->findChild<QTextBrowser*>(QStringLiteral("sdkSelectionDetails"));
    ASSERT_NE(changes, nullptr);
    ASSERT_NE(log, nullptr);
    EXPECT_GT(changes->topLevelItemCount(), 0);
    EXPECT_FALSE(log->toPlainText().isEmpty());
    field("Runtime/SDK source path")->setText(dir.filePath("missing-sdk"));
    EXPECT_EQ(steps()->rowCount(), 0);
    EXPECT_EQ(changes->topLevelItemCount(), 0);
    EXPECT_TRUE(log->toPlainText().isEmpty());
    EXPECT_TRUE(page->findChild<QLabel*>(QStringLiteral("sdkDiffSummary"))->text().isEmpty());
    EXPECT_FALSE(primary()->isEnabled());
    field("Runtime/SDK source path")->setText(sdkPath);
    EXPECT_TRUE(primary()->isEnabled());
    ASSERT_TRUE(QFile::remove(QDir(sdkPath).filePath("harken-integration.json")));
    checkButton()->click();
    EXPECT_FALSE(primary()->isEnabled());
    makeSdk();
    checkButton()->click();
}

TEST_F(EngineIntegrationsPageTest, SelectsFailureAndReplacesDetailsFromPreviousCheck)
{
    makeSdk();
    page->beginEditForProject(harkenPath, executablePath, unityPath);
    ASSERT_TRUE(waitForVersion());
    startSdkSelection();
    checkButton()->click();
    auto* details = page->findChild<QPlainTextEdit*>(QStringLiteral("integrationDetails"));
    const QString firstResult = details->toPlainText();
    ASSERT_TRUE(QFile::remove(QDir(sdkPath).filePath("harken-integration.json")));
    checkButton()->click();
    EXPECT_NE(details->toPlainText(), firstResult);
}

}
