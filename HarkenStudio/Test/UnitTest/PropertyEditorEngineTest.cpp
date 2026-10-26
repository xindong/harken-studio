#include "Widgets/Inspector/PropertyEditorEngine.h"
#include "Widgets/Inspector/ProjectItemInspectorRows.h"
#include "Widgets/Common/CollapsibleSection.h"
#include "ProjectTypes.h"
#include "QtControlSkin.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>
#include <QPlainTextEdit>
#include <QScopeGuard>

#include <memory>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

struct NotesFixture
{
    VNX::PropertyEditorEngine engine;
    std::unique_ptr<QWidget> page;
    QPlainTextEdit* edit = nullptr;

    NotesFixture()
    {
        engine.setSchema({VNX::notesInspectorDescriptor()}, VNX::ComponentType::_Count);
        engine.setItemType(VNX::ProjectItem::Type::Sound);
        page.reset(engine.buildPage());
        edit = page->findChild<QPlainTextEdit*>(QStringLiteral("prop_notes"));
    }
};

}

TEST(PropertyEditorEngineNotes, NarrowLayoutsSettleWithoutChangingText)
{
    ensureApp();
    const QString style = qApp->styleSheet();
    const auto restoreStyle = qScopeGuard([&] { qApp->setStyleSheet(style); });
    qApp->setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
    NotesFixture fx;
    ASSERT_NE(fx.edit, nullptr);
    auto* section = fx.page->findChild<VNX::CollapsibleSection*>();
    ASSERT_NE(section, nullptr);
    section->setDynamicContent(true);
    fx.page->resize(480, 240);
    fx.page->show();

    struct LayoutActivity : QObject {
        QWidget* root = nullptr;
        int events = 0;
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::MetaCall) {
                const auto* widget = qobject_cast<QWidget*>(watched);
                if (widget && (widget == root || root->isAncestorOf(widget))) ++events;
            }
            return false;
        }
    } activity;
    activity.root = fx.page.get();
    qApp->installEventFilter(&activity);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&activity); });
    const QStringList samples{QString(), QStringLiteral("x"), QStringLiteral("notes"),
        QString::fromUtf16(u"\u97f3\u6548\u5907\u6ce8"), QString(4096, QLatin1Char('x')),
        QStringLiteral("First line.\nSecond line.\nThird line.")};
    for (bool expanded : {true, false}) {
        SCOPED_TRACE(expanded);
        section->setExpanded(expanded);
        for (int height : {VNX::QtControlSkin::current().fieldHeight(), 96}) {
            SCOPED_TRACE(height);
            fx.edit->setFixedHeight(height);
            for (const auto& text : samples) {
                SCOPED_TRACE(text.size());
                fx.edit->setPlainText(text);
                for (int width : {320, 128, 64, 48, 32, 24, 16, 8, 4, 1, 0,
                                  1, 4, 8, 16, 24, 32, 48, 64, 128, 320}) {
                    SCOPED_TRACE(width);
                    fx.edit->setFixedWidth(width);
                    int quietPasses = 0;
                    for (int pass = 0; pass < 32 && quietPasses < 2; ++pass) {
                        activity.events = 0;
                        QApplication::processEvents();
                        quietPasses = activity.events == 0 ? quietPasses + 1 : 0;
                    }
                    ASSERT_EQ(quietPasses, 2);
                    EXPECT_EQ(fx.edit->toPlainText(), text);
                }
            }
        }
    }
}
