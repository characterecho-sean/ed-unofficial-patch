# NVIDIA Reflex validation and latency measurements

## Status

This change contains native VR Reflex pacing, latency markers, mode controls
and their regression tests. The narrowed source passed full native Windows
validation. The optional input-response collector is outside this PR.

The historical benchmark results below retain their original source-build
provenance and software endpoints. In a six-pass GPU-saturated stationary VR
comparison, median sender arm publication to observed UI GPU completion was
104.64 ms Off, 86.39 ms On and 85.60 ms On + Boost. Each enabled pass improved
median and p95 against both bracketing Off passes. A lighter workload and an
earlier head-pose comparison showed no useful, repeatable improvement. Boost
did not establish an additional benefit over On.

These are software VR measurements, not physical headset latency. There is no
new runtime benchmark or physical-headset flight for the narrowed change. A
successful mode readback and ordinary frame timings are not input-latency
measurements. Engine hooks support game build 334925 using its exact PE
identity and entry, callsite and scheduler-protocol pins. Other builds leave
Reflex unavailable; this does not extend the existing address resolver.

## Reflex frame timing

The native VR session calls NVIDIA's Sleep once at the guarded application
iteration boundary, before input. Off still makes that call. The normal
update's return ends the simulation region. The retained render-job ticket
identifies the HMD pose returned to that job and its submitted eye textures.
The first tracked immediate-context command entry after pose receipt begins the
CPU render-submission region. ED-UP may subsequently suppress that draw; this
timestamp identifies CPU submission work, not the first forwarded driver
command or GPU execution. Its end waits for both the render job and the
successfully drawn eye mirror. Simulation, rendering and Present can overlap.

The optional source capability is shared by the graphics and native runtime
modules. It checks device, session, job, pose sequence and actual acceptance of
both eyes. The finished texture carries that source into the desktop mirror.
Mixed, reused, backward or unqualified sources emit no Present markers. Present
markers surround the first real DXGI call on its calling thread. Failed or
occluded Presents cannot certify completion. A successful same-source retry can
publish CPU completion without repeating those markers; its driver timing is
not paired to the retry. The game's original mirror route has no qualified
source yet.

PCL statistics remain active in every mode and on other GPU vendors. Removed
PCL ping messages wait for the next actual InputManager boundary. Normal game
shutdown stops the SDK worker outside loader teardown. Passive message
observation remains available when `advanced.input_gate = 0`; it does not
enable keyboard suppression. The pinned NVIDIA SDK license ships with binaries
using its statistics header. Removed game-window left-button presses, including
double-clicks, also queue the SDK flash marker for the next actual InputManager
boundary, independently of a flash-indicator setting.

With `advanced.app_gpu_timing = on`, the graphics log's `native_reflex_frame`
rows use QPC for the interval from the game's HMD pose receipt to desktop
Present return. This is a CPU interval. `native_reflex_latency` rows retain
NVIDIA's separate microsecond clock for simulation-to-GPU completion. Neither
is physical head-motion-to-photon latency. Detailed frame rows and driver
timing reads are off otherwise; baseline SDK markers, availability and
aggregate health remain active. The integration follows the [native Reflex SDK
guide](https://github.com/NVIDIA-RTX/REFLEX/blob/main/NVIDIA%20Reflex%20SDK%20Integration%20Guide.pdf)
and [NVAPI interfaces](https://docs.nvidia.com/nvapi/group__dx.html).

## Regression tests

The normal native Windows validation build runs the repository's Reflex policy,
engine-hook, PCL, frame-session, frame-provider, input-message and
mirror-source tests. It also validates settings and SDK notice distribution.
These fixtures use repository-owned fakes and system D3D11/WARP devices where
needed. They require no game, headset or external simulator; window and input
fixtures need an interactive Windows desktop. Prerequisites and the build
command are documented in [Building](building.md).

The runtime keeps source-qualified frame markers and optional detailed timing
under `advanced.app_gpu_timing`. The historical input-response measurements
below are retained as benchmark evidence; their capture instrumentation is not
shipped by this PR.

## Stationary real-UI comparison

On 2026-11-25, the validated working tree based on `f09604f8` included the v3
keyboard instrumentation, the stable-Present reporting fix and the preparation
eye-pair fix used for these measurements. The tested `d3d11.dll` SHA-256 was
`0e588b279957b6dec9d85add743d84ebec5ce2a7fac177084f633b14bdef62e5`. The game
was build 334925, stationary in Pilot Assessment at Mawson Dock, awaiting Auto
Launch. The hardware was an Intel Core i9-14900KS, RTX 3080 Ti, driver 560.94
and Windows 11 build 26200. A stationary software VR session used SteamVR
2.18.4/OpenXR, with a fixed reference pose across passes.

For this comparison, Up and Down inputs alternated to change an existing game
UI selection. Successful buffered keyboard reads inside InputManager supplied
source 5 receipts. The receiver required opposite-state preparation followed by
changed pixels in both accepted eyes. It painted no response. Each workload
used six passes in Off, On, On + Boost, On + Boost, On, Off order, with 30
measured inputs per pass after at least 90 new source frames and a successful
64-Present driver readback. Within each workload the profiles differed only in
Reflex mode. DLL, sender, protocol, INI and graphics-file hashes were checked;
preparation inputs and correctness preflights were excluded from measurements.

These historical intervals start at the sender's shared arm-publication
`sentQpc`, before publishing Armed and calling `SendInput`. They include that
publication work. The historical values below have not been retimed.

| Workload | Mode | Samples | Arm publication to observed UI GPU completion median / p95 (ms) | Arm publication to paired submit median / p95 (ms) |
| --- | --- | ---: | ---: | ---: |
| Lighter: DLSS K, 1484 by 835 input, 2560 by 1440 output per eye | Off | 60 | 87.16 / 92.43 | 83.51 / 89.08 |
| Same | On | 60 | 87.43 / 92.83 | 83.76 / 89.18 |
| Same | On + Boost | 60 | 86.85 / 92.34 | 82.99 / 88.88 |
| GPU-saturated: DLAA K, 4096 by 2304 input and output per eye | Off | 60 | 104.64 / 108.96 | 59.53 / 65.20 |
| Same | On | 60 | 86.39 / 92.68 | 60.51 / 66.48 |
| Same | On + Boost | 60 | 85.60 / 94.84 | 61.23 / 67.08 |

Both workloads completed all 180 measured inputs with no rejected, missing or
error samples and no discrepancy between raw QPC endpoints and sender metrics.
Each had 180 logged UI receipts and 177 logged native-source joins; the final
three source rows were not flushed in the per-pass captures. The raw IPC
endpoints for those inputs were complete and did not depend on that log join.
GPU utilization medians were 39 to 42 percent in the lighter workload and 98
percent in every GPU-saturated pass. Sampled graphics clocks did not show a
consistent Boost increase.

In the GPU-saturated workload, the two Off medians were 104.99 and 104.33 ms;
On was 85.03 and 86.84 ms; On + Boost was 85.44 and 85.60 ms. Every enabled
pass had a lower median and p95 than both Off passes. Pooled medians improved
17.4 percent On and 18.2 percent On + Boost. This is a repeatable descriptive
reduction for this workload. It does not demonstrate an extra Boost benefit, an
improvement in input API receipt or CPU submit time, or a benefit under the
lighter workload.

Matching UI source IDs to NVIDIA's driver records covered 175 of the 180
GPU-saturated inputs: 58 Off, 57 On and 60 On + Boost. The other five lacked
the final driver rows in the captured log. Median render-submission start to
GPU start was 26.58 ms Off, 4.65 ms On and 4.32 ms On + Boost. GPU execution
medians were 31.86, 32.56 and 33.00 ms respectively. This independent clock
supports reduced time waiting for GPU work to begin rather than faster shader
execution. The logged interval also includes submission work; it is not an
isolated measurement of the operating-system render queue.

The GPU endpoint is the CPU observer's fence-completion notification after the
actual changed UI pixels in both eyes. It includes notification and
thread-scheduling delay. It is not a calibrated GPU timestamp, photon
measurement or head-motion response. The two workloads also use different
resolution and antialiasing settings; compare modes within a workload rather
than attributing differences between workloads to Reflex. The scene remained
docked throughout. Temporary test graphics and INI changes were restored.

The source used for this historical comparison passed all 207 native Windows
validation jobs before collection. Optional FSR SDKs and archived executables
for builds 332841 and 334473 were unavailable. The tested build's 338 address
resolutions passed.

## Native head-pose comparison

On 2026-11-25, code commit `f09604f8a63ec05c0bb74c51fbb65f41938c534d` ran Pilot
Assessment at Mawson Dock, awaiting Auto Launch. The hardware and rendering
setup matched the earlier comparison below. The software headset pose used
identical, bounded head yaw between minus and plus four degrees every 500 ms,
restoring the saved pose after each pass. No painted response was used.

Each pass measured 30 seconds after at least 90 new qualified source frames and
a successful final 64-Present driver readback. The order was Off, On, On +
Boost, On + Boost, On, Off. The profiles differed only in Reflex mode.

| Mode | Frames across two passes | Pose receipt to desktop Present median / p95 (ms) | NVIDIA simulation start to GPU end median / p95 (ms) |
| --- | ---: | ---: | ---: |
| Off | 1,920 | 12.01 / 14.49 | 35.04 / 37.41 |
| On | 1,918 | 12.40 / 14.77 | 34.90 / 37.24 |
| On + Boost | 1,949 | 12.48 / 14.90 | 35.05 / 37.49 |

The two GPU medians were 35.20 and 34.93 ms for Off, 34.88 and 34.92 ms for On,
and 35.18 and 34.93 ms for On + Boost. The first Off pose-to-Present median was
11.37 ms; the final Off median was 12.51 ms. This drift and the small
between-mode differences do not establish a repeatable benefit or a systematic
head-pose latency regression. The endpoints exclude physical display latency.
The tutorial also raises its launch platform automatically while awaiting Auto
Launch; this collection did not timestamp that transition. Startup scene drift
is therefore an additional limitation of this comparison.

Accepted NVIDIA Sleep calls averaged about six microseconds. OpenXR owner wait
medians were about 13 to 15 ms, while GPU utilization was about 38 to 40
percent. These observations support a workload paced mainly by the runtime and
CPU. They do not establish a fixed frame-rate cap. The advertised display rate
was 90 Hz; application cadence was about 32 frames per second. Sampled graphics
clocks did not demonstrate a higher clock with Boost.

A separate read-only inspection of the existing completed engine history joined
256 records to their exact rendered-source tokens. InputManager had returned
before pose receipt in all 256 records, with a median gap of 14.07 ms. This
establishes an input-phase-to-pose gap, not the precise entry and return of
each OpenXR wait or a keyboard-to-pixel response. The actual keyboard-to-UI
comparison is reported above; a shorter marker interval alone would not
demonstrate a real response improvement.

The full native Windows build passed all 207 jobs before the exact source was
committed and promoted through its matching full-build receipt. The engine
relay rig passed 55 checks. Optional FSR SDKs and the archived executables for
builds 332841 and 334473 were unavailable; their optional checks were skipped.
The tested build's 338 address resolutions passed.

## Earlier synthetic software comparison

On 2026-11-24, code commit `68ef8538a21e0ee86bc6e439635852dd685193a4` ran Pilot
Assessment at Mawson Dock. The ship stayed on the launch pad, waiting for Auto
Launch. The system used an Intel Core i9-14900KS, RTX 3080 Ti, NVIDIA driver
560.94 and Windows 11 build 26200. The software VR session used SteamVR
2.18.4/OpenXR at an advertised 90 Hz. DLSS preset K rendered 1484 by 835 input
to 2560 by 1440 output per eye. All three profiles differed only in the
low-latency setting.

Each mode received one sequential run of 60 pulses, in Off, On, On + Boost
order. Every sample used source 3: a removed game-window mouse message. The GPU
endpoint followed the diagnostic response in both full-frame treated eye
textures. Both eye submissions were accepted. Driver requests succeeded and
final 64-Present readbacks matched the requested enabled/disabled state; the
API does not expose a separate Boost readback bit.

| Mode | Samples | Input receipt median / p95 (ms) | Diagnostic GPU response median / p95 (ms) | Paired submit median / p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Off | 60 | 0.32 / 0.49 | 23.30 / 34.35 | 19.76 / 30.10 |
| On | 60 | 0.33 / 0.56 | 24.16 / 35.52 | 20.32 / 31.72 |
| On + Boost | 60 | 0.33 / 0.47 | 22.05 / 34.01 | 17.89 / 30.32 |

All values start at the sender's shared arm-publication timestamp, before
calling `SendInput`. Percentiles use nearest rank; the median uses the middle
pair. Raw QPC endpoints independently reproduce the reported values. The
initial three-pulse check and incomplete setup attempts are excluded. Captures
before and after each mode show the same docked view.

On did not lower the median or p95 in this run. Boost's median was 1.25 ms
below Off, while its p95 was only 0.34 ms below Off. One sequential comparison
with these mixed results does not demonstrate a consistent benefit or qualify
actual gameplay input lag. Optical/HMD latency and Elite's simulation response
remain unmeasured. That comparison build used the driver mode API without
per-frame Reflex Sleep or NVIDIA latency markers.

This run used a 1000 ms interval, longer than the observed 500 ms Windows
double-click threshold, with 80 ms button holds. The historical capture
accepted ordinary left-button messages. A second down classified as
`WM_LBUTTONDBLCLK` did not qualify; a missing receipt was rejected rather than
reported as zero latency.

The full native Windows build passed, including production shaders/DLLs, 110
IPC/state checks, 20 sender self-tests, the real atomic-helper ABI check, the
input observer tests and the 4356-check native-frame fixture. Optional
FSR3/FSR4 engine tests had no SDK; archived-executable checks for builds 332841
and 334473 had no matching executable; the turret-HUD debug-layer linkage
control was unavailable and reflection checks ran instead. The validated tree
was committed and promoted through the matching full-build receipt.
