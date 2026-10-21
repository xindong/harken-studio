#include "Transactions/TransactionDiagnostics.h"

#include "Platform/PlatformPaths.h"
#include "Transactions/InstallExecutor.h"
#include "Transactions/TransactionJournal.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace VNH {

std::vector<TransactionSummary> TransactionDiagnostics::summarize()
{
    std::vector<TransactionSummary> summaries;
    QHash<QString, int> indexById;

    QFile file(TransactionJournal::filePath());
    if (!file.open(QIODevice::ReadOnly))
        return summaries;

    while (!file.atEnd()) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readLine());
        if (!doc.isObject())
            continue;
        const QJsonObject entry = doc.object();
        const QString transactionId =
            entry[QStringLiteral("transactionId")].toString();
        if (transactionId.isEmpty())
            continue;

        int index;
        const auto found = indexById.constFind(transactionId);
        if (found == indexById.constEnd()) {
            TransactionSummary summary;
            summary.transactionId = transactionId;
            summary.firstAt = entry[QStringLiteral("at")].toString();
            index = static_cast<int>(summaries.size());
            indexById.insert(transactionId, index);
            summaries.push_back(summary);
        } else {
            index = found.value();
        }

        TransactionSummary& summary = summaries[static_cast<size_t>(index)];
        ++summary.entryCount;
        summary.lastAt = entry[QStringLiteral("at")].toString();
        const QString product = entry[QStringLiteral("product")].toString();
        if (!product.isEmpty())
            summary.product = product;
        const QString buildId =
            entry[QStringLiteral("targetBuildId")].toString();
        if (!buildId.isEmpty())
            summary.targetBuildId = buildId;
        const QString state = entry[QStringLiteral("state")].toString();
        if (!state.isEmpty())
            summary.lastState = state;
        if (state != QStringLiteral("Reclaimed")) {
            summary.failureCode = entry[QStringLiteral("failureCode")].toString();
            summary.failureDetail = entry[QStringLiteral("failureDetail")].toString();
        }
    }

    std::stable_sort(summaries.begin(), summaries.end(),
                     [](const TransactionSummary& a,
                        const TransactionSummary& b) {
                         return a.lastAt > b.lastAt;
                     });
    return summaries;
}

QString TransactionDiagnostics::localizeDetail(const QString& detail)
{
    if (detail.contains(QLatin1Char('\n'))) {
        QStringList lines = detail.split(QLatin1Char('\n'));
        for (QString& line : lines)
            line = localizeDetail(line);
        return lines.join(QLatin1Char('\n'));
    }
    static const struct {
        QRegularExpression expression;
        const char* source;
    } messages[] = {
        {QRegularExpression(QStringLiteral(R"(^HarkenStudio is still running from this version: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "HarkenStudio is still running from this version: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot move the install into place: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot move the install into place: %1")},
        {QRegularExpression(QStringLiteral(R"(^Installation retained at (.*)\. Restart Hub to recover it\.$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Installation retained at %1. Restart Hub to recover it.")},
        {QRegularExpression(QStringLiteral(R"(^Cannot open the install index for reading: (.*) \(Qt error ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot open the install index for reading: %1 (Qt error %2: %3)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot read the install index: (.*) \(Qt error ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot read the install index: %1 (Qt error %2: %3)")},
        {QRegularExpression(QStringLiteral(R"(^Install index is corrupt: (.*) \(offset ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Install index is corrupt: %1 (offset %2: %3)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot create file directory: (.*)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot create file directory: %1")},
        {QRegularExpression(QStringLiteral(R"(^Cannot open file for writing: (.*) \(Qt error ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot open file for writing: %1 (Qt error %2: %3)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot write file: (.*) \(Qt error ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot write file: %1 (Qt error %2: %3)")},
        {QRegularExpression(QStringLiteral(R"(^Cannot commit file: (.*) \(Qt error ([0-9]+): (.*)\)$)")),
         QT_TRANSLATE_NOOP("HarkenHub", "Cannot commit file: %1 (Qt error %2: %3)")},
    };
    for (const auto& message : messages) {
        const auto match = message.expression.match(detail);
        if (!match.hasMatch())
            continue;
        const QString translated = QCoreApplication::translate("HarkenHub", message.source);
        return match.lastCapturedIndex() == 3
            ? translated.arg(match.captured(1), match.captured(2), match.captured(3))
            : translated.arg(match.captured(1));
    }
    if (detail == QStringLiteral("Another install transaction is running"))
        return QCoreApplication::translate("HarkenHub", "Another install transaction is running");
    return detail;
}

QString TransactionDiagnostics::failureMessage(const QString& transactionId, int exitCode)
{
    if (exitCode == kInstallMachineDataUnavailableExitCode)
        return QCoreApplication::translate("HarkenHub",
            "Hub could not create its shared data folder:\n%1\n\n"
            "Check the folder's permissions and available disk space, then try again.")
                .arg(machineDataDir());

    for (const auto& summary : summarize()) {
        if (summary.transactionId != transactionId)
            continue;
        const bool reclaimedFailure = summary.lastState == QStringLiteral("Reclaimed") &&
            (!summary.failureCode.isEmpty() || !summary.failureDetail.isEmpty());
        if (summary.lastState != QStringLiteral("Failed") && !reclaimedFailure)
            continue;
        QString reason;
        if (summary.failureCode == QStringLiteral("TargetConflict"))
            reason = QCoreApplication::translate("HarkenHub",
                "The installation folder for this Studio version already exists. Hub will not overwrite it. "
                "Use the existing installation, or uninstall it from Hub before installing again.");
        else if (summary.failureCode == QStringLiteral("InstallBusy"))
            reason = QCoreApplication::translate("HarkenHub",
                "Another installation is running. Wait for it to finish, then try again.");
        else if (summary.failureCode == QStringLiteral("VersionInUse"))
            reason = QCoreApplication::translate("HarkenHub",
                "Close Studio and other programs running from this installation, then retry Repair. No installation files were replaced.");
        else if (summary.failureCode == QStringLiteral("ManifestRejected"))
            reason = QCoreApplication::translate("HarkenHub", "The release manifest failed verification. Installation was blocked.");
        else if (summary.failureCode == QStringLiteral("PackageRejected"))
            reason = QCoreApplication::translate("HarkenHub", "The installation package failed verification. Download it again and retry.");
        else if (summary.failureCode == QStringLiteral("StagingFailed"))
            reason = QCoreApplication::translate("HarkenHub", "The installation files could not be copied to the temporary folder.");
        else if (summary.failureCode == QStringLiteral("ExtractFailed"))
            reason = QCoreApplication::translate("HarkenHub", "The installation package could not be extracted.");
        else
            reason = QCoreApplication::translate("HarkenHub", "Installation failed.");
        if (!summary.failureDetail.isEmpty())
            reason += QStringLiteral("\n\n") + localizeDetail(summary.failureDetail);
        if (!summary.failureCode.isEmpty())
            reason += QCoreApplication::translate("HarkenHub", "\n\nError code: %1").arg(summary.failureCode);
        return reason;
    }
    return {};
}

QJsonArray TransactionDiagnostics::toJson(
    const std::vector<TransactionSummary>& summaries)
{
    QJsonArray array;
    for (const auto& summary : summaries) {
        QJsonObject object;
        object[QStringLiteral("transactionId")] = summary.transactionId;
        object[QStringLiteral("product")] = summary.product;
        object[QStringLiteral("targetBuildId")] = summary.targetBuildId;
        object[QStringLiteral("lastState")] = summary.lastState;
        if (!summary.failureCode.isEmpty())
            object[QStringLiteral("failureCode")] = summary.failureCode;
        if (!summary.failureDetail.isEmpty())
            object[QStringLiteral("failureDetail")] = summary.failureDetail;
        object[QStringLiteral("firstAt")] = summary.firstAt;
        object[QStringLiteral("lastAt")] = summary.lastAt;
        object[QStringLiteral("entryCount")] = summary.entryCount;
        array.append(object);
    }
    return array;
}

}
