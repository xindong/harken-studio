
#pragma once

#include <QObject>
#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

namespace VNX {

class DialogManager : public QObject
{
    Q_OBJECT

public:
    static DialogManager& instance();

    void registerDialog(const QString& id, QDialog* dialog);

    void unregisterDialog(const QString& id);

    QDialog* getDialog(const QString& id) const;

    QStringList openDialogIds() const;

    bool dismissDialog(const QString& id, int result = QDialog::Rejected);

private:
    explicit DialogManager(QObject* parent = nullptr);
    ~DialogManager() override = default;

    QHash<QString, QDialog*> dialogs_;

    Q_DISABLE_COPY(DialogManager)
};

}
