
#pragma once

#include <QLineEdit>

class QAction;

namespace VNX {

class SearchField : public QLineEdit
{
    Q_OBJECT

public:
    explicit SearchField(QWidget* parent = nullptr);

private:
    void applyTheme();

    QAction* searchAction_ = nullptr;
    QAction* clearAction_ = nullptr;
};

}
