#include <QApplication>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <cstdio>

#include <gtest/gtest.h>

namespace {
QMutex diagnosticMutex;
QStringList diagnosticMessages;
QElapsedTimer diagnosticClock;

void diagnosticMessage(QtMsgType, const QMessageLogContext&, const QString& message)
{
    QMutexLocker lock(&diagnosticMutex);
    diagnosticMessages.append(QString::number(diagnosticClock.nsecsElapsed()) + " " + message);
}

class DiagnosticListener : public testing::EmptyTestEventListener
{
    void OnTestEnd(const testing::TestInfo& info) override
    {
        if (!qEnvironmentVariableIsSet("HARKEN_ISSUE51_DUMP") || info.result()->Failed())
            return;
        QMutexLocker lock(&diagnosticMutex);
        for (const QString& message : diagnosticMessages)
            std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        std::fflush(stderr);
    }

    void OnTestStart(const testing::TestInfo&) override
    {
        QMutexLocker lock(&diagnosticMutex);
        diagnosticMessages.clear();
        diagnosticClock.restart();
    }

    void OnTestPartResult(const testing::TestPartResult& result) override
    {
        if (!result.failed())
            return;
        QMutexLocker lock(&diagnosticMutex);
        for (const QString& message : diagnosticMessages)
            std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        std::fflush(stderr);
    }
};
}

int main(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#if defined(Q_OS_WIN)
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen")
        && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", QStandardPaths::writableLocation(QStandardPaths::FontsLocation).toUtf8());
#endif
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    ::testing::InitGoogleTest(&argc, argv);
    diagnosticClock.start();
    qInstallMessageHandler(diagnosticMessage);
    testing::UnitTest::GetInstance()->listeners().Append(new DiagnosticListener);
    return RUN_ALL_TESTS();
}
