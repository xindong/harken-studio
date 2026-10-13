#include "UI/LogConsoleDialog.h"

#include "App/FileLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QUrl>
#include <QVBoxLayout>

namespace VNH {

namespace {

QString localizeUpdateState(const QString& state)
{
    if (state == QStringLiteral("AwaitingBridge"))
        return QCoreApplication::translate("HubLog", "AwaitingBridge");
    if (state == QStringLiteral("AwaitingRestart"))
        return QCoreApplication::translate("HubLog", "AwaitingRestart");
    if (state == QStringLiteral("Downloading"))
        return QCoreApplication::translate("HubLog", "Downloading");
    if (state == QStringLiteral("Downloaded"))
        return QCoreApplication::translate("HubLog", "Downloaded");
    if (state == QStringLiteral("Verifying"))
        return QCoreApplication::translate("HubLog", "Verifying");
    if (state == QStringLiteral("Verified"))
        return QCoreApplication::translate("HubLog", "Verified");
    if (state == QStringLiteral("Installed"))
        return QCoreApplication::translate("HubLog", "Installed");
    if (state == QStringLiteral("MetadataAvailable"))
        return QCoreApplication::translate("HubLog", "MetadataAvailable");
    if (state == QStringLiteral("DownloadFailed"))
        return QCoreApplication::translate("HubLog", "DownloadFailed");
    if (state == QStringLiteral("VerificationFailed"))
        return QCoreApplication::translate("HubLog", "VerificationFailed");
    if (state == QStringLiteral("InstallFailed"))
        return QCoreApplication::translate("HubLog", "InstallFailed");
    return state;
}

QString localizeLandingStage(const QString& stage)
{
    if (stage == QStringLiteral("VerifyInstaller"))
        return QCoreApplication::translate("HubLog", "VerifyInstaller");
    if (stage == QStringLiteral("Backup"))
        return QCoreApplication::translate("HubLog", "Backup");
    if (stage == QStringLiteral("HandoffPlace"))
        return QCoreApplication::translate("HubLog", "HandoffPlace");
    if (stage == QStringLiteral("Dispatch"))
        return QCoreApplication::translate("HubLog", "Dispatch");
    return stage;
}

QString localizeLogMessage(const QString& message)
{
    if (message == QStringLiteral(
                       "A hub update is queued; the background service exits "
                       "so the update bridge can land it")) {
        return QCoreApplication::translate(
            "HubLog",
            "A hub update is queued; the background service exits so the "
            "update bridge can land it");
    }

    QRegularExpressionMatch match;
    static const QRegularExpression statePattern(
        QStringLiteral("^(.+?) (\\S+) state -> ([A-Za-z]+)$"));
    match = statePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate("HubLog", "%1 %2 state -> %3")
            .arg(match.captured(1), match.captured(2),
                 localizeUpdateState(match.captured(3)));
    }

    static const QRegularExpression studioCheckPattern(QStringLiteral(
        "^HarkenStudio (\\S+) check: latest build is already installed$"));
    match = studioCheckPattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog",
                   "HarkenStudio %1 check: latest build is already installed")
            .arg(match.captured(1));
    }

    static const QRegularExpression studioPollPattern(QStringLiteral(
        "^HarkenStudio (\\S+) poll result: latest build is already "
        "installed$"));
    match = studioPollPattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog",
                   "HarkenStudio %1 poll result: latest build is already "
                   "installed")
            .arg(match.captured(1));
    }

    static const QRegularExpression studioSequencePattern(QStringLiteral(
        "^HarkenStudio (\\S+) seq (\\d+) is available on the server$"));
    match = studioSequencePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog",
                   "HarkenStudio %1 seq %2 is available on the server")
            .arg(match.captured(1), match.captured(2));
    }

    static const QRegularExpression selfUpdatePattern(
        QStringLiteral("^Hub self-update to (\\S+) completed$"));
    match = selfUpdatePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog", "Hub self-update to %1 completed")
            .arg(match.captured(1));
    }

    static const QRegularExpression landingStagePattern(QStringLiteral(
        "^Hub landing stage ([A-Za-z]+): (\\d+) ms$"));
    match = landingStagePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog", "Hub landing stage %1: %2 ms")
            .arg(localizeLandingStage(match.captured(1)), match.captured(2));
    }

    static const QRegularExpression hubBuildPattern(
        QStringLiteral("^Harken Hub (.+)$"));
    match = hubBuildPattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate("HubLog", "Harken Hub %1")
            .arg(match.captured(1));
    }

    static const QRegularExpression pipeWritePattern(QStringLiteral(
        "^QWindowsPipeWriter: write failed\\. \\((.*)\\)$"));
    match = pipeWritePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog", "QWindowsPipeWriter: write failed. (%1)")
            .arg(match.captured(1));
    }

    static const QRegularExpression updateCheckFailurePattern(
        QStringLiteral("^HarkenStudio update check failed: (.+)$"));
    match = updateCheckFailurePattern.match(message);
    if (match.hasMatch()) {
        return QCoreApplication::translate(
                   "HubLog", "HarkenStudio update check failed: %1")
            .arg(match.captured(1));
    }

    return message;
}

}

QString LogConsoleDialog::localizeTimestamp(const QString& line)
{
    constexpr qsizetype kTimestampLength = 24;
    if (line.size() < kTimestampLength ||
        line.at(kTimestampLength - 1) != QLatin1Char('Z'))
        return line;

    const QDateTime timestamp = QDateTime::fromString(
        line.left(kTimestampLength), Qt::ISODateWithMs);
    if (!timestamp.isValid())
        return line;

    return timestamp.toLocalTime().toString(
               QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) +
           line.mid(kTimestampLength);
}

QString LogConsoleDialog::localizeLine(const QString& line)
{
    QString localized = localizeTimestamp(line);
    const qsizetype levelEnd = localized.indexOf(QStringLiteral("] "));
    if (levelEnd < 0)
        return localized;

    const qsizetype levelStart = localized.lastIndexOf(
        QLatin1Char('['), levelEnd);
    if (levelStart < 0)
        return localized;

    const QString level = localized.mid(levelStart, levelEnd - levelStart + 1);
    QString translatedLevel = level;
    if (level == QStringLiteral("[INFO]"))
        translatedLevel = QCoreApplication::translate("HubLog", "[INFO]");
    else if (level == QStringLiteral("[WARN]"))
        translatedLevel = QCoreApplication::translate("HubLog", "[WARN]");
    else if (level == QStringLiteral("[ERROR]"))
        translatedLevel = QCoreApplication::translate("HubLog", "[ERROR]");
    else if (level == QStringLiteral("[FATAL]"))
        translatedLevel = QCoreApplication::translate("HubLog", "[FATAL]");
    else if (level == QStringLiteral("[DEBUG]"))
        translatedLevel = QCoreApplication::translate("HubLog", "[DEBUG]");

    return localized.left(levelStart) + translatedLevel + QLatin1Char(' ') +
           localizeLogMessage(localized.mid(levelEnd + 2));
}

LogConsoleDialog::LogConsoleDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Harken Hub Activity Log (%1)")
                       .arg(QCoreApplication::applicationVersion()));
    setModal(false);
    resize(860, 420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    view_ = new QPlainTextEdit(this);
    view_->setObjectName(QStringLiteral("logConsoleView"));
    view_->setReadOnly(true);
    view_->setMaximumBlockCount(5000);
    view_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(view_);

    auto* buttonRow = new QHBoxLayout();
    auto* openFolderButton =
        new QPushButton(tr("Open logs folder"), this);
    connect(openFolderButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(FileLog::logDir()));
    });
    buttonRow->addWidget(openFolderButton);
    buttonRow->addStretch();
    auto* closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    tailer_.setFiles({FileLog::filePathFor(QStringLiteral("background")),
                      FileLog::filePathFor(QStringLiteral("gui"))});
    connect(&tailer_, &LogTailer::appended, this,
            &LogConsoleDialog::appendText);
}

void LogConsoleDialog::appendText(const QString& text)
{
    QScrollBar* scroll = view_->verticalScrollBar();
    const bool follow = scroll->value() >= scroll->maximum() - 4;

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        if (FileLog::isIgnorableFrameworkMessage(rawLine))
            continue;
        QString color = QStringLiteral("#C8CDD6");
        if (rawLine.contains(QStringLiteral("[ERROR]")) ||
            rawLine.contains(QStringLiteral("[FATAL]")))
            color = QStringLiteral("#F1707A");
        else if (rawLine.contains(QStringLiteral("[WARN]")))
            color = QStringLiteral("#E0C060");
        const QString line = localizeLine(rawLine);
        view_->appendHtml(QStringLiteral("<span style=\"color:%1;\">%2</span>")
                              .arg(color, line.toHtmlEscaped()));
    }

    if (follow)
        scroll->setValue(scroll->maximum());
}

void LogConsoleDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    tailer_.start(1000);
}

void LogConsoleDialog::closeEvent(QCloseEvent* event)
{
    tailer_.stop();
    QDialog::closeEvent(event);
}

}
