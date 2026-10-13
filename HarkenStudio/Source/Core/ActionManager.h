
#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QMap>
#include <QStringList>
#include <functional>
#include <memory>
#include <QThread>
#include <QCoreApplication>
#include <QDateTime>

class QUndoCommand;

namespace VNX {

enum class ActionSource { GUI, IPC, Remote, Internal };

struct ActionResult {
    bool success = false;
    QJsonObject data;
    QString errorCode;
    QString errorMessage;
    std::function<void()> undoFn;
    std::function<void()> redoFn;
    QString undoText;
    int mergeId = 0;
    std::function<bool()> canReplay;
};

struct ActionDef {
    QString id;
    QString displayName;
    QString category;
    QJsonObject paramSchema;
    bool undoable = true;

    std::function<ActionResult(const QJsonObject& args)> handler;
    std::function<QString(const QJsonObject& args)> validate;
};

struct TransactionExecutionContext {
    std::shared_ptr<const void> token;
};

struct TransactionCallbacks {
    std::function<QJsonObject(const QString& cmd, const QJsonObject& args,
                              const TransactionExecutionContext& context)> commandExecutor;
    std::function<bool(const QString& cmd, const QJsonObject& args,
                       QJsonObject* rollbackStep, QJsonObject* previewEntry, QString* error,
                       const TransactionExecutionContext& context)> rollbackPreparer;
    std::function<bool(const QJsonObject& step, QString* error,
                       const TransactionExecutionContext& context)> rollbackExecutor;
    std::function<bool(const QString& cmd)> isPreviewSupported;
    std::function<bool(const QString& cmd)> isKnownCommand;
};

class ActionManager : public QObject {
    Q_OBJECT

public:
    static ActionManager& instance();

    void registerAction(const ActionDef& def);
    bool hasAction(const QString& id) const;

    ActionResult execute(const QString& actionId,
                         const QJsonObject& args,
                         ActionSource source = ActionSource::Internal,
                         const QObject* originator = nullptr,
                         QUndoCommand* undoParent = nullptr);

    // GUI originator of the execute() call currently on the stack (nullptr
    // outside execute, including undo/redo replay). Handlers forward it into
    // itemPropertyChanged / busChanged so panels can filter their own echo.
    const QObject* executeOriginator() const { return executeOriginator_; }

    const ActionDef* findAction(const QString& id) const;
    QStringList actionNames() const;
    QStringList actionsInCategory(const QString& category) const;
    QJsonObject actionCatalog() const;

    void setTransactionCallbacks(const TransactionCallbacks& callbacks);
    TransactionExecutionContext transactionExecutionContext() const;
    void setTransactionExecutionContext(const TransactionExecutionContext& context);
    QJsonObject preview(const QJsonObject& args);
    QJsonObject confirm(const QJsonObject& args);
    QJsonObject apply(const QJsonObject& args);
    QJsonObject rollback(const QJsonObject& args);
    QJsonObject getTransaction(const QJsonObject& args);

    void clearAllForTesting();

signals:
    void actionExecuted(const QString& actionId,
                        const QJsonObject& args,
                        const QJsonObject& result,
                        VNX::ActionSource source);

private:
    ActionManager();
    ~ActionManager() override = default;

    QString nextTransactionId();
    QJsonObject buildTransactionSummary(const QJsonObject& tx) const;
    static QJsonObject txSuccess(const QJsonObject& result);
    static QJsonObject txError(const QString& code, const QString& msg,
                               const QString& suggestion = {});

    QMap<QString, ActionDef> actions_;
    const QObject* executeOriginator_ = nullptr;

    QMap<QString, QJsonObject> transactions_;
    quint64 txCounter_ = 0;
    TransactionCallbacks txCallbacks_;
    TransactionExecutionContext txExecutionContext_;
};

class AudioEngine;
class ProjectManager;
class MainWindow;
class Hierarchy;
class ProjectExplorer;
class BlueprintCanvasWidget;
class IPCServer;

}

namespace HarkenEditor {
class AuditionManager;
}

namespace VNX {

void registerMixerActions(ActionManager& mgr,
                          AudioEngine* engine,
                          ProjectManager* projectManager,
                          MainWindow* mainWindow);
void registerFieldGroupActions(ActionManager& mgr, ProjectManager* projectManager);
void registerProjectActions(ActionManager& mgr,
                           ProjectManager* projectManager,
                           ProjectExplorer* projectExplorer,
                           std::function<Hierarchy*()> hierarchyResolver);
void registerProjectPlatformActions(ActionManager& mgr, ProjectManager* projectManager);
void registerContainerActions(ActionManager& mgr,
                              ProjectManager* projectManager);
void registerEditorActions(ActionManager& mgr,
                           ProjectManager* projectManager,
                           AudioEngine* engine,
                           BlueprintCanvasWidget* blueprintCanvas,
                           IPCServer* ipcServer);
void registerAuditionActions(ActionManager& mgr,
                              HarkenEditor::AuditionManager* auditionManager,
                              AudioEngine* engine = nullptr);
void registerSpatialActions(ActionManager& mgr,
                            AudioEngine* engine,
                            ProjectManager* projectManager);
void registerBindingActions(ActionManager& mgr);
void registerAdvancedActions(ActionManager& mgr,
                             AudioEngine* engine);
void registerAssetActions(ActionManager& mgr,
                          AudioEngine* engine,
                          ProjectManager* projectManager);
void registerSystemActions(ActionManager& mgr,
                           AudioEngine* engine,
                           MainWindow* mainWindow,
                           QMap<quintptr, QJsonObject>* activePlaybacks);
void registerReadonlyActions(ActionManager& mgr, IPCServer* ipcServer);

void setupTransactionCallbacks(IPCServer* ipcServer);

}
