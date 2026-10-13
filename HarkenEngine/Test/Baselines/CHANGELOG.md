# Golden baseline changelog

This tree holds the Runtime recorded or frozen expectations ("golden
baselines") its UnitTest and E2E suites compare against: data whose correctness
cannot be established by reviewing the code alone, and whose
modification redefines released behavior. Test code (analytic golden
cases, ABI pins, contract seams) stays in the corresponding test suite;
only recorded data lives here.

Discipline (binding): every change to a baseline file under this tree
lands in the **same commit** as an entry in this changelog — the tree's
single ledger — stating **what** changed, **why**, and the **evidence**
behind the new recording (test run, live listen, analysis). Baseline
changes are never silent.

## Runtime formats

File format: `<scenario>.f32peaks` — binary little-endian; a `uint32`
block count, then per rendered block two `float32` values (the peak
absolute sample of the left and right master output channels). Files are
recorded by running `harken_runtime_tests` with `HARKEN_WRITE_BASELINES=1` and
verified by the `BaselineRegression` tests at 1e-4 absolute tolerance
per value (one recording must hold across both build toolchains).

Multichannel format: `<scenario>.f32mcpeaks` — binary little-endian; a
`uint32` block count, a `uint32` channel count, then per rendered block
`channelCount` `float32` peak-absolute values in the device channel order
(Appendix A). Used by the 5.1 / 7.1 multichannel scenarios so every device
channel is regression-covered; recorded and verified the same way, same
1e-4 tolerance. A distinct extension so the two formats can never be
cross-read; the stereo `.f32peaks` format is unchanged.

Kernel-characterization format: `<name>_<toolchain>.f32kernel` — binary,
native byte order; a `uint32` magic, a `uint32` channel count, a `uint32`
frame count, then `channelCount * frameCount` `float32` samples in planar
order (all of channel 0, then channel 1). Unlike the peak-envelope formats
above, this records the full per-sample kernel output and is compared
**bit-exactly** (tolerance zero): it freezes the released insert-effect
kernels sample for sample. Recorded via `HARKEN_WRITE_BASELINES=1` and
verified by the `*KernelCharacterization` unit tests. Because the comparison
is bit-exact and the kernel math is not identical across compilers, goldens
are **per-toolchain**: each build channel records and compares its own file,
named by a compile-time suffix (`clang` for any Clang front end including
clang-cl, `msvc` for a pure MSVC front end). So each kernel has two files
(`_clang` and `_msvc`), each authoritative only for the toolchain that
recorded it; recording on a channel that lacks its file is a normal,
documented event, not a regression.

## Entries (newest first)

- 2026-10-13 all baselines - moved with the Runtime tests from
  `HarkenTest/Runtime/Baselines/` to `HarkenEngine/Test/Baselines/`.
  Tests and recorded data now belong to the Engine project. Evidence:
  all 16 recorded data files match their previous SHA-256 hashes;
  no baseline was recorded or changed.

- 2026-10-13 all baselines — moved the recorded data from
  `HarkenTest/Baselines/Runtime/` and its changelog from
  `HarkenTest/Baselines/CHANGELOG.md` into `HarkenTest/Runtime/Baselines/`.
  Both consuming suites belong to Runtime, so the directory now reflects
  product ownership while remaining shared by UnitTest and E2E. Evidence:
  SHA-256 hashes match for all 16 recorded data files before and after
  the move; no baseline was recorded or changed.

- 2026-10-09 Correct the mono reference level introduced by #673. Matching
  quiet 2D and 3D output did not preserve the original source: both centered
  stereo channels were 3.0103 dB too low. Mono now uses unity per side at
  stereo center, squared gain 2 across non-mono layouts, and unity in a
  native mono mix. Stereo-source matrices and authored gains are unchanged.
  - Restore the exact pre-#673 bytes for nine centered 2D scenarios:
    `bus_graph_scene`, `bus_ramp_fade_stop`, `variable_lowpass_sweep`,
    `streamed_subregion_loop`, `noncommuting_effect_chain`, `combined_m2_scene`,
    `priority_steal_scene`, `park_return_scene`, and
    `multichannel_5_1_perbus_custom_fold`. This also restores the original
    input to nonlinear bus effects; it does not rescale their final peaks.
  - Independently derive `flyby_pan_attenuation`, `multichannel_5_1_pan_bed`,
    and `multichannel_7_1_to_stereo_fold` with the frozen pre-matrix Runtime
    at `3475746bb3550549f50914ea012164f9f0ac7b95`. Its unchanged reference
    harness reproduces all twelve old baselines before and after recording.
    The only harness change multiplies the three spatial harmonic sources
    by sqrt(2); these sources have no voice inserts, so this is equivalent
    to scaling their linear pan output before summing the unchanged bed.
    Each new scene repeats successfully; the pure flyby also agrees with
    independently scaled old peaks within 7.2e-5. The candidate renderer
    does not generate these expectations.
  All block/channel counts and the existing 1e-4 tolerance are unchanged.
  Original-source sample tests additionally require unity at center in
  2D/3D and throughout Mix transitions. The reported explosion WAV was
  captured twice through the real Runtime NullBackend: all 237428 source
  frames match exactly on both outputs, with zero peak/RMS difference.
  This numerical evidence does not claim device or human acceptance.

- 2026-10-09 Centered mono 2D now uses the same equal-power front pair as
  mono 3D: each output is `1/sqrt(2)` of the post-voice signal. Updated ten
  Runtime peak envelopes for this intentional sound-law change, with no
  source, author gain, timing, lifecycle, or tolerance compensation:
  - `bus_ramp_fade_stop`, `variable_lowpass_sweep`, `streamed_subregion_loop`,
    `noncommuting_effect_chain`, `combined_m2_scene`, `priority_steal_scene`,
    and `park_return_scene`: multiply each frozen peak by `1/sqrt(2)`.
    Their voice effects precede the changed fold and retain their original
    input and state. The largest difference against the independent
    reference render is below `1e-7`.
  - `bus_graph_scene`: an independent float scalar model reproduces the
    old envelope exactly, then applies the new coefficient before the
    A-to-B send, B fader and nonlinear feedback delay. Scaling the old
    final peaks alone would be incorrect.
  - `multichannel_5_1_perbus_custom_fold`: independently regenerate the
    deterministic noise and six-channel bed, apply the new mono coefficient
    before the custom/reset bus matrix, sum samples, then take block peaks.
    The same model reproduces every old value exactly before the change.
  - `multichannel_5_1_pan_bed`: a separate reference build from main
    `3475746bb3550549f50914ea012164f9f0ac7b95` first reproduces all twelve
    old scenarios. Its only behavior change is multiplying centered mono
    2D scratch by `1/sqrt(2)` after voice FX and before the old fold.
    The reference then records and repeats all twelve scenarios in an
    isolated directory. FC/LFE/surround peaks stay byte-identical; the
    moving 3D source and multichannel bed are untouched. This reference
    also agrees with the nine mathematical derivations above within `1e-7`.
  The pure 3D flyby and 7.1-to-stereo baselines remain byte-identical.
  All block/channel counts and the `1e-4` comparison tolerance are retained.
  Evidence is numerical NullBackend validation; it does not claim device
  playback or human listening acceptance.

- 2026-09-14 `bus_ramp_fade_stop.f32peaks` — refreshed after PLAN-12 R1
  frame-accurate Bus Volume (`OutputGain`, N=ceil(ms*rate/1000)). 300 ms at
  48 kHz is 14400 frames; the prior envelope still assumed whole-block
  rounding (57×256=14592). Peak shape is the product, not a looser gate:
  `kPeakTolerance` stays 1e-4. First exceeds the old recording at block 49
  (left, delta 1.46e-4), shortly after the scripted −18 dB ramp at block 47;
  maximum delta 2.83e-3 at block 103 (left). Same 282-block stereo layout.
  Recorded with `HARKEN_WRITE_BASELINES=1` on the Ninja+clang Debug channel;
  `BaselineRegression.BusRampFadeStop` re-run green in default mode. Other
  `.f32peaks` / `.f32mcpeaks` not retouched.
  - `bus_ramp_fade_stop.f32peaks`: sha256
    `df9d712ea382fb9c91ea768747936619c6c89cf8e1e1292f061cb9a87a34649b`

- 2026-07-29 all baselines — directory moved from
  `HarkenTest/Runtime/E2E/baselines/` to `HarkenTest/Baselines/Runtime/`,
  and this changelog promoted to `HarkenTest/Baselines/CHANGELOG.md` as
  the single ledger for the whole tree (per-product subdirectories;
  future Editor/Hub recordings land beside `Runtime/` under the same
  discipline). Rationale: the old `E2E/` placement was misleading — the
  directory already served two suites (the E2E `BaselineRegression`
  scenes and the UnitTest `*KernelCharacterization` goldens) — and a
  single stable root gives review guards one path to watch. `git mv`,
  byte content of every file unchanged (git rename detection reports
  100% on all 17 files). The `HARKEN_BASELINE_DIR` compile definition and
  both `#error` sentinels (`BaselineRegressionTest.cpp`,
  `EffectKernelCharacterizationTest.cpp`) now point here. Evidence: all
  19 baseline-consuming tests (`BaselineRegression*` plus
  `*KernelCharacterization*`) green in default mode on the Ninja+clang
  Debug channel after the move; no recording was performed.

- 2026-07-21 `flyby_pan_attenuation.f32peaks`,
  `multichannel_5_1_pan_bed.f32mcpeaks`, and
  `multichannel_7_1_to_stereo_fold.f32mcpeaks` — refreshed after the
  live spatial-target smoothing production change (first-evaluation
  snap plus ~10 ms pan/attenuation transition on moving voices,
  production paths `HarkenEngine/Source/Runtime/render/src/VoiceCascade.cpp`
  and `HarkenEngine/Source/Runtime/render/src/VoicePool.cpp`) shifted the
  per-block peak timing of the three moving-voice composite scenes; the
  prior envelopes predate that behavior. Why the product output is
  trusted: the rendered envelopes of all three scenes are byte-identical
  between the pre-existing smoothing tree (`2a8cd305`, before any
  Spatialization Mix work) and the current tree, the analytic pan/downmix
  goldens (`multichannel_pan_golden_test`, `downmix_golden_test`) and the
  sample-domain continuity tests stay green, and no tolerance was
  changed. Divergence versus the prior recordings: flyby first exceeds
  1e-4 at block 78 (left, delta 1.08e-4), maximum delta 7.79e-4 at block
  155 (left), correlation 0.998773 with best lag -1 block; 5.1 pan bed
  first exceeds at block 17 channel 2 (delta 1.06e-4), maximum delta
  2.63e-2 at block 148 channel 1, correlation 0.999903 lag 0; 7.1 fold
  first exceeds at block 1 channel 0 (delta 1.00e-4), maximum delta
  2.30e-2 at block 140 channel 0, correlation 0.999555 lag 0 — timing
  drift of the same envelope shape, not a level or image change.
  Recorded with `HARKEN_WRITE_BASELINES=1` on the Ninja+clang Debug
  channel; all twelve `BaselineRegression` tests verified green in
  default mode on both toolchains (Ninja+clang Debug and MSVC
  windows-vs-dev Debug) after recording.
  - `flyby_pan_attenuation.f32peaks`: sha256
    `96ff216738e2d87713ec76c21a7b2dd7701c5c648f5d9a5811fbfc157e7680fd`
  - `multichannel_5_1_pan_bed.f32mcpeaks`: sha256
    `53f898e07224812b2c427235bc045d0cedc239ba8c78d34d797ee5428f481c88`
  - `multichannel_7_1_to_stereo_fold.f32mcpeaks`: sha256
    `4cf8870ff2e7a9125f13d3ec0a90eacd03cabc29637b7c06f05923f939a0b547`

- 2026-07-19 `flyby_pan_attenuation.f32peaks` and
  `variable_lowpass_sweep.f32peaks` — refreshed after replacing the
  per-tick hard cutoff with a 10 ms filter crossfade. Production paths:
  `HarkenEngine/Source/Runtime/dsp/include/harken/dsp/Biquad.h`,
  `HarkenEngine/Source/Runtime/render/src/BuiltinEffects.cpp`,
  `HarkenEngine/Source/Runtime/render/src/VoiceCascade.cpp`, and
  `HarkenEngine/Source/Runtime/render/src/VoicePool.cpp`; regression coverage:
  `HarkenTest/Runtime/E2E/BaselineRegressionTest.cpp`. Evidence: Flyby first
  exceeds the prior 1e-4 tolerance at block 112 near closest approach,
  reverses sign at the geometry turn, and has a finite single peak with a
  maximum delta of 1.65e-4. VariableLowpass first differs at block 1, has a
  finite maximum delta of 2.09e-2, and correlates at 0.997006 with best lag 0
  (the expected sub-block transition). Recorded and verified with the Debug
  and RelWithDebInfo channels using the two focused baseline tests.
  - `flyby_pan_attenuation.f32peaks`: sha256
    `662fb3220bc8949e6a3f2177e6c07182ebf1f53859bd2e45a2b291c78da3c05c`
  - `variable_lowpass_sweep.f32peaks`: sha256
    `720b17a7013c2ef00534d61972d64cf3d86dd89d8659f54f99e4be82d4346fda`

- 2026-07-16 `delay_kernel_characterization_{clang,msvc}.f32kernel` — initial
  recording (feedback-delay kernel, sample-exact characterization; one file
  per toolchain). Stimulus: the kernel is driven at the InsertEffect seam
  (Acquire -> StateFor -> BuildEffect -> reset -> ApplyEffectDesc) at 48 kHz
  stereo with descriptor delayMs=10, feedback=0.5, wet=1.0, dry=1.0 over a
  deterministic input (unit impulse on frame 0 of both channels plus a fixed
  xorshift32 pseudo-random tail, seed 0x1234ABCD, samples mapped to exact
  multiples of 1/131072 in [-0.25,0.25)), 8 blocks of 256 frames; each of the
  four parameters is stepped once via setParam at a distinct block boundary:
  block 4 feedback=0.7, block 5 wet=0.6, block 6 dry=0.4, block 7
  delayMs=15.0. Why: freezes the released delay recurrence and soft-saturation
  output byte for byte, including every parameter's mid-stream routing.
  How to re-record: build the target channel, then run harken_runtime_tests with
  the environment variable `HARKEN_WRITE_BASELINES=1` and gtest filter
  `--gtest_filter=DelayKernelCharacterization.FullOutputGolden`; the clang
  channel (preset windows-ninja-clang-dev / build-nc) writes the `_clang`
  file, the MSVC channel (preset windows-vs-dev / build-vs) writes the `_msvc`
  file. Verified bit-exactly by the same test in default mode.
  - `_clang`: sha256: 5c2bb2ac5b5386c0345a5ff70031690dc1ee5ea8200c77814ebde1cd7bcbdfdf | toolchain: clang-cl 19.1.5 (VS2022 LLVM) | config: RelWithDebInfo, windows-ninja-clang-dev
  - `_msvc`:  sha256: 5c2bb2ac5b5386c0345a5ff70031690dc1ee5ea8200c77814ebde1cd7bcbdfdf | toolchain: MSVC 14.42.34433 (VS 17 2022) | config: Debug, windows-vs-dev
  - Note: both toolchains currently produce identical bytes for this stimulus
    (exactly representable input); the per-toolchain split is kept defensively.

- 2026-07-16 `tone_kernel_characterization_{clang,msvc}.f32kernel` — initial
  recording (three-band-tone kernel, sample-exact characterization; one file
  per toolchain). Stimulus: same seam, input, and 8x256 block layout as the
  delay golden. Base descriptor (block 0): low shelf +6 dB @ 200 Hz, peak
  -4 dB @ 1000 Hz Q=1.5, high shelf +3 dB @ 6000 Hz. Each of the seven tone
  parameters is then stepped once via setParam at a distinct block boundary:
  block 1 lowShelfGainDb=-10, block 2 lowShelfFreqHz=400, block 3
  peakGainDb=12, block 4 peakFreqHz=3000, block 5 peakQ=6.0, block 6
  highShelfGainDb=-8, block 7 highShelfFreqHz=9000 (all inside the validated
  ranges, each visibly reshaping the spectrum). Why: freezes the released RBJ
  coefficient derivation and biquad state update byte for byte, and pins every
  setParam index's routing so a swapped dispatch entry is caught.
  How to re-record: build the target channel, then run harken_runtime_tests with
  `HARKEN_WRITE_BASELINES=1` and gtest filter
  `--gtest_filter=ToneKernelCharacterization.FullOutputGolden`; the clang
  channel (windows-ninja-clang-dev / build-nc) writes `_clang`, the MSVC
  channel (windows-vs-dev / build-vs) writes `_msvc`. Verified bit-exactly by
  the same test in default mode.
  - `_clang`: sha256: 7f43fc859808cea004ff99cf7c9181968c93fd87b940b5dc9120c22935d62bce | toolchain: clang-cl 19.1.5 (VS2022 LLVM) | config: RelWithDebInfo, windows-ninja-clang-dev
  - `_msvc`:  sha256: 7f43fc859808cea004ff99cf7c9181968c93fd87b940b5dc9120c22935d62bce | toolchain: MSVC 14.42.34433 (VS 17 2022) | config: Debug, windows-vs-dev
  - Note: both toolchains currently produce identical bytes for this stimulus
    (exactly representable input); the per-toolchain split is kept defensively.

- 2026-07-12 `multichannel_5_1_pan_bed.f32mcpeaks` — initial recording
  (M5 §11.2 multichannel render, recorded regression; 5.1 mix, 5.1 device —
  the master does not fold, six channels captured). A mono point source
  sweeps the front arc (FL->FC->FR), a six-channel 5.1 bed fills every plane
  including the surrounds and LFE, and a mono voice runs a non-commuting
  feedback-delay + three-band-tone chain — multi-part content, an effect, and
  a pan sweep across the full mix width. Why: freezes the widened N-channel
  mix + forward-arc pan end to end, complementing the analytic per-channel pan
  goldens (`multichannel_pan_golden_test`). Evidence: recorded by the
  commander with HARKEN_WRITE_BASELINES=1 on the Ninja+clang Debug channel;
  sha256 65caa48600744cbef13ae631d2da58c93aafab9df19eefbca263ede96f0eed57;
  verified green on both toolchains; mutation spot-check: a pan-law angle
  mutation turned this scene red (the 7.1 fold scene went red with it — that
  scene pans too; the per-bus scene stayed green).

- 2026-07-12 `multichannel_7_1_to_stereo_fold.f32mcpeaks` — initial
  recording (M5 §11.2/§11.3 master->device output downmix, recorded
  regression; 7.1 mix, stereo device — the master folds, two channels
  captured). A front-arc pan sweep and a full eight-channel 7.1 bed (side and
  back surrounds included) fold to stereo through the master's default matrix
  (ITU-R BS.775 fronts + the pinned engineering extension for the 7.1
  surrounds). Why: freezes the master output fold on a live multi-voice scene,
  complementing the analytic `downmix_golden_test` closed forms. Evidence:
  recorded by the commander with HARKEN_WRITE_BASELINES=1 on the Ninja+clang
  Debug channel; sha256
  309dd4b191bf5583a35c1e08291480fd5cd3ceff120e6d6fdd2228f1a848e3d0; verified
  green on both toolchains; mutation spot-check: the -3 dB default-fold
  coefficient mutated to 0.5 turned exactly this scene red (the other two
  stayed green — the pan-bed does not fold, and the per-bus scene's mono
  front-pair content rides the unit ITU front coefficients).

- 2026-07-12 `multichannel_5_1_perbus_custom_fold.f32mcpeaks` — initial
  recording (M5 §11.3 per-bus non-master fold, recorded regression; 5.1 mix,
  5.1 device — the master does not fold, six channels captured). A sub-bus
  with a STEREO output layout and a custom asymmetric downmix matrix
  (Lo = 1.5*FL, Ro = 0.5*FR) folds a mono 2D voice and name-places it into the
  captured master fronts; a direct master 5.1 bed fills FC / LFE / the
  surrounds, proving the per-bus fold is isolated to the fronts and the master
  itself does not fold. At the midpoint the matrix is reset to the ITU default,
  so the envelope pins both the custom matrix and the reset path. Why: freezes
  the per-bus configurable-matrix fold boundary (`Vn_SetBusOutputLayout` +
  `Vn_SetBusDownmixMatrix` + `Vn_ResetBusDownmixMatrix`), complementing the
  isolated analytic `bus_downmix_test`. Evidence: recorded by the commander
  with HARKEN_WRITE_BASELINES=1 on the Ninja+clang Debug channel; sha256
  d57745a59271811abcf95be3ac18650aae2fb7a6734ed4021a9c6b4a1a1cf3e1; verified
  green on both toolchains; mutation spot-check: a per-bus fold gain halving
  turned exactly this scene red (the other two stayed green).

- 2026-07-11 all baselines — directory moved from
  `HarkenTest/Runtime/golden/baselines/` to `HarkenTest/Runtime/E2E/baselines/`
  (test-tree recategorization: the golden directory dissolved — analytic
  cases joined `Runtime/UnitTest/`, the recorded-baseline regression and its
  data moved to `Runtime/E2E/`; shared helpers promoted to the top-level
  `TestSupport/` library). `git mv`, byte content of every `.f32peaks`
  unchanged (git rename detection reports 100% on every file). The
  `HARKEN_BASELINE_DIR` compile definition now points here. Evidence: full
  `harken_tests` suite 425/425 green on the VS channel (Debug) after the move;
  the Ninja+Clang channel could not be configured fresh due to a pre-existing
  front-end third-party defect (qmsetup manifest collision, recorded in the
  milestone-4 plan's open defects) — unrelated to this move.
- 2026-07-10 all baselines — directory moved from `tests/golden/baselines/`
  to `HarkenTest/Runtime/golden/baselines/` (front-end migration layout;
  `git mv`, byte content of every `.f32peaks` unchanged). The
  `HARKEN_BASELINE_DIR` compile definition now points here. Evidence: full
  `harken_tests` suite green on both toolchains after the move, and per-file
  sha256 identical before/after.
- 2026-07-10 `bus_graph_scene.f32peaks` — initial recording — M3 §9
  acceptance, live listen PASS 2026-07-10 (round 2; round 1 exposed the
  behave-plays-compiled-2D defect, fixed in the same milestone with
  zero drift in this baseline). A voice into submix
  bus A, an additive A->B send at −6 dB, a feedback delay on B running
  post-fader, B's fader split and BusVolume, all folded in published
  plan order (A and B before the master). Recorded on the Clang channel
  (`HARKEN_WRITE_BASELINES=1`), verified green on both toolchains (full
  suite); mutation evidence: dropping the voice/send scatter, or
  finalizing bus B before bus A (ignoring the published plan order),
  turns exactly this comparison red.
- 2026-07-10 `park_return_scene.f32peaks` — initial recording — M3 §9
  acceptance, live listen PASS 2026-07-10 (round 2; round 1 exposed the
  behave-plays-compiled-2D defect, fixed in the same milestone with
  zero drift in this baseline). An infinite-loop
  voice driven below the −40 dB park threshold parks (a silent middle
  section), then is raised audible and returns to resume rendering at
  its virtual offset. Same recording/verification as above; mutation
  evidence: suppressing the return (leaving the voice parked) or
  resuming it at frame 0 instead of the virtual offset turns exactly
  this comparison red.
- 2026-07-10 `priority_steal_scene.f32peaks` — initial recording — M3 §9
  acceptance, live listen PASS 2026-07-10 (round 2; round 1 exposed the
  behave-plays-compiled-2D defect, fixed in the same milestone with
  zero drift in this baseline). A full two-voice
  pool under steal-quietest admits a higher-priority voice by stealing
  the quietest at the mid-run block, so the mix changes timbre there.
  Same recording/verification as above; mutation evidence: flipping the
  steal victim selection from quietest to loudest (or refusing the
  steal so the incoming voice never sounds) turns exactly this
  comparison red.
- 2026-07-09 `streamed_subregion_loop.f32peaks` — initial recording — M2
  §7 acceptance, live listen PASS 2026-07-09. A streamed `.sounddata`
  voice looping a sub-region five passes on the null backend; the source
  carries a file-position-dependent amplitude contour so the envelope
  encodes which region is playing (95 distinct block peaks of 282).
  Recorded on the Clang channel (`HARKEN_WRITE_BASELINES=1`), verified
  green on both toolchains (full suite); mutation evidence: re-seeking
  the streaming-thread loop wrap to frame 0 instead of the loop start
  turns exactly this comparison red.
- 2026-07-09 `noncommuting_effect_chain.f32peaks` — initial recording —
  M2 §7 acceptance, live listen PASS 2026-07-09. One voice with the
  feedback delay in slot 0 and the three-band tone in slot 1 (the
  audible-order pair — the delay's soft-saturated feedback loop makes
  the chain order non-commuting). Same recording/verification as above;
  mutation evidence: halving the delay feedback contribution turns this
  comparison red.
- 2026-07-09 `combined_m2_scene.f32peaks` — initial recording — M2 §7
  acceptance, live listen PASS 2026-07-09. Deterministic null-backend
  miniature of the milestone acceptance scene: a streamed music voice,
  an in-memory infinite-loop ambience voice, and a delayed one-shot
  through the feedback delay, ending in a fade-out. Same recording/
  verification as above; mutation evidence: disabling the in-memory
  loop wrap turns exactly this comparison red (and the delay-feedback
  mutation above also reds it through the one-shot's insert effect).
- 2026-07-08 `flyby_pan_attenuation.f32peaks` — initial recording — M1
  §17 acceptance, live listen PASS 2026-07-08. Recorded on the Clang
  channel (`HARKEN_WRITE_BASELINES=1`), verified green on both toolchains
  (Clang and MSVC, full suite); mutation evidence: halving the azimuth
  pan projection turns this comparison red.
- 2026-07-08 `bus_ramp_fade_stop.f32peaks` — initial recording — M1 §17
  acceptance, live listen PASS 2026-07-08. Same recording/verification
  as above; mutation evidence: scaling the bus ramp end gain by 0.9
  turns this comparison red.
- 2026-07-08 `variable_lowpass_sweep.f32peaks` — initial recording — M1
  §17 acceptance, live listen PASS 2026-07-08. Same recording/
  verification as above; mutation evidence: halving the lowpass cutoff
  handed to the coefficient builder turns this comparison red.
