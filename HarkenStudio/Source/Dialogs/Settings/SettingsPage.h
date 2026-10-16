
#pragma once

#include "Widgets/Common/DialogFormLayout.h"

#include <QVBoxLayout>
#include <QWidget>

namespace VNX {

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

    virtual QString pageId() const = 0;
    virtual QString categoryLabel() const = 0;
    virtual void load() = 0;
    virtual bool apply() = 0;
    virtual bool isDirty() const = 0;
    virtual void revert();

signals:
    void dirtyChanged();

protected:
    QVBoxLayout* makePageBody();
    QWidget* makeSection(const QString& title);
    DialogFormLayout* makeForm();
};

}
