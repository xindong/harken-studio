
#pragma once

#include "Core/ProjectTemplateLoader.h"

#include <QString>
#include <QWidget>
#include <vector>

class QButtonGroup;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTextEdit;
class QTreeWidget;

namespace VNX {

class TemplateChooserWidget : public QWidget {
    Q_OBJECT

public:
    struct Result {
        QString             projectName;
        QString             templateId;
        ProjectTemplateData templateData;
    };

    explicit TemplateChooserWidget(QWidget* parent = nullptr);
    ~TemplateChooserWidget() override = default;

    void setNameFieldVisible(bool visible);
    bool isNameFieldVisible() const;

    QString projectName() const;
    void    setProjectName(const QString& name);

    Result result() const;

    bool isValidForCreate() const;

signals:
    void validityChanged(bool valid);
    void templateChanged();

private slots:
    void onTemplateButtonClicked(int id);
    void onPlusCardClicked();
    void onNameChanged();

private:
    void buildUi();
    void rebuildTemplateCards();
    void applySelection(int id);
    void selectTemplateById(const QString& templateId);
    void populateTrees(const ProjectTemplateData& data);
    void clearTrees();
    void showErrorPage(const ValidationResult& vr);
    void showPreviewPage();
    void hidePreviewArea();
    void notifyValidityChanged();

    QPushButton* makeCard(const QString& text,
                          const QString& tooltip = QString());

    QLabel*          nameLabel_      = nullptr;
    QLineEdit*       nameEdit_       = nullptr;
    bool             nameFieldVisible_ = true;

    QWidget*         cardsContainer_ = nullptr;
    QGridLayout*     cardsGrid_      = nullptr;
    QButtonGroup*    templateGroup_  = nullptr;
    QPushButton*     plusCard_       = nullptr;

    QStackedWidget*  previewStack_   = nullptr;
    QWidget*         previewPage_    = nullptr;
    QTreeWidget*     treeBus_        = nullptr;
    QTreeWidget*     treeAtten_      = nullptr;
    QWidget*         errorPage_      = nullptr;
    QLabel*          errorSummary_   = nullptr;
    QTextEdit*       errorText_      = nullptr;

    std::vector<TemplateLocation> currentLocations_;
    int  selectedCardId_   = 0;
    bool errorPageVisible_ = false;
    bool lastValidity_     = false;
    Result result_;
};

}
