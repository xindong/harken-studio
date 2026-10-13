
#include "DialogManager.h"
#include <QThread>
#include <QCoreApplication>

namespace VNX {

DialogManager& DialogManager::instance()
{
    static DialogManager inst;
    return inst;
}

DialogManager::DialogManager(QObject* parent)
    : QObject(parent)
{
}

void DialogManager::registerDialog(const QString& id, QDialog* dialog)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (id.isEmpty() || !dialog) return;

    dialogs_[id] = dialog;

    connect(dialog, &QObject::destroyed, this, [this, id]() {
        dialogs_.remove(id);
    });
}

void DialogManager::unregisterDialog(const QString& id)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    dialogs_.remove(id);
}

QDialog* DialogManager::getDialog(const QString& id) const
{
    return dialogs_.value(id, nullptr);
}

QStringList DialogManager::openDialogIds() const
{
    return dialogs_.keys();
}

bool DialogManager::dismissDialog(const QString& id, int result)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    QDialog* dlg = dialogs_.value(id, nullptr);
    if (!dlg) return false;

    dlg->done(result);
    return true;
}

}
