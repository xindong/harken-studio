#pragma once

#include <QLineEdit>
#include <QUuid>

#include <functional>

namespace VNX {

struct PhysicalFolder;

class InlineRenameEdit : public QLineEdit
{
    Q_OBJECT

public:
    using RenameHandler = std::function<QString(const QString&, bool)>;

    InlineRenameEdit(const QString& name, RenameHandler handler, QWidget* parent = nullptr);
    void begin(const QString& name);
    bool finish(bool commit);
    static RenameHandler projectItemHandler(const QUuid& guid);
    static RenameHandler folderHandler(PhysicalFolder* folder);

signals:
    void finished(bool committed);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QString validationError(const QString& name) const;
    void refreshValidation();

    RenameHandler handler_;
    QString commitError_;
    bool active_ = true;
    bool committing_ = false;
};

}
