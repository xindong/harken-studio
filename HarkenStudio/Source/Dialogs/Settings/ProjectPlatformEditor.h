#pragma once

#include "Core/ProjectPlatform.h"

#include <QWidget>
#include <QStringList>

class QComboBox;
class QLineEdit;

namespace harken::studio { class DialogFormLayout; }

namespace harken::studio {

class ProjectPlatformEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectPlatformEditor(QWidget* parent = nullptr);
    void setPlatform(const ProjectPlatform& platform, const QStringList& otherNames = {});
    ProjectPlatform platform() const { return platform_; }
    void setReadOnly(bool readOnly);
    bool hasPendingName() const;
    void finishNameEditing();
    void discardNameEditing();
    void addField(const QString& label, QWidget* field);

signals:
    void platformChanged(const ProjectPlatform& before);

private:
    void updateControls();
    QString nameError() const;

    ProjectPlatform platform_;
    QStringList otherNames_;
    bool readOnly_ = false;
    QLineEdit* name_ = nullptr;
    QComboBox* base_ = nullptr;
    DialogFormLayout* form_ = nullptr;
};

}
