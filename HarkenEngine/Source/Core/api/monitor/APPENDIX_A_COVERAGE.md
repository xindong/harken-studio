# Appendix A coverage matrix (M0-6)

This file is the reviewable, version-controlled copy of PLAN-10 appendix A.
Empty `Header field` / `testId` cells are a freeze blocker. M0-1 froze the
envelope. M0-2 appends the payload structs named below. Later packages add
fixtures and codecs only; they must not invent a parallel header.

M0-6 locks each later-milestone fact to an explicit gtest id and a category.
`check_appendix_a.py --check` is the mechanical gate. Deferred rows are not
green. ModelOnly ids never count toward the Runtime exit door.

Columns:

- `category`: `RuntimeVerified` | `ContractVerified` | `ModelOnly` | `deferred`
- `testId`: exact `Suite.Case`. Substring match is forbidden.
- `producingPackage`: M0 pack that submitted the fixture / pin.
- `fixturePath`: checked-in bytes relative to `HarkenEngine/Test/UnitTest/`,
  or empty when the row is a compile-time pin.

| Later milestone fact | Header field | category | testId | producingPackage | fixturePath | deferredOwner | deferredTarget | deferredReason |
|---|---|---|---|---|---|---|---|---|
| Playable / Action / Voice / Bus / Effect instance and final properties | `VnMonitorRecordHeader.entityKind`; `VnMonitorStateUpsert` / `VnMonitorStateTombstone` | ContractVerified | `MonitorFixture.RecordLegalFourSurfaces` | m0-2 | monitor/legal_event.bin |  |  |  |
| Causal request to Action to Playable to Voice to Bus/Effect | header instance / parent handles; `VnMonitorEventPayload` handles | ContractVerified | `MonitorFixture.RecordCheckedInLegalEvent` | m0-2 | monitor/legal_event.bin |  |  |  |
| Nine identity dimensions plus three fingerprints | `stableId`, `instanceHandle`, `instanceGeneration`, `owner*` on tap, fingerprints | ContractVerified | `MonitorFixture.RecordGenerationFieldsDistinct` | m0-2 | monitor/legal_event.bin |  |  |  |
| Three time axes plus device uncertainty | `executeFrame`, `graphFrame`, `devicePresentationEstimate`, uncertainty, epochs | ContractVerified | `MonitorFixture.RecordCheckedInLegalEvent` | m0-2 | monitor/legal_event.bin |  |  |  |
| Reason registry | `VnMonitorRegistryKind`; `Vn_MonitorCaptureRegistry`; header `reason` | ContractVerified | `MonitorFixture.PlanRegistryRevisionKeepsOldDescriptor` | m0-3 | monitor/plan/registry.bin |  |  |  |
| Queue / Transition / Tail events | `VN_MONITOR_RECORD_EVENT`; `VnMonitorEventPayload.eventClass`; entity kinds | ContractVerified | `MonitorFixture.RecordCheckedInLegalEvent` | m0-2 | monitor/legal_event.bin |  |  |  |
| DSP timing / hotspot / critical-path unavailable | `VnMonitorTimeSeriesSample`; `validityFlags`; `VN_MONITOR_VALID_UNSUPPORTED` | ContractVerified | `MonitorFixture.RecordNumericNotReadable` | m0-2 | monitor/legal_event.bin |  |  |  |
| Typed tap descriptor / ExactInstance / FollowSlot | `VnMonitorTapDescriptor`; `VN_MONITOR_TAP_SELECTOR_*` | ContractVerified | `MonitorFixture.PlanRegistryRevisionKeepsOldDescriptor` | m0-3 | monitor/plan/registry.bin |  |  |  |
| Audio chunk provenance / measurement segment | `VnMonitorAudioChunk`; header `measurementSegmentGeneration` | ContractVerified | `MonitorFixture.RecordGenerationFieldsDistinct` | m0-2 | monitor/legal_event.bin |  |  |  |
| Waveform / Scope / Spectrum / Meter / GR | `VnMonitorWaveformBucket` / `VnMonitorScopeResult` / `VnMonitorSpectrumPayload` / `VnMonitorMeterPayload` / `VnMonitorGainReductionPayload`; LUFS Unsupported | ContractVerified | `MonitorFixture.RecordWaveformFields` | m0-2 | monitor/legal_event.bin |  |  |  |
| MeterBlockRing overflow if reused is Aggregate only | `VnMonitorAggregatePayload` | ContractVerified | `MonitorAbiPin.CTranslationUnitCanCallPublicAbi` | m0-1 |  |  |  |  |
| CapturePlan / lease / Stop empty slot | `VnMonitorCapturePlan`; `VnMonitorCaptureControlStatus`; `VN_MONITOR_DEDICATED_STOP_SLOTS` | ContractVerified | `MonitorFixture.PlanStopSequenceEmptyIdempotent` | m0-3 | monitor/plan/stop.bin |  |  |  |
| Registry revision / tap descriptor identity | `VN_MONITOR_RECORD_REGISTRY_REVISION`; `VN_MONITOR_RECORD_TAP_BINDING_CHANGED` | ContractVerified | `MonitorFixture.PlanRegistryRevisionKeepsOldDescriptor` | m0-3 | monitor/plan/registry.bin |  |  |  |
| Capability / effective plan / validity / drop | `validityFlags`; `VnMonitorDropSummary` | ContractVerified | `MonitorFixture.PlanMeasurementLayersStayIndependent` | m0-3 | monitor/plan/layers.bin |  |  |  |
| Capacity default / hard / overflow result | `VN_MONITOR_DEFAULT_*`; `VN_MONITOR_HARD_*` | ContractVerified | `MonitorFixture.PlanCapacityConstantsMatchContract` | m0-3 | monitor/plan/capacity.expected.json |  |  |  |
| Session file magic / chunk / footer / comparison four-state | `VN_MONITOR_SESSION_FILE_MAGIC`; packed 32/24; comparison enum | ContractVerified | `MonitorFixture.SessionCompareFourStates` | m0-4 | monitor/session/legal.bin |  |  |  |
| Remote LocalAcl / LanMtls / allowlist / multi-client drop stage | `VN_MONITOR_SECURITY_*`; handshake / frame counter / remote op / drop stage | ContractVerified | `MonitorFixture.RemoteLocalAclAndLanMtlsRoundTrip` | m0-5 | monitor/remote/handshake_local_acl.bin |  |  |  |
| Open G-gaps stay Unsupported | `VnMonitorCapabilities.monitorImplemented`; LUFS supported fields | RuntimeVerified | `MonitorAbiPin.OpenGGapsStayUnsupportedWhenEngineExistsWithoutWriting` | m0-6 |  |  |  |  |
| Stop / lease live slot run | CapturePlan slot execution | deferred |  |  |  | M1 | M1-4 | Live plan slots belong to Hub CapturePlan, not M0 static fixtures. |
| Poll across Term | `Vn_MonitorPoll` retirement | deferred |  |  |  | M1 | M1-1 | Cross-Term Poll is a Hub session lifetime fact. Listed in MonitorModelOnlyTests.txt so it cannot enter the Runtime exit door. |
| Measurement algorithms | Waveform / Scope / Spectrum / Meter workers | deferred |  |  |  | M3 | M3-2 | Algorithms stay Unsupported until M3 workers. M0 only froze payload fields. |
