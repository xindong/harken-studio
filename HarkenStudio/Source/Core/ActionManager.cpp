
#include "ActionManager.h"
#include "UndoCommands.h"
#include <QDebug>

namespace VNX {


ActionManager::ActionManager()
    : QObject(nullptr)
{
}

ActionManager& ActionManager::instance()
{
    static ActionManager inst;
    return inst;
}


void ActionManager::registerAction(const ActionDef& def)
{
    if (actions_.contains(def.id)) {
        qWarning() << "[ActionManager] Overwriting existing action:" << def.id;
    }
    actions_.insert(def.id, def);
}

bool ActionManager::hasAction(const QString& id) const
{
    return actions_.contains(id);
}


ActionResult ActionManager::execute(const QString& actionId,
                                     const QJsonObject& args,
                                     ActionSource source,
                                     const QObject* originator,
                                     QUndoCommand* undoParent)
{
    Q_ASSERT(QThread::currentThread() == qApp->thread());

    auto* def = findAction(actionId);
    if (!def) {
        return {false, {}, "UNKNOWN_ACTION",
                QString("Action not found: %1").arg(actionId)};
    }

    if (def->validate) {
        QString err = def->validate(args);
        if (!err.isEmpty()) {
            return {false, {}, "VALIDATION_ERROR", err};
        }
    }

    const QObject* previousOriginator = executeOriginator_;
    executeOriginator_ = originator;
    ActionResult result = def->handler(args);
    executeOriginator_ = previousOriginator;

    if (!result.success && source != ActionSource::IPC) {
        qWarning() << "[ActionManager]" << actionId << "failed:"
                   << result.errorCode << result.errorMessage;
    }

    if (result.success && def->undoable && result.undoFn) {
        auto* command = new RuntimeActionCommand(
            result.undoText.isEmpty() ? def->displayName : result.undoText,
            std::move(result.undoFn),
            std::move(result.redoFn),
            result.mergeId,
            undoParent,
            std::move(result.canReplay),
            std::move(result.authoringWriteSet));
        if (!undoParent)
            UndoRedoManager::instance().push(command);
    }

    if (result.success) {
        emit actionExecuted(actionId, args, result.data, source);
    }

    return result;
}


const ActionDef* ActionManager::findAction(const QString& id) const
{
    auto it = actions_.constFind(id);
    if (it == actions_.constEnd())
        return nullptr;
    return &it.value();
}

// All registered action ids, unsorted; a lightweight alternative to actionCatalog().
QStringList ActionManager::actionNames() const
{
    QStringList names;
    names.reserve(actions_.size());
    for (auto it = actions_.constBegin(); it != actions_.constEnd(); ++it)
        names.append(it.key());
    return names;
}

QStringList ActionManager::actionsInCategory(const QString& category) const
{
    QStringList result;
    for (auto it = actions_.constBegin(); it != actions_.constEnd(); ++it) {
        if (it.value().category == category)
            result.append(it.key());
    }
    return result;
}

QJsonObject ActionManager::actionCatalog() const
{
    QJsonObject catalog;
    for (auto it = actions_.constBegin(); it != actions_.constEnd(); ++it) {
        const ActionDef& def = it.value();
        QJsonObject entry;
        entry["id"] = def.id;
        entry["displayName"] = def.displayName;
        entry["category"] = def.category;
        entry["paramSchema"] = def.paramSchema;
        entry["undoable"] = def.undoable;
        catalog[def.id] = entry;
    }
    return catalog;
}


void ActionManager::clearAllForTesting()
{
    actions_.clear();
    transactions_.clear();
    txCounter_ = 0;
}


void ActionManager::setTransactionCallbacks(const TransactionCallbacks& callbacks)
{
    txCallbacks_ = callbacks;
}

TransactionExecutionContext ActionManager::transactionExecutionContext() const
{
    return txExecutionContext_;
}

void ActionManager::setTransactionExecutionContext(
    const TransactionExecutionContext& context)
{
    txExecutionContext_ = context;
}

QString ActionManager::nextTransactionId()
{
    return QString("tx_%1").arg(++txCounter_);
}

QJsonObject ActionManager::buildTransactionSummary(const QJsonObject& tx) const
{
    QJsonObject summary;
    summary["tx_id"] = tx.value("tx_id").toString();
    summary["label"] = tx.value("label").toString();
    summary["status"] = tx.value("status").toString();
    summary["confirmed"] = tx.value("confirmed").toBool(false);
    summary["applied"] = tx.value("applied").toBool(false);
    summary["created_at"] = tx.value("created_at").toString();

    if (tx.contains("confirmed_at")) summary["confirmed_at"] = tx.value("confirmed_at");
    if (tx.contains("applied_at")) summary["applied_at"] = tx.value("applied_at");
    if (tx.contains("rolled_back_at")) summary["rolled_back_at"] = tx.value("rolled_back_at");
    if (tx.contains("error")) summary["error"] = tx.value("error");

    const QJsonArray actions = tx.value("actions").toArray();
    summary["actions_count"] = actions.size();
    if (tx.contains("preview")) summary["preview"] = tx.value("preview");
    if (tx.contains("apply_results")) summary["apply_results"] = tx.value("apply_results");
    if (tx.contains("rollback_steps"))
    {
        const QJsonArray steps = tx.value("rollback_steps").toArray();
        summary["rollback_steps"] = steps;
        summary["rollback_steps_count"] = steps.size();
    }
    if (tx.contains("rollback_report")) summary["rollback_report"] = tx.value("rollback_report");
    return summary;
}

QJsonObject ActionManager::txSuccess(const QJsonObject& result)
{
    QJsonObject r;
    r["status"] = "ok";
    r["result"] = result;
    return r;
}

QJsonObject ActionManager::txError(const QString& code, const QString& msg, const QString& suggestion)
{
    QJsonObject r;
    r["status"] = "error";
    r["code"] = code;
    r["message"] = msg;
    if (!suggestion.isEmpty()) r["suggestion"] = suggestion;
    return r;
}


QJsonObject ActionManager::preview(const QJsonObject& args)
{
    if (!args.contains("actions") || !args.value("actions").isArray())
    {
        return txError("INVALID_ARGS", "actions array is required");
    }

    const QJsonArray rawActions = args.value("actions").toArray();
    if (rawActions.isEmpty())
    {
        return txError("INVALID_ARGS", "actions array cannot be empty");
    }
    if (rawActions.size() > 64)
    {
        return txError("INVALID_ARGS", "actions array exceeds max size (64)");
    }

    QJsonArray normalizedActions;
    QJsonArray previews;

    for (int i = 0; i < rawActions.size(); ++i)
    {
        const QJsonObject actionObj = rawActions[i].toObject();
        const QString cmd = actionObj.value("cmd").toString().trimmed();
        QJsonObject actionArgs = actionObj.value("args").toObject();
        actionArgs.remove("preview");

        if (cmd.isEmpty())
        {
            return txError("INVALID_ARGS",
                           QString("actions[%1].cmd is required").arg(i));
        }
        if (txCallbacks_.isKnownCommand && !txCallbacks_.isKnownCommand(cmd))
        {
            return txError("UNKNOWN_COMMAND",
                           QString("actions[%1] references unknown command '%2'").arg(i).arg(cmd));
        }
        if (txCallbacks_.isPreviewSupported && !txCallbacks_.isPreviewSupported(cmd))
        {
            return txError(
                "PREVIEW_NOT_SUPPORTED",
                QString("actions[%1] command '%2' does not support preview mode yet").arg(i).arg(cmd),
                "Current transactional preview supports: set_bus_properties, create_bus, set_send/remove_send/set_send_level, and container create/add/remove/reorder/set_property commands.");
        }

        QJsonObject rollbackStep;
        QJsonObject previewEntry;
        QString prepError;
        if (!txCallbacks_.rollbackPreparer ||
            !txCallbacks_.rollbackPreparer(cmd, actionArgs, &rollbackStep, &previewEntry, &prepError,
                                            txExecutionContext_))
        {
            return txError(
                "PREVIEW_VALIDATION_FAILED",
                QString("actions[%1] %2: %3").arg(i).arg(cmd).arg(prepError),
                "Fix action arguments/context and retry preview.");
        }

        previewEntry["index"] = i;
        previews.append(previewEntry);
        normalizedActions.append(QJsonObject{
            {"cmd", cmd},
            {"args", actionArgs}
        });
    }

    const QString txID = nextTransactionId();
    const QString label = args.value("label").toString().trimmed();

    QJsonObject tx;
    tx["tx_id"] = txID;
    tx["label"] = label.isEmpty() ? QString("tx:%1").arg(txID) : label;
    tx["status"] = "previewed";
    tx["confirmed"] = false;
    tx["applied"] = false;
    tx["created_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    tx["source"] = args.value("source").toString("ipc");
    tx["actions"] = normalizedActions;
    tx["preview"] = previews;
    tx["rollback_steps"] = QJsonArray{};
    tx["apply_results"] = QJsonArray{};

    transactions_[txID] = tx;

    QJsonObject result = buildTransactionSummary(tx);
    result["stage"] = "preview";
    result["requires_confirm"] = true;
    result["supported_preview"] = true;
    return txSuccess(result);
}


QJsonObject ActionManager::confirm(const QJsonObject& args)
{
    const QString txID = args.value("tx_id").toString().trimmed();
    if (txID.isEmpty())
    {
        return txError("INVALID_ARGS", "tx_id is required");
    }

    auto it = transactions_.find(txID);
    if (it == transactions_.end())
    {
        return txError("TX_NOT_FOUND", QString("Transaction '%1' not found").arg(txID));
    }

    QJsonObject tx = it.value();
    const QString status = tx.value("status").toString();
    if (status == QLatin1String("applied"))
    {
        return txError("TX_ALREADY_APPLIED", QString("Transaction '%1' is already applied").arg(txID));
    }
    if (status == QLatin1String("rolled_back"))
    {
        return txError("TX_ALREADY_ROLLED_BACK", QString("Transaction '%1' was already rolled back").arg(txID));
    }
    if (status == QLatin1String("cancelled"))
    {
        return txError("TX_CANCELLED", QString("Transaction '%1' was cancelled").arg(txID));
    }

    const bool doConfirm = args.value("confirm").toBool(true);
    if (!doConfirm)
    {
        tx["status"] = "cancelled";
        tx["confirmed"] = false;
        tx["cancelled_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        it.value() = tx;

        QJsonObject result = buildTransactionSummary(tx);
        result["stage"] = "confirm";
        result["confirmed"] = false;
        return txSuccess(result);
    }

    tx["status"] = "confirmed";
    tx["confirmed"] = true;
    tx["confirmed_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    it.value() = tx;

    QJsonObject result = buildTransactionSummary(tx);
    result["stage"] = "confirm";
    result["confirmed"] = true;
    return txSuccess(result);
}


QJsonObject ActionManager::apply(const QJsonObject& args)
{
    const QString txID = args.value("tx_id").toString().trimmed();
    if (txID.isEmpty())
    {
        return txError("INVALID_ARGS", "tx_id is required");
    }

    auto it = transactions_.find(txID);
    if (it == transactions_.end())
    {
        return txError("TX_NOT_FOUND", QString("Transaction '%1' not found").arg(txID));
    }

    QJsonObject tx = it.value();
    if (!tx.value("confirmed").toBool(false))
    {
        return txError("TX_NOT_CONFIRMED", QString("Transaction '%1' is not confirmed").arg(txID),
                       "Call confirm_actions(tx_id) before apply_actions.");
    }
    if (tx.value("applied").toBool(false))
    {
        QJsonObject result = buildTransactionSummary(tx);
        result["stage"] = "apply";
        result["already_applied"] = true;
        return txSuccess(result);
    }

    const QJsonArray actions = tx.value("actions").toArray();
    QJsonArray rollbackSteps;
    QJsonArray applyResults;

    auto runRollback = [this](const QJsonArray& steps, QJsonArray* report) -> bool {
        bool allOk = true;
        for (int i = steps.size() - 1; i >= 0; --i)
        {
            const QJsonObject step = steps[i].toObject();
            QString rbError;
            const bool ok = txCallbacks_.rollbackExecutor
                            ? txCallbacks_.rollbackExecutor(step, &rbError, txExecutionContext_)
                            : false;
            if (report)
            {
                report->append(QJsonObject{
                    {"step_index", i},
                    {"kind", step.value("kind").toString()},
                    {"ok", ok},
                    {"error", rbError}
                });
            }
            if (!ok) allOk = false;
        }
        return allOk;
    };

    for (int i = 0; i < actions.size(); ++i)
    {
        const QJsonObject actionObj = actions[i].toObject();
        const QString cmd = actionObj.value("cmd").toString().trimmed();
        QJsonObject actionArgs = actionObj.value("args").toObject();
        actionArgs.remove("preview");

        QJsonObject rollbackStep;
        QJsonObject previewEntry;
        QString prepError;
        if (!txCallbacks_.rollbackPreparer ||
            !txCallbacks_.rollbackPreparer(cmd, actionArgs, &rollbackStep, &previewEntry, &prepError,
                                            txExecutionContext_))
        {
            QJsonArray rollbackReport;
            const bool rollbackOk = runRollback(rollbackSteps, &rollbackReport);

            tx["status"] = rollbackOk ? "apply_failed_rolled_back" : "apply_failed_partial_rollback";
            tx["error"] = QString("Prepare rollback failed at step %1 (%2): %3")
                              .arg(i)
                              .arg(cmd)
                              .arg(prepError);
            tx["failed_step"] = i;
            tx["failed_command"] = cmd;
            tx["rollback_steps"] = rollbackSteps;
            tx["rollback_report"] = rollbackReport;
            it.value() = tx;

            QJsonObject err = txError(
                rollbackOk ? "TX_APPLY_FAILED_ROLLED_BACK" : "TX_APPLY_FAILED_ROLLBACK_FAILED",
                QString("Failed before apply at step %1 (%2): %3")
                    .arg(i)
                    .arg(cmd)
                    .arg(prepError));
            err["tx_id"] = txID;
            err["failed_step"] = i;
            err["failed_command"] = cmd;
            err["rollback_ok"] = rollbackOk;
            err["rollback_report"] = rollbackReport;
            return err;
        }

        const QJsonObject response = txCallbacks_.commandExecutor
                                     ? txCallbacks_.commandExecutor(cmd, actionArgs, txExecutionContext_)
                                     : txError("NO_EXECUTOR", "Transaction command executor not configured");
        if (response.value("status").toString() != QLatin1String("ok"))
        {
            QJsonArray rollbackReport;
            const bool rollbackOk = runRollback(rollbackSteps, &rollbackReport);

            const QString failCode = response.value("code").toString("UNKNOWN_ERROR");
            const QString failMessage = response.value("message").toString("Unknown error");
            tx["status"] = rollbackOk ? "apply_failed_rolled_back" : "apply_failed_partial_rollback";
            tx["error"] = QString("Step %1 (%2) failed: [%3] %4")
                              .arg(i)
                              .arg(cmd)
                              .arg(failCode)
                              .arg(failMessage);
            tx["failed_step"] = i;
            tx["failed_command"] = cmd;
            tx["rollback_steps"] = rollbackSteps;
            tx["rollback_report"] = rollbackReport;
            it.value() = tx;

            QJsonObject err = txError(
                rollbackOk ? "TX_APPLY_FAILED_ROLLED_BACK" : "TX_APPLY_FAILED_ROLLBACK_FAILED",
                QString("Apply failed at step %1 (%2): [%3] %4")
                    .arg(i)
                    .arg(cmd)
                    .arg(failCode)
                    .arg(failMessage));
            err["tx_id"] = txID;
            err["failed_step"] = i;
            err["failed_command"] = cmd;
            err["rollback_ok"] = rollbackOk;
            err["rollback_report"] = rollbackReport;
            return err;
        }

        if (cmd == QLatin1String("create_bus"))
        {
            const QJsonObject resultObj = response.value("result").toObject();
            const qint64 createdBusID = resultObj.value("bus_id").toInteger(-1);
            if (createdBusID <= 0)
            {
                QJsonArray rollbackReport;
                const bool rollbackOk = runRollback(rollbackSteps, &rollbackReport);

                tx["status"] = rollbackOk ? "apply_failed_rolled_back" : "apply_failed_partial_rollback";
                tx["error"] = QString("create_bus did not return valid bus_id at step %1").arg(i);
                tx["failed_step"] = i;
                tx["failed_command"] = cmd;
                tx["rollback_steps"] = rollbackSteps;
                tx["rollback_report"] = rollbackReport;
                it.value() = tx;

                QJsonObject err = txError(
                    rollbackOk ? "TX_APPLY_FAILED_ROLLED_BACK" : "TX_APPLY_FAILED_ROLLBACK_FAILED",
                    QString("create_bus missing bus_id at step %1").arg(i));
                err["tx_id"] = txID;
                err["failed_step"] = i;
                err["failed_command"] = cmd;
                err["rollback_ok"] = rollbackOk;
                err["rollback_report"] = rollbackReport;
                return err;
            }

            rollbackStep = QJsonObject{
                {"kind", "command"},
                {"cmd", "delete_bus"},
                {"args", QJsonObject{
                    {"bus_id", createdBusID}
                }}
            };
        }

        rollbackSteps.append(rollbackStep);
        applyResults.append(QJsonObject{
            {"index", i},
            {"cmd", cmd},
            {"result", response.value("result").toObject()}
        });
    }

    tx["status"] = "applied";
    tx["applied"] = true;
    tx["applied_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    tx["rollback_steps"] = rollbackSteps;
    tx["apply_results"] = applyResults;
    it.value() = tx;

    QJsonObject result = buildTransactionSummary(tx);
    result["stage"] = "apply";
    result["rollback_steps_count"] = rollbackSteps.size();
    return txSuccess(result);
}


QJsonObject ActionManager::rollback(const QJsonObject& args)
{
    const QString txID = args.value("tx_id").toString().trimmed();
    if (txID.isEmpty())
    {
        return txError("INVALID_ARGS", "tx_id is required");
    }

    auto it = transactions_.find(txID);
    if (it == transactions_.end())
    {
        return txError("TX_NOT_FOUND", QString("Transaction '%1' not found").arg(txID));
    }

    QJsonObject tx = it.value();
    const QString status = tx.value("status").toString();
    if (status == QLatin1String("rolled_back"))
    {
        QJsonObject result = buildTransactionSummary(tx);
        result["stage"] = "rollback";
        result["already_rolled_back"] = true;
        return txSuccess(result);
    }
    if (!tx.value("applied").toBool(false))
    {
        return txError("TX_NOT_APPLIED", QString("Transaction '%1' is not applied").arg(txID));
    }

    const QJsonArray steps = tx.value("rollback_steps").toArray();
    if (steps.isEmpty())
    {
        return txError("TX_NO_ROLLBACK_DATA", QString("Transaction '%1' has no rollback steps").arg(txID));
    }

    bool allOk = true;
    QJsonArray rollbackReport;
    for (int i = steps.size() - 1; i >= 0; --i)
    {
        const QJsonObject step = steps[i].toObject();
        QString rbError;
        const bool ok = txCallbacks_.rollbackExecutor
                        ? txCallbacks_.rollbackExecutor(step, &rbError, txExecutionContext_)
                        : false;
        rollbackReport.append(QJsonObject{
            {"step_index", i},
            {"kind", step.value("kind").toString()},
            {"ok", ok},
            {"error", rbError}
        });
        if (!ok) allOk = false;
    }

    tx["rollback_report"] = rollbackReport;
    tx["rolled_back_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    tx["applied"] = false;
    tx["status"] = allOk ? "rolled_back" : "rollback_failed";
    it.value() = tx;

    if (!allOk)
    {
        QJsonObject err = txError("TX_ROLLBACK_FAILED",
                                  QString("Transaction '%1' rollback failed on one or more steps").arg(txID));
        err["tx_id"] = txID;
        err["rollback_report"] = rollbackReport;
        return err;
    }

    QJsonObject result = buildTransactionSummary(tx);
    result["stage"] = "rollback";
    result["rollback_steps_count"] = steps.size();
    return txSuccess(result);
}


QJsonObject ActionManager::getTransaction(const QJsonObject& args)
{
    const QString txID = args.value("tx_id").toString().trimmed();
    if (txID.isEmpty())
    {
        return txError("INVALID_ARGS", "tx_id is required");
    }

    auto it = transactions_.find(txID);
    if (it == transactions_.end())
    {
        return txError("TX_NOT_FOUND", QString("Transaction '%1' not found").arg(txID));
    }

    return txSuccess(buildTransactionSummary(it.value()));
}

}
