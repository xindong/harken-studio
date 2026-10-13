#include "InlineRenameEdit.h"

#include "Core/ProjectManager.h"
#include "Core/QtControlSkin.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QValidator>

#include <utility>

namespace VNX {
namespace {

class RenameValidator final : public QValidator
{
public:
    RenameValidator(std::function<QString(const QString&)> validate, QObject* parent)
        : QValidator(parent), validate_(std::move(validate)) {}

    State validate(QString& input, int&) const override
    {
        return validate_(input).isEmpty() ? Acceptable : Intermediate;
    }

private:
    std::function<QString(const QString&)> validate_;
};

}

InlineRenameEdit::InlineRenameEdit(const QString& name, RenameHandler handler, QWidget* parent)
    : QLineEdit(parent), handler_(std::move(handler))
{
    setObjectName(QStringLiteral("inlineRenameEdit"));
    setValidator(new RenameValidator([this](const QString& value) {
        return validationError(value);
    }, this));
    connect(this, &QLineEdit::textChanged, this, [this] {
        commitError_.clear();
        refreshValidation();
    });
    setText(name);
    selectAll();
    refreshValidation();
}

InlineRenameEdit::RenameHandler InlineRenameEdit::projectItemHandler(const QUuid& guid)
{
    return [guid](const QString& name, bool commit) {
        auto& manager = ProjectManager::instance();
        auto* item = manager.findItemByGuid(guid);
        if (!item) return tr("The object no longer exists.");
        QString error;
        if (commit) manager.renameItem(item, name, &error);
        else manager.validateItemRename(item, name, &error);
        return error;
    };
}

InlineRenameEdit::RenameHandler InlineRenameEdit::folderHandler(PhysicalFolder* folder)
{
    return [folder](const QString& name, bool commit) {
        auto& manager = ProjectManager::instance();
        const auto* project = manager.currentProject();
        if (!project || project->folderBucket(folder).isEmpty())
            return tr("The folder no longer exists.");
        QString error;
        if (!manager.validateFolderRename(folder, name, &error)) return error;
        if (commit && name != folder->name) {
            if (!manager.renameFolder(folder, name)) return tr("Failed to rename folder.");
        }
        return QString{};
    };
}

QString InlineRenameEdit::validationError(const QString& name) const
{
    return commitError_.isEmpty() ? handler_(name, false) : commitError_;
}

void InlineRenameEdit::refreshValidation()
{
    const QString error = validationError(text());
    setToolTip(error);
    QtControlSkin::setSemanticProperty(this, "inputInvalid", !error.isEmpty());
}

void InlineRenameEdit::begin(const QString& name)
{
    active_ = true;
    commitError_.clear();
    setText(name);
    refreshValidation();
    show();
    setFocus(Qt::OtherFocusReason);
    selectAll();
}

bool InlineRenameEdit::finish(bool commit)
{
    if (!active_ || committing_) return false;
    if (commit) {
        refreshValidation();
        if (!hasAcceptableInput()) return false;
        committing_ = true;
        QPointer<InlineRenameEdit> self(this);
        const auto handler = handler_;
        const QString error = handler(text(), true);
        if (!self) return error.isEmpty();
        committing_ = false;
        if (!error.isEmpty()) {
            commitError_ = error;
            refreshValidation();
            return false;
        }
    }
    active_ = false;
    hide();
    emit finished(commit);
    return true;
}

void InlineRenameEdit::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish(event->key() != Qt::Key_Escape);
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void InlineRenameEdit::focusOutEvent(QFocusEvent* event)
{
    QLineEdit::focusOutEvent(event);
    if (event->reason() != Qt::PopupFocusReason)
        finish(true);
}

}
