#include "FramelessChromeInstall.h"
#include "Core/FramelessChrome.h"

#include <QWidget>

#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)
#include "DialogTitleBar.h"
#include <QWKWidgets/widgetwindowagent.h>
#include <QDialog>
#include <QEvent>
#include <QLayout>
#include <QMargins>
#include <QPushButton>
#include <QString>
#endif

namespace VNX {

#if defined(HARKEN_HAS_QWINDOWKIT) && defined(Q_OS_WIN)

namespace {

class ChromeResizeFilter : public QObject {
public:
    ChromeResizeFilter(QWidget* bar, QObject* parent) : QObject(parent), bar_(bar) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            if (auto* w = qobject_cast<QWidget*>(watched)) {
                bar_->setGeometry(0, 0, w->width(), bar_->minimumHeight());
                bar_->raise();
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* bar_;
};

}

void installFramelessChrome()
{
    setFramelessChromeInstaller([](QWidget* window, const QString& title) {
        if (!window || window->property("harkenFramelessChrome").toBool())
            return;
        window->setProperty("harkenFramelessChrome", true);

        auto* bar = new DialogTitleBar(window);
        bar->setTitle(title.isEmpty() ? window->windowTitle() : title);
        QObject::connect(window, &QWidget::windowTitleChanged,
                         bar, &DialogTitleBar::setTitle);
        bar->show();

        auto* agent = new QWK::WidgetWindowAgent(window);
        agent->setup(window);
        agent->setTitleBar(bar);
        agent->setSystemButton(QWK::WindowAgentBase::Close, bar->closeButton());

        QObject::connect(bar->closeButton(), &QPushButton::clicked, window, [window]() {
            if (auto* dlg = qobject_cast<QDialog*>(window))
                dlg->reject();
            else
                window->close();
        }, Qt::QueuedConnection);

        const int barH = bar->minimumHeight();
        bar->setGeometry(0, 0, window->width(), barH);
        bar->raise();
        if (auto* lay = window->layout()) {
            const QMargins m = lay->contentsMargins();
            lay->setContentsMargins(m.left(), m.top() + barH, m.right(), m.bottom());
        }
        window->installEventFilter(new ChromeResizeFilter(bar, window));
    });
}

#else

void installFramelessChrome() {}

#endif

}
