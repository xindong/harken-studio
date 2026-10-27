#include "UI/DialogLayout.h"
#include "UI/MessageDialog.h"
#include "UI/ProgressDialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {

void expectContainedActions(QDialog& dialog)
{
    const QSize available = dialog.screen()->availableGeometry().size();
    EXPECT_LT(dialog.frameGeometry().width(), available.width());
    EXPECT_LT(dialog.frameGeometry().height(), available.height());
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (button->isVisible())
            EXPECT_TRUE(dialog.rect().contains(QRect(button->mapTo(&dialog, QPoint()), button->size())));
    }
}

TEST(DialogLayoutTest, MessagesPreserveLongTextWithoutGrowingBeyondTheScreen)
{
    const QStringList messages{
        QStringLiteral("Short notification."),
        QStringLiteral("A diagnostic entry with a path: D:/Project/Audio/\n").repeated(1000),
        QStringLiteral("D:/") + QString(20000, QLatin1Char('x')),
        QString(20000, QChar(0x754c))};
    for (qsizetype i = 0; i < messages.size(); ++i) {
        VNH::MessageDialog dialog(QMessageBox::Warning, QStringLiteral("Diagnostics"),
                                  messages.at(i), QMessageBox::Cancel, nullptr);
        auto* approve = dialog.addButton(QStringLiteral("Continue"), QMessageBox::AcceptRole);
        dialog.show();
        QCoreApplication::processEvents();
        auto* text = dialog.findChild<QPlainTextEdit*>();
        ASSERT_NE(text, nullptr);
        EXPECT_EQ(text->toPlainText(), messages.at(i));
        EXPECT_TRUE(text->isReadOnly());
        expectContainedActions(dialog);
        if (i > 0) {
            EXPECT_GT(text->verticalScrollBar()->maximum(), 0);
            text->moveCursor(QTextCursor::End);
            text->ensureCursorVisible();
            EXPECT_EQ(text->verticalScrollBar()->value(), text->verticalScrollBar()->maximum());
        }
        const QString screenshots = qEnvironmentVariable("HARKEN_HUB_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty())
            EXPECT_TRUE(dialog.grab().save(QDir(screenshots).filePath(QStringLiteral("message-%1.png").arg(i))));
        approve->click();
        EXPECT_EQ(dialog.clickedButton(), approve);
    }
}

TEST(DialogLayoutTest, DynamicTextScrollsWithoutMovingActionsOrResizingTheWindow)
{
    VNH::ProgressDialog dialog(QStringLiteral("Starting..."), QStringLiteral("Cancel"), nullptr);
    QSignalSpy cancelled(&dialog, &VNH::ProgressDialog::canceled);
    dialog.show();
    QCoreApplication::processEvents();
    const QSize initialSize = dialog.size();
    const QString content = QStringLiteral("A long diagnostic status\n").repeated(1000);
    dialog.setLabelText(content);
    QCoreApplication::processEvents();
    EXPECT_EQ(dialog.size(), initialSize);
    auto* text = dialog.findChild<VNH::DialogTextView*>();
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(text->toPlainText(), content);
    EXPECT_GT(text->verticalScrollBar()->maximum(), 0);
    expectContainedActions(dialog);
    QTest::keyClick(&dialog, Qt::Key_Escape);
    EXPECT_EQ(cancelled.count(), 1);
}

TEST(DialogLayoutTest, OversizedDefaultsAndMinimumsFitTheAvailableScreen)
{
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QPlainTextEdit(&dialog));
    VNH::configureDialog(&dialog, QSize(10000, 10000), QSize(9000, 9000));
    dialog.show();
    QCoreApplication::processEvents();
    expectContainedActions(dialog);
    EXPECT_LE(dialog.minimumWidth(), dialog.maximumWidth());
    EXPECT_LE(dialog.minimumHeight(), dialog.maximumHeight());
    dialog.hide();
    dialog.show();
    QCoreApplication::processEvents();
    expectContainedActions(dialog);
}

TEST(DialogLayoutTest, EscapeAndDefaultButtonsPreserveTheRequestedDecision)
{
    VNH::MessageDialog cancel(QMessageBox::Warning, QStringLiteral("Confirm"),
                              QStringLiteral("Apply changes?"), QMessageBox::Cancel, nullptr);
    cancel.addButton(QStringLiteral("Apply"), QMessageBox::AcceptRole);
    cancel.show();
    QTest::keyClick(&cancel, Qt::Key_Escape);
    EXPECT_EQ(cancel.clickedButton(), cancel.button(QMessageBox::Cancel));
    EXPECT_EQ(cancel.result(), QMessageBox::Cancel);

    VNH::MessageDialog choice(QMessageBox::Question, QStringLiteral("Confirm"),
                              QStringLiteral("Apply changes?"), QMessageBox::Yes | QMessageBox::No, nullptr);
    choice.setDefaultButton(qobject_cast<QPushButton*>(choice.button(QMessageBox::No)));
    choice.show();
    QTest::keyClick(&choice, Qt::Key_Return);
    EXPECT_EQ(choice.clickedButton(), choice.button(QMessageBox::No));
    EXPECT_EQ(choice.result(), QMessageBox::No);
}

}
