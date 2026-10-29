
#pragma once

#include "Widgets/Common/DialogFormLayout.h"

#include <QList>
#include <QVBoxLayout>
#include <QWidget>

namespace VNX {

class ToastPresenter;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

    virtual QString pageId() const = 0;
    virtual QString categoryLabel() const = 0;
    virtual void load() = 0;
    bool applySearchFilter(const QString& query);
protected:
    void showError(const QString& message);
    QVBoxLayout* makePageBody();
    QWidget* makeSection(const QString& title);
    DialogFormLayout* makeForm();

private:
    ToastPresenter* feedback_ = nullptr;
    QList<QWidget*> sections_;
};

}
