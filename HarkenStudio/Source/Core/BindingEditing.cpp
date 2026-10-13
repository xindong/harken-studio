#include "BindingEditing.h"

#include "Components/VMixNode.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"

#include <QJsonObject>
#include <QUuid>

namespace VNX {
namespace BindingEditing {

namespace {

bool mintMissingIds(std::vector<ParameterBindingDef>* entries)
{
    if (!entries) return false;
    ProjectData* project = ProjectManager::instance().currentProject();
    for (ParameterBindingDef& binding : *entries) {
        if (!binding.typed || !harkenstudio::IsNull(binding.runtimeId()))
            continue;
        if (!project) return false;
        binding.guid = QUuid::createUuid();
    }
    return true;
}

}

VMixNode* ledgerHost(ProjectItem* item, BindingHostClass hostClass)
{
    return static_cast<VMixNode*>(
        BindingContractSemantics::resolveHostComponent(item, hostClass));
}

const VMixNode* ledgerHost(const ProjectItem* item, BindingHostClass hostClass)
{
    return static_cast<const VMixNode*>(
        BindingContractSemantics::resolveHostComponent(item, hostClass));
}

VMixNode* ledgerHost(ProjectItem* item, const BindingAddress& address)
{
    return ledgerHost(item, address.hostClass);
}

const VMixNode* ledgerHost(const ProjectItem* item, const BindingAddress& address)
{
    return ledgerHost(item, address.hostClass);
}

BindingHostClass hostClassOf(const ProjectItem* item)
{
    return BindingContractSemantics::hostClassOf(
        item ? item->type : ProjectItemType::Sound);
}

QString changeKey(const BindingAddress& address)
{
    return QStringLiteral("bindings.") + BindingSemantics::addressKey(address);
}

namespace {

QJsonObject hostArgs(const ProjectItem* item, const QString& text)
{
    QJsonObject args;
    if (item)
        args[QStringLiteral("guid")] = item->guid.toString(QUuid::WithoutBraces);
    if (!text.isEmpty())
        args[QStringLiteral("undo_text")] = text;
    return args;
}

}

QJsonObject upsertArgs(const ProjectItem* item, const ParameterBindingDef& binding,
                       const QString& text)
{
    QJsonObject args = hostArgs(item, text);
    args[QStringLiteral("binding")] = binding.toJson();
    return args;
}

QJsonObject removeArgs(const ProjectItem* item, harkenstudio::ObjectId runtimeStableId,
                       const QString& text)
{
    QJsonObject args = hostArgs(item, text);
    args[QStringLiteral("runtime_stable_id")] = objectIdString(runtimeStableId);
    return args;
}

QJsonObject removeAllArgs(const ProjectItem* item, const BindingAddress& address,
                          const QString& text)
{
    QJsonObject args = hostArgs(item, text);
    args[QStringLiteral("address")] = BindingSemantics::addressToJson(address);
    return args;
}

QJsonObject reorderArgs(const ProjectItem* item, const BindingAddress& address,
                        int fromLocal, int toLocal, const QString& text)
{
    QJsonObject args = hostArgs(item, text);
    args[QStringLiteral("address")] = BindingSemantics::addressToJson(address);
    args[QStringLiteral("from")] = fromLocal;
    args[QStringLiteral("to")] = toLocal;
    return args;
}

bool apply(ProjectItem* item, const BindingLedger& ledger,
           const QObject* originator)
{
    auto* mix = ledgerHost(item, hostClassOf(item));
    if (!mix) return false;
    std::vector<ParameterBindingDef> entries = ledger.entries();
    if (!mintMissingIds(&entries)) return false;
    BindingLedger checked;
    if (!checked.replace(std::move(entries))) return false;

    std::vector<BindingAddress> addresses;
    auto collect = [&addresses](const BindingLedger& source) {
        for (const ParameterBindingDef& binding : source.entries()) {
            if (!binding.typed) continue;
            bool seen = false;
            for (const BindingAddress& address : addresses) {
                if (address == binding.address) {
                    seen = true;
                    break;
                }
            }
            if (!seen) addresses.push_back(binding.address);
        }
    };
    collect(mix->bindings);
    collect(checked);

    mix->bindings = std::move(checked);
    item->markDirty();
    if (addresses.empty()) {
        emit ProjectManager::instance().itemPropertyChanged(
            item, QStringLiteral("bindings"), originator);
        return true;
    }
    for (const BindingAddress& address : addresses) {
        emit ProjectManager::instance().itemPropertyChanged(
            item, changeKey(address), originator);
    }
    return true;
}

bool apply(ProjectItem* item, const BindingLedger& ledger,
           const BindingAddress& address, const QObject* originator)
{
    auto* mix = ledgerHost(item, address);
    if (!mix) return false;
    std::vector<ParameterBindingDef> entries = ledger.entries();
    if (!mintMissingIds(&entries)) return false;
    BindingLedger checked;
    if (!checked.replace(std::move(entries))) return false;
    mix->bindings = std::move(checked);
    item->markDirty();
    emit ProjectManager::instance().itemPropertyChanged(
        item, changeKey(address), originator);
    return true;
}

bool upsert(ProjectItem* item, ParameterBindingDef binding,
            const QObject* originator)
{
    auto* mix = ledgerHost(item, binding.address);
    if (!mix) return false;
    if (harkenstudio::IsNull(binding.runtimeId())) {
        ProjectData* project = ProjectManager::instance().currentProject();
        if (!project) return false;
        binding.guid = QUuid::createUuid();
    }
    const BindingAddress address = binding.address;
    BindingLedger ledger = mix->bindings;
    if (!ledger.upsert(std::move(binding))) return false;
    BindingLedger checked;
    if (!checked.replace(ledger.entries())) return false;
    return apply(item, std::move(checked), address, originator);
}

bool remove(ProjectItem* item, harkenstudio::ObjectId runtimeStableId,
            const QObject* originator)
{
    auto* mix = ledgerHost(item, hostClassOf(item));
    if (!mix) return false;
    const ParameterBindingDef* current = mix->bindings.find(runtimeStableId);
    if (!current) return false;
    const BindingAddress address = current->address;
    BindingLedger ledger = mix->bindings;
    if (!ledger.remove(runtimeStableId)) return false;
    BindingLedger checked;
    if (!checked.replace(ledger.entries())) return false;
    return apply(item, std::move(checked), address, originator);
}

bool removeAll(ProjectItem* item, const BindingAddress& address,
               const QObject* originator)
{
    auto* mix = ledgerHost(item, address);
    if (!mix) return false;
    BindingLedger ledger = mix->bindings;
    if (!ledger.removeAll(address)) return false;
    BindingLedger checked;
    if (!checked.replace(ledger.entries())) return false;
    return apply(item, std::move(checked), address, originator);
}

bool reorder(ProjectItem* item, const BindingAddress& address,
             int fromLocal, int toLocal, const QObject* originator)
{
    auto* mix = ledgerHost(item, address);
    if (!mix) return false;
    BindingLedger ledger = mix->bindings;
    if (!ledger.reorder(address, fromLocal, toLocal)) return false;
    BindingLedger checked;
    if (!checked.replace(ledger.entries())) return false;
    return apply(item, std::move(checked), address, originator);
}

}

}
