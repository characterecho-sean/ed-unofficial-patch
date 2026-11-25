# Input latency diagnostic

## Status

The diagnostic is implemented behind `advanced.input_latency_probe` and is off
by default. Native Windows validation passed for code commit `68ef8538`. The
stationary comparison completed all 180 pulses with no rejected or missing
endpoints. It does not establish a consistent latency improvement across the
enabled modes. The temporary setting remains while this latency-validation
investigation is open. A successful driver-mode readback and ordinary frame
timings are not input latency measurements.

## What is measured

A test sender publishes a unique pulse in a process-specific shared-memory
mapping and timestamps a mouse-button injection with QueryPerformanceCounter.
The existing input owner observes the first returned DirectInput press or
left-button message removed by the game's own `PeekMessageA` call. The message
must belong to a window in the game process and use `PM_REMOVE`. An observed
release is required before a press; foreign devices/windows, failed reads,
custom layouts, peek-only reads and overflow reads do not qualify. This is an
input API receipt; it does not prove that Elite's simulation acted on the
input. A source field identifies the exact API route for each sample.

After all native eye treatments, the receiver paints a 16 by 16 white
diagnostic response in each actual submitted eye texture. Both responses and
both accepted submissions must have the same native sequence. A D3D11 fence
after the response commands wakes a CPU observer without stalling the producer
context. Its QPC stamp is an observed GPU-completion upper bound; it includes
notification and thread-scheduling delay. It is not a calibrated timestamp of
GPU execution.

The sender reports injection to input receipt, injection to observed response
completion, input receipt to observed response completion, and injection to the
accepted stereo-submit CPU endpoint. GPU completion and submit return can occur
in either order. Neither endpoint measures the monitor or HMD's photons.

The response belongs to the diagnostic. This does not establish how quickly
Elite's gameplay simulation, weapons or physics respond to that input. The
probe also does not enable per-frame Reflex sleep or change the selected driver
policy; identical instrumentation is used for Off, On and On + Boost.

## Collection and rejection

Enable `advanced.app_gpu_timing = on` and `advanced.input_latency_probe = on`
in the VR profile. Keep the game foreground, use a stationary training scene
and the same view and graphics settings for all three modes. The portable
sender is `tools/input_latency_probe.py`; its help lists the required process
identity, session, window and output arguments. Run it in the game's
interactive Windows session. The sender never takes focus from another
application. Output is a fresh JSONL file containing provenance, individual
pulses and the summary.

A pulse times out after two seconds. Mode or configuration changes, device
replacement, unknown texture formats, wrong producer threads, mixed sequences,
duplicate eyes, failed GPU work and rejected submissions invalidate the pulse.
A missing or rejected endpoint is never represented as zero milliseconds.
Disabling the setting or closing the native session releases the mapping,
observer thread and graphics resources. With the diagnostic off, the input and
render paths only check an atomic gate.

The IPC layout and ownership rules live in
`src/common/input_latency_protocol.h`; the receiver and sender publish terminal
results through Windows Interlocked operations. Mapping names include both PID
and process creation time to reject stale senders after PID reuse.

## Stationary software comparison

On 2026-11-24, code commit `68ef8538a21e0ee86bc6e439635852dd685193a4` ran Pilot
Assessment at Mawson Dock. The ship stayed on the launch pad, waiting for Auto
Launch. The system used an Intel Core i9-14900KS, RTX 3080 Ti, NVIDIA driver
560.94 and Windows 11 build 26200. A Mod Linker virtual HMD used SteamVR
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

All values start at the sender's injection timestamp. Percentiles use nearest
rank; the median uses the middle pair. Raw QPC endpoints independently
reproduce the reported values. The initial three-pulse check and incomplete
setup attempts are excluded. Captures before and after each mode show the same
docked view.

On did not lower the median or p95 in this run. Boost's median was 1.25 ms
below Off, while its p95 was only 0.34 ms below Off. One sequential comparison
with these mixed results does not demonstrate a consistent benefit or qualify
actual gameplay input lag. Optical/HMD latency and Elite's simulation response
remain unmeasured. The branch still uses the driver mode API without per-frame
Reflex sleep or NVIDIA latency markers.

Use an interval longer than Windows' double-click threshold for source 3. This
run used 1000 ms with an observed 500 ms threshold and 80 ms button holds. A
second down classified as `WM_LBUTTONDBLCLK` does not qualify as the ordinary
left-button message this probe observes; missing receipt is rejected, never
reported as zero latency.

The full native Windows build passed, including production shaders/DLLs, 110
IPC/state checks, 20 sender self-tests, the real atomic-helper ABI check, the
input observer tests and the 4356-check native-frame fixture. Optional
FSR3/FSR4 engine tests had no SDK; archived-executable checks for builds 332841
and 334473 had no matching executable; the turret-HUD debug-layer linkage
control was unavailable and reflection checks ran instead. The validated tree
was committed and promoted through the matching full-build receipt.
