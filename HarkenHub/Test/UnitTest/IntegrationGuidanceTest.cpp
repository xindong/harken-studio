#include "Integrations/EngineTargetProbe.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "Integrations/IntegrationStore.h"
#include "Integrations/ProjectIntegrationExecutor.h"
#include "Integrations/Unity/UnityInstallation.h"
#include "Projects/ProjectStore.h"
#include "UI/EngineIntegrationsPage.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QTranslator>

#include <memory>

class IntegrationGuidanceTest : public QObject
{
    Q_OBJECT

    std::unique_ptr<QTemporaryDir> dir;
    VNH::EngineTargetRecord target;
    QString source;
    QString installed;

    void write(const QString& path, const QByteArray& data)
    {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), data.size());
    }

    VNH::UnityIntegrationRequest request() const
    {
        VNH::UnityIntegrationRequest value;
        value.unityProjectPath = target.canonicalRoot;
        value.sdkSourcePath = source;
        value.confirmTargetChanges = true;
        return value;
    }

    void install()
    {
        const auto result = VNH::HubUnityIntegrationService::run(request());
        QStringList details;
        for (const auto& step : result.steps)
            if (step.status == VNH::UnityIntegrationStepStatus::Failed)
                details << step.id << step.summary << step.cause << step.details;
        QVERIFY2(result.success, qPrintable(details.join('\n')));
    }

private slots:
    void init()
    {
        QVERIFY(QDir().mkpath(".temp"));
        dir = std::make_unique<QTemporaryDir>(QDir::current().filePath(".temp/hub-guidance-XXXXXX"));
        QVERIFY(dir->isValid());
        qputenv("HARKEN_HUB_USER_ROOT", dir->filePath("user").toUtf8());
        qputenv("HARKEN_HUB_MACHINE_ROOT", dir->filePath("machine").toUtf8());
        qputenv("HARKEN_HUB_VERSIONS_ROOT", dir->filePath("versions").toUtf8());
        target = {};
        target.canonicalRoot = dir->filePath("UnityGame");
        source = dir->filePath("Source");
        installed = QDir(target.canonicalRoot).filePath("Packages/com.harken.core");
        for (const auto* folder : {"Assets", "Packages", "ProjectSettings"})
            QVERIFY(QDir().mkpath(QDir(target.canonicalRoot).filePath(folder)));
        write(source + "/package.json", R"({"name":"com.harken.core","version":"1.0.0"})");
        QJsonArray plugins;
        QJsonObject expected;
        for (const auto* platform : {"Windows", "macOS", "Linux"}) {
            const QString runtime = QString("Runtime/Plugins/%1/runtime.bin").arg(platform);
            write(source + '/' + runtime, "runtime");
            plugins.append(QJsonObject{{"platform", platform}, {"alternatives", QJsonArray{runtime}}});
            expected.insert(platform, QJsonArray{platform});
        }
        write(source + "/harken-integration.json", QJsonDocument(QJsonObject{
            {"schema_version", 3}, {"package_name", "com.harken.core"},
            {"supports_custom_install_path", true}, {"required_files", QJsonArray{"package.json"}},
            {"expected_platforms_by_editor_host", expected}, {"native_plugins", plugins}}).toJson());
    }

    void cleanup()
    {
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QCoreApplication::processEvents();
        qunsetenv("HARKEN_HUB_USER_ROOT");
        qunsetenv("HARKEN_HUB_MACHINE_ROOT");
        qunsetenv("HARKEN_HUB_VERSIONS_ROOT");
        dir.reset();
    }

    void damagedMetadataCanActuallyBeRepaired_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QByteArray>("damage");
        QTest::newRow("missing-package") << QString("package.json") << QByteArray();
        QTest::newRow("invalid-package") << QString("package.json") << QByteArray("invalid");
        QTest::newRow("missing-manifest") << QString("harken-integration.json") << QByteArray();
        QTest::newRow("invalid-manifest") << QString("harken-integration.json") << QByteArray("invalid");
        QTest::newRow("wrong-manifest-schema") << QString("harken-integration.json") << QByteArray("{\"schema_version\":1}");
    }

    void damagedMetadataCanActuallyBeRepaired()
    {
        QFETCH(QString, file);
        QFETCH(QByteArray, damage);
        install();
        write(installed + "/package.json.meta", "guid: keep-this-guid");
        write(installed + "/Custom.txt", "user data");
        if (damage.isEmpty()) QVERIFY(QFile::remove(installed + '/' + file));
        else write(installed + '/' + file, damage);
        const auto observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Integration Package metadata needs repair");
        QVERIFY(observation.canChangeSdk);
        QVERIFY(!observation.canMoveSdk);
        QVERIFY(!observation.canRemoveSdk);
        auto check = request();
        check.verifyInstalledOnly = true;
        QVERIFY(!VNH::HubUnityIntegrationService::run(check).success);
        install();
        QVERIFY(VNH::HubUnityIntegrationService::run(check).success);
        QFile metadata(installed + "/package.json.meta");
        QVERIFY(metadata.open(QIODevice::ReadOnly));
        QCOMPARE(metadata.readAll(), "guid: keep-this-guid");
        QFile custom(installed + "/Custom.txt");
        QVERIFY(custom.open(QIODevice::ReadOnly));
        QCOMPARE(custom.readAll(), "user data");
        const auto repaired = VNH::EngineTargetProbe::observe(target);
        QVERIFY(repaired.canChangeSdk && repaired.canMoveSdk && repaired.canRemoveSdk);
    }

    void missingAndInvalidRecordsHaveSeparateGuidance()
    {
        install();
        const QString record = installed + "/.harken-integration-state.json";
        QVERIFY(QFile::remove(record));
        auto observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Installation record missing");
        QVERIFY(!observation.canChangeSdk && !observation.canMoveSdk && !observation.canRemoveSdk);
        QVERIFY(observation.remediation.contains("own backup"));
        write(record, "{}");
        observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Installation record invalid");
        QVERIFY(!observation.canChangeSdk && !observation.canMoveSdk && !observation.canRemoveSdk);
    }

    void dependencyRestrictsMoveAndRemovalButAllowsChange()
    {
        install();
        write(QDir(target.canonicalRoot).filePath("Packages/manifest.json"),
              R"({"dependencies":{"com.harken.core":"1.0.0"}})");
        const auto observation = VNH::EngineTargetProbe::observe(target);
        QVERIFY(observation.dependencyRestricted);
        QVERIFY(observation.canChangeSdk);
        QVERIFY(!observation.canMoveSdk && !observation.canRemoveSdk);
        auto check = request();
        check.checkOnly = true;
        QVERIFY(VNH::HubUnityIntegrationService::run(check).success);
    }

    void embeddedLockRecordAllowsMaintenanceWithoutIgnoringRealDependencies()
    {
        install();
        write(target.canonicalRoot + "/Packages/manifest.json", R"({"dependencies":{}})");
        QJsonObject resolved{{"com.harken.core", QJsonObject{
            {"version", "file:com.harken.core"}, {"depth", 0}, {"source", "embedded"},
            {"dependencies", QJsonObject{}}}}};
        const auto writeLock = [&]() {
            write(target.canonicalRoot + "/Packages/packages-lock.json",
                  QJsonDocument(QJsonObject{{"dependencies", resolved}}).toJson());
        };
        writeLock();
        auto observation = VNH::EngineTargetProbe::observe(target);
        QVERIFY(observation.status.isEmpty());
        QVERIFY(!observation.dependencyRestricted);
        QVERIFY(observation.canChangeSdk && observation.canMoveSdk && observation.canRemoveSdk);
        auto check = request();
        check.checkOnly = true;
        QVERIFY(VNH::HubUnityIntegrationService::run(check).success);
        resolved.insert("com.example.consumer", QJsonObject{
            {"version", "1.0.0"}, {"source", "registry"}, {"depth", 0},
            {"dependencies", QJsonObject{{"com.harken.core", "1.0.0"}}}});
        writeLock();
        observation = VNH::EngineTargetProbe::observe(target);
        QVERIFY(observation.dependencyRestricted);
        QVERIFY(observation.canChangeSdk && !observation.canMoveSdk && !observation.canRemoveSdk);
        resolved.remove("com.example.consumer");
        writeLock();
        write(target.canonicalRoot + "/Packages/manifest.json",
              R"({"dependencies":{"com.harken.core":"file:com.harken.core"}})");
        observation = VNH::EngineTargetProbe::observe(target);
        QVERIFY(observation.dependencyRestricted && !observation.canRemoveSdk);
        write(target.canonicalRoot + "/Packages/manifest.json", R"({"dependencies":{}})");
        auto remove = request();
        remove.removeIntegration = true;
        remove.confirmRemoval = true;
        remove.removalBackupPath = dir->filePath("removal-backup");
        QVERIFY(VNH::HubUnityIntegrationService::run(remove).success);
    }

    void duplicateAndInterruptedMigrationNeedDifferentRecovery()
    {
        install();
        write(QDir(target.canonicalRoot).filePath("Assets/OtherSDK/package.json"),
              R"({"name":"com.harken.core","version":"1.0.0"})");
        auto observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Multiple Integration Package installations");
        QVERIFY(!observation.canChangeSdk && !observation.canMoveSdk && !observation.canRemoveSdk);
        write(VNX::UnityInstallation::migrationJournalPath(target.canonicalRoot), "{}");
        observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Migration unfinished");
        QVERIFY(!observation.canChangeSdk && observation.canMoveSdk && !observation.canRemoveSdk);
        QVERIFY(observation.remediation.contains("Recover Integration Package Migration"));
    }

    void damagedUnrealDescriptorAllowsRepairAndRemoval()
    {
        target.engineKind = VNH::EngineKind::Unreal;
        target.canonicalRoot = dir->filePath("UnrealGame");
        target.descriptorPath = target.canonicalRoot + "/Game.uproject";
        write(target.descriptorPath, R"({"EngineAssociation":"5.6"})");
        write(source + "/Harken.uplugin", R"({"VersionName":"1.0.0"})");
        write(source + "/Source/HarkenCore/Include/harken/Harken.h", "#include <harken/ObjectId.h>\n");
        write(source + "/Source/HarkenCore/Include/harken/ObjectId.h", "header");
        write(source + "/Source/HarkenCore/Lib/Win64/HarkenCoreImport.lib", "import");
        write(source + "/Binaries/ThirdParty/HarkenCore/Win64/HarkenCore.dll", "runtime");
        write(source + "/Binaries/ThirdParty/HarkenCore/Mac/libHarkenCore.dylib", "runtime");
        write(source + "/Binaries/ThirdParty/HarkenCore/Linux/x86_64/libHarkenCore.so", "runtime");
        using Executor = VNH::ProjectIntegrationExecutor;
        using Operation = VNH::IntegrationOperation;
        auto plan = Executor::planUnreal(target, source, Operation::Install);
        QVERIFY2(plan.valid, qPrintable(plan.error));
        auto result = Executor::executeUnreal(target, plan, Operation::Install, true);
        QVERIFY2(result.success, qPrintable(result.error));
        const QString plugin = target.canonicalRoot + "/Plugins/Harken/Harken.uplugin";
        write(plugin, "invalid");
        const auto observation = VNH::EngineTargetProbe::observe(target);
        QCOMPARE(observation.status, "Integration Package metadata needs repair");
        QVERIFY(observation.canChangeSdk && observation.canRemoveSdk && !observation.canMoveSdk);
        plan = Executor::planUnreal(target, source, Operation::Repair);
        QVERIFY2(plan.valid, qPrintable(plan.error));
        result = Executor::executeUnreal(target, plan, Operation::Repair, true);
        QVERIFY2(result.success, qPrintable(result.error));
        write(plugin, "invalid");
        plan = Executor::planUnreal(target, {}, Operation::Remove);
        QVERIFY2(plan.valid, qPrintable(plan.error));
        result = Executor::executeUnreal(target, plan, Operation::Remove, true);
        QVERIFY2(result.success, qPrintable(result.error));
    }

    void associationAndResolutionInBothLanguages_data()
    {
        QTest::addColumn<bool>("chinese");
        QTest::newRow("en") << false;
        QTest::newRow("zh") << true;
    }

    void associationAndResolutionInBothLanguages()
    {
        QFETCH(bool, chinese);
        const QString originalStyle = qApp->styleSheet();
        const auto restoreStyle = qScopeGuard([&]() { qApp->setStyleSheet(originalStyle); });
        qApp->setStyleSheet(QStringLiteral(
            "QWidget { background-color: #0D111A; color: #E0E0E0; font-family: 'Segoe UI'; font-size: 12px; }"
            "QPushButton { background-color: #2A3140; border: 1px solid #3D4554; padding: 8px 16px; }"
            "QPushButton:disabled { background-color: #171C28; color: #555555; border-color: #272D3A; }"));
        QTranslator translator;
        if (chinese) {
            QVERIFY(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
            QCoreApplication::installTranslator(&translator);
        }
        const auto cleanupTranslator = qScopeGuard([&]() { QCoreApplication::removeTranslator(&translator); });
        const QString unmanaged = QDir(target.canonicalRoot).filePath("Packages/com.harken.core/package.json");
        write(unmanaged, R"({"name":"com.harken.core","version":"0.3.0"})");
        const QString project = dir->filePath("Audio.hsp");
        write(project, "{}");
        VNH::IntegrationStore integrations;
        VNH::EngineTargetStore targets;
        VNH::ProjectStore projects;
        QVERIFY(integrations.open(dir->filePath("integrations.db")));
        QVERIFY(targets.open(dir->filePath("targets.db")));
        QVERIFY(projects.open(dir->filePath("projects.db")));
        VNH::EngineIntegrationsPage page(&integrations, &targets, &projects);
        page.resize(760, 600);
        page.show();
        page.beginEditForProject(project, {}, target.canonicalRoot);
        auto* verify = page.findChild<QPushButton*>("verifySdk");
        QVERIFY(verify);
        QVERIFY(QTest::qWaitFor([&]() { return verify->isEnabled(); }));
        QVERIFY(integrations.list().empty());
        const auto* association = page.findChild<QLabel*>("projectAssociationHint");
        QVERIFY(association);
        const char* associationText = "Linking adds this project pair to Engine Integrations and establishes a one-to-one relationship in this Hub. Unlink the pair before linking either project to another. Integration Package checks and management do not require linking.";
        QCOMPARE(association->text(), QCoreApplication::translate("HarkenHub", associationText));
        if (chinese) QVERIFY(association->text() != QString::fromLatin1(associationText));
        QVERIFY(!page.findChild<QPushButton*>("changeSdk")->isEnabled());
        verify->click();
        auto* resolution = page.findChild<QPushButton*>("sdkResolution");
        QVERIFY(resolution && resolution->isVisible() && resolution->isEnabled());
        bool opened = false;
        QTimer inspectDialog;
        connect(&inspectDialog, &QTimer::timeout, &page, [&]() {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || dialog->objectName() != "sdkResolutionDialog") return;
            inspectDialog.stop();
            const auto closeDialog = qScopeGuard([&]() { dialog->reject(); });
            QCOMPARE(dialog->objectName(), "sdkResolutionDialog");
            const auto* details = dialog->findChild<QPlainTextEdit*>("sdkResolutionDetails");
            QVERIFY(details && details->isReadOnly());
            const QString text = details->toPlainText();
            QVERIFY(text.contains(QCoreApplication::translate("HarkenHub", "Installation record missing")));
            QVERIFY(text.contains("ownership record") || chinese);
            if (chinese) QVERIFY(!text.contains("Installation record missing"));
            for (auto* button : dialog->findChildren<QPushButton*>())
                QVERIFY(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
            const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty())
                QVERIFY(dialog->grab().save(QDir(screenshots).filePath(chinese ? "guidance-dialog-zh.png" : "guidance-dialog-en.png")));
            opened = true;
        });
        inspectDialog.start(10);
        resolution->click();
        QVERIFY(opened);
        QVERIFY(integrations.list().empty());
        QFile original(unmanaged);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), R"({"name":"com.harken.core","version":"0.3.0"})");
        const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            QVERIFY(page.grab().save(QDir(screenshots).filePath(chinese ? "guidance-page-zh.png" : "guidance-page-en.png")));
    }
};

QTEST_MAIN(IntegrationGuidanceTest)
#include "IntegrationGuidanceTest.moc"
