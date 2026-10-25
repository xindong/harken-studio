#pragma once

#include "../../Core/PlatformContext.h"

#include <QToolBar>

class QToolButton;
class QAction;
class QComboBox;
class QWidget;

namespace VNX {

class PlatformContextService;

class PlatformBar : public QToolBar
{
    Q_OBJECT

public:
    explicit PlatformBar(QWidget* parent = nullptr);
    explicit PlatformBar(PlatformContextService& service, QWidget* parent = nullptr);
    QToolButton* addLeadingAction(QAction* action, const QString& objectName = {});
    void addCenterWidget(QWidget* widget);
    void addTrailingWidget(QWidget* widget);

private:
    void rebuild();
    void rebuildLanguages();
    void applyContext(const PlatformContext& context);
    void applyLanguage(const QString& language);
    void onAllToggled(bool on);
    void onPlatformChanged(int index);
    void onLanguageChanged(int index);

    PlatformContextService* service_ = nullptr;
    QWidget* host_ = nullptr;
    QWidget* leading_ = nullptr;
    QWidget* center_ = nullptr;
    QWidget* trailing_ = nullptr;
    QToolButton* allButton_ = nullptr;
    QComboBox* platformCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    bool updating_ = false;
};

}
