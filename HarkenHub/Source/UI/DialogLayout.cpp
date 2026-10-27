#include "UI/DialogLayout.h"

#include <QAbstractTextDocumentLayout>
#include <QDialog>
#include <QEvent>
#include <QScreen>
#include <QTextDocument>
#include <QWindow>

#include <algorithm>

namespace VNH {

namespace {

class DialogSizeConstraint : public QObject
{
public:
    DialogSizeConstraint(QDialog* dialog, const QSize& preferred, const QSize& minimum)
        : QObject(dialog), dialog_(dialog), preferred_(preferred), minimum_(minimum)
    {
        dialog_->installEventFilter(this);
        dialog_->resize(preferred_);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == dialog_ && event->type() == QEvent::Show) {
            if (!connected_) {
                connect(dialog_->windowHandle(), &QWindow::screenChanged, this,
                        [this](QScreen* screen) {
                            watchScreen(screen);
                            constrain(false);
                        });
                connected_ = true;
            }
            watchScreen(dialog_->screen());
            constrain(firstShow_);
            firstShow_ = false;
        }
        return QObject::eventFilter(object, event);
    }

private:
    void watchScreen(QScreen* screen)
    {
        disconnect(screenGeometryConnection_);
        screenGeometryConnection_ = connect(screen, &QScreen::availableGeometryChanged,
                                             this, [this]() { constrain(false); });
    }

    void constrain(bool initial)
    {
        const QSize available = dialog_->screen()->availableGeometry().size();
        const QSize maximum(available.width() * 85 / 100,
                            available.height() * 85 / 100);
        dialog_->setMinimumSize(minimum_.boundedTo(maximum));
        dialog_->setMaximumSize(maximum);
        dialog_->resize((initial ? preferred_ : dialog_->size()).boundedTo(maximum));
    }

    QDialog* dialog_;
    QSize preferred_;
    QSize minimum_;
    QMetaObject::Connection screenGeometryConnection_;
    bool firstShow_ = true;
    bool connected_ = false;
};

}

void configureDialog(QDialog* dialog, const QSize& preferredSize, const QSize& minimumSize)
{
    new DialogSizeConstraint(dialog, preferredSize, minimumSize);
}

DialogTextView::DialogTextView(QWidget* parent, Qt::TextFormat format)
    : QTextBrowser(parent), format_(format)
{
    setReadOnly(true);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setMaximumHeight(120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(document(), &QTextDocument::contentsChanged, this, [this]() { updateGeometry(); });
}

void DialogTextView::setText(const QString& text)
{
    if (format_ == Qt::RichText)
        setHtml(text);
    else
        setPlainText(text);
}

QString DialogTextView::text() const
{
    return toPlainText();
}

QSize DialogTextView::sizeHint() const
{
    if (document()->isEmpty())
        return QSize(320, 0);
    const int height = qCeil(document()->documentLayout()->documentSize().height())
        + 2 * frameWidth() + 4;
    return QSize(320, std::clamp(height, minimumSizeHint().height(), maximumHeight()));
}

QSize DialogTextView::minimumSizeHint() const
{
    return QSize(80, document()->isEmpty() ? 0 : fontMetrics().height() + 12);
}

void DialogTextView::resizeEvent(QResizeEvent* event)
{
    QTextBrowser::resizeEvent(event);
    updateGeometry();
}

}
