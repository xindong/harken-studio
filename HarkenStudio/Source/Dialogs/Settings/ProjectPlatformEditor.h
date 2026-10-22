#pragma once

#include "Core/ProjectPlatform.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLineEdit;

namespace VNX {

class ProportionalTreeWidget;
class TableWidget;

class ProjectPlatformEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ProjectPlatformEditor(QWidget* parent = nullptr);
    void setPlatform(const ProjectPlatform& platform);
    ProjectPlatform platform() const { return platform_; }
    void setReadOnly(bool readOnly);

signals:
    void platformChanged();

private:
    void refreshPolicies(int selected = -1);
    void updateControls();
    void resetProfile();
    void storePolicy(const SizePolicy& policy, int index);

    ProjectPlatform platform_;
    bool readOnly_ = false;
    QLineEdit* name_ = nullptr;
    QComboBox* base_ = nullptr;
    QCheckBox* resample_ = nullptr;
    TableWidget* content_ = nullptr;
    ProportionalTreeWidget* policies_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

}
