# Runtime test cost and coverage review

This review separates test-content improvements from CI policy. No workflow,
runner, budget, CMake test selection, capacity default or PR/daily label is changed.
The PR keeps the 64/128/256/512 production-capacity checks in their original gate.

## Evidence and limits

The historical [successful Core run](https://github.com/xindong/venus-studio/actions/runs/37602412958)
used head `20b039cac9804d4688a253122f4b80727a87a5ad` on 2026-10-07.
Its Windows job took 1,793 seconds: grouped tests took 1,547.656 seconds,
and compilation took 217 seconds. Two other recorded runs passed every group
but exceeded the unchanged 2,000-second test budget, at 2,105.797 and
2,115.531 seconds. These are historical baselines, not this PR's measurements.

The inventory contains 287 PR groups and 4,035 registered tests, rather than
287 individual tests. The first 10 slow groups account for about 33.7% of test
time; 113 short groups together take 2.864 seconds. Group launch overhead is
about 7.2 seconds. Deleting short rule tests or combining processes cannot
explain a reduction of many minutes.

A local Release Null-backend probe measured approximately 0.8-0.9 seconds per
Init/Term cycle and 2.3-2.5 GiB of process-private memory. This is not a hosted-CI
memory measurement or proof that production initialization should be changed.
Engine tests perform real loading, publication, audio progress and retirement.
A heavy fixture is not automatically unnecessary.

The timing inventory covers all registered PR entries. Semantic review focuses
on the groups and individual cases below; it is not a proof that every one of
4,035 tests is independently necessary or redundant. Current-head validation
and actual timing results belong in the PR's verification section.

## Decisions implemented in this PR

| Test or support code | Value protected | Disposition and preserved oracle |
| --- | --- | --- |
| `AudioSliceLease.ClaimAndInvalidateRaceHasOneWholeOutcome` | Claim/invalidate atomicity and post-slice invalidation consumption | Keep all 10,000 rounds and outcome assertions. Reuse two barrier-synchronized threads instead of creating 20,000 threads. Fresh lease construction remains per round. |
| `MonitorMeasure.BudgetP99PrintsFixedRingCapacity` | Producer ring allocation/lifetime | Replace the misleading skipped P99 name with `FixedRingCapacityIsStableAcrossSessionReopen`. Check zero before allocation and stable capacity through close/reopen. This is not device P99 qualification. |
| `PackComposition.PublishedCommitTailIsNothrowAndCannotRefuse` | Three noexcept signatures | Keep all three `static_assert`s at file scope; remove the empty runtime `SUCCEED` shell. This was a daily entry, so its removal is not a PR-time saving. |
| `TimelinePlanner.KeyedRngSeedDependsOnlyOnKey` | Key separation and order-independent deterministic hashing | Remove only the identical-input self-comparison. The later cross-order comparisons still check determinism; unequal-key and unequal-coordinate checks remain. No material speed gain is claimed. |
| Action rollback and deferred-entry fault hooks | Actual injected failure and rollback-owner preservation | Match the hooks to the test-observation static library, independently of the capacity-override option; assert the one-shot fault is consumed. Shipping ABI and defaults are unchanged. |
| Pack handoff PCM diagnostics | Per-block silence and retirement evidence | Preserve every audio assertion and sample traversal. Buffer the diagnostic trace and print on failing scope exit instead of writing shared fixed-name files. |
| Remote multicast identity fixture | Authoritative reply belongs to this engine instance/request | Queue a foreign announcement, ignore unrelated packets and still require this instance's endpoint identity. A first unrelated datagram is not a product failure. This is fixture correctness, not a major speed claim. |
| Five signal-only test translation units | Signal, resampler, filter and voice-manager checks | Include `SignalTestSupport.h` directly; keep helper implementations unchanged. They no longer acquire full Engine/TestUtil dependencies. No PCH switch or CI configuration is needed. |

## Expensive coverage retained

Historical group times below are from the successful baseline, not predictions
of the final candidate. Genuine integration paths must not be replaced with a
model that cannot reproduce the same publication, audio or ownership failure.

| Group | Seconds / cases | Why retained; useful next investigation |
| --- | --- | --- |
| AuthoredStopAudio | 141.656 / 159 | Stop/cancel, exact source ownership, PCM silence and deferred retirement. Distinguish local rule checks from actual audio/control integration before considering a smaller fixture. |
| OriginalSourceSubmission | 92.766 / 128 | Genuine body installation, loans, release acknowledgements and generation reuse. An invented owner or fake release would weaken the oracle. |
| BusAdmissionStart | 45.532 / 51 | Independent admission conditions, Prepare/Commit races and no partial publish. Map each matrix axis to a distinct refusal path before removing a combination. |
| VariableCurrentSlice | 41.375 / 62 | Current-slice timing, cancellation and whole-slice atomicity. Preserve real audio progression at the relevant boundary. |
| VoicePool | 41.344 / 42 | Voice lifetime, stream end and reclamation. A rate/layout variant can be proposed for lower frequency only after equivalent PR protection is identified. |
| StreamPlayback | 40.313 / 62 | Starvation, end, seek and streaming-state transitions require actual stream progress. |
| CanonicalPackSetActivate | 35.593 / 50 | Activation/replacement and rejection integrity must exercise the real package/publication seam. |
| MonitorMeasure | 33.890 / 115 | Keep implemented observation/budget contracts; do not count model or device-only qualification as verified by Null playback. |

`VariableTicket.OutcomeExpiredVsUnknownTicket` traverses the 4,096-entry history
twice to distinguish expired from unknown. Simply reducing the loop would miss
the second boundary. A future local-service rewrite must demonstrate both
retention boundaries and retain the public API linkage before replacing it.

`VoiceCapacity.ScaleTiersKeepIndependentBoundariesAtomic` keeps all original
production tiers. Tiny or asymmetric capacities can complement them, but are
not automatically equivalent to the supported production sizes.

## Daily candidates and unresolved qualification

No tests are moved to daily in this PR. Lower-frequency candidates are repeated
production-scale stress matrices and long repetitions after a representative
integration check is already proven. Any proposal must list the original IDs,
distinct failure modes, replacement PR assertions, daily execution mechanism,
relevant-change triggers and any increased detection latency. Existing daily
mechanisms should be used; this review does not authorize new CI workflows,
selection changes or renaming tests to evade a gate. Required configuration
changes belong to the repository's CI owner.

The two `MonitorModelOnly` entries are explicit placeholders referenced by the
model contract inventory; retain their truthful skips until corresponding live
behavior is mapped. `MemguardScope.AllocationObserverDetectsRealLogicAllocAndRealloc`
needs its functioning observer configuration; a Release skip proves nothing
about allocation safety. Neither is converted to a green empty assertion.

## Compilation and remaining cost

Daily source is still compiled by the existing Runtime target. Three large
mixed test files and the static/shared/hub-absent Runtime variants remain cost
centers. The variants have different test/export/Hub contracts; combining all
objects without proving identical compile inputs would change what is tested.
This PR narrows five header dependency chains and records that broader build
organization needs separate evidence. It does not claim that an unchanged
build graph stops compiling daily source.

Report cold compilation, representative header rebuilds, single-file rebuilds
and test execution separately. Do not present a cache hit, sums of parallel task
times, or earlier experiments with changed CI as this PR's speedup. The earlier
15-minute target is not a product requirement or an acceptance claim here.
