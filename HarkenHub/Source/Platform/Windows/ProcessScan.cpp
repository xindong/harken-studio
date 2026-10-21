#include "Platform/ProcessScan.h"

#include <QDir>
#include <QScopeGuard>

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <comdef.h>

namespace VNH {

bool runningUnrealEditors(QList<RunningProcess>* processes)
{
    processes->clear();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return false;
    const auto uninitialize = qScopeGuard([&]() {
        if (SUCCEEDED(initialized)) CoUninitialize();
    });
    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&locator)))) return false;
    const auto releaseLocator = qScopeGuard([&]() { locator->Release(); });
    IWbemServices* services = nullptr;
    if (FAILED(locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr,
            nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services))) return false;
    const auto releaseServices = qScopeGuard([&]() { services->Release(); });
    if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
            nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE))) return false;
    IEnumWbemClassObject* enumerator = nullptr;
    if (FAILED(services->ExecQuery(_bstr_t(L"WQL"), _bstr_t(
            L"SELECT ExecutablePath, CommandLine FROM Win32_Process WHERE "
            L"Name = 'UnrealEditor.exe' OR Name = 'UE4Editor.exe' OR "
            L"Name LIKE 'UnrealEditor-%' OR Name LIKE 'UE4Editor-%'"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator))) return false;
    const auto releaseEnumerator = qScopeGuard([&]() { enumerator->Release(); });
    for (;;) {
        IWbemClassObject* object = nullptr;
        ULONG count = 0;
        const HRESULT status = enumerator->Next(5000, 1, &object, &count);
        if (FAILED(status) || status == WBEM_S_TIMEDOUT) return false;
        if (!count) return true;
        const auto releaseObject = qScopeGuard([&]() { object->Release(); });
        _variant_t executable;
        _variant_t commandLine;
        if (FAILED(object->Get(L"ExecutablePath", 0, &executable, nullptr, nullptr))
            || FAILED(object->Get(L"CommandLine", 0, &commandLine, nullptr, nullptr))
            || commandLine.vt != VT_BSTR || !commandLine.bstrVal) return false;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(commandLine.bstrVal, &argc);
        if (!argv) return false;
        RunningProcess process;
        if (executable.vt == VT_BSTR)
            process.executable = QString::fromWCharArray(executable.bstrVal);
        for (int i = 1; i < argc; ++i)
            process.arguments.append(QString::fromWCharArray(argv[i]));
        LocalFree(argv);
        processes->append(process);
    }
}

QStringList runningExecutablesUnder(const QString& directory)
{
    return runningExecutablesUnder(directory, -1);
}

QStringList runningExecutablesUnder(const QString& directory,
                                    qint64 excludePid)
{
    QStringList matches;
    const QString prefix = QDir::cleanPath(QDir(directory).absolutePath()) +
                           QStringLiteral("/");
    if (prefix.length() <= 1)
        return matches;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return matches;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (excludePid >= 0 &&
                entry.th32ProcessID == static_cast<DWORD>(excludePid))
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t buffer[MAX_PATH * 2];
            DWORD size = static_cast<DWORD>(std::size(buffer));
            if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                const QString imagePath = QDir::cleanPath(
                    QDir::fromNativeSeparators(
                        QString::fromWCharArray(buffer, static_cast<int>(size))));
                if (imagePath.startsWith(prefix, Qt::CaseInsensitive))
                    matches.append(imagePath);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return matches;
}

QString executablePathForPid(qint64 pid)
{
    if (pid <= 0)
        return QString();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(pid));
    if (!process)
        return QString();
    QString result;
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(process, 0, buffer, &size))
        result = QDir::cleanPath(QDir::fromNativeSeparators(
            QString::fromWCharArray(buffer, static_cast<int>(size))));
    CloseHandle(process);
    return result;
}

bool isProcessRunning(qint64 pid)
{
    if (pid <= 0)
        return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool running = WaitForSingleObject(process, 0) != WAIT_OBJECT_0;
    CloseHandle(process);
    return running;
}

bool terminateProcessById(qint64 pid)
{
    if (pid <= 0)
        return false;
    HANDLE process =
        OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool ok = TerminateProcess(process, 1) != 0;
    CloseHandle(process);
    return ok;
}

}
