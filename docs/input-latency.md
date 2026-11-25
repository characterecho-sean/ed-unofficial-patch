# Input latency diagnostic

## Status

The diagnostic is implemented behind `advanced.input_latency_probe` and is off
by default. Native validation and the stationary-training comparison are pending.
The temporary setting remains while this latency-validation investigation is
open. A successful driver-mode readback and ordinary frame timings are not input
latency measurements.

## What is measured

A test sender publishes a unique pulse in a process-specific shared-memory
mapping and timestamps a mouse-button injection with QueryPerformanceCounter.
The existing DirectInput owner observes the first successful game-owned read
that returns the pressed button. An observed release is required before a press;
foreign devices, failed reads, custom layouts and peek/overflow reads do not
qualify.

After all native eye treatments, the receiver paints a 16 by 16 white diagnostic
response in each actual submitted eye texture. Both responses and both accepted
submissions must have the same native sequence. A D3D11 fence after the response
commands wakes a CPU observer without stalling the producer context. Its QPC
stamp is an observed GPU-completion upper bound; it includes notification and
thread-scheduling delay. It is not a calibrated timestamp of GPU execution.

The sender reports injection to input receipt, injection to observed response
completion, input receipt to observed response completion, and injection to the
accepted stereo-submit CPU endpoint. GPU completion and submit return can occur
in either order. Neither endpoint measures the monitor or HMD's photons.

The response belongs to the diagnostic. This does not establish how quickly
Elite's gameplay simulation, weapons or physics respond to that input. The
probe also does not enable per-frame Reflex sleep or change the selected driver
policy; identical instrumentation is used for Off, On and On + Boost.

## Collection and rejection

Enable `advanced.app_gpu_timing = on` and
`advanced.input_latency_probe = on` in the VR profile. Keep the game foreground,
use a stationary training scene and the same view and graphics settings for all
three modes. The portable sender is `tools/input_latency_probe.py`; its help
lists the required process identity, session, window and output arguments.
Run it in the game's interactive Windows session. The sender never takes focus
from another application. Output is a fresh JSONL file containing provenance,
individual pulses and the summary.

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
