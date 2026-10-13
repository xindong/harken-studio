#include "Core/ProjectUpgradeMessages.h"
#include "Core/ProjectUpgradeManager.h"

#include <gtest/gtest.h>

#include <QStringList>

#include <array>

namespace {

TEST(ProjectUpgradeMessages, EveryStableIssueHasReadableText)
{
    for (int value = static_cast<int>(VNX::ProjectIssue::ProjectNotFound);
         value <= static_cast<int>(VNX::ProjectIssue::InternalFailure); ++value) {
        const auto issue = static_cast<VNX::ProjectIssue>(value);
        const VNX::ProjectUpgradeMessages::Presentation presentation =
            VNX::ProjectUpgradeMessages::presentation(issue);
        const QString message = presentation.primaryText();
        EXPECT_FALSE(message.isEmpty()) << VNX::projectIssueCode(issue).toStdString();
        EXPECT_FALSE(presentation.reason.isEmpty());
        EXPECT_FALSE(presentation.nextStep.isEmpty());
        EXPECT_FALSE(presentation.technicalDetails.isEmpty());
        EXPECT_FALSE(message.contains(VNX::projectIssueCode(issue)));
        EXPECT_TRUE(message.contains(QStringLiteral("project"), Qt::CaseInsensitive)
                    || issue == VNX::ProjectIssue::SchemaArchiveDamaged)
            << message.toStdString();
    }
}

TEST(ProjectUpgradeMessages, StableCodesDoNotExposeInternalTerms)
{
    const QStringList forbidden{
        QStringLiteral("schema"),
        QStringLiteral("contract"),
        QStringLiteral("transaction"),
        QStringLiteral("fingerprint"),
        QStringLiteral("parent_ref"),
    };
    for (VNX::ProjectIssue issue : {
             VNX::ProjectIssue::ProjectFileDamaged,
             VNX::ProjectIssue::ProjectChangedDuringUpgrade,
             VNX::ProjectIssue::ProjectSchemaStateMismatch,
             VNX::ProjectIssue::ProjectUpgradeRecoveryRequired,
         }) {
        const QString message = VNX::ProjectUpgradeMessages::userMessage(issue).toLower();
        for (const QString& word : forbidden)
            EXPECT_FALSE(message.contains(word)) << message.toStdString();
    }
}

TEST(ProjectUpgradeMessages, StableCodeStringUsesSameMessage)
{
    const QString code = VNX::projectIssueCode(VNX::ProjectIssue::ProjectInUse);
    EXPECT_EQ(VNX::ProjectUpgradeMessages::userMessage(code, true),
              VNX::ProjectUpgradeMessages::userMessage(
                  VNX::ProjectIssue::ProjectInUse));
}

TEST(ProjectUpgradeMessages, OrdinaryEditorTextIsUnchanged)
{
    const QString ordinary = QStringLiteral("Select an object for the new Action to target.");
    EXPECT_EQ(VNX::ProjectUpgradeMessages::userMessage(ordinary), ordinary);
}

TEST(ProjectUpgradeMessages, EveryUpgradePhaseHasPlainProgressText)
{
    const std::array phases{
        VNX::ProjectUpgradePhase::Analyzing,
        VNX::ProjectUpgradePhase::CopyingStaging,
        VNX::ProjectUpgradePhase::Migrating,
        VNX::ProjectUpgradePhase::ValidatingStaging,
        VNX::ProjectUpgradePhase::BackingUp,
        VNX::ProjectUpgradePhase::Replacing,
        VNX::ProjectUpgradePhase::ValidatingProject,
        VNX::ProjectUpgradePhase::Committing,
        VNX::ProjectUpgradePhase::RollingBack,
        VNX::ProjectUpgradePhase::Completed,
    };
    for (const VNX::ProjectUpgradePhase phase : phases) {
        const QString text = VNX::ProjectUpgradeMessages::progressText(phase);
        EXPECT_FALSE(text.isEmpty());
        EXPECT_FALSE(text.contains(QStringLiteral("schema"),
                                   Qt::CaseInsensitive));
        EXPECT_FALSE(text.contains(QStringLiteral("transaction"),
                                   Qt::CaseInsensitive));
    }
}

TEST(ProjectUpgradeMessages, UpgradeProgressUsesRealCountsAndBytes)
{
    VNX::ProjectUpgradeProgress start;
    start.phase = VNX::ProjectUpgradePhase::CopyingStaging;
    start.totalBytes = 1000;
    VNX::ProjectUpgradeProgress middle = start;
    middle.completedBytes = 500;
    VNX::ProjectUpgradeProgress end = middle;
    end.completedBytes = 1000;
    EXPECT_LT(VNX::ProjectUpgradeMessages::progressPercent(start),
              VNX::ProjectUpgradeMessages::progressPercent(middle));
    EXPECT_LT(VNX::ProjectUpgradeMessages::progressPercent(middle),
              VNX::ProjectUpgradeMessages::progressPercent(end));

    VNX::ProjectUpgradeProgress completed;
    completed.phase = VNX::ProjectUpgradePhase::Completed;
    EXPECT_EQ(VNX::ProjectUpgradeMessages::progressPercent(completed), 100);
}

}
