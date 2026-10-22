#include "Localization/HubLocalization.h"
#include "Integrations/HubUnityIntegrationService.h"
#include "UI/LogConsoleDialog.h"
#include "Transactions/TransactionDiagnostics.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTranslator>
#include <QScopeGuard>

using VNH::HubLocalization;

TEST(HubLocalizationTest, CustomSdkSafetyMessagesTranslateWithoutChangingPaths)
{
    const QString path = QStringLiteral("D:/Game %2/Assets/ThirdParty/Harken SDK");
    const QStringList messages = {
        QStringLiteral("Cannot check whether this Unity project is open: %1 (system error %2). Check project permissions and try again.")
            .arg(path, "5"),
        QStringLiteral("Cannot move directory from %1 to %2 (system error %3: %4).")
            .arg(path, path + ".backup", "5", "Access denied"),
        QStringLiteral("Check file write protection, directory permissions and processes holding files open at %1. If checkout or unlocking is required, do that first, then retry. The existing SDK was not changed.").arg(path),
        QStringLiteral("Choose a safe directory inside the Unity project: %1").arg(path),
        QStringLiteral("An SDK is already installed at %1. Use that location or remove it before installing elsewhere.").arg(path),
        QStringLiteral("Cannot read %1: Access denied").arg(path),
        QStringLiteral("Cannot read SDK file for migration: %1").arg(path),
        QStringLiteral("Cannot read SDK file: %1").arg(path),
        QStringLiteral("Cannot commit %1: Access denied").arg(path),
        QStringLiteral("Check write permission for the Hub report directory: %1").arg(path),
        QStringLiteral("Cannot determine the SDK package identity: %1").arg(path),
        QStringLiteral("SDK discovery exceeded its safety limits at %1. Check local package references and reduce the scanned content.").arg(path),
        QStringLiteral("External package discovery exceeded its safety limits at %1. Point file: dependencies at individual package directories and remove unrelated content before retrying.").arg(path),
        QStringLiteral("External package path has no package.json: %1. Point the file: dependency at the package directory.").arg(path),
        QStringLiteral("A local package path includes the Unity project: %1. Reference a package directory instead.").arg(path),
        QStringLiteral("Multiple SDK installations were found. Resolve these conflicts before continuing:\n%1\nD:/Game/Packages/com.harken.core").arg(path),
        QStringLiteral("Resolve SDK package dependencies before removal; Unity could load the referenced SDK after the embedded copy is removed:\ncom.harken.core: file:../LocalSDK"),
        QStringLiteral("Ready to remove 12 Harken SDK/runtime file(s) and preserve 3 project-specific file(s)."),
        QStringLiteral("Installed package: %1").arg(path),
        QStringLiteral("To integrate again later, select or drop this SDK source: %1").arg(path),
        QStringLiteral("Backed up and removed the Harken SDK and native runtimes; preserved 3 project-specific file(s)."),
        QStringLiteral("Installed SDK 1.2.3 from a verified staging directory; copied 12 managed file(s) and preserved 3 target-only file(s)."),
        QStringLiteral("Verified the content integrity of 12 Harken-managed SDK file(s)."),
        QStringLiteral("179 SDK text file(s) differ only in LF/CRLF line endings; contents are unchanged."),
        QStringLiteral("42 documentation/sample file(s) are missing; SDK updates can continue."),
        QStringLiteral("Missing documentation/sample files (does not block SDK updates):"),
        QStringLiteral("12 SDK file(s) differ from their contents at installation."),
        QStringLiteral("2 SDK file(s) recorded at installation are missing."),
        QStringLiteral("1 SDK file(s) could not be read, so verification could not finish."),
        QStringLiteral("Check-only mode did not change %1.").arg(path),
        QStringLiteral("Review the file list before confirming the SDK change."),
        QStringLiteral("All Harken-managed files match SDK 1.2.3; 12 Unity metadata file(s) and 2 optional target runtime file(s) are retained as expected."),
        QStringLiteral("All Harken-managed files match SDK 1.2.3; 1 project-specific file(s) need review, while 12 Unity metadata and 2 optional runtime file(s) are retained normally."),
        QStringLiteral("The project-specific files are outside Harken SDK ownership, so the check cannot determine whether they are still needed."),
        QStringLiteral("Review only the project-specific files listed below. Unity-generated metadata requires no action."),
    };
    for (const QString& message : messages)
        EXPECT_EQ(VNH::HubUnityIntegrationService::localizeText(message), message);

    QTranslator translator;
    ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
    QCoreApplication::installTranslator(&translator);
    const auto removeTranslator = qScopeGuard([&]() {
        QCoreApplication::removeTranslator(&translator);
    });
    for (const char* source : {"Change SDK...", "Change SDK", "Apply SDK Change...",
                              "Review recommended", "Normal", "Latest local SDK version: %1",
                              "Studio %1 | %2 SDK %3", "File Changes", "Check Log", "Check Log (review recommended: %1)",
                              "SDK version: %1 → %2", "Add %1", "Update %1", "Remove %1", "Keep %1",
                              "No SDK files need to be changed.", "Review recommended for %1 item(s). View check log.",
                              "SDK check failed. Review the check log for details.",
                              "Select Game Project", "Select a Unity project", "Uninstall SDK...",
                              "Choose a directory inside this project's Plugins folder, for example Plugins/Audio/Harken.",
                              "The SDK cannot be placed inside another Unreal plugin. Choose a separate directory under Plugins.",
                              "Multiple Harken plugins were found. Keep one installation under Plugins and move the others outside Plugins before checking again.\n%1",
                              "The Unreal migration record is invalid. Preserve both SDK locations and the backup; restore the record from its migration backup before retrying.",
                              "Repair the installed Unreal SDK with Change SDK before moving it.",
                              "The current and new SDK locations must be separate directories.",
                              "The migration backup is missing or changed. Restore the original backup before recovering; no SDK files were changed.",
                              "SDK files changed after migration was interrupted, or both locations are occupied. Preserve those files and restore one location to the migration backup's contents before recovering.",
                              "The migration recovery record could not be saved. Check project permissions and retry.",
                              "Another Harken plugin exists outside the migration locations. Preserve it outside Plugins before recovering.",
                              "Resolve the reported plugin locations or path access errors, then check again. Hub must identify one SDK installation before changing its files.",
                              "SDK migration completed. All installed files and project relationships were kept.",
                              "Use Change SDK with a complete Unreal plugin source to repair it, or Uninstall SDK to remove the recorded files after backup. Repair the plugin metadata before moving it.",
                              "This Unreal project is open. Close it before changing the SDK, then run the safety check again.",
                              "An Unreal Editor uses a relative project path with the same project name as this target. Reopen it with an absolute .uproject path so Hub can identify the project before changing SDK files.",
                              "Could not check Unreal Editor processes. Check permissions and retry the safety check.",
                              "SDK files changed after the safety check. Run the check again before continuing.",
                              "Move SDK...", "SDK installed.", "Check Current SDK Status", "Check SDK Changes", "Results and Logs",
                              "Choose SDK Folder...", "The selected SDK source is unavailable.",
                              "SDK operation", "Processing SDK files...", "Processing SDK files... Items inspected: %1", "SDK inspection cancelled.",
                              "Stop Scan", "Stopping...", "SDK scan stopped",
                              "SDK scan stopped. The check is incomplete; no SDK files were changed.",
                              "Run the SDK check again to finish inspecting this project.",
                              "Current SDK Location", "(SDK Version: %1)",
                              "This Unity project is open. Close it before changing the SDK, then run the safety check again.",
                              "Inspect installed Unity SDK",
                              "Installed SDK verification passed. No files were changed.",
                              "Installed SDK verification did not pass. Review the results; no files were changed.",
                              "The installed SDK has incomplete or invalid verification data.",
                              "Installed SDK files do not match the installation record or could not be read.",
                              "The installation record is complete. File contents are checked in the next step.",
                              "Review the listed content differences to confirm whether they were intentional. To restore the original SDK files, use Change SDK and select the complete package for the installed version.",
                              "Restore the listed missing files from a matching backup, or use Change SDK and select the complete package for the installed version.",
                              "Check read permissions and whether another program is preventing access to the listed files, then check the SDK status again.",
                              "Missing SDK files:", "SDK files with contents different from installation:", "Unreadable SDK files:",
                              "Projects linked. SDK files were not changed.",
                              "Migrate...", "Recover...", "Review SDK migration",
                              "Safety Check", "Continue...", "Safety check required.",
                              "Checking SDK migration safety...",
                              "The approved SDK migration is running...",
                              "Migration destination changed. Safety check required.",
                              "Safety check passed. No SDK files were moved.",
                              "Safety check failed. No SDK files were moved.",
                              "SDK migration could not be completed.",
                              "An unfinished SDK migration must be recovered before continuing."})
        EXPECT_FALSE(translator.translate("HarkenHub", source).isEmpty()) << source;
    for (const QString& message : messages) {
        const QString localized = VNH::HubUnityIntegrationService::localizeText(message);
        EXPECT_NE(localized, message);
        if (!message.contains(QLatin1Char('%'))) EXPECT_FALSE(localized.contains(QLatin1Char('%')));
        if (message.contains(path)) EXPECT_TRUE(localized.contains(path));
        if (message.contains(QLatin1Char('\n')))
            EXPECT_EQ(localized.mid(localized.indexOf(QLatin1Char('\n'))),
                      message.mid(message.indexOf(QLatin1Char('\n'))));
    }
}

TEST(HubLocalizationTest, MaintenanceDiagnosticsTranslateWithoutLosingEvidence)
{
    const QString path = QStringLiteral("D:/Game %2/Studio %3");
    const QStringList details = {
        QStringLiteral("HarkenStudio is still running from this version: %1").arg(path),
        QStringLiteral("Cannot move the install into place: %1").arg(path),
        QStringLiteral("Installation retained at %1. In Versions, choose Repair to recover it.").arg(path),
        QStringLiteral("Cannot open the install index for reading: %1 (Qt error 5: Access denied)").arg(path),
        QStringLiteral("Cannot read the install index: %1 (Qt error 5: Access denied)").arg(path),
        QStringLiteral("Install index is corrupt: %1 (offset 5: invalid JSON)").arg(path),
        QStringLiteral("Cannot create file directory: %1").arg(path),
        QStringLiteral("Cannot open file for writing: %1 (Qt error 5: Access denied)").arg(path),
        QStringLiteral("Cannot write file: %1 (Qt error 5: Access denied)").arg(path),
        QStringLiteral("Cannot commit file: %1 (Qt error 5: Access denied)").arg(path),
    };
    const QStringList logs = {
        QStringLiteral("[WARN] Install recovery failed: ") + details[5],
        QStringLiteral("[ERROR] Repair failed: VersionInUse ") + details[0],
        QStringLiteral("[ERROR] Install failed: CommitFailed ") + details[1],
        details[2],
    };
    for (const auto& detail : details)
        EXPECT_EQ(VNH::TransactionDiagnostics::localizeDetail(detail), detail);
    for (const auto& log : logs)
        EXPECT_EQ(VNH::LogConsoleDialog::localizeLine(log), log);
    QTranslator translator;
    ASSERT_TRUE(translator.load(QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
    QCoreApplication::installTranslator(&translator);
    const auto removeTranslator = qScopeGuard([&]() {
        QCoreApplication::removeTranslator(&translator);
    });
    for (const auto& detail : details) {
        const QString translated = VNH::TransactionDiagnostics::localizeDetail(detail);
        EXPECT_NE(translated, detail);
        EXPECT_TRUE(translated.contains(path));
        if (detail.contains("Access denied"))
            EXPECT_TRUE(translated.contains("5") && translated.contains("Access denied"));
    }
    for (const auto& log : logs) {
        const QString translated = VNH::LogConsoleDialog::localizeLine(log);
        EXPECT_NE(translated, log);
        EXPECT_TRUE(translated.contains(path));
        EXPECT_FALSE(translated.contains("HarkenStudio is still running"));
        EXPECT_FALSE(translated.contains("Install index is corrupt"));
        if (log.contains("VersionInUse"))
            EXPECT_TRUE(translated.contains("VersionInUse"));
    }
    const auto translatedLines = VNH::TransactionDiagnostics::localizeDetail(
        details.join(QLatin1Char('\n'))).split(QLatin1Char('\n'));
    ASSERT_EQ(translatedLines.size(), details.size());
    for (qsizetype i = 0; i < details.size(); ++i)
        EXPECT_EQ(translatedLines[i], VNH::TransactionDiagnostics::localizeDetail(details[i]));
    for (const char* source : {
        "Close Studio and other programs running from this installation, then retry Repair. No installation files were replaced.",
        "Another install transaction is running",
        "Some files needed for the project upgrade are read-only or not writable.",
        "Check file write protection and access permissions, then try again. If your version control workflow requires checkout or unlocking, do that first. No project data was changed.",
        "Affected files:\n%1"
    })
        EXPECT_FALSE(translator.translate("HarkenHub", source).isEmpty()) << source;
}

TEST(HubLocalizationTest, SystemPreferenceFollowsChineseSystemLocale)
{
    EXPECT_EQ(HubLocalization::resolveLanguage(
                  HubLocalization::systemPreference(), QLocale("zh_CN")),
              HubLocalization::simplifiedChinesePreference());
}

TEST(HubLocalizationTest, SystemPreferenceFallsBackToEnglish)
{
    EXPECT_EQ(HubLocalization::resolveLanguage(
                  HubLocalization::systemPreference(), QLocale("fr_FR")),
              HubLocalization::englishPreference());
}

TEST(HubLocalizationTest, ExplicitPreferenceOverridesSystemLocale)
{
    EXPECT_EQ(HubLocalization::resolveLanguage(
                  HubLocalization::englishPreference(), QLocale("zh_CN")),
              HubLocalization::englishPreference());
    EXPECT_EQ(HubLocalization::resolveLanguage(
                  HubLocalization::simplifiedChinesePreference(),
                  QLocale("en_US")),
              HubLocalization::simplifiedChinesePreference());
}

TEST(HubLocalizationTest, UnknownPreferenceUsesSystemLocale)
{
    EXPECT_EQ(HubLocalization::resolveLanguage(QStringLiteral("unknown"),
                                               QLocale("zh_CN")),
              HubLocalization::simplifiedChinesePreference());
}

TEST(HubLocalizationTest, SimplifiedChineseCatalogIsCompleteAndLoadable)
{
    QTranslator translator;
    ASSERT_TRUE(translator.load(
        QString::fromUtf8(HARKEN_HUB_TRANSLATION_FILE)));
    QCoreApplication::installTranslator(&translator);

    EXPECT_EQ(QCoreApplication::translate("VNH::HubWindow", "Language"),
              QString::fromUtf8("语言"));
    EXPECT_EQ(QCoreApplication::translate("HarkenHub", "Projects"),
              QString::fromUtf8("工程"));
    EXPECT_EQ(QCoreApplication::translate("HarkenHub", "Manage Integration"),
              QStringLiteral("\u96c6\u6210\u7ba1\u7406"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Project recommendation"),
              QStringLiteral("\u5de5\u7a0b\u63a8\u8350\u7248\u672c"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Project file no longer exists: %1")
                  .arg(QStringLiteral("demo.hsp")),
              QStringLiteral("\u5de5\u7a0b\u6587\u4ef6\u5df2\u4e0d\u5b58\u5728\uff1ademo.hsp"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Studio capabilities must use the three-field version 2 report."),
              QStringLiteral("Studio \u80fd\u529b\u62a5\u544a\u5fc5\u987b\u4f7f\u7528\u4ec5\u542b\u4e09\u4e2a\u5b57\u6bb5\u7684\u7248\u672c 2 \u534f\u8bae\u3002"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Install finished · self-test issue"),
              QString::fromUtf8("安装完成 · 自检发现问题"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Launch report protocol version mismatch"),
              QStringLiteral("\u542f\u52a8\u56de\u62a5\u534f\u8bae\u7248\u672c\u4e0d\u5339\u914d"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Project Upgrade Required"),
              QStringLiteral("\u5de5\u7a0b\u9700\u8981\u5347\u7ea7"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Upgrade and Open"),
              QStringLiteral("\u5347\u7ea7\u5e76\u6253\u5f00"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "The project changed after the safety check. Click Open Project to check it again."),
              QStringLiteral("\u5de5\u7a0b\u5728\u5b89\u5168\u68c0\u67e5\u540e\u53d1\u751f\u4e86\u53d8\u5316\u3002\u8bf7\u91cd\u65b0\u70b9\u51fb\u201c\u6253\u5f00\u5de5\u7a0b\u201d\u8fdb\u884c\u68c0\u67e5\u3002"));
    EXPECT_EQ(QCoreApplication::translate(
                  "HarkenHub", "Cannot open the project creation progress channel."),
              QStringLiteral("\u65e0\u6cd5\u6253\u5f00\u5de5\u7a0b\u521b\u5efa\u8fdb\u5ea6\u901a\u9053\u3002"));
    EXPECT_EQ(VNH::LogConsoleDialog::localizeLine(
                  QStringLiteral("[INFO] HarkenHub 0.1.36-dev state -> "
                                 "AwaitingBridge")),
              QStringLiteral("[\u4fe1\u606f] HarkenHub 0.1.36-dev "
                             "\u72b6\u6001 \u2192 \u7b49\u5f85\u66f4\u65b0"
                             "\u6865\u63a5"));
    EXPECT_EQ(VNH::LogConsoleDialog::localizeLine(QStringLiteral(
                  "[INFO] HarkenStudio 0.7.0-dev check: latest build is "
                  "already installed")),
              QStringLiteral("[\u4fe1\u606f] HarkenStudio 0.7.0-dev "
                             "\u68c0\u67e5\uff1a\u5df2\u5b89\u88c5\u6700\u65b0"
                             "\u6784\u5efa"));
    EXPECT_EQ(VNH::LogConsoleDialog::localizeLine(QStringLiteral(
                  "[INFO] HarkenStudio 0.7.0-dev poll result: latest build "
                  "is already installed")),
              QStringLiteral("[\u4fe1\u606f] HarkenStudio 0.7.0-dev "
                             "\u8f6e\u8be2\u7ed3\u679c\uff1a\u5df2\u5b89"
                             "\u88c5\u6700\u65b0\u6784\u5efa"));

    QCoreApplication::removeTranslator(&translator);
}
