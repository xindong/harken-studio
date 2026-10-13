#pragma once

#include <QDialog>

namespace VNH {

class ReleaseNotesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ReleaseNotesDialog(QWidget* parent = nullptr);
    bool hasLoadedCatalog() const;

private:
    bool catalogLoaded_ = false;
};

}
