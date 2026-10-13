
#pragma once

#include <QFormLayout>
#include <QGroupBox>
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
    QVBoxLayout* makePageBody(const QString& title);
    QVBoxLayout* makePageBody(QWidget* host, const QString& title);
    QGroupBox* makeGroup(const QString& title);
    QFormLayout* makeForm();
};

}
