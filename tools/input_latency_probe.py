#!/usr/bin/env python3
"""Collect stationary-game input-latency diagnostic pulses (Windows only).

The instrumented game must already be foreground in the sender's interactive
Windows session. Each pulse moves the cursor to a caller-selected client point
(default x=0.98, y=0.97), sends only left-button down/up, and waits for the game's
actual input reader and paired eye response. Choose a benign point while the
game is stationary. This tool never activates a window or sends gameplay keys.

Metrics distinguish injection-to-game-consumption, consumption-to-GPU-fence
notification, injection-to-GPU-fence notification, and injection-to-paired-submit
CPU endpoint. GPU notification is an observed completion upper bound, not a GPU
timestamp, display/photons measurement, or gameplay/physics response. Rejected,
incomplete, or invalid samples never receive synthetic zero latency values.

--dry-run validates arguments and the fresh output destination without opening
Windows resources, moving the cursor, injecting input, or writing any files.
--self-test uses only standard-library fake backends, including on Windows.
--self-test-atomic-helper verifies the real Win64 atomic helper on private
memory without opening the game, moving the cursor, or injecting input.
Live collection loads build/tools/input_latency_atomic.dll beside this source
tree by default; --atomic-helper selects another built helper DLL.
The IPC word layout is owned by src/common/input_latency_protocol.h.
"""

import argparse
from contextlib import redirect_stdout
import ctypes
from dataclasses import dataclass
from datetime import datetime, timezone
import io
import json
import math
import ntpath
import os
from pathlib import Path
import statistics
import sys
import tempfile
import time
import unittest
from unittest import mock


MAGIC = 0x45445550494C5031
VERSION = 1
WORD_COUNT = 32
SHARED_SIZE = WORD_COUNT * 8
UINT64_MAX = (1 << 64) - 1
IDLE, RESERVED, ARMED, CONSUMED, RENDERING, COMPLETE, REJECTED, STOPPED = range(8)
MODES = {"off": 0, "on": 1, "boost": 2}
REASONS = ("None", "Timeout", "InvalidRequest", "GenerationChanged", "WrongThread",
           "TextureUnsupported", "MixedFrame", "DuplicateEye", "GraphicsFailure",
           "SubmitFailed", "DeviceRemoved", "Stopped")
FIELDS = ("magic", "version", "size", "pid", "creationTime", "frequency", "state",
          "ready", "generation", "mode", "request", "sentQpc", "consumedQpc",
          "renderSequence", "eyeMask", "submitMask", "gpuCompleteQpc",
          "submitCompleteQpc", "reason", "source")
INDEX = {name: index for index, name in enumerate(FIELDS)}
TIMEOUT_SECONDS = 3.0
POLL_SECONDS = 0.01
DEFAULT_ATOMIC_HELPER = Path(__file__).resolve().parent.parent / "build" / "tools" / "input_latency_atomic.dll"
METRICS = {
    "injection_to_consumption_ms": "sentQpc to actual game input consumption",
    "consumption_to_gpu_notification_ms": "game consumption to GPU-fence CPU notification (upper bound)",
    "injection_to_gpu_notification_ms": "sentQpc to GPU-fence CPU notification (upper bound)",
    "injection_to_paired_submit_ms": "sentQpc to successful paired native-submit CPU endpoint",
}


class ProbeError(RuntimeError):
    """A refusal or ambiguous observation that must stop further input."""


class InputRefused(ProbeError):
    """SendInput explicitly inserted zero events; this call owns no button."""


@dataclass(frozen=True)
class Config:
    pid: int
    creation_time: int
    session: int
    image: str
    hwnd: int
    mode: str
    output: Path
    count: int = 60
    interval_ms: float = 400.0
    hold_ms: float = 80.0
    x: float = 0.98
    y: float = 0.97
    atomic_helper: Path = None

    @property
    def mapping_name(self):
        return "Local\\EDUP.InputLatency.%d.%d" % (self.pid, self.creation_time)

    def validate(self):
        """Check portable syntax and destination freshness without writing."""
        if not 0 < self.pid <= 0xFFFFFFFF:
            raise ValueError("--pid must be a positive DWORD")
        if not 0 < self.creation_time <= UINT64_MAX:
            raise ValueError("--creation-time must be a positive 64-bit FILETIME integer")
        if not 0 < self.session <= 0xFFFFFFFF:
            raise ValueError("--session must identify an interactive session (not session 0)")
        drive, tail = ntpath.splitdrive(self.image)
        if not drive or not tail.startswith(("\\", "/")) or not self.image.lower().endswith(".exe"):
            raise ValueError("--image must be an absolute Windows executable path")
        if not 0 < self.hwnd <= (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1:
            raise ValueError("--hwnd must be a positive pointer-sized window handle")
        if self.mode not in MODES:
            raise ValueError("--mode must be off, on, or boost")
        if not 1 <= self.count <= 100000:
            raise ValueError("--count must be between 1 and 100000")
        if not all(math.isfinite(v) for v in (self.interval_ms, self.hold_ms, self.x, self.y)):
            raise ValueError("timings and coordinates must be finite")
        if not 0 < self.hold_ms <= 1000 or not self.hold_ms <= self.interval_ms <= 60000:
            raise ValueError("require 0 < --hold-ms <= 1000 and hold-ms <= interval-ms <= 60000")
        if not 0 <= self.x <= 1 or not 0 <= self.y <= 1:
            raise ValueError("--x and --y must be normalized client coordinates in [0, 1]")
        if self.output.exists() or self.output.is_symlink():
            raise ValueError("--output already exists; select a fresh JSONL path")
        if not self.output.parent.is_dir():
            raise ValueError("--output parent directory must already exist")


def normalized_image(image):
    return ntpath.normcase(ntpath.normpath(image))


def atomic_helper_path(path=None):
    return (DEFAULT_ATOMIC_HELPER if path is None else Path(path)).resolve()


def load_atomic_helper(path=None):
    """Bind owned intrinsic wrappers; never assume system DLL atomic exports."""
    if sys.platform != "win32" or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise ProbeError("the atomic helper requires 64-bit Windows Python")
    helper_path = atomic_helper_path(path)
    if not helper_path.is_file():
        raise ProbeError("atomic helper not found: %s; build tools/input_latency_atomic.cpp first" % helper_path)
    try:
        # Load the absolute helper and restrict dependency lookup to its folder
        # and trusted default directories, excluding the current working folder.
        library = ctypes.WinDLL(str(helper_path), use_last_error=True, winmode=0x1100)
        compare = library.edupInputLatencyCompareExchange64
        exchange = library.edupInputLatencyExchange64
        pointer = ctypes.POINTER(ctypes.c_int64)
        compare.argtypes = [pointer, ctypes.c_int64, ctypes.c_int64]
        exchange.argtypes = [pointer, ctypes.c_int64]
        compare.restype = exchange.restype = ctypes.c_uint64
    except (OSError, AttributeError) as error:
        raise ProbeError("cannot load atomic helper %s: %s" % (helper_path, error)) from error
    return library


def atomic_helper_self_test(path=None):
    """Exercise real exports on private aligned memory, including pointer guards."""
    try:
        helper = load_atomic_helper(path)
        word = ctypes.c_int64(7)
        if ctypes.addressof(word) % 8:
            raise ProbeError("ctypes test word is not 64-bit aligned")
        compare, exchange = helper.edupInputLatencyCompareExchange64, helper.edupInputLatencyExchange64
        pointer = ctypes.byref(word)
        checks = [compare(pointer, 11, 7) == 7 and word.value == 11,
                  compare(pointer, 13, 7) == 11 and word.value == 11,
                  exchange(pointer, -1) == 11 and word.value == -1,
                  compare(pointer, 17, -1) == UINT64_MAX and word.value == 17]
        if not all(checks):
            raise ProbeError("atomic helper CAS/Exchange semantics failed")
        scratch = (ctypes.c_ubyte * 24)()
        base = ctypes.addressof(scratch)
        misaligned = ctypes.cast(base + (1 if base % 8 == 0 else 0), ctypes.POINTER(ctypes.c_int64))
        for address in (None, misaligned):
            for operation, arguments in ((compare, (address, 99, 17)), (exchange, (address, 99))):
                ctypes.set_last_error(0)
                if operation(*arguments) != UINT64_MAX or ctypes.get_last_error() != 87:
                    raise ProbeError("atomic helper invalid-pointer guard failed")
        if any(scratch):
            raise ProbeError("atomic helper modified misaligned scratch memory")
    except (ProbeError, OSError) as error:
        print("input_latency_probe: atomic helper self-test failed: " + str(error), file=sys.stderr)
        return 1
    print("input_latency_probe: atomic helper self-test passed: " + str(atomic_helper_path(path)))
    return 0


def validate_target(config, facts):
    """Check observed identity and desktop facts identically in live/fake routes."""
    if not facts["alive"]:
        raise ProbeError("target process exited or cannot be observed")
    if facts["creation_time"] != config.creation_time:
        raise ProbeError("target process creation time changed")
    if normalized_image(facts["image"]) != normalized_image(config.image):
        raise ProbeError("target executable image does not match --image")
    if (not config.session or facts["target_session"] != config.session
            or facts["sender_session"] != config.session):
        raise ProbeError("sender and target must share the expected interactive session")
    if facts["window_pid"] != config.pid:
        raise ProbeError("--hwnd no longer belongs to the target PID")
    if not facts["window_exists"] or not facts["visible"] or facts["minimized"]:
        raise ProbeError("target client is absent, hidden, or minimized")
    if facts["foreground"] != config.hwnd:
        raise ProbeError("expected target client must already be foreground; focus will not be changed")


def decode(words):
    """Retain all protocol words, including reserved fields, in every snapshot."""
    if len(words) != WORD_COUNT:
        raise ProbeError("mapping snapshot is not 32 words")
    return dict(zip(FIELDS, words[:len(FIELDS)]), reserved=words[len(FIELDS):], words=list(words))


class WindowsBackend:
    """Own process/map handles; all desktop checks are read-only until position()."""

    def __init__(self, config):
        if sys.platform != "win32":
            raise ProbeError("live collection requires Windows; --dry-run and --self-test are portable")
        self.config = config
        self.process = self.mapping = self.view = None
        self._bind()
        try:
            self.process = self.kernel.OpenProcess(0x00101000, False, config.pid)
            if not self.process:
                self._error("OpenProcess")
            self.validate_identity()
            self.mapping = self.kernel.OpenFileMappingW(0x0006, False, config.mapping_name)
            if not self.mapping:
                self._error("OpenFileMappingW (enable input-latency diagnostics in the target)")
            self.view = self.kernel.MapViewOfFile(self.mapping, 0x0006, 0, 0, SHARED_SIZE)
            if not self.view:
                self._error("MapViewOfFile")
            if self.view % 8:
                raise ProbeError("shared mapping is not 64-bit aligned")
            value = ctypes.c_int64()
            if not self.kernel.QueryPerformanceFrequency(ctypes.byref(value)) or value.value <= 0:
                self._error("QueryPerformanceFrequency")
            self.frequency = value.value
        except BaseException:
            self.close()
            raise

    def _bind(self):
        """Declare Win32 ABI types on the live route; no pointer uses DWORD.

        INPUT/SendInput: https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-input
        Atomic publication: https://learn.microsoft.com/windows/win32/api/winnt/nf-winnt-interlockedcompareexchange64
        """
        class Point(ctypes.Structure):
            _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32)]

        class Rect(ctypes.Structure):
            _fields_ = [(n, ctypes.c_int32) for n in ("left", "top", "right", "bottom")]

        class FileTime(ctypes.Structure):
            _fields_ = [("low", ctypes.c_uint32), ("high", ctypes.c_uint32)]

        class MouseInput(ctypes.Structure):
            _fields_ = [("dx", ctypes.c_int32), ("dy", ctypes.c_int32),
                        ("mouseData", ctypes.c_uint32), ("dwFlags", ctypes.c_uint32),
                        ("time", ctypes.c_uint32), ("dwExtraInfo", ctypes.c_size_t)]

        class InputUnion(ctypes.Union):
            _fields_ = [("mi", MouseInput)]

        class Input(ctypes.Structure):
            _anonymous_ = ("payload",)
            _fields_ = [("type", ctypes.c_uint32), ("payload", InputUnion)]

        self.Point, self.Rect, self.FileTime, self.Input = Point, Rect, FileTime, Input
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.atomic = load_atomic_helper(self.config.atomic_helper)
        handle, dword, boolean = ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int
        pointer = ctypes.POINTER
        declarations = (
            (self.kernel, "OpenProcess", [dword, boolean, dword], handle),
            (self.kernel, "CloseHandle", [handle], boolean),
            (self.kernel, "WaitForSingleObject", [handle, dword], dword),
            (self.kernel, "GetProcessTimes", [handle] + [pointer(FileTime)] * 4, boolean),
            (self.kernel, "ProcessIdToSessionId", [dword, pointer(dword)], boolean),
            (self.kernel, "QueryFullProcessImageNameW", [handle, dword, ctypes.c_wchar_p, pointer(dword)], boolean),
            (self.kernel, "OpenFileMappingW", [dword, boolean, ctypes.c_wchar_p], handle),
            (self.kernel, "MapViewOfFile", [handle, dword, dword, dword, ctypes.c_size_t], handle),
            (self.kernel, "UnmapViewOfFile", [handle], boolean),
            (self.kernel, "QueryPerformanceCounter", [pointer(ctypes.c_int64)], boolean),
            (self.kernel, "QueryPerformanceFrequency", [pointer(ctypes.c_int64)], boolean),
            (self.user, "GetWindowThreadProcessId", [handle, pointer(dword)], dword),
            (self.user, "GetForegroundWindow", [], handle),
            (self.user, "IsWindow", [handle], boolean),
            (self.user, "IsWindowVisible", [handle], boolean),
            (self.user, "IsIconic", [handle], boolean),
            (self.user, "GetClientRect", [handle, pointer(Rect)], boolean),
            (self.user, "ClientToScreen", [handle, pointer(Point)], boolean),
            (self.user, "WindowFromPoint", [Point], handle),
            (self.user, "SetCursorPos", [ctypes.c_int, ctypes.c_int], boolean),
            (self.user, "GetCursorPos", [pointer(Point)], boolean),
            (self.user, "GetAsyncKeyState", [ctypes.c_int], ctypes.c_int16),
            (self.user, "SendInput", [dword, pointer(Input), ctypes.c_int], dword),
        )
        for library, name, arguments, result in declarations:
            function = getattr(library, name)
            function.argtypes, function.restype = arguments, result

    @staticmethod
    def _error(operation):
        raise ProbeError("%s failed (Win32 error %d)" % (operation, ctypes.get_last_error()))

    def _session(self, pid):
        value = ctypes.c_uint32()
        if not self.kernel.ProcessIdToSessionId(pid, ctypes.byref(value)):
            self._error("ProcessIdToSessionId")
        return value.value

    def validate_identity(self):
        """Refuse recycled/exited targets, other sessions, and foreground changes."""
        config = self.config
        alive = self.kernel.WaitForSingleObject(self.process, 0) == 0x102
        if not alive:
            raise ProbeError("target process exited or cannot be observed")
        times = [self.FileTime() for _ in range(4)]
        if not self.kernel.GetProcessTimes(self.process, *(ctypes.byref(t) for t in times)):
            self._error("GetProcessTimes")
        image, length = ctypes.create_unicode_buffer(32768), ctypes.c_uint32(32768)
        if not self.kernel.QueryFullProcessImageNameW(self.process, 0, image, ctypes.byref(length)):
            self._error("QueryFullProcessImageNameW")
        owner = ctypes.c_uint32()
        self.user.GetWindowThreadProcessId(config.hwnd, ctypes.byref(owner))
        validate_target(config, {
            "alive": alive, "creation_time": (times[0].high << 32) | times[0].low,
            "image": image.value, "target_session": self._session(config.pid),
            "sender_session": self._session(os.getpid()), "window_pid": owner.value,
            "window_exists": bool(self.user.IsWindow(config.hwnd)),
            "visible": bool(self.user.IsWindowVisible(config.hwnd)),
            "minimized": bool(self.user.IsIconic(config.hwnd)),
            "foreground": self.user.GetForegroundWindow(),
        })

    def released(self):
        return not (self.user.GetAsyncKeyState(0x01) & 0x8000)

    def position(self):
        """Move only inside the validated client; refuse overlays or changed cursor."""
        self.validate_identity()
        if not self.released():
            raise ProbeError("left button is already held; no input was injected")
        rect = self.Rect()
        if not self.user.GetClientRect(self.config.hwnd, ctypes.byref(rect)):
            self._error("GetClientRect")
        width, height = rect.right - rect.left, rect.bottom - rect.top
        if width <= 0 or height <= 0:
            raise ProbeError("target client has no usable area")
        point = self.Point(rect.left + round(self.config.x * (width - 1)),
                           rect.top + round(self.config.y * (height - 1)))
        if not self.user.ClientToScreen(self.config.hwnd, ctypes.byref(point)):
            self._error("ClientToScreen")
        if self.user.WindowFromPoint(point) != self.config.hwnd:
            raise ProbeError("selected client point is obscured or belongs to another window")
        if not self.user.SetCursorPos(point.x, point.y):
            self._error("SetCursorPos")
        actual = self.Point()
        if (not self.user.GetCursorPos(ctypes.byref(actual)) or (actual.x, actual.y) != (point.x, point.y)
                or self.user.WindowFromPoint(actual) != self.config.hwnd):
            raise ProbeError("cursor did not remain at the validated target point")
        self.validate_identity()
        if not self.released():
            raise ProbeError("left button became held before injection")

    def _word(self, index):
        if not self.view or self.view % 8 or not 0 <= index < WORD_COUNT:
            raise ProbeError("invalid or unaligned shared-memory word")
        return ctypes.cast(self.view + index * 8, ctypes.POINTER(ctypes.c_int64))

    def compare_state(self, expected, desired):
        return self.atomic.edupInputLatencyCompareExchange64(self._word(INDEX["state"]), desired, expected)

    def write_word(self, name, value):
        self.atomic.edupInputLatencyExchange64(self._word(INDEX[name]), ctypes.c_int64(value).value)

    def snapshot(self):
        """Acquire state around atomic word reads; terminal publication owns results."""
        for _ in range(8):
            before = self.compare_state(IDLE, IDLE)
            words = [self.atomic.edupInputLatencyCompareExchange64(self._word(i), 0, 0)
                     for i in range(WORD_COUNT)]
            if before == words[INDEX["state"]] == self.compare_state(IDLE, IDLE):
                return words
        raise ProbeError("mapping state changed repeatedly during snapshot")

    def qpc(self):
        value = ctypes.c_int64()
        if not self.kernel.QueryPerformanceCounter(ctypes.byref(value)) or value.value <= 0:
            self._error("QueryPerformanceCounter")
        return value.value

    def button(self, pressed):
        event = self.Input()
        event.type, event.mi.dwFlags = 0, 0x0002 if pressed else 0x0004
        if self.user.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
            failure = InputRefused if pressed else ProbeError
            raise failure("SendInput left-button %s inserted zero events (Win32 error %d); stopping" %
                          ("down" if pressed else "up", ctypes.get_last_error()))

    @staticmethod
    def now():
        return time.monotonic()

    @staticmethod
    def sleep(seconds):
        time.sleep(seconds)

    def close(self):
        if self.view:
            self.kernel.UnmapViewOfFile(self.view)
            self.view = None
        for name in ("mapping", "process"):
            handle = getattr(self, name)
            if handle:
                self.kernel.CloseHandle(handle)
                setattr(self, name, None)


class Collector:
    """Single sender state machine; never clears an active receiver-owned state."""

    def __init__(self, config, backend):
        self.config, self.backend = config, backend
        self.generation = None
        self.last_request = 0
        self.latest = None

    def snapshot(self):
        data = decode(self.backend.snapshot())
        self.latest = data
        expected = {"magic": MAGIC, "version": VERSION, "size": SHARED_SIZE,
                    "pid": self.config.pid, "creationTime": self.config.creation_time,
                    "frequency": self.backend.frequency, "mode": MODES[self.config.mode]}
        for name, value in expected.items():
            if data[name] != value:
                raise ProbeError("mapping %s mismatch (expected %d, observed %d)" % (name, value, data[name]))
        if (data["frequency"] <= 0 or not data["generation"]
                or data["state"] not in range(8) or data["ready"] not in (0, 1)):
            raise ProbeError("invalid mapping frequency/generation/state/ready")
        if self.generation is None:
            self.generation = data["generation"]
        if data["generation"] != self.generation:
            raise ProbeError("mapping generation changed; stop and start a fresh run")
        return data

    def _wait(self, deadline):
        remaining = deadline - self.backend.now()
        if remaining <= 0:
            raise ProbeError("timed out after 3 seconds; delivery is ambiguous, no further input")
        self.backend.sleep(min(POLL_SECONDS, remaining))

    def sample(self, index):
        """Collect one pulse, retaining a failed record and releasing only owned input."""
        record = {"kind": "sample", "index": index}
        owned_button = reserved = armed = terminal = False
        request = sent = None
        try:
            deadline = self.backend.now() + TIMEOUT_SECONDS
            while True:
                self.backend.validate_identity()
                data = self.snapshot()
                if data["state"] != IDLE:
                    raise ProbeError("receiver is not Idle; refusing another sender's or unfinished request")
                if data["ready"] == 1 and self.backend.released():
                    break
                self._wait(deadline)
            if self.backend.compare_state(IDLE, RESERVED) != IDLE:
                raise ProbeError("receiver reservation raced; no input was injected")
            reserved = True
            data = self.snapshot()
            if data["state"] != RESERVED or data["ready"] != 1:
                raise ProbeError("receiver lost released/Reserved readiness before arm")
            request = max(self.backend.qpc(), self.last_request + 1, data["request"] + 1)
            if request > UINT64_MAX:
                raise ProbeError("request counter exhausted")
            record["request"] = request
            self.backend.position()
            self.backend.validate_identity()
            data = self.snapshot()
            if data["state"] != RESERVED or data["ready"] != 1 or not self.backend.released():
                raise ProbeError("receiver or left-button readiness changed before arm")
            self.backend.write_word("request", request)
            sent = self.backend.qpc()
            self.backend.write_word("sentQpc", sent)
            if self.backend.compare_state(RESERVED, ARMED) != RESERVED:
                raise ProbeError("Armed publication refused; no input was injected")
            armed, reserved = True, False
            self.last_request = request
            # Own a potentially inserted down across interrupts/foreign-call
            # errors. Only an explicit zero-insert refusal cancels ownership.
            owned_button = True
            try:
                self.backend.button(True)
            except InputRefused:
                owned_button = False
                raise
            deadline = self.backend.now() + TIMEOUT_SECONDS
            hold_until = self.backend.now() + self.config.hold_ms / 1000.0
            try:
                while self.backend.now() < hold_until:
                    self.backend.validate_identity()
                    self.backend.sleep(max(0.0, min(POLL_SECONDS, hold_until - self.backend.now())))
            finally:
                if owned_button:
                    self.backend.button(False)
                    owned_button = False
            while True:
                self.backend.validate_identity()
                data = self.snapshot()
                if data["request"] != request or data["sentQpc"] != sent:
                    raise ProbeError("request identity changed after injection")
                if data["state"] in (COMPLETE, REJECTED):
                    terminal = True
                    break
                if data["state"] not in (ARMED, CONSUMED, RENDERING):
                    raise ProbeError("receiver left active request without a terminal result")
                self._wait(deadline)
            if data["state"] == REJECTED:
                reason = data["reason"]
                record.update(status="rejected", error="receiver rejected pulse: " +
                              (REASONS[reason] if reason < len(REASONS) else str(reason)))
            else:
                if (data["reason"] != 0 or not 0 < sent <= data["consumedQpc"]
                        or not data["renderSequence"] or data["eyeMask"] != 3 or data["submitMask"] != 3
                        or data["gpuCompleteQpc"] < data["consumedQpc"]
                        or data["submitCompleteQpc"] < data["consumedQpc"]
                        or data["source"] not in (1, 2)):
                    raise ProbeError("Complete result has invalid timestamps, paired eyes/submits, or input source")
                scale = 1000.0 / data["frequency"]
                record.update(status="complete", metrics={
                    "injection_to_consumption_ms": (data["consumedQpc"] - sent) * scale,
                    "consumption_to_gpu_notification_ms": (data["gpuCompleteQpc"] - data["consumedQpc"]) * scale,
                    "injection_to_gpu_notification_ms": (data["gpuCompleteQpc"] - sent) * scale,
                    "injection_to_paired_submit_ms": (data["submitCompleteQpc"] - sent) * scale,
                })
        except Exception as error:
            record.update(status="error", error=str(error))
        except KeyboardInterrupt:
            record.update(status="error", error="interrupted; no further input")
        finally:
            if owned_button:
                try:
                    self.backend.button(False)
                    owned_button = False
                except Exception as error:
                    record.update(status="error", error="owned left-button release failed: " + str(error))
            # Reserved belongs to this sender. Terminal results are acknowledged
            # only while request, mode, and generation still belong to this run.
            if reserved or terminal:
                try:
                    current = self.snapshot()
                    same = (not armed or (current["request"] == request and current["sentQpc"] == sent))
                    if same and ((reserved and current["state"] == RESERVED)
                                 or (terminal and current["state"] in (COMPLETE, REJECTED))):
                        if self.backend.compare_state(current["state"], IDLE) != current["state"]:
                            raise ProbeError("receiver changed while acknowledging owned result")
                    else:
                        raise ProbeError("receiver ownership changed before acknowledgement")
                except Exception as error:
                    record.update(status="error", error=str(error))
            else:
                try:
                    self.snapshot()
                except Exception:
                    # snapshot() captures raw words before validating them. Keep
                    # the original refusal if identity/mode has also changed.
                    pass
            if self.latest is not None:
                record["snapshot"] = self.latest
            if record.get("status") != "complete":
                record.pop("metrics", None)
        return record


def summary(samples, error=None):
    """Aggregate only verified complete samples; absent data stays absent."""
    complete = [sample for sample in samples if sample["status"] == "complete"]
    result = {"kind": "summary", "status": "error" if error or len(complete) != len(samples) else "complete",
              "attempted": len(samples), "complete": len(complete),
              "rejected": sum(s["status"] == "rejected" for s in samples),
              "errors": sum(s["status"] == "error" for s in samples), "metrics": {}}
    for name in METRICS:
        values = sorted(sample["metrics"][name] for sample in complete)
        if values:
            result["metrics"][name] = {"min": values[0], "median": statistics.median(values),
                                       "p95": values[max(0, math.ceil(len(values) * 0.95) - 1)],
                                       "max": values[-1], "mean": statistics.mean(values)}
    if error:
        result["error"] = error
    return result


def write_jsonl(stream, record):
    stream.write(json.dumps(record, allow_nan=False, sort_keys=True) + "\n")
    stream.flush()


def collect(config, backend_factory):
    """Exclusive-create output first, then preserve metadata, failures and summary."""
    samples, backend, error = [], None, None
    with config.output.open("x", encoding="utf-8", newline="\n") as stream:
        write_jsonl(stream, {"kind": "metadata", "protocol_version": VERSION,
                            "started_utc": datetime.now(timezone.utc).isoformat(),
                            "target": {"pid": config.pid, "creation_time": config.creation_time,
                                       "session": config.session, "image": config.image, "hwnd": config.hwnd},
                            "mode": config.mode, "mapping": config.mapping_name,
                            "atomic_helper": str(atomic_helper_path(config.atomic_helper)),
                            "count": config.count, "interval_ms": config.interval_ms, "hold_ms": config.hold_ms,
                            "client_point": {"x": config.x, "y": config.y}, "metric_definitions": METRICS,
                            "limits": "GPU-fence notification is an upper bound, not photons or gameplay physics"})
        try:
            backend = backend_factory(config)
            collector = Collector(config, backend)
            next_start = backend.now()
            for index in range(1, config.count + 1):
                while backend.now() < next_start:
                    backend.validate_identity()
                    backend.sleep(max(0.0, min(POLL_SECONDS, next_start - backend.now())))
                next_start = backend.now() + config.interval_ms / 1000.0
                sample = collector.sample(index)
                samples.append(sample)
                write_jsonl(stream, sample)
                if sample["status"] != "complete":
                    error = sample["error"]
                    break
        except Exception as failure:
            error = str(failure)
        except KeyboardInterrupt:
            error = "interrupted; no further input"
        finally:
            if backend is not None:
                backend.close()
            result = summary(samples, error)
            write_jsonl(stream, result)
    return result


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument("--self-test", action="store_true", help="run portable fake-backend protocol and safety tests")
    result.add_argument("--self-test-atomic-helper", action="store_true", help="verify real Win64 helper exports on private memory; no game/input")
    result.add_argument("--dry-run", action="store_true", help="validate only; no Windows resources, input, or writes")
    result.add_argument("--atomic-helper", type=Path, help="native atomic helper DLL (default: repo/build/tools/input_latency_atomic.dll)")
    result.add_argument("--pid", type=lambda value: int(value, 0), help="exact target process ID")
    result.add_argument("--creation-time", type=lambda value: int(value, 0), help="exact process birth FILETIME integer")
    result.add_argument("--session", type=lambda value: int(value, 0), help="expected interactive Windows session ID")
    result.add_argument("--image", help="expected absolute Windows executable path")
    result.add_argument("--hwnd", type=lambda value: int(value, 0), help="exact foreground client HWND (decimal or 0xHEX)")
    result.add_argument("--mode", choices=MODES, help="current game mode; the tool never changes mode")
    result.add_argument("--output", type=Path, help="fresh JSONL path in an existing directory (never overwritten)")
    result.add_argument("--count", type=int, default=60, help="pulse count (default: 60)")
    result.add_argument("--interval-ms", type=float, default=400.0, help="minimum pulse start interval (default: 400)")
    result.add_argument("--hold-ms", type=float, default=80.0, help="left-button hold duration (default: 80)")
    result.add_argument("--x", type=float, default=0.98, help="normalized benign client x coordinate (default: 0.98)")
    result.add_argument("--y", type=float, default=0.97, help="normalized benign client y coordinate (default: 0.97)")
    return result


def main(argv=None, backend_factory=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    argument_parser = parser()
    args = argument_parser.parse_args(arguments)
    if args.self_test:
        if arguments != ["--self-test"]:
            argument_parser.error("--self-test must be used alone")
        return self_test()
    if args.self_test_atomic_helper:
        allowed = parser().parse_args(["--self-test-atomic-helper"] +
                                     ([] if args.atomic_helper is None else ["--atomic-helper", str(args.atomic_helper)]))
        if vars(args) != vars(allowed):
            argument_parser.error("--self-test-atomic-helper accepts only --atomic-helper")
        return atomic_helper_self_test(args.atomic_helper)
    required = ("pid", "creation_time", "session", "image", "hwnd", "mode", "output")
    missing = ["--" + name.replace("_", "-") for name in required if getattr(args, name) is None]
    if missing:
        argument_parser.error("required: " + ", ".join(missing))
    config = Config(**{name: getattr(args, name) for name in Config.__dataclass_fields__})
    try:
        config.validate()
        if args.dry_run:
            print("input_latency_probe: dry-run valid; no resources, input, or files written")
            print("mapping=%s mode=%s output=%s" % (config.mapping_name, config.mode, config.output))
            return 0
        result = collect(config, backend_factory or WindowsBackend)
    except (ValueError, ProbeError, OSError) as error:
        print("input_latency_probe: " + str(error), file=sys.stderr)
        return 2
    print("input_latency_probe: %d/%d complete; %s" % (result["complete"], config.count, result["status"]))
    if result.get("error"):
        print(result["error"], file=sys.stderr)
    return 0 if result["status"] == "complete" else 1


class FakeBackend:
    """Deterministic receiver and identity faults; never imports Windows APIs."""

    def __init__(self, config, behavior="complete"):
        self.config, self.behavior = config, behavior
        self.frequency, self.clock = 1000000, 1.0
        self.words = [0] * WORD_COUNT
        for name, value in {"magic": MAGIC, "version": VERSION, "size": SHARED_SIZE,
                            "pid": config.pid, "creationTime": config.creation_time,
                            "frequency": self.frequency, "ready": 1, "generation": 7,
                            "mode": MODES[config.mode]}.items():
            self.words[INDEX[name]] = value
        self.events, self.down, self.closed = [], False, False
        self.facts = {"alive": True, "creation_time": config.creation_time, "image": config.image,
                      "target_session": config.session, "sender_session": config.session,
                      "window_pid": config.pid, "window_exists": True, "visible": True,
                      "minimized": False, "foreground": config.hwnd}

    def validate_identity(self):
        self.events.append("identity")
        if self.behavior == "identity" or (self.behavior == "identity_after_down" and self.down):
            self.facts["creation_time"] += 1
        validate_target(self.config, self.facts)

    def released(self):
        return not self.down and self.behavior != "physical_down"

    def position(self):
        self.validate_identity()
        self.events.append("position")

    def compare_state(self, expected, desired):
        old = self.words[INDEX["state"]]
        self.events.append(("cas", expected, desired))
        if self.behavior == "arm_race" and desired == ARMED:
            self.words[INDEX["state"]] = STOPPED
            return STOPPED
        if old == expected:
            self.words[INDEX["state"]] = desired
        return old

    def write_word(self, name, value):
        self.events.append((name, value))
        self.words[INDEX[name]] = value

    def snapshot(self):
        return list(self.words)

    def qpc(self):
        self.events.append("qpc")
        return int(self.clock * self.frequency)

    def button(self, pressed):
        self.events.append("down" if pressed else "up")
        if pressed and self.behavior == "send_refused":
            raise InputRefused("SendInput refused")
        self.down = pressed
        if pressed and self.behavior == "send_ambiguous":
            raise OSError("ambiguous error after down insertion")
        if pressed:
            self.words[INDEX["ready"]] = 0
            if self.behavior not in ("timeout", "identity_after_down"):
                sent = self.words[INDEX["sentQpc"]]
                result = {"state": COMPLETE, "consumedQpc": sent + 1000, "renderSequence": 8,
                          "eyeMask": 3, "submitMask": 3, "gpuCompleteQpc": sent + 4000,
                          "submitCompleteQpc": sent + 3000, "source": 1}
                if self.behavior == "rejected":
                    result.update(state=REJECTED, reason=5)
                if self.behavior == "bad_time":
                    result["consumedQpc"] = sent - 1
                if self.behavior == "bad_pair":
                    result["submitMask"] = 1
                if self.behavior == "generation":
                    result["generation"] = 8
                if self.behavior == "mode":
                    result["mode"] = (MODES[self.config.mode] + 1) % 3
                if self.behavior == "request":
                    result["request"] = self.words[INDEX["request"]] + 1
                for name, value in result.items():
                    self.words[INDEX[name]] = value
        else:
            self.words[INDEX["ready"]] = 1

    def now(self):
        return self.clock

    def sleep(self, seconds):
        self.clock += seconds

    def close(self):
        self.closed = True


def self_test():
    class Tests(unittest.TestCase):
        def config(self, output=Path("unused.jsonl"), **kwargs):
            return Config(pid=123, creation_time=456, session=2, image=r"C:\Game\EliteDangerous64.exe",
                          hwnd=789, mode="on", output=output, **kwargs)

        def sample(self, behavior="complete"):
            config = self.config()
            backend = FakeBackend(config, behavior)
            return Collector(config, backend).sample(1), backend

        def test_complete_order_metrics_and_raw_words(self):
            record, backend = self.sample()
            self.assertEqual(record["status"], "complete")
            self.assertEqual(record["metrics"], dict(zip(METRICS, (1.0, 3.0, 4.0, 3.0))))
            self.assertEqual(len(record["snapshot"]["words"]), WORD_COUNT)
            self.assertEqual(backend.words[INDEX["state"]], IDLE)
            events = backend.events
            self.assertLess(events.index(("cas", IDLE, RESERVED)), events.index("position"))
            at = events.index(("sentQpc", 1000000)) - 1
            self.assertEqual(events[at + 1:at + 4], [("sentQpc", 1000000), ("cas", RESERVED, ARMED), "down"])
            self.assertEqual(events.count("down"), 1)
            self.assertEqual(events.count("up"), 1)
            self.assertFalse(backend.down)

        def test_request_monotonic_and_receiver_ready_required(self):
            config = self.config()
            backend = FakeBackend(config)
            backend.words[INDEX["request"]] = 41
            collector = Collector(config, backend)
            first = collector.sample(1)["request"]
            second = collector.sample(2)["request"]
            self.assertGreaterEqual(first, 1000000)
            self.assertGreater(second, first)
            backend.words[INDEX["ready"]] = 0
            result = collector.sample(3)
            self.assertEqual(result["status"], "error")
            self.assertEqual(backend.events.count("down"), 2)

        def test_identity_and_physical_button_refusals(self):
            for behavior in ("identity", "physical_down"):
                with self.subTest(behavior=behavior):
                    result, backend = self.sample(behavior)
                    self.assertEqual(result["status"], "error")
                    self.assertNotIn("down", backend.events)
                    self.assertNotIn("up", backend.events)

        def test_each_identity_boundary_is_checked(self):
            changes = {"alive": False, "creation_time": 457, "image": r"C:\Other\game.exe",
                       "target_session": 3, "sender_session": 3, "window_pid": 124,
                       "window_exists": False, "visible": False, "minimized": True, "foreground": 790}
            for name, value in changes.items():
                with self.subTest(name=name):
                    config = self.config()
                    backend = FakeBackend(config)
                    backend.facts[name] = value
                    result = Collector(config, backend).sample(1)
                    self.assertEqual(result["status"], "error")
                    self.assertNotIn("down", backend.events)
                    self.assertNotIn("up", backend.events)

        def test_release_after_identity_change_while_held(self):
            result, backend = self.sample("identity_after_down")
            self.assertEqual(result["status"], "error")
            self.assertEqual(backend.events.count("up"), 1)
            self.assertFalse(backend.down)
            self.assertEqual(backend.words[INDEX["state"]], ARMED)

        def test_timeout_never_clears_receiver_or_invents_metrics(self):
            result, backend = self.sample("timeout")
            self.assertEqual(result["status"], "error")
            self.assertIn("timed out", result["error"])
            self.assertGreaterEqual(backend.now(), 4.0)
            self.assertLess(backend.now(), 4.1)
            self.assertEqual(backend.words[INDEX["state"]], ARMED)
            self.assertNotIn("metrics", result)
            self.assertEqual(backend.events.count("up"), 1)

        def test_terminal_rejection_acknowledged_without_measurement(self):
            result, backend = self.sample("rejected")
            self.assertEqual(result["status"], "rejected")
            self.assertIn("TextureUnsupported", result["error"])
            self.assertEqual(backend.words[INDEX["state"]], IDLE)
            self.assertNotIn("metrics", result)

        def test_bad_results_and_changed_ownership(self):
            for behavior in ("bad_time", "bad_pair", "generation", "mode", "request"):
                with self.subTest(behavior=behavior):
                    result, backend = self.sample(behavior)
                    self.assertEqual(result["status"], "error")
                    self.assertNotIn("metrics", result)
                    self.assertEqual(backend.events.count("up"), 1)
                    if behavior in ("generation", "mode", "request"):
                        self.assertEqual(backend.words[INDEX["state"]], COMPLETE)

        def test_publication_race_and_send_refusal(self):
            for behavior in ("arm_race", "send_refused"):
                with self.subTest(behavior=behavior):
                    result, backend = self.sample(behavior)
                    self.assertEqual(result["status"], "error")
                    self.assertNotIn("up", backend.events)
                    self.assertEqual(backend.words[INDEX["state"]], STOPPED if behavior == "arm_race" else ARMED)

        def test_ambiguous_insertion_releases_owned_button_and_stops(self):
            result, backend = self.sample("send_ambiguous")
            self.assertEqual(result["status"], "error")
            self.assertEqual(backend.events.count("down"), 1)
            self.assertEqual(backend.events.count("up"), 1)
            self.assertFalse(backend.down)
            self.assertEqual(backend.words[INDEX["state"]], ARMED)

        def test_header_refusals_and_busy_state(self):
            for name in ("magic", "version", "size", "pid", "creationTime", "frequency", "mode", "state"):
                with self.subTest(name=name):
                    config = self.config()
                    backend = FakeBackend(config)
                    backend.words[INDEX[name]] += 1
                    result = Collector(config, backend).sample(1)
                    self.assertEqual(result["status"], "error")
                    self.assertNotIn("down", backend.events)
                    self.assertNotIn("up", backend.events)

        def test_jsonl_failure_preserved_and_exclusive(self):
            with tempfile.TemporaryDirectory() as folder:
                config = self.config(Path(folder) / "samples.jsonl", count=3)
                backend = FakeBackend(config, "timeout")
                result = collect(config, lambda _: backend)
                rows = [json.loads(line) for line in config.output.read_text(encoding="utf-8").splitlines()]
                self.assertEqual([row["kind"] for row in rows], ["metadata", "sample", "summary"])
                self.assertEqual(result["attempted"], 1)
                self.assertEqual(result["metrics"], {})
                self.assertTrue(backend.closed)
                before = config.output.read_bytes()
                with self.assertRaises(FileExistsError):
                    collect(config, lambda _: self.fail("existing output must prevent resource opening"))
                self.assertEqual(config.output.read_bytes(), before)

        def test_main_collection_and_factory_failure_preserve_summary(self):
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "live.jsonl"
                arguments = ["--pid", "123", "--creation-time", "456", "--session", "2", "--image",
                             r"C:\Game\EliteDangerous64.exe", "--hwnd", "789", "--mode", "off",
                             "--output", str(output), "--count", "2"]
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(main(arguments, FakeBackend), 0)
                rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
                self.assertEqual([row["kind"] for row in rows], ["metadata", "sample", "sample", "summary"])
                self.assertEqual(rows[-1]["complete"], 2)
                output.unlink()

                def unavailable(_):
                    raise ProbeError("mapping unavailable")

                with redirect_stdout(io.StringIO()), mock.patch("sys.stderr", io.StringIO()):
                    self.assertEqual(main(arguments, unavailable), 1)
                rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
                self.assertEqual([row["kind"] for row in rows], ["metadata", "summary"])
                self.assertEqual(rows[-1]["error"], "mapping unavailable")
                self.assertEqual(rows[-1]["metrics"], {})

        def test_win32_abi_without_windows_resources(self):
            backend = WindowsBackend.__new__(WindowsBackend)
            backend.config = self.config()
            with mock.patch.object(ctypes, "WinDLL", mock.Mock(), create=True) as libraries, \
                    mock.patch(__name__ + ".load_atomic_helper") as helper:
                backend._bind()
            helper.assert_called_once_with(None)
            self.assertEqual(libraries.call_count, 2)
            self.assertNotIn("InterlockedCompareExchange64", backend.kernel._mock_children)
            self.assertNotIn("InterlockedExchange64", backend.kernel._mock_children)
            self.assertEqual(ctypes.sizeof(backend.Input), 40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)
            self.assertEqual(ctypes.sizeof(backend.Point), 8)
            self.assertEqual(ctypes.sizeof(backend.Rect), 16)
            self.assertEqual(ctypes.sizeof(backend.FileTime), 8)

        def test_atomic_helper_loader_uses_owned_exports(self):
            if ctypes.sizeof(ctypes.c_void_p) != 8:
                self.skipTest("helper ABI requires a 64-bit interpreter")
            path = Path("helper-fixture.dll")
            with mock.patch.object(sys, "platform", "win32"), \
                    mock.patch.object(Path, "is_file", return_value=True), \
                    mock.patch.object(ctypes, "WinDLL", mock.Mock(), create=True) as loader:
                library = load_atomic_helper(path)
            loader.assert_called_once_with(str(path.resolve()), use_last_error=True, winmode=0x1100)
            self.assertIs(library.edupInputLatencyCompareExchange64.restype, ctypes.c_uint64)
            self.assertIs(library.edupInputLatencyExchange64.restype, ctypes.c_uint64)
            self.assertEqual(len(library.edupInputLatencyCompareExchange64.argtypes), 3)
            self.assertEqual(len(library.edupInputLatencyExchange64.argtypes), 2)

        def test_real_helper_test_dispatch_accepts_only_helper_override(self):
            with mock.patch(__name__ + ".atomic_helper_self_test", return_value=0) as helper:
                self.assertEqual(main(["--self-test-atomic-helper", "--atomic-helper", "fixture.dll"]), 0)
            helper.assert_called_once_with(Path("fixture.dll"))
            with mock.patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
                main(["--self-test-atomic-helper", "--dry-run"])

        def test_main_dry_run_writes_nothing_and_never_opens_backend(self):
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "dry.jsonl"
                arguments = ["--pid", "123", "--creation-time", "456", "--session", "2", "--image",
                             r"C:\Game\EliteDangerous64.exe", "--hwnd", "0x315", "--mode", "boost",
                             "--output", str(output), "--dry-run"]
                with redirect_stdout(io.StringIO()), mock.patch("builtins.open", side_effect=AssertionError("no file opens")), \
                        mock.patch(__name__ + ".load_atomic_helper", side_effect=AssertionError("no DLL loads")):
                    self.assertEqual(main(arguments, lambda _: self.fail("dry-run opened resources")), 0)
                self.assertEqual(list(Path(folder).iterdir()), [])
                with redirect_stdout(io.StringIO()), mock.patch("sys.stderr", io.StringIO()):
                    self.assertEqual(main(arguments + ["--x", "nan"], lambda _: self.fail("invalid dry-run opened resources")), 2)
                self.assertEqual(list(Path(folder).iterdir()), [])

        def test_config_and_empty_summary(self):
            with tempfile.TemporaryDirectory() as folder:
                for updates in ({"session": 0}, {"image": r"\Game\game.exe"}, {"hold_ms": 0},
                                {"x": 1.1}, {"interval_ms": float("inf")}, {"count": 0}):
                    config = self.config(Path(folder) / "fresh.jsonl")
                    values = dict(config.__dict__, **updates)
                    with self.subTest(updates=updates), self.assertRaises(ValueError):
                        Config(**values).validate()
            self.assertEqual(summary([], "no receiver")["metrics"], {})

    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
