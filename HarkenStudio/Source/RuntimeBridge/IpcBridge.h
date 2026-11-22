#pragma once

// IpcBridge -- IPC command + snapshot surface of the runtime bridge.
//
// Scope: the terminal interface the editor's IPC handlers program against
// (IPCHandlers_*, IPCServerInternal). Handlers decode wire requests into
// bridge commands and encode bridge results/snapshots into replies; they see
// this header only -- never a runtime type, never another bridge cluster's
// internals reached around this surface.
//
// Interface intent (M4 alignment): the M4 remote channel mirrors the public
// C API through a generated command table (spec §6.3) and streams
// vintage-stamped snapshots plus droppable advisory notifications (§6.4).
// This surface is shaped as that same command/snapshot pair so that when the
// generated mirror lands, only the bridge's private side changes -- handlers
// keep their call shapes. Commands address content by stable u64 asset IDs
// (spec §2.3); replies are never dropped, advisory streams may be.
//
// All functions operate on the bridge-wide ACTIVE session unless noted
// (global / sessionless). Status fidelity: IpcQueryStatus distinguishes
// no-session from missing-subsystem so handlers keep their exact wire error
// code + message pairs; the AudioEngine-object null check stays handler-side.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include "DiagnosticsBridge.h"   // IpcQueryStatus (single home) + snapshot structs

#include <QString>
#include <cstdint>
#include <vector>

namespace harken::studio {
namespace bridge {

//==============================================================================
// Plugins. The old-runtime plugin-host command surface is gone; this listing
// query remains as the capability-map probe (reports NoEngine/Unavailable,
// never fabricates plugins).
//==============================================================================

struct PluginDesc {
    QString  name, vendor;
    uint32_t typeId = 0, version = 0;
    bool     isSource = false;
    uint32_t paramCount = 0;
    struct Param {
        QString name, unit;
        float defaultValue = 0, minValue = 0, maxValue = 0;
    };
    std::vector<Param> params;
};

IpcQueryStatus pluginList(std::vector<PluginDesc>& out, uint32_t& outCount);

// Mixer format introspection (buffer-config reply's sample-rate half; 0 when
// no session, as before).
int32_t mixerSampleRate();

// True while an engine session is live. Handlers use this only to keep their
// wire error-precedence (session-missing checked before parameter
// validation); it carries no other meaning.
bool engineSessionActive();

// Probe for the CPU-budget manager chain (session -> mixer thread) so the
// set-config handler keeps its distinct not-initialized / not-available
// errors ahead of parameter validation; NoManager = mixer thread missing.
IpcQueryStatus cpuBudgetManagerStatus();

uint64_t monitorGlobalFilterMask();                             // global -- works sessionless

//==============================================================================
// Replay verification (sessionless file utility).
//==============================================================================

struct FileCompareResult {
    bool identical = false, lengthMatch = false;
    uint64_t comparedBytes = 0, differentBytes = 0;
};
bool compareFiles(const QString& referencePath, const QString& candidatePath,
                  FileCompareResult& out);

} // namespace bridge
} // namespace harken::studio
