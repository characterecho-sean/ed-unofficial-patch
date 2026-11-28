# Linux/Proton launcher headset detection (2026-11-28)

## Status

There is enough evidence for a targeted launcher update. Match native
OpenXR manifest precedence, read the runtime container's environment hints,
expire verification when the selected runtime changes, and explain why a
live probe could not verify a headset. The launcher can do this in the
existing game prefix. One GE-Proton11-14 / Steam Linux Runtime 4 setup with
WiVRn 26.9 and a Quest 3 reached **Ready** through the signed loader.
Flatpak Steam, Protontricks, Epic and other runtimes still need route-specific
checks. Launcher validation can run with Elite closed.

## What the local observation establishes

On one existing Elite prefix with WiVRn installed as a Flatpak, the
standalone launcher lacked the native import despite a valid
`wineopenxr64.json`. A Win32 environment probe saw `HOME`,
`XDG_CONFIG_DIRS`, `XDG_RUNTIME_DIR` and `XR_RUNTIME_JSON` unset and
a DOS-form `WINEHOMEDIR`. Native `XDG_CONFIG_DIRS` began with
`/usr/lib/pressure-vessel/overrides/etc/xdg`. Win32 could read the generic
`openxr/1/active_runtime.json` there after import.

In the earlier launcher run, the wrapper import default of `1` plus the
manifest fallback yielded Ready, with `xrGetSystem` result `0` and no extra
writable mount. The combined test covers a WiVRn Flatpak runtime. Flatpak
Steam needs its own test.

A new Win32 environment-only test ran in the existing prefix through the
same wrapper with the import flag set to `0` and `1`. The imported manifest
was visible only with `1`, and `WINE_HOST_XDG_CONFIG_DIRS` then included the
import directory. Both runs exposed `WINE_HOST_HOME` and
`WINE_HOST_XDG_RUNTIME_DIR`. The corresponding plain names remained unset.
This isolates the effect of the import flag on manifest visibility. The
remaining comparison is how the launcher handles those inputs with each
discovery path. Research used the `saved-profiles` worktree at
`e424d9dfb1db155b19ba9bee6131be1e447e5775` with its existing uncommitted
changes.
[`tools/install_proton.py`](../tools/install_proton.py) · [`src/installer/vr_headset.cpp`](../src/installer/vr_headset.cpp).

## What the loaders actually select

- **Windows side.** The pinned x64 loader (1.1.46) takes a secure Win32
  `XR_RUNTIME_JSON` override first, otherwise `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime`
  from the process architecture's registry view. ED-UP's `edup_openxr.ini`
  `system` mode clears the Windows override. Explicit mode respects an
  existing Win32 override. The launcher's HKCU fallback differs from the
  loader. Proton copies the bridge JSON during prefix setup. The JSON names
  `wineopenxr.dll`, whose Unix side links the native loader. `runinprefix`
  skips the normal prefix setup path, so verify the existing registration
  and bridge.
  [ED-UP mode](../src/openxr/native_local_config.h#L92) · [Loader pin](../tools/fetch_openxr_loader.py#L18) · [Khronos 1.1.46 implementation](https://github.com/KhronosGroup/OpenXR-SDK/blob/release-1.1.46/src/loader/manifest_file.cpp#L365-L411) · [Proton prefix code](https://github.com/ValveSoftware/Proton/blob/proton_11.0/proton) · [Bridge manifest](https://github.com/ValveSoftware/Proton/blob/proton_11.0/wineopenxr/wineopenxr64.json) · [Bridge build](https://github.com/ValveSoftware/Proton/blob/proton_11.0/Makefile.in#L440-L457).
- **Unix side.** The native loader first uses its own secure
  `XR_RUNTIME_JSON`, then `XDG_CONFIG_HOME` (or `HOME/.config`), ordered
  `XDG_CONFIG_DIRS` (default `/etc/xdg`), then compiled `SYSCONFDIR` and
  optional `EXTRASYSCONFDIR`. In **each** directory it tries
  `openxr/1/active_runtime.<abi>.json` (e.g. `x86_64`) before the generic
  filename. It selects the first existing candidate before parsing it.
  A higher-priority generic file beats a lower-priority ABI-specific one.
  The ABI belongs to the native loader behind the bridge. The candidate in
  this worktree checks generic filenames only and omits the compiled system
  directories. [Khronos loader design](https://registry.khronos.org/OpenXR/specs/1.1/loader.html) · [Exact 1.1.46 search](https://github.com/KhronosGroup/OpenXR-SDK/blob/release-1.1.46/src/loader/manifest_file.cpp#L265-L353).
- **Environment and layers.** Wine builds the Windows process environment
  separately. Valve's Wine 11 branch treats `HOME` and `XDG_*` specially
  (normally `WINE_HOST_`-prefixed, with Wine overrides) and filters native
  `XR_*` on initial import. `WINEHOMEDIR` is a mapped path, useful as a
  home hint after validation. Measure the actual runner before depending on
  those names. The probe's in-process removal of a POSIX value from the
  Win32 PEB allows the registered bridge to be selected while leaving native
  environment selection to its loader. Keep environment actions scoped to
  the child and runtime mode. API layers have separate manifests and
  loader discovery, including Windows layer registry keys and the explicit
  `XR_API_LAYER_PATH` search override. Pressure Vessel imports runtime and
  layer manifests separately. Neither a Windows layer nor a POSIX JSON set
  as a Win32 override supplies the native runtime bridge.
  [Wine environment construction](https://github.com/ValveSoftware/wine/blob/proton_11.0/dlls/ntdll/unix/env.c) · [Wine PEB environment](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/env.c) · [ED-UP probe helper](../src/installer/vr_headset.cpp#L1290) · [Khronos layer source](https://github.com/KhronosGroup/OpenXR-SDK/blob/release-1.1.46/src/loader/manifest_file.cpp#L950-L1004) · [Valve import flags](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/main/pressure-vessel/wrap.c#L475).

## Implementation bounds and route coverage

- **Steam wrapper.** Default the import only when running through the
  resolved Pressure Vessel entry point, preserve explicit values including
  `0`, and pass the setting to the selected Steam Flatpak sandbox when
  applicable. The standalone wrapper has its own invocation of the runtime.
  Elite's Steam launch options are a separate configuration. Valve rewrites
  manifests for the container, gives the primary architecture the generic
  filename, and prepends its imported XDG directory when an import exists.
  Use the imported view for classification. [Steam plan](../tools/install_proton.py#L829) · [Valve runtime import](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/main/pressure-vessel/runtime.c#L8497) · [Valve naming](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/main/pressure-vessel/runtime.c#L5174) · [WiVRn Steam notes](https://github.com/WiVRn/WiVRn/blob/master/docs/steamvr.md).
- **Manifest hints.** Prefer the runtime container's environment. Use the
  observed `WINE_HOST_*` names as read-only hints and verify runner variants
  before adding an environment handoff. If the config-dir list is missing,
  use the known imported directory as a bounded hint, followed by known
  system directories. Keep per-directory ABI ordering. Reject relative
  and ambiguous base paths, preserve case and spaces, and validate any DOS
  home hint as fully qualified. Map absolute POSIX paths to the prefix's
  visible filesystem. A missing or restricted Wine drive mapping leaves
  the native path unverified. Keep reads bounded
  (the current text read cap is 4 MiB). Preserve the distinction between
  absent, unreadable and malformed higher-priority candidates. Relative
  library paths are interpreted by the real loader, and the launcher can
  treat a Linux library as unverified if its Win32 view differs. Discovery
  should read metadata only, with no manifest copying, registry edits,
  recursive search or loading a DLL from the hint. [Candidate and read code](../src/installer/vr_headset.cpp#L662) · [XDG absolute path rule](https://specifications.freedesktop.org/basedir/latest/#variables) · [Khronos runtime manifest design](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/main/specification/loader/runtime.adoc).
- **Flatpak Steam and Protontricks.** Flatpak Steam needs the relevant
  manifest, deployment and IPC path visible before the inner import.
  Valve's import exposes known sockets such as `wivrn/comp_ipc` and
  `monado_comp_ipc`. WiVRn documents scoped read-only grants for Steam.
  Valve treats Flatpak Steam as unsupported for SteamVR itself because of
  DRM leasing and asynchronous reprojection.
  Keep grants user-controlled and test precedence of saved Flatpak
  environment overrides. Protontricks selects a Steam app prefix for an
  external EXE. Verify its launch environment and Flatpak permissions
  separately. [Valve socket import](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/raw/main/pressure-vessel/wrap-openxr.c) · [Flatpak sandbox documentation](https://docs.flatpak.org/en/latest/sandbox-permissions.html) · [WiVRn Flatpak setup](https://github.com/WiVRn/WiVRn#steam-flatpak) · [Valve Linux VR requirements](https://help.steampowered.com/en/faqs/view/18A4-1E10-8A94-3DDA) · [Protontricks launch and permissions](https://github.com/Matoking/protontricks#usage).
- **Epic.** Preserve the selected existing prefix and runner. The wrapper
  has separate Wine, direct Proton `runinprefix`, and installed UMU routes,
  including recorded manager Flatpak contexts. The Steam import default is
  outside these branches. For UMU, verify the selected runtime actually
  consumes the flag and the outer manager forwards it before adding any
  default. Plain Wine and direct Proton need an independently proven bridge,
  native loader and accessible runtime. Epic game sign-in belongs to its
  manager. [Epic wrapper plan](../tools/install_proton.py#L721) · [UMU environment and invocation](https://github.com/Open-Wine-Components/umu-launcher/blob/main/umu/umu_run.py) · [Packaged Linux instructions](../release/README-launcher-linux.txt).

## Live checks and acceptance

- Keep process names, IPC path existence, USB IDs, manifest names and
  OpenVR driver entries as hints. The current Win32 snapshot can complete
  while the bounded `Z:\proc` scan sees only a subset of host processes.
  A missing process is ambiguous across sandboxes, and an existing socket
  can be stale or inaccessible. Tying `shouldRunLiveOpenXrProbe` to a known
  classification and visible service can therefore suppress a valid query.
  Missing liveness should remain unverified. Any additional one-shot probe
  path needs route-specific proof of safe liveness and Elite exclusion.
  Retain the SteamVR start/hang guard. [Launcher rules](../src/installer/vr_headset.cpp#L934) · [PID visibility](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html) · [Valve Flatpak subsandbox PID sharing](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/main/pressure-vessel/wrap.c#L562) · [Unix socket semantics](https://man7.org/linux/man-pages/man7/unix.7.html) · [Proton SteamVR guard](https://github.com/ValveSoftware/Proton/blob/proton_11.0/wineopenxr/openxr_loader.c#L78-L84).
- Keep the signed release-manifest and exact loader size/hash checks
  in the helper, scoped DLL search, 2.5-second timeout/cancellation, and
  no live query while elevated, actively playing, or with Elite seen in
  the snapshot. A trusted loader still invokes the selected runtime and
  its layers. A sandbox-local snapshot cannot establish host-wide absence.
  Keep periodic refresh static. Key cached live evidence to the install
  generation and effective native manifest as well as the Windows path
  (the bridge and generic manifest paths can stay constant across runtime
  switches). A synthetic reproduction using the compiled launcher code
  gave the refresh merge a fresh Monado Standby report and a previous
  WiVRn Ready report through the same bridge. The merge returned Ready
  with the previous WiVRn headset. Include the resolved runtime target or
  manifest content in the identity, and age verification when connection
  changes may be invisible, such as when the server stays running after
  a headset disconnects. [Signed loader and helper](../src/installer/vr_headset.cpp#L1233) · [Static merge](../src/installer/vr_headset.cpp#L1596) · [UI scheduling and timeout](../src/installer/launcher_gui.inc#L176).
- Report what the probe established. Distinguish a missing Windows bridge,
  a native runtime the launcher could not see, a skipped probe, a timeout,
  a loader or runtime error, and a successful instance that reported the
  HMD form factor unavailable (`XR_ERROR_FORM_FACTOR_UNAVAILABLE`). Show
  **Runtime unverified** when a probe lacks sufficient evidence, with the
  reason where known. A runtime can still be available when its service
  or socket is hidden from the launcher. [Status resolution](../src/installer/vr_headset.cpp#L1405).
- For a fresh Ready result, the real loader created an instance and
  reported a non-null HMD system ID from `xrGetSystem`. Runtime/system
  properties can supply names. The device's driver, streamer and runtime
  must support the combination. SteamVR can be the runtime with ALVR as
  transport, and WiVRn or Monado can be the native runtime behind Proton's
  bridge. Separate history documents an Elite session that found a WiVRn
  HMD but produced zero VR frame submissions. [Khronos xrGetSystem](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetSystem.html) · [WiVRn device list](https://github.com/WiVRn/WiVRn#about) · [Monado device support](https://monado.freedesktop.org/#supported-hardware) · [ALVR requirements](https://github.com/alvr-org/ALVR#requirements) · [Prior ED-UP flight](linux-native-openxr-launch-centre-2026-09-22.md).

### Evidence to collect, in order

1. Exercise the launcher's discovery using `WINE_HOST_*` and the fallback
   with import on and off on the known setup with Elite closed. Record
   runner and runtime builds, package types, prefix identity, sanitized
   native and Win32 environment, selected ABI/manifest, IPC and process visibility, signature/hash
   verification, probe result and card state. Repeat with headset
   disconnected, reconnected, and server stopped.
2. Extend the existing wrapper and launcher rigs for per-directory order,
   relative paths, missing/corrupt manifests, host-only versus registered
   bridge, same bridge with changed native runtime, stale results, signed
   payload rejection, blocked snapshots/elevation/running Elite and
   timeout. Demonstrate failure without the fix, then use Native Windows
   CI for C++ validation. [Wrapper self-tests](../tools/install_proton.py#L2261) · [Launcher probe tests](../tools/launcher_test/launcher_test.cpp#L7197) · [Build workflow](../.github/workflows/release.yml).
3. Repeat a launcher-only positive and negative check with native Steam
   and SteamVR first, then Flatpak Steam with WiVRn before and after the
   documented grants. Next check Protontricks and an existing Epic/UMU
   prefix, followed by Monado and SteamVR with ALVR as hardware permits.
   Publish coverage only for tested tuples. A later game flight needs
   build-matched evidence of graphics session, tracking and frame
   submission before claiming in-game compatibility.
