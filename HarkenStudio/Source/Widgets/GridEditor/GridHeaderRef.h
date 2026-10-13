#pragma once

#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <QEvent>
#include <QLabel>
#include <QLayout>
#include <QResizeEvent>

namespace VNX {

class GridHeaderRef : public ObjectRefWidget
{
public:
    explicit GridHeaderRef(QWidget* parent)
        : ObjectRefWidget(parent)
        , name_(findChild<QLabel*>(QStringLiteral("refName")))
    {
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name_->setMinimumWidth(0);
        setMinimumWidth(0);
    }

    void setSelected(bool selected)
    {
        QtControlSkin::setSemanticProperty(this, "selected", selected);
    }

protected:
    bool event(QEvent* event) override
    {
        const bool handled = ObjectRefWidget::event(event);
        if (event->type() == QEvent::LayoutRequest)
            elideName();
        return handled;
    }

    void resizeEvent(QResizeEvent* event) override
    {
        ObjectRefWidget::resizeEvent(event);
        layout()->activate();
        elideName();
    }

private:
    void elideName()
    {
        if (!name_)
            return;
        const auto* item = ProjectManager::instance().findItemByObjectID(currentObjectID());
        if (!item) {
            name_->setToolTip(toolTip());
            return;
        }
        const QString text = name_->fontMetrics().elidedText(item->name, Qt::ElideRight, name_->width());
        if (name_->text() != text)
            name_->setText(text);
        name_->setToolTip(item->name);
    }

    QLabel* name_ = nullptr;
};

}
