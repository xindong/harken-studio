#pragma once

#include <QSize>
#include <QTextBrowser>

class QDialog;

namespace VNH {

void configureDialog(QDialog* dialog, const QSize& preferredSize,
                     const QSize& minimumSize = QSize(320, 180));

class DialogTextView : public QTextBrowser
{
    Q_OBJECT

public:
    explicit DialogTextView(QWidget* parent, Qt::TextFormat format = Qt::PlainText);
    void setText(const QString& text);
    QString text() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    Qt::TextFormat format_;
};

}
