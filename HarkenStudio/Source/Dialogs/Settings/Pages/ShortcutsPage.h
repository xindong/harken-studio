
#pragma once

#include "Dialogs/Settings/SettingsPage.h"

#include <QHash>
#include <QKeySequence>

class QKeySequenceEdit;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace VNX {

inline constexpr const char* kSettingsPageShortcuts = "shortcuts";

class ShortcutsPage : public SettingsPage
{
    Q_OBJECT

public:
    explicit ShortcutsPage(QWidget* parent = nullptr);

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;
    bool apply() override;
    bool isDirty() const override;
    void revert() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void populateShortcutTree();
    void filterShortcutTree(const QString& filter);
    void refreshShortcutRow(QTreeWidgetItem* item);
    void beginShortcutCapture(QTreeWidgetItem* item);
    void cancelShortcutCapture();
    void commitShortcutCapture();
    void stageShortcut(const QString& id, const QKeySequence& sequence);
    void resetShortcutsToDefaults();
    QKeySequence effectiveShortcut(const QString& id) const;
    QString effectiveConflict(const QString& id, const QKeySequence& sequence) const;

    QLineEdit* shortcutSearchEdit_ = nullptr;
    QTreeWidget* shortcutTree_ = nullptr;
    QTreeWidgetItem* shortcutCaptureItem_ = nullptr;
    QKeySequenceEdit* shortcutCaptureEdit_ = nullptr;
    QHash<QString, QKeySequence> pendingShortcuts_;
    QHash<QString, QKeySequence> revertShortcuts_;
    QHash<QString, QKeySequence> appliedShortcuts_;
};

}
