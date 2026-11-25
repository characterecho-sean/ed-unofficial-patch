#!/usr/bin/env python3
"""Collect stationary-game input-latency diagnostic pulses (Windows only).

The instrumented game must already be foreground in the sender's interactive
Windows session. Each pulse moves the cursor to a caller-selected client point
(default x=0.98, y=0.97), sends only left-button down/up, and waits for the game's
actual input reader and paired eye response. Choose a benign point while the
game is stationary. This tool never activates a window.

Explicit --input-key UP|DOWN selects real UI response mode (IPC v2). It requires
normalized --response-region x0,y0,x1,y1, --expected-orange-state on|off, and
--orange-low-ppm/--orange-high-ppm classification bounds. The right-eye region
defaults to the same rectangle, or --response-region-right overrides it. The
receiver must prove both eyes are in the opposite state and observe the selected
key released before input is armed. Each successful pulse alternates UP/DOWN and
the expected state; any failure stops the run. Keyboard mode never moves the
cursor and sends no Enter or other keys. No synthetic response is drawn in UI
mode. Regions refer to each final eye texture, not desktop/window coordinates.
Orange pixel classification uses device code values R>=96, G>=48, R>=G+16,
G>=B+24, B<=96. An off state has at most orange-low-ppm orange pixels per
million; an on state has at least orange-high-ppm. Values between the bounds
are ambiguous and never qualify as a baseline or completed response.

Consumption means receipt at a game-owned input API: a DirectInput state packet
(source 1), a non-peek buffered DirectInput event (source 2), or a game-window
mouse message removed by the game's PeekMessageA call with PM_REMOVE (source 3).
This endpoint does not establish that gameplay processed the input.
UI mode requires source 4: a successful 256-byte DirectInput keyboard state read
inside the validated game InputManager tick, carrying its nonzero tick token.
Paired eye pixels supply the separate evidence that the requested UI changed.

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
VERSION = 2
WORD_COUNT = 32
SHARED_SIZE = WORD_COUNT * 8
UINT64_MAX = (1 << 64) - 1
IDLE, RESERVED, ARMED, CONSUMED, RENDERING, COMPLETE, REJECTED, STOPPED = range(8)
PREPARING, PREPARED = 8, 9
KEYS = {"UP": (0xC8, 0x26, 0x48), "DOWN": (0xD0, 0x28, 0x50)}  # DIK, VK, extended scan code
MODES = {"off": 0, "on": 1, "boost": 2}
SOURCES = {1: "directinput_state", 2: "directinput_buffered_event",
           3: "window_mouse_message_peekmessagea_remove", 4: "directinput_keyboard_state_input_manager_tick"}
REASONS = ("None", "Timeout", "InvalidRequest", "GenerationChanged", "WrongThread",
           "TextureUnsupported", "MixedFrame", "DuplicateEye", "GraphicsFailure",
           "SubmitFailed", "DeviceRemoved", "Stopped", "BaselineMismatch", "CandidateOverflow")
FIELDS = ("magic", "version", "size", "pid", "creationTime", "frequency", "state",
          "ready", "generation", "mode", "request", "sentQpc", "consumedQpc",
          "renderSequence", "eyeMask", "submitMask", "gpuCompleteQpc",
          "submitCompleteQpc", "reason", "source", "responseKind", "inputScanCode", "roiLeft", "roiRight",
          "orangeLowPpm", "orangeHighPpm", "targetOn", "baselineLeftPpm", "baselineRightPpm",
          "responseLeftPpm", "responseRightPpm", "consumedTickToken")
REQUEST_FIELDS = FIELDS[20:27]
INDEX = {name: index for index, name in enumerate(FIELDS)}
TIMEOUT_SECONDS = 3.0
POLL_SECONDS = 0.01
DEFAULT_ATOMIC_HELPER = Path(__file__).resolve().parent.parent / "build" / "tools" / "input_latency_atomic.dll"
METRICS = {
    "injection_to_consumption_ms": "sentQpc to game input API receipt (not gameplay processing)",
    "consumption_to_gpu_notification_ms": "game input API receipt to GPU-fence CPU notification (upper bound)",
    "injection_to_gpu_notification_ms": "sentQpc to GPU-fence CPU notification (upper bound)",
    "injection_to_paired_submit_ms": "sentQpc to successful paired native-submit CPU endpoint",
}


class ProbeError(RuntimeError):
    """A refusal or ambiguous observation that must stop further input."""


class InputRefused(ProbeError):
    """SendInput explicitly inserted zero events; this call owns no input edge."""


class ReceiverRejected(ProbeError):
    """An owned receiver request ended with an explicit refusal before input."""


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
    input_key: str = None
    response_region: tuple = None
    response_region_right: tuple = None
    expected_orange_state: str = None
    orange_low_ppm: int = None
    orange_high_ppm: int = None

    @property
    def ui(self):
        return self.input_key is not None

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
        ui_options = (self.input_key, self.response_region, self.expected_orange_state,
                      self.orange_low_ppm, self.orange_high_ppm)
        if any(value is not None for value in ui_options + (self.response_region_right,)):
            if any(value is None for value in ui_options):
                raise ValueError("UI mode requires --input-key, --response-region, --expected-orange-state, "
                                 "--orange-low-ppm and --orange-high-ppm together")
            if self.input_key not in KEYS or self.expected_orange_state not in ("on", "off"):
                raise ValueError("UI mode accepts only UP/DOWN and expected orange state on/off")
            if not 0 <= self.orange_low_ppm < self.orange_high_ppm <= 1000000:
                raise ValueError("require 0 <= --orange-low-ppm < --orange-high-ppm <= 1000000")
            pack_roi(self.response_region)
            if self.response_region_right is not None:
                pack_roi(self.response_region_right)
        if self.output.exists() or self.output.is_symlink():
            raise ValueError("--output already exists; select a fresh JSONL path")
        if not self.output.parent.is_dir():
            raise ValueError("--output parent directory must already exist")


def normalized_image(image):
    return ntpath.normcase(ntpath.normpath(image))


def pack_roi(region):
    """Pack normalized x0,y0,x1,y1 into four uint16 words without empty boxes."""
    if len(region) != 4 or not all(math.isfinite(value) and 0 <= value <= 1 for value in region):
        raise ValueError("response region must have four finite normalized coordinates in [0, 1]")
    values = tuple(round(value * 65535) for value in region)
    if not values[0] < values[2] or not values[1] < values[3]:
        raise ValueError("response region must have x0 < x1 and y0 < y1 after uint16 quantization")
    return sum(value << (16 * index) for index, value in enumerate(values))


def parse_roi(value):
    try:
        region = tuple(float(part) for part in value.split(","))
        pack_roi(region)
        return region
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def orange_matches(fraction, target_on, low, high):
    return 0 <= fraction <= 1000000 and (fraction >= high if target_on else fraction <= low)


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
    return dict(zip(FIELDS, words), reserved=words[20:] if words[1] == 1 else [], words=list(words))


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

        class KeyboardInput(ctypes.Structure):
            _fields_ = [("wVk", ctypes.c_uint16), ("wScan", ctypes.c_uint16),
                        ("dwFlags", ctypes.c_uint32), ("time", ctypes.c_uint32), ("dwExtraInfo", ctypes.c_size_t)]

        class InputUnion(ctypes.Union):
            _fields_ = [("mi", MouseInput), ("ki", KeyboardInput)]

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

    def released(self, key=None):
        return not (self.user.GetAsyncKeyState(0x01 if key is None else KEYS[key][1]) & 0x8000)

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

    def key(self, key, pressed):
        """Send only an owned extended UP/DOWN scan-code edge, never a character."""
        if key not in KEYS:
            raise ProbeError("only UP and DOWN keys are permitted")
        event = self.Input()
        event.type, event.ki.wScan = 1, KEYS[key][2]
        event.ki.dwFlags = 0x0008 | 0x0001 | (0 if pressed else 0x0002)
        if self.user.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
            failure = InputRefused if pressed else ProbeError
            raise failure("SendInput %s %s inserted zero events (Win32 error %d); stopping" %
                          (key, "down" if pressed else "up", ctypes.get_last_error()))

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
        self.version = None
        self.last_request = 0
        self.latest = None
        self.next_key = config.input_key
        self.next_target = config.expected_orange_state == "on"

    def snapshot(self):
        data = decode(self.backend.snapshot())
        self.latest = data
        expected = {"magic": MAGIC, "size": SHARED_SIZE,
                    "pid": self.config.pid, "creationTime": self.config.creation_time,
                    "frequency": self.backend.frequency, "mode": MODES[self.config.mode]}
        for name, value in expected.items():
            if data[name] != value:
                raise ProbeError("mapping %s mismatch (expected %d, observed %d)" % (name, value, data[name]))
        if data["version"] not in ((2,) if self.config.ui else (1, 2)):
            raise ProbeError("UI mode requires IPC v2" if self.config.ui else "unsupported mapping protocol version")
        if self.version is None:
            self.version = data["version"]
        if self.version != data["version"]:
            raise ProbeError("mapping protocol version changed")
        if (data["frequency"] <= 0 or not data["generation"]
                or data["state"] not in range(10 if self.version == 2 else 8) or data["ready"] not in (0, 1)):
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

    def _parameters(self, key, target_on):
        if self.version == 1:
            return None
        if not self.config.ui:
            return dict.fromkeys(REQUEST_FIELDS, 0)
        return dict(zip(REQUEST_FIELDS, (1, KEYS[key][0], pack_roi(self.config.response_region),
                                        pack_roi(self.config.response_region_right or self.config.response_region),
                                        self.config.orange_low_ppm, self.config.orange_high_ppm, int(target_on))))

    @staticmethod
    def _check_request(data, request, parameters, sent=None):
        if data["request"] != request or (sent is not None and data["sentQpc"] != sent):
            raise ProbeError("request identity changed after publication")
        if parameters is not None and any(data[name] != value for name, value in parameters.items()):
            raise ProbeError("immutable request parameters changed after publication")

    def _baseline(self, data, target_on):
        if (not data["renderSequence"] or data["ready"] != 1
                or not all(orange_matches(data[name], not target_on, self.config.orange_low_ppm,
                                          self.config.orange_high_ppm)
                           for name in ("baselineLeftPpm", "baselineRightPpm"))):
            raise ProbeError("Prepared lacks a paired opposite-state baseline and released-key readiness")
        return {"render_sequence": data["renderSequence"], "left_ppm": data["baselineLeftPpm"],
                "right_ppm": data["baselineRightPpm"]}

    def sample(self, index):
        """Collect one pulse; UI requests prepare real pixels before an owned key edge."""
        record = {"kind": "sample", "index": index}
        owned_input = reserved = armed = terminal = False
        prepared = published = False
        request = sent = None
        parameters = None
        key, target_on = self.next_key, self.next_target
        baseline = None
        terminal_evidence = None
        if self.config.ui:
            record.update(input_key=key, expected_orange_state=target_on, response_kind="real_ui")
        send = (lambda pressed: self.backend.key(key, pressed)) if self.config.ui else self.backend.button
        released = (lambda: self.backend.released(key)) if self.config.ui else self.backend.released
        try:
            deadline = self.backend.now() + TIMEOUT_SECONDS
            while True:
                self.backend.validate_identity()
                data = self.snapshot()
                if data["state"] != IDLE:
                    raise ProbeError("receiver is not Idle; refusing another sender's or unfinished request")
                if (self.config.ui or data["ready"] == 1) and released():
                    break
                self._wait(deadline)
            if self.backend.compare_state(IDLE, RESERVED) != IDLE:
                raise ProbeError("receiver reservation raced; no input was injected")
            reserved = True
            data = self.snapshot()
            if data["state"] != RESERVED or (not self.config.ui and data["ready"] != 1):
                raise ProbeError("receiver lost released/Reserved readiness before arm")
            request = max(self.backend.qpc(), self.last_request + 1, data["request"] + 1)
            if request > UINT64_MAX:
                raise ProbeError("request counter exhausted")
            record["request"] = request
            parameters = self._parameters(key, target_on)
            if not self.config.ui:
                self.backend.position()
            self.backend.validate_identity()
            data = self.snapshot()
            if data["state"] != RESERVED or (not self.config.ui and data["ready"] != 1) or not released():
                raise ProbeError("receiver or left-button readiness changed before arm")
            if parameters is not None:
                for name, value in parameters.items():
                    self.backend.write_word(name, value)
            self.backend.write_word("request", request)
            published = True
            arm_phase = RESERVED
            if self.config.ui:
                sent = 0
                self.backend.write_word("sentQpc", 0)
                if self.backend.compare_state(RESERVED, PREPARING) != RESERVED:
                    raise ProbeError("Preparing publication refused; no input was injected")
                reserved = False
                deadline = self.backend.now() + TIMEOUT_SECONDS
                while True:
                    self.backend.validate_identity()
                    data = self.snapshot()
                    self._check_request(data, request, parameters, 0)
                    if data["state"] == PREPARED:
                        prepared = True
                        baseline = self._baseline(data, target_on)
                        record["baseline"] = baseline
                        break
                    if data["state"] == REJECTED:
                        terminal = True
                        terminal_evidence = {name: data[name] for name in ("state",) + FIELDS[12:20] + FIELDS[27:]}
                        reason = data["reason"]
                        raise ReceiverRejected("receiver rejected UI preparation: " +
                                               (REASONS[reason] if reason < len(REASONS) else str(reason)))
                    if data["state"] != PREPARING:
                        raise ProbeError("receiver left UI preparation without Prepared or Rejected")
                    self._wait(deadline)
                self.backend.validate_identity()
                if not released():
                    raise ProbeError("selected key became held before Armed publication")
                data = self.snapshot()
                self._check_request(data, request, parameters, 0)
                if data["state"] != PREPARED or self._baseline(data, target_on) != baseline:
                    raise ProbeError("UI baseline changed before Armed publication")
                arm_phase = PREPARED
            sent = self.backend.qpc()
            self.backend.write_word("sentQpc", sent)
            if self.backend.compare_state(arm_phase, ARMED) != arm_phase:
                raise ProbeError("Armed publication refused; no input was injected")
            armed, reserved, prepared = True, False, False
            self.last_request = request
            # Own a potentially inserted down across interrupts/foreign-call
            # errors. Only an explicit zero-insert refusal cancels ownership.
            owned_input = True
            try:
                send(True)
            except InputRefused:
                owned_input = False
                raise
            deadline = self.backend.now() + TIMEOUT_SECONDS
            hold_until = self.backend.now() + self.config.hold_ms / 1000.0
            try:
                while self.backend.now() < hold_until:
                    self.backend.validate_identity()
                    self.backend.sleep(max(0.0, min(POLL_SECONDS, hold_until - self.backend.now())))
            finally:
                if owned_input:
                    send(False)
                    owned_input = False
            while True:
                self.backend.validate_identity()
                data = self.snapshot()
                self._check_request(data, request, parameters, sent)
                if data["state"] in (COMPLETE, REJECTED):
                    terminal = True
                    terminal_evidence = {name: data[name] for name in ("state",) + FIELDS[12:20] + FIELDS[27:]}
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
                        or data["source"] not in ((4,) if self.config.ui else (1, 2, 3))):
                    raise ProbeError("Complete result has invalid timestamps, paired eyes/submits, or input source")
                if self.config.ui:
                    if (not data["consumedTickToken"] or data["renderSequence"] <= baseline["render_sequence"]
                            or data["baselineLeftPpm"] != baseline["left_ppm"]
                            or data["baselineRightPpm"] != baseline["right_ppm"]
                            or not all(orange_matches(data[name], target_on, self.config.orange_low_ppm,
                                                      self.config.orange_high_ppm)
                                       for name in ("responseLeftPpm", "responseRightPpm"))):
                        raise ProbeError("Complete lacks a qualified keyboard tick and paired real UI pixel response")
                    record["response"] = {"left_ppm": data["responseLeftPpm"], "right_ppm": data["responseRightPpm"],
                                          "consumed_tick_token": data["consumedTickToken"]}
                scale = 1000.0 / data["frequency"]
                record.update(status="complete", source=data["source"], source_label=SOURCES[data["source"]], metrics={
                    "injection_to_consumption_ms": (data["consumedQpc"] - sent) * scale,
                    "consumption_to_gpu_notification_ms": (data["gpuCompleteQpc"] - data["consumedQpc"]) * scale,
                    "injection_to_gpu_notification_ms": (data["gpuCompleteQpc"] - sent) * scale,
                    "injection_to_paired_submit_ms": (data["submitCompleteQpc"] - sent) * scale,
                })
        except ReceiverRejected as error:
            record.update(status="rejected", error=str(error))
        except Exception as error:
            record.update(status="error", error=str(error))
        except KeyboardInterrupt:
            record.update(status="error", error="interrupted; no further input")
        finally:
            if owned_input:
                try:
                    send(False)
                    owned_input = False
                except Exception as error:
                    record.update(status="error", error="owned input release failed: " + str(error))
            # Reserved belongs to this sender. Terminal results are acknowledged
            # only while request, mode, and generation still belong to this run.
            if reserved or prepared or terminal:
                try:
                    current = self.snapshot()
                    if published:
                        self._check_request(current, request, parameters, sent)
                    if terminal_evidence is not None and any(current[name] != value for name, value in terminal_evidence.items()):
                        raise ProbeError("terminal evidence changed before acknowledgement")
                    if ((reserved and current["state"] == RESERVED)
                                 or (prepared and current["state"] == PREPARED)
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
        if self.config.ui and record.get("status") == "complete":
            self.next_key = "DOWN" if key == "UP" else "UP"
            self.next_target = not target_on
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
                            "response_kind": "real_ui" if config.ui else "diagnostic_marker",
                            "ui": {"first_key": config.input_key, "first_expected_orange_state": config.expected_orange_state,
                                   "left_region": config.response_region,
                                   "right_region": config.response_region_right or config.response_region,
                                   "orange_low_ppm": config.orange_low_ppm, "orange_high_ppm": config.orange_high_ppm,
                                   "alternate_only_after_complete": True} if config.ui else None,
                            "input_source_definitions": SOURCES,
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
    result.add_argument("--hold-ms", type=float, default=80.0, help="owned button/key hold duration (default: 80)")
    result.add_argument("--x", type=float, default=0.98, help="normalized benign client x coordinate (default: 0.98)")
    result.add_argument("--y", type=float, default=0.97, help="normalized benign client y coordinate (default: 0.97)")
    result.add_argument("--input-key", type=str.upper, choices=KEYS, help="opt into real UI mode; first UP/DOWN pulse, then alternate on success")
    result.add_argument("--response-region", type=parse_roi, metavar="X0,Y0,X1,Y1", help="normalized final-eye UI rectangle; required in UI mode")
    result.add_argument("--response-region-right", type=parse_roi, metavar="X0,Y0,X1,Y1", help="right-eye rectangle (default: same as --response-region)")
    result.add_argument("--expected-orange-state", choices=("on", "off"), help="first pulse's expected orange state; alternates on success")
    result.add_argument("--orange-low-ppm", type=int, help="maximum orange pixels per million for an off/gray state; required in UI mode")
    result.add_argument("--orange-high-ppm", type=int, help="minimum orange pixels per million for an on state; required in UI mode")
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

    def __init__(self, config, behavior="complete", source=1):
        self.config, self.behavior = config, behavior
        self.source = source
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

    def released(self, key=None):
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
                          "submitCompleteQpc": sent + 3000, "source": self.source}
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


class FakeUIBackend(FakeBackend):
    """Publish a paired baseline and selected UI response without live resources."""

    def __init__(self, config, behavior="complete"):
        super().__init__(config, behavior, source=4)
        self.baseline_sequence = -5
        self.terminal_reads = 0
        self.words[INDEX["ready"]] = 0  # UI preparation cannot depend on mouse packets.

    def compare_state(self, expected, desired):
        old = super().compare_state(expected, desired)
        if old == RESERVED and desired == PREPARING and self.behavior != "preparation_timeout":
            self.baseline_sequence += 10
            target = bool(self.words[INDEX["targetOn"]])
            baseline = 0 if target else 1000000
            if self.behavior == "bad_baseline":
                baseline = 1000000 if target else 0
            for name, value in {"state": PREPARED, "ready": 1, "renderSequence": self.baseline_sequence,
                                "baselineLeftPpm": baseline, "baselineRightPpm": baseline}.items():
                self.words[INDEX[name]] = value
            if self.behavior == "preparation_rejected":
                self.words[INDEX["state"]], self.words[INDEX["reason"]] = REJECTED, 12
            if self.behavior == "parameter_change_prepared":
                self.words[INDEX["roiLeft"]] += 1
        return old

    def key(self, key, pressed):
        self.events.append(("key", key, pressed))
        super().button(pressed)
        if pressed and self.words[INDEX["state"]] == COMPLETE:
            response = 1000000 if self.words[INDEX["targetOn"]] else 0
            values = {"renderSequence": self.baseline_sequence + 3, "responseLeftPpm": response,
                      "responseRightPpm": response, "consumedTickToken": 55}
            if self.behavior == "bad_response":
                values["responseRightPpm"] = 1000000 - response
            if self.behavior == "missing_tick":
                values["consumedTickToken"] = 0
            if self.behavior == "wrong_source":
                values["source"] = 3
            if self.behavior == "old_sequence":
                values["renderSequence"] = self.baseline_sequence
            if self.behavior == "parameter_change_complete":
                values["roiRight"] = self.words[INDEX["roiRight"]] + 1
            if self.behavior == "baseline_change_complete":
                values["baselineLeftPpm"] = 1000000 - self.words[INDEX["baselineLeftPpm"]]
            for name, value in values.items():
                self.words[INDEX[name]] = value

    def released(self, key=None):
        if self.behavior == "key_held_prepared" and self.words[INDEX["state"]] == PREPARED:
            return False
        return super().released(key)

    def snapshot(self):
        if self.words[INDEX["state"]] == COMPLETE:
            self.terminal_reads += 1
            if self.behavior == "terminal_change" and self.terminal_reads == 2:
                self.words[INDEX["responseRightPpm"]] += 1
        return super().snapshot()


def self_test():
    class Tests(unittest.TestCase):
        def config(self, output=Path("unused.jsonl"), **kwargs):
            return Config(pid=123, creation_time=456, session=2, image=r"C:\Game\EliteDangerous64.exe",
                          hwnd=789, mode="on", output=output, **kwargs)

        def sample(self, behavior="complete"):
            config = self.config()
            backend = FakeBackend(config, behavior)
            return Collector(config, backend).sample(1), backend

        def ui_config(self, **updates):
            values = dict(input_key="UP", response_region=(0.1, 0.2, 0.3, 0.4),
                          expected_orange_state="off", orange_low_ppm=10000, orange_high_ppm=200000)
            values.update(updates)
            return self.config(**values)

        def test_ui_configuration_is_explicit_and_regions_are_checked(self):
            config = self.ui_config()
            config.validate()
            self.assertTrue(config.ui)
            self.assertNotEqual(pack_roi(config.response_region), 0)
            for updates in ({"input_key": None}, {"response_region": None},
                            {"expected_orange_state": None}, {"orange_low_ppm": None},
                            {"orange_high_ppm": 10000}, {"response_region": (0.3, 0.2, 0.1, 0.4)},
                            {"response_region": (0.1, 0.2, float("nan"), 0.4)}):
                with self.subTest(updates=updates), self.assertRaises(ValueError):
                    self.ui_config(**updates).validate()

        def test_ui_prepared_proof_and_success_only_alternation(self):
            config = self.ui_config()
            backend = FakeUIBackend(config)
            collector = Collector(config, backend)
            first = collector.sample(1)
            self.assertEqual(first["status"], "complete")
            self.assertEqual(first["input_key"], "UP")
            self.assertEqual(first["source"], 4)
            self.assertGreater(first["snapshot"]["consumedTickToken"], 0)
            self.assertGreater(first["snapshot"]["renderSequence"], first["baseline"]["render_sequence"])
            self.assertNotIn("position", backend.events)
            self.assertLess(backend.events.index(("cas", RESERVED, PREPARING)),
                            backend.events.index(("cas", PREPARED, ARMED)))
            second = collector.sample(2)
            self.assertEqual(second["input_key"], "DOWN")
            self.assertTrue(second["expected_orange_state"])
            backend.behavior = "timeout"
            failed = collector.sample(3)
            self.assertEqual(failed["status"], "error")
            self.assertEqual(collector.next_key, "UP")
            self.assertEqual(backend.events.count(("key", "UP", True)), 2)
            self.assertEqual(backend.events.count(("key", "UP", False)), 2)

        def test_ui_invalid_pixel_receipt_or_request_proof_never_gets_metrics(self):
            for behavior in ("bad_baseline", "bad_response", "missing_tick", "wrong_source", "old_sequence",
                             "parameter_change_prepared", "parameter_change_complete", "baseline_change_complete", "terminal_change"):
                with self.subTest(behavior=behavior):
                    config = self.ui_config()
                    backend = FakeUIBackend(config, behavior)
                    collector = Collector(config, backend)
                    record = collector.sample(1)
                    self.assertEqual(record["status"], "error")
                    self.assertNotIn("metrics", record)
                    self.assertEqual(collector.next_key, "UP")
                    self.assertFalse(collector.next_target)
                    self.assertFalse(backend.down)
                    if behavior in ("bad_baseline", "parameter_change_prepared"):
                        self.assertNotIn(("key", "UP", True), backend.events)
                    if behavior == "parameter_change_prepared":
                        self.assertEqual(backend.words[INDEX["state"]], PREPARED)
                    if behavior in ("parameter_change_complete", "terminal_change"):
                        self.assertEqual(backend.words[INDEX["state"]], COMPLETE)

        def test_ui_preparation_timeout_rejection_and_owned_prepared_cancel(self):
            for behavior, status, state in (("preparation_timeout", "error", PREPARING),
                                            ("preparation_rejected", "rejected", IDLE),
                                            ("key_held_prepared", "error", IDLE)):
                with self.subTest(behavior=behavior):
                    config = self.ui_config()
                    backend = FakeUIBackend(config, behavior)
                    record = Collector(config, backend).sample(1)
                    self.assertEqual(record["status"], status)
                    self.assertEqual(backend.words[INDEX["state"]], state)
                    self.assertNotIn(("key", "UP", True), backend.events)
                    self.assertNotIn(("key", "UP", False), backend.events)
                    self.assertNotIn("metrics", record)
                    if behavior == "preparation_timeout":
                        self.assertLess(backend.now(), 4.1)

        def test_ui_owned_key_cleanup_for_identity_ambiguous_delivery_and_arm_race(self):
            for behavior, releases in (("identity_after_down", 1), ("send_ambiguous", 1),
                                       ("send_refused", 0), ("arm_race", 0)):
                with self.subTest(behavior=behavior):
                    config = self.ui_config()
                    backend = FakeUIBackend(config, behavior)
                    record = Collector(config, backend).sample(1)
                    self.assertEqual(record["status"], "error")
                    self.assertEqual(backend.events.count(("key", "UP", False)), releases)
                    self.assertFalse(backend.down)
                    self.assertNotIn("position", backend.events)
                    self.assertNotIn("metrics", record)

        def test_protocol_v1_mouse_compatibility_and_ui_refusal(self):
            for ui in (False, True):
                config = self.ui_config() if ui else self.config()
                backend = FakeUIBackend(config) if ui else FakeBackend(config)
                backend.words[INDEX["version"]] = 1
                record = Collector(config, backend).sample(1)
                self.assertEqual(record["status"], "error" if ui else "complete")
                if ui:
                    self.assertNotIn("down", backend.events)
                else:
                    self.assertFalse(any(isinstance(event, tuple) and event[0] in REQUEST_FIELDS for event in backend.events))

        def test_ui_main_dry_run_and_jsonl_use_explicit_regions_and_source(self):
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "ui.jsonl"
                arguments = ["--pid", "123", "--creation-time", "456", "--session", "2", "--image",
                             r"C:\Game\EliteDangerous64.exe", "--hwnd", "789", "--mode", "on", "--output", str(output),
                             "--input-key", "UP", "--response-region", "0.1,0.2,0.3,0.4",
                             "--response-region-right", "0.2,0.2,0.4,0.4", "--expected-orange-state", "off",
                             "--orange-low-ppm", "10000", "--orange-high-ppm", "200000", "--count", "2"]
                with redirect_stdout(io.StringIO()), \
                        mock.patch.object(Path, "open", side_effect=AssertionError("dry-run opened a file")), \
                        mock.patch(__name__ + ".load_atomic_helper", side_effect=AssertionError("dry-run loaded a DLL")):
                    self.assertEqual(main(arguments + ["--dry-run"], lambda _: self.fail("dry-run opened Windows resources")), 0)
                self.assertEqual(list(Path(folder).iterdir()), [])
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(main(arguments, FakeUIBackend), 0)
                rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
                self.assertEqual([row["kind"] for row in rows], ["metadata", "sample", "sample", "summary"])
                self.assertEqual(rows[0]["response_kind"], "real_ui")
                self.assertEqual([row["input_key"] for row in rows[1:3]], ["UP", "DOWN"])
                self.assertEqual([row["source"] for row in rows[1:3]], [4, 4])
                self.assertNotEqual(rows[1]["snapshot"]["roiLeft"], rows[1]["snapshot"]["roiRight"])
                self.assertEqual(rows[-1]["complete"], 2)

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

        def test_game_input_api_sources_accept_message_receipt(self):
            for source in (1, 2, 3):
                with self.subTest(source=source):
                    config = self.config()
                    backend = FakeBackend(config, source=source)
                    record = Collector(config, backend).sample(1)
                    self.assertEqual(record["status"], "complete")
                    self.assertEqual(record["source"], source)
                    self.assertEqual(record["source_label"], SOURCES[source])
                    self.assertEqual(record["snapshot"]["source"], source)
                    self.assertEqual(record["metrics"], dict(zip(METRICS, (1.0, 3.0, 4.0, 3.0))))

        def test_unknown_input_api_source_never_gets_metrics(self):
            for source in (0, 4, UINT64_MAX):
                with self.subTest(source=source):
                    config = self.config()
                    backend = FakeBackend(config, source=source)
                    record = Collector(config, backend).sample(1)
                    self.assertEqual(record["status"], "error")
                    self.assertNotIn("metrics", record)
                    self.assertNotIn("source_label", record)
                    self.assertEqual(record["snapshot"]["source"], source)
                    self.assertEqual(backend.words[INDEX["state"]], IDLE)

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

        def test_win32_key_edges_use_only_extended_up_down_scan_codes(self):
            backend = WindowsBackend.__new__(WindowsBackend)
            backend.config = self.ui_config()
            with mock.patch.object(ctypes, "WinDLL", mock.Mock(), create=True), \
                    mock.patch(__name__ + ".load_atomic_helper"):
                backend._bind()
            edges = []

            def capture(count, pointer, size):
                event = pointer._obj
                edges.append((event.type, event.ki.wVk, event.ki.wScan, event.ki.dwFlags))
                self.assertEqual(count, 1)
                self.assertEqual(size, ctypes.sizeof(backend.Input))
                return 1

            backend.user.SendInput.side_effect = capture
            for key in ("UP", "DOWN"):
                backend.key(key, True)
                backend.key(key, False)
            self.assertEqual(edges, [(1, 0, 0x48, 9), (1, 0, 0x48, 11), (1, 0, 0x50, 9), (1, 0, 0x50, 11)])
            with self.assertRaises(ProbeError):
                backend.key("ENTER", True)
            self.assertEqual(len(edges), 4)

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
