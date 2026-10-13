#include "Widgets/TitleBar/TitleBarWidget.h"

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QMenuBar>
#include <QMetaObject>
#include <QScreen>
#include <QString>
#include <QWidget>
#include <QWindow>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class ApplicationStyleSheetRestore
{
public:
    ApplicationStyleSheetRestore() : styleSheet_(qApp->styleSheet()) {}
    ~ApplicationStyleSheetRestore() { qApp->setStyleSheet(styleSheet_); }

    void forceMenuPointSize(int pointSize)
    {
        qApp->setStyleSheet(
            styleSheet_ + QStringLiteral("\n#titleBarMenuBar { font-size: %1pt; }")
                              .arg(pointSize));
    }

private:
    QString styleSheet_;
};

void expectMenuItemFillsHeight(const QMenuBar& menu)
{
    ASSERT_FALSE(menu.actions().isEmpty());
    const QRect geometry = menu.actionGeometry(menu.actions().front());
    EXPECT_EQ(geometry.top(), 0);
    EXPECT_EQ(geometry.bottom(), menu.height() - 1);
}

}

TEST(TitleBarWidget, EmbedsMenuBarAtLeastAsTallAsItems)
{
    ensureApp();
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);

    EXPECT_EQ(bar.height(), menu.height());
    EXPECT_GE(bar.height(), 36);
    EXPECT_GE(bar.height(), menu.fontMetrics().height());
    EXPECT_GE(menu.minimumWidth(), 1);
}

TEST(TitleBarWidget, MenuItemFillsChromeHeight)
{
    ensureApp();
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    bar.resize(960, bar.height());
    bar.show();
    QApplication::processEvents();

    expectMenuItemFillsHeight(menu);

    bar.hide();
}

TEST(TitleBarWidget, PreservesExistingMenuBarStyleSheet)
{
    ensureApp();
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    const QString customStyle = QStringLiteral("QMenuBar { color: rgb(1, 2, 3); }");
    menu.setStyleSheet(customStyle);
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    QApplication::processEvents();

    EXPECT_TRUE(menu.styleSheet().contains(customStyle));
    EXPECT_TRUE(menu.styleSheet().contains(QStringLiteral("padding-top")));
}

TEST(TitleBarWidget, GrowsWhenMenuFontExceedsDefaultChrome)
{
    ensureApp();
    ApplicationStyleSheetRestore restore;
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    restore.forceMenuPointSize(28);
    menu.ensurePolished();
    QEvent fontChange(QEvent::StyleChange);
    QApplication::sendEvent(&bar, &fontChange);
    QApplication::processEvents();

    const int textHeight = menu.fontMetrics().height();
    ASSERT_GE(textHeight, 36);
    EXPECT_EQ(bar.height(), menu.height());
    EXPECT_GE(bar.height(), textHeight);
    expectMenuItemFillsHeight(menu);
    EXPECT_EQ(bar.minButton()->height(), bar.height());
    EXPECT_EQ(bar.maxButton()->height(), bar.height());
    EXPECT_EQ(bar.closeButton()->height(), bar.height());
}

TEST(TitleBarWidget, RelayoutsOnApplicationFontChange)
{
    ensureApp();
    ApplicationStyleSheetRestore restore;
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    QApplication::processEvents();
    const int before = bar.height();
    EXPECT_GE(before, 36);

    restore.forceMenuPointSize(28);
    menu.ensurePolished();
    QEvent fontChange(QEvent::ApplicationFontChange);
    QApplication::sendEvent(&bar, &fontChange);
    QApplication::processEvents();

    EXPECT_EQ(bar.height(), menu.height());
    EXPECT_GT(bar.height(), before);
    EXPECT_GE(bar.height(), menu.fontMetrics().height());
}

TEST(TitleBarWidget, ResyncsChromeWhenScreenDpiChangesWithoutAFontEvent)
{
    ensureApp();
    ApplicationStyleSheetRestore restore;
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    bar.show();
    QApplication::processEvents();

    QWindow* handle = bar.windowHandle();
    ASSERT_TRUE(handle);
    QScreen* screen = handle->screen();
    ASSERT_TRUE(screen);

    restore.forceMenuPointSize(28);
    menu.ensurePolished();
    const int before = bar.height();
    ASSERT_LT(before, menu.fontMetrics().height());

    QMetaObject::invokeMethod(screen, "logicalDotsPerInchChanged", Qt::DirectConnection,
                              Q_ARG(qreal, screen->logicalDotsPerInch()));
    QApplication::processEvents();

    EXPECT_GT(bar.height(), before);
    EXPECT_EQ(bar.height(), menu.height());
    EXPECT_GE(bar.height(), menu.fontMetrics().height());
    expectMenuItemFillsHeight(menu);

    bar.hide();
}

TEST(TitleBarWidget, KeepsMenuItemsVisibleInsteadOfExtension)
{
    ensureApp();
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    menu.addMenu(QStringLiteral("Edit"));
    menu.addMenu(QStringLiteral("Layout"));
    menu.addMenu(QStringLiteral("View"));
    menu.addMenu(QStringLiteral("Tool"));
    menu.addMenu(QStringLiteral("Help"));
    bar.embedMenuBar(&menu);
    bar.resize(960, bar.height());
    bar.show();
    QApplication::processEvents();

    EXPECT_GE(menu.width(), menu.minimumWidth());
    for (QAction* action : menu.actions()) {
        const QRect geometry = menu.actionGeometry(action);
        EXPECT_FALSE(geometry.isEmpty()) << action->text().toStdString();
        EXPECT_GE(geometry.left(), 0) << action->text().toStdString();
        EXPECT_LT(geometry.right(), menu.width()) << action->text().toStdString();
    }

    bar.hide();
}

TEST(TitleBarWidget, ResyncsMinimumWidthWhenMenuActionsChange)
{
    ensureApp();
    VNX::TitleBarWidget bar;
    QMenuBar menu;
    menu.addMenu(QStringLiteral("File"));
    bar.embedMenuBar(&menu);
    QApplication::processEvents();
    const int before = menu.minimumWidth();

    menu.addMenu(QStringLiteral("AVeryLongAdditionalMenuTitle"));
    QApplication::processEvents();

    EXPECT_GT(menu.minimumWidth(), before);
}
