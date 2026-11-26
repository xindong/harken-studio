#pragma once

#include "WindowPlacement.h"

#include <QMessageBox>
#include <QResizeEvent>

namespace harken::studio {

class StudioMessageBox : public QMessageBox
{
public:
    using QMessageBox::QMessageBox;

    void setContent(const QString& text, const QString& details = {})
    {
        const QFontMetrics font = fontMetrics();
        const QSize available = screen()->availableGeometry().size();
        const int width = qMin(available.width() / 2, 72 * font.averageCharWidth());
        const auto fits = [&](const QString& value, int height) {
            const QRect area(0, 0, width, height);
            const QSize size = font.boundingRect(area, Qt::TextWordWrap, value).size();
            return size.width() <= width && size.height() <= height;
        };
        QStringList paragraphs;
        const bool needsPreview = !fits(text, qMin(available.height() / 4, 8 * font.lineSpacing()));
        if (needsPreview)
            paragraphs.append(text);
        if (!informativeText().isEmpty()
            && !fits(text + QStringLiteral("\n\n") + informativeText(), available.height() / 3)) {
            paragraphs.append(informativeText());
            setInformativeText({});
        }
        if (!details.isEmpty())
            paragraphs.append(details);
        if (!paragraphs.isEmpty())
            setOption(QMessageBox::Option::DontUseNativeDialog);
        setTextFormat(Qt::PlainText);
        if (needsPreview) {
            const QString firstLine = text.trimmed().section(QLatin1Char('\n'), 0, 0).simplified();
            setText(font.elidedText(firstLine, Qt::ElideRight, width));
        } else {
            setText(text);
        }
        setDetailedText(paragraphs.join(QStringLiteral("\n\n")));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QMessageBox::resizeEvent(event);
        if (isVisible() && event->oldSize().isValid()
            && (event->size().width() > event->oldSize().width()
                || event->size().height() > event->oldSize().height()))
            moveWindowInsideScreen(this);
    }
};

inline void showWarningMessage(QWidget* parent, const QString& title, const QString& text,
                               const QString& details = {})
{
    StudioMessageBox box(QMessageBox::Warning, title, {}, QMessageBox::Ok, parent);
    box.setContent(text, details);
    box.exec();
}

}
