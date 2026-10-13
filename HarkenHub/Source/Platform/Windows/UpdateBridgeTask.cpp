#include "Platform/UpdateBridgeTask.h"

#include <QDir>

#include <windows.h>
#include <comdef.h>
#include <taskschd.h>

#include <memory>

namespace VNH {

namespace {

const wchar_t kTaskName[] = L"HarkenSoundHubUpdateBridge";
const wchar_t kTaskSddl[] = L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;AU)";

struct ComScope
{
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p)
            p->Release();
    }
    T** out() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

QString hresultError(const QString& stage, HRESULT hr)
{
    return QStringLiteral("%1 failed (0x%2)")
        .arg(stage)
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

bool connectTaskService(ComPtr<ITaskService>& service,
                        ComPtr<ITaskFolder>& root, QString* error)
{
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_ITaskService,
                                  reinterpret_cast<void**>(service.out()));
    if (FAILED(hr)) {
        if (error)
            *error = hresultError(QStringLiteral("Task service creation"), hr);
        return false;
    }
    hr = service->Connect(_variant_t(), _variant_t(), _variant_t(),
                          _variant_t());
    if (FAILED(hr)) {
        if (error)
            *error = hresultError(QStringLiteral("Task service connection"), hr);
        return false;
    }
    hr = service->GetFolder(_bstr_t(L"\\"), root.out());
    if (FAILED(hr)) {
        if (error)
            *error = hresultError(QStringLiteral("Task folder lookup"), hr);
        return false;
    }
    return true;
}

}

QString updateBridgeTaskName()
{
    return QString::fromWCharArray(kTaskName);
}

bool updateBridgeTaskAvailable()
{
    ComScope com;
    if (!com.ok())
        return false;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!connectTaskService(service, root, nullptr))
        return false;
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(_bstr_t(kTaskName), task.out())) || !task)
        return false;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled)))
        return false;
    return enabled == VARIANT_TRUE;
}

bool registerUpdateBridgeTask(const QString& hubExePath, QString* error)
{
    ComScope com;
    if (!com.ok()) {
        *error = hresultError(QStringLiteral("COM initialization"), com.hr);
        return false;
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!connectTaskService(service, root, error))
        return false;

    ComPtr<ITaskDefinition> definition;
    HRESULT hr = service->NewTask(0, definition.out());
    if (FAILED(hr)) {
        *error = hresultError(QStringLiteral("Task definition"), hr);
        return false;
    }

    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(definition->get_Principal(principal.out())) && principal) {
        principal->put_UserId(_bstr_t(L"S-1-5-18"));
        principal->put_LogonType(TASK_LOGON_SERVICE_ACCOUNT);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }

    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(definition->get_Settings(settings.out())) && settings) {
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_StartWhenAvailable(VARIANT_FALSE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_ExecutionTimeLimit(_bstr_t(L"PT2H"));
    }

    ComPtr<IActionCollection> actions;
    hr = definition->get_Actions(actions.out());
    if (FAILED(hr) || !actions) {
        *error = hresultError(QStringLiteral("Task actions"), hr);
        return false;
    }
    ComPtr<IAction> action;
    hr = actions->Create(TASK_ACTION_EXEC, action.out());
    if (FAILED(hr) || !action) {
        *error = hresultError(QStringLiteral("Task action creation"), hr);
        return false;
    }
    ComPtr<IExecAction> execAction;
    hr = action->QueryInterface(IID_IExecAction,
                                reinterpret_cast<void**>(execAction.out()));
    if (FAILED(hr) || !execAction) {
        *error = hresultError(QStringLiteral("Task exec action"), hr);
        return false;
    }
    const QString nativePath = QDir::toNativeSeparators(hubExePath);
    execAction->put_Path(_bstr_t(nativePath.toStdWString().c_str()));
    execAction->put_Arguments(_bstr_t(L"--consume-update-queue"));

    ComPtr<IRegisteredTask> registered;
    hr = root->RegisterTaskDefinition(
        _bstr_t(kTaskName), definition.p, TASK_CREATE_OR_UPDATE, _variant_t(),
        _variant_t(), TASK_LOGON_SERVICE_ACCOUNT, _variant_t(kTaskSddl),
        registered.out());
    if (FAILED(hr)) {
        *error = hresultError(QStringLiteral("Task registration"), hr);
        return false;
    }
    return true;
}

bool unregisterUpdateBridgeTask(QString* error)
{
    ComScope com;
    if (!com.ok()) {
        *error = hresultError(QStringLiteral("COM initialization"), com.hr);
        return false;
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!connectTaskService(service, root, error))
        return false;
    const HRESULT hr = root->DeleteTask(_bstr_t(kTaskName), 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        *error = hresultError(QStringLiteral("Task removal"), hr);
        return false;
    }
    return true;
}

bool runUpdateBridgeTask(QString* error)
{
    ComScope com;
    if (!com.ok()) {
        *error = hresultError(QStringLiteral("COM initialization"), com.hr);
        return false;
    }
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (!connectTaskService(service, root, error))
        return false;
    ComPtr<IRegisteredTask> task;
    HRESULT hr = root->GetTask(_bstr_t(kTaskName), task.out());
    if (FAILED(hr) || !task) {
        *error = hresultError(QStringLiteral("Task lookup"), hr);
        return false;
    }
    ComPtr<IRunningTask> running;
    hr = task->Run(_variant_t(), running.out());
    if (FAILED(hr)) {
        *error = hresultError(QStringLiteral("Task start"), hr);
        return false;
    }
    return true;
}

}
