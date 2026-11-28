# Questions

Common questions, setup answers, and confirmed fixes for ED-UP (formerly EDVR). Each section links to the detailed guide page when there is more to read.

- [General and safety](#general-and-safety)
- [Downloading, installing, and updating](#downloading-installing-and-updating)
- [Launching Elite, stores, and accounts](#launching-elite-stores-and-accounts)
- [The F8 menu, hotkeys, and settings files](#the-f8-menu-hotkeys-and-settings-files)
- [Anti-aliasing, upscaling, and resolution](#anti-aliasing-upscaling-and-resolution)
- [VR runtimes, headsets, and launching in VR](#vr-runtimes-headsets-and-launching-in-vr)
- [Performance, stuttering, VRAM, and crashes](#performance-stuttering-vram-and-crashes)
- [On foot in VR and visual quirks](#on-foot-in-vr-and-visual-quirks)
- [Flat screen, XeSS Frame Generation, AMD FSR 4, and Linux](#flat-screen-xess-frame-generation-amd-fsr-4-and-linux)
- [Other mods and overlays](#other-mods-and-overlays)

---

## General and safety

### Does it cost anything?

No. It stays free. A tip at [Ko-fi](https://ko-fi.com/seancharacterecho) is welcome and is never required.

### Can it affect my account?

ED-UP leaves your account, ships, cargo, credits, ARX/paid assets, and missions alone. The patch sends nothing of its own. While the game is loading, it keeps the game's own download connections open so later files skip a new handshake. The optional launcher asks GitHub about updates and, if you turn direct launch on, signs in to Frontier the way the game's own launcher does. It leaves anti-cheat untouched, and it runs under its own name. Frontier has not endorsed it. Frontier's rules are Frontier's. [What it changes](What-it-changes.md) is the concrete list.

### Does it work with Horizons?

No. The launcher accepts **Odyssey** only and stops when it finds the older Horizons client.

### Can I play on a monitor only?

Yes. Choose **Flat only**, or choose **VR and flat** and launch Elite on the monitor. [Install on Windows](Install-on-Windows.md).

### Does it run on a Steam Deck?

Yes. Switch to Desktop Mode and use the Linux launcher. Try **DittoSR** on the F8 Performance page with native display resolution and Supersampling at `0.65` to `0.75`. [Install on Linux](Install-on-Linux.md).

---

## Downloading, installing, and updating

### Which file do I download? Where did `edvr-installer.exe` go?

- **Windows:** Download **only** `ed-up-launcher.exe` from the [download page](https://characterecho-sean.github.io/ed-unofficial-patch/) or the [latest release](https://github.com/characterecho-sean/ed-unofficial-patch/releases/latest). Ignore the component `.zip` files (`ed-up-core-*.zip`, `ed-up-dlss-*.zip`, `ed-up-fsr4-*.zip`, `ed-up-xess-*.zip`), `.json`, and `.sig` files — the launcher fetches and verifies only the packages your machine needs and self-updates automatically.
- **Linux and Steam Deck:** Download `ed-up-launcher-linux-<version>.zip`, unzip it, and run `./ed-up-launcher.sh` as your normal user (without `sudo`). Launch Odyssey once beforehand so the Wine/Proton prefix exists.
- **VR and flat are one install:** Pick **VR and flat** unless you only play one mode. There are no longer separate VR and flat downloads.

### Windows Defender, Chrome, or SmartScreen flagged it as a virus

A graphics DLL sitting beside a game often triggers antivirus heuristics.

- Releases from **v0.22.3** onward are Authenticode-signed by **CharacterEcho LLC**. If SmartScreen still warns, click **More info**, then **Run anyway** (you can verify the file against `SHA256SUMS` on the release page).
- If Defender silently quarantined `d3d11.dll` (the launcher says installed, but F8 does nothing), open **Windows Security -> Virus & threat protection -> Protection history**, allow the file, add an exclusion if needed, and press **Repair** in the ED-UP launcher.

### The launcher says it is running with administrator rights when I started it normally

Do not tick "Run as administrator" on `ed-up-launcher.exe`. If the message still appears when launched normally:

1. **Windows UAC is set to "Never notify":** On an Administrator account, turning UAC all the way off makes Windows silently launch every app elevated. Raise the UAC slider at least one step above "Never notify" (and update to **v0.23.0+**, which also improves handling when UAC is disabled).
2. **Elite is inside `C:\Program Files (x86)\`:** Windows requires elevation to write there. Moving your Steam library to another folder or drive avoids permission prompts.

### The Install button is disabled, or the update download failed

- **Elite is still running:** Close the game and check **Task Manager** (`Ctrl+Shift+Esc`) to make sure `EliteDangerous64.exe` is gone. If the Install button stays disabled afterwards, restart `ed-up-launcher.exe` — downloaded files remain cached under `%LOCALAPPDATA%\EDUP\launcher`.
- **Download failed:** Temporary GitHub CDN errors happen occasionally. Retry directly in the launcher rather than using a third-party download manager.

### How do I update, and what happens when Frontier updates Elite?

- **Updating ED-UP:** Open the launcher and press **Download & update** (no need to uninstall first; your settings are kept).
- **When Frontier updates Elite:** Elite still starts. Any fix that no longer matches the new game build turns itself off and logs it; Native On-Foot VR waits for an ED-UP release matched to the new build.
- **After a Frontier or Epic file verify:** Verifying game files can delete `d3d11.dll`. Open the ED-UP launcher and click **Repair**. Your settings stay safe under `%LOCALAPPDATA%\EDUP`. [Updates and uninstall](Updates-and-uninstall.md).

### How do I remove it?

Open the launcher and press **Uninstall**. That removes ED-UP's files, restores Elite's original `openvr_api.dll`, and restores any chained mod such as EDHM. Settings stay by default; tick **Clean purge on uninstall** (or run `ed-up-launcher.exe --uninstall --purge`) to also delete settings, logs, backups, and the settings mirror.

Do **not** manually delete Elite's `Openvr` folder — that folder belongs to the game. Taking out the launcher alone leaves the patch installed. [Updates and uninstall](Updates-and-uninstall.md).

---

## Launching Elite, stores, and accounts

### Do I have to launch Elite through the ED-UP launcher?

No. Once ED-UP is installed into the Odyssey folder, Elite loads `d3d11.dll` automatically. You can keep starting Elite from Steam, Epic, Frontier's launcher, or `min-ed-launcher`. [The launcher](The-launcher.md).

### I own it on Steam and on Epic

On Windows the launcher lists each copy; pick the one you want. On Linux, when both a Steam and an Epic copy are installed, name the store with `--store steam` or `--store epic`. [Epic on Linux](Install-on-Linux.md#epic-on-linux).

### Epic Games Store: F8 does nothing, or Play does not start Elite

1. Close Elite and end any leftover `EliteDangerous64.exe` in Task Manager.
2. Open the **Epic Games Launcher**, sign in, and **leave it open**.
3. Open ED-UP normally (not as administrator) and confirm it selected your **Epic** `Products\elite-dangerous-odyssey-64` folder.
4. Click **Repair** if Epic recently verified files, with **VR and flat** selected.
5. Start Elite from the Epic window first and press **F8** to confirm ED-UP loaded. Once confirmed, ED-UP's **Play** button works while Epic stays open.

### Direct launch on Steam logs into my Steam save instead of my Frontier account

Tick **Use Frontier account login** under Launch in the ED-UP launcher (or pass `--frontier-login`). [The launcher](The-launcher.md#starting-through-steam-past-the-steam-window).

### How do I use ED-UP with Steam Launch Options or `min-ed-launcher`?

- **Steam Launch Options** (keeps Steam Overlay and Steam Input active; replace `<your-name>` with your Windows username, because Steam does not expand `%LOCALAPPDATA%`):

```text
"C:\Users\<your-name>\AppData\Local\EDUP\launcher\ed-up-launcher.exe" --headless %command%
```

- **`min-ed-launcher` pre-launch update hook** (updates and installs without starting the game through ED-UP):

```text
"%LOCALAPPDATA%\EDUP\launcher\ed-up-launcher.exe" --update --install --dir "C:\Path\To\Products\elite-dangerous-odyssey-64"
```

### The launcher remembered my Frontier password. Where is it?

In `frontier-login.dat`, under `%LOCALAPPDATA%\EDUP\launcher\`, and only while remember is ticked. Untick the box, or remove the launcher, and that file is deleted. [The launcher](The-launcher.md).

---

## The F8 menu, hotkeys, and settings files

### Where do my settings live? Why did editing an `.ini` file do nothing?

- Settings live outside the game folder so game updates cannot wipe them:
  - **Windows:** `%LOCALAPPDATA%\EDUP\<game-folder>-<store>\` (`edup.ini` for VR, `edup-flat.ini` for flat).
  - **Linux (Steam Proton):** `steamapps/compatdata/359320/pfx/drive_c/users/steamuser/AppData/Local/EDUP/elite-dangerous-odyssey-64-steam/`.
  - Settings from older releases (`edvr.ini` and `edvr-flat.ini` beside `EliteDangerous64.exe`) are copied on first save.
- **VR and flat have separate files:** Monitor sessions read `edup-flat.ini` first and only fall back to `edup.ini` when `edup-flat.ini` does not exist. Editing `edup.ini` has no effect in flat mode once `edup-flat.ini` exists.
- Prefer changing settings through **F8** in-game or the launcher's **Settings** tab. [Settings](Settings.md).

### F8 does not open the menu

Check these in order:

1. **Antivirus quarantined `d3d11.dll`:** Look beside `EliteDangerous64.exe` for `d3d11.dll` and an `edup_logs` (or `edvr_logs`) folder. If missing, allow in antivirus and click **Repair**.
2. **EDHM or ReShade conflict:** If EDHM was installed or updated after ED-UP, click **Repair** in the ED-UP launcher so ED-UP chain-loads `d3d11_edhm.dll`. If **ReShade (`dxgi.dll`)** is present, remove or rename `dxgi.dll` to test.
3. **Headset unplugged while Elite is still in 3D mode:** If `Settings.xml` holds a VR mode (`3`..`6`) and the headset is disconnected, VR init fails and ED-UP stands down. Start with **Play flat** or `/novr`, or set `<StereoscopicMode>0</StereoscopicMode>` in `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml`.
4. **Laptop or compact keyboard:** Try `Fn + F8`.
5. **In VR:** The panel spawns where you were looking. Press **Shift+F8** to bring it in front of you.

### What are the main hotkeys, and how do I rebind them?

| Key | What it does |
| --- | --- |
| **F8** | Open or close the ED-UP menu (`Tab` / `Shift+Tab` switches pages; arrow keys and `Enter` change rows) |
| **Shift+F8** | Recenter the 3D menu panel in VR |
| **F7** | Live anti-aliasing On/Off comparison (does not write to disk) |
| **Pause** | Dump camera history right after a transition flash for bug reports |
| **NumLock** | Write a short 1-second shader/state report into the log when reporting shimmer or flicker |

Rebind **F8** from the launcher's **Settings** tab, the VR **Hotkeys** tab, or `edup.ini` / `edup-flat.ini`. For tenkeyless keyboards without `NumLock`, rebind `dump_draws` in `edup-flat.ini`. [Settings](Settings.md).

---

## Anti-aliasing, upscaling, and resolution

### How do I turn on DLSS, FSR, DittoSR, or XeSS?

Anti-aliasing starts **Off** by default so you can verify a clean install first.

1. In **Elite Dangerous -> Options -> Graphics**:
   - Set **Anti-Aliasing** to **Off** (do not stack Elite's FXAA or FSR on top of ED-UP).
   - Set **Upscaling** to **Normal** (turns off Elite's built-in AMD FSR 1.0).
2. Press **F8 -> Performance -> Anti-aliasing** and choose:
   - **DLSS** (NVIDIA RTX GPUs only — non-RTX machines do not receive `nvngx_dlss.dll`).
   - **FSR 4.1** (AMD RX 7000 / 9000 series).
   - **FSR 3.1** (any GPU).
   - **DittoSR** (lightweight upscaler for any D3D11 GPU; well suited to handhelds and mid-range GPUs).
   - **XeSS-SR** (any GPU; fastest on Intel Arc, and required on flat for XeSS Frame Generation).
   - **TAA** (`On`, ED-UP's built-in temporal AA).

Do not stack **NVIDIA DLDSR** or **Lossless Scaling (LSFG)** with ED-UP upscaling; choose one or the other.

### Where are DLSS Quality, Balanced, and Performance? I only see DLAA

ED-UP has no separate Quality/Performance dropdown; it uses Elite's own scale slider:

- **On a monitor (flat):** Keep **Display Resolution** at your monitor's native resolution and adjust **Supersampling**:
  - `1.0` runs at native resolution (**DLAA** — the F8 row labels DLSS as `DLAA` at `1.0`).
  - `0.85` gives near-native quality with modest GPU savings.
  - `0.75` is the recommended flat upscaling baseline.
  - `0.65` gives a stronger FPS boost (good for 4K on mid-range GPUs or 1080p handhelds/APUs).
  - `0.50` gives the largest GPU savings (for example, 1080p upscaled to 4K).
- **In VR:**
  - Leave Elite's **Supersampling** at **`1.0`** (always — below `1.0`, Elite scales the world before ED-UP sees it and shows a headset warning).
  - Lower **HMD Image Quality** (`0.5` to `0.75`; `0.5` is Elite's floor) to set the internal render size fed into DLSS or FSR.
  - Control the final per-eye output size in **one place** — either **F8 -> Performance -> OpenXR resolution** (saved per headset, requires restart) or your headset runtime's resolution slider — rather than stacking multipliers in both.

### Which DLSS preset should I use (K, L, or M)?

- **K** (default): Best balance of speed and clarity. Recommended baseline for VR and general play.
- **M**: Newer transformer model — slightly crisper edges and less shimmer on holographic lines, at a higher GPU cost. Popular on flat screens and on VR rigs with GPU headroom.
- **L**: Heaviest GPU cost (intended for very low render scales). Usually slower than `M` without improving normal scales.

### Why did the HUD get blurry after lowering HMD Quality? What should UI quality and HUD AA be set to?

- **Blurry HUD right after lowering HMD Quality:** Elite caches cockpit panel resolution on creation. Return to the main menu, disembark and re-enter your ship, or restart Elite after changing **HMD Image Quality** or **UI quality** so panels rebuild at the new size.
- **UI quality (`100%` default, `125%`, `150%`):** Keeps cockpit panels and menus crisp above the upscaler. On 8 to 12 GB GPUs, stay at **`100%`** (`150%` uses 2.25x the pixels and can cause VRAM pressure or Galaxy Map lag on lighter GPUs).
- **HUD Anti-Aliasing (`Off` default, `Light (FXAA)`, `Sharp (SMAA)`):** Smooths jagged edges on the HUD layer. On 4K monitors and high-resolution headsets, leaving it **Off** keeps small text sharpest and saves ~100 to 160 MB of VRAM per eye.
- **Sharpening:** `30%` (`0.3`) is a reasonable starting point; high values cause edge ringing.

### Why do planets smear in supercruise, and why did space dust fade?

- **Planet and moon smearing in supercruise:** Fast-moving limbs in supercruise can trail under any temporal anti-aliasing (DLAA, DLSS, FSR, TAA) because previous-frame history blends into the new frame. Press **F7** to toggle AA off and verify. No setting keeps DLAA on and removes that trail.
- **Faint space dust:** Temporal filters treat thin 1-pixel streaks as noise. If you run **EDHM**, increasing supercruise dust brightness and length in EDHM's settings brings it back.

---

## VR runtimes, headsets, and launching in VR

### Do I need OpenComposite or SteamVR?

- **Leave OpenComposite uninstalled.** ED-UP ships its own native OpenXR runtime (`openvr_api.dll` + `openxr_loader.dll`) and OpenComposite conflicts with it.
- **SteamVR is optional** for headsets with their own OpenXR runtime. Set Windows' active OpenXR runtime to the headset software you want before launching Elite:
  - **Quest 2/3 over Virtual Desktop:** Set OpenXR runtime to **VDXR** in the Virtual Desktop streamer (avoids SteamVR overhead and SteamVR launch issues).
  - **Meta Link / Air Link:** Set **Meta** as the active OpenXR runtime and connect the headset before launching.
  - **Pimax:** Set **Pimax OpenXR** as the active runtime (or **SteamVR** only if you specifically need SteamVR overlays like Desktop+; do not run Pimax OpenXR while manually starting SteamVR).
  - **Index, PSVR2, Bigscreen Beyond, Vive, Steam Link, Steam Frame:** Set **SteamVR** as Windows' active OpenXR runtime.

Start your headset software and wake/connect the headset **before** launching Elite. [Headsets](Headsets.md).

### I use a Quest, Air Link, Pimax, or PSVR2. Will it run?

The headset path is actively flown on Quest 3 (Virtual Desktop VDXR and Air Link) and Pimax (PiOpenXR), and is widely used on SteamVR headsets including Valve Index, Bigscreen Beyond, PSVR2 (PC adapter), and WMR/Oasis. Set that headset's software as Windows' active OpenXR runtime before launching. [Headsets](Headsets.md).

### Elite launches flat instead of VR, or 3D options only show Anaglyph and Side by Side

Check these causes in order:

1. **Headset not awake within 15 seconds:** ED-UP opens an OpenXR session and waits up to 15 seconds for the headset to report ready. If SteamVR is not open yet or the headset is asleep on your desk (common on PSVR2), startup times out and Elite falls back to flat. Start your runtime and wake/wear the headset before clicking Play.
2. **Wrong active OpenXR runtime:** Confirm Windows' active OpenXR runtime matches the headset software you are running. After a session, `edup_logs` should contain two logs; only one log means Elite bypassed ED-UP's VR runtime.
3. **Steam overriding `Settings.xml`:** In **Steam -> Elite Dangerous -> Properties -> General**, ensure **Selected Launch Option** is not locked to the desktop option (`/novr`). In the ED-UP launcher, pick your **VR 3D mode** (`HMD (Headphones)`) and consider ticking **Start Elite directly (no Frontier launcher)** so `/vr` is passed directly.
   - Do **not** set `Settings.xml` to read-only to work around this: `/novr` still overrides it in memory, and the launcher intentionally refuses to edit a read-only `Settings.xml` on non-direct launches.
4. **Crash guard tripped:** If Elite froze or was force-closed within the first few seconds of the previous launch, ED-UP disables hooks for one launch as a safety fallback, which opens flat. Launch once more and check the underlying crash.
5. **OpenKneeboard launch order:** Start OpenKneeboard **after** Elite has launched; starting it before can cause a black headset screen and a tiny desktop window.

### Headset-specific tips

- **PSVR2 (PC adapter):** SteamVR's 100% default is ~3400x3468 per eye, which overflows 12 GB GPUs quickly. Set SteamVR to **90 Hz**, lower custom/per-app resolution to **~68%** (~2800x2860 per eye), turn **Motion Smoothing Off**, and wake the headset in SteamVR before launching.
- **Quest 2/3 (Virtual Desktop):** Use **VDXR**. **H.264+** gives the lowest latency, **HEVC** is balanced, and **AV1** adds latency in VR. If you see a hitch every 10 seconds with low GPU/CPU load, disable the **Oculus Audio** device in Windows Sound Settings.
- **Pimax canted headsets (8K X, 5K, etc.):** Turn **Enable Parallel Projection** off in Pimax Play (with PimaxXR) to cut GPU time by roughly 30%. *(If a headset like Dream Air shows severe IPD/double vision, turn Parallel Projection on.)* Keep **Pimax Motion Smoothing** and **Pimax NIS** off.
- **SteamVR quirks:**
  - Disable **Steam Overlay** for Elite if opening it freezes the headset or causes a crash on exit.
  - To stop the SteamVR "Resume Game" dashboard popping up, set `enableDashboard` and `startDashboardFromAppLaunch` to `false` in `SteamVR/resources/settings/default.vrsettings`.
  - If SteamVR Beta breaks Desktop+ or hurts FPS (SteamVR Beta bumped default render scale to 150%), revert to stable SteamVR or check your per-app resolution slider.
- **Steam Frame:** Keep line-of-sight to the wireless dongle (enable Multi-link if available), and turn off Steam Frame eye tracking if you see screen tears.

### Does the desktop mirror window hurt VR performance?

No. ED-UP's **Mirror window** (`F8 -> Mirror window`) copies an already-rendered eye at negligible cost and prevents Elite from rendering its own separate desktop view, so old `DisplaySettings.xml` tweaks are unnecessary.

- For streaming or recording without hex/fish-eye distortion, capture the desktop mirror window and set its aspect crop to `16:9` in `F8 -> Mirror window`.
- If **Steam Input virtual menus** do not appear on the mirror, set the mirror window mode to `game`.
- Turning the mirror window off hides the F8 menu on your desktop monitor.

---

## Performance, stuttering, VRAM, and crashes

### Multi-second freezes, head-locked dragging, or FPS collapsing to 5 to 12 FPS

This is almost always **VRAM overflow** (Windows paging GPU memory over PCIe into system RAM), most common on **8 to 12 GB GPUs** and high-resolution headsets:

- Elite allocates up to **85% of total card VRAM** without checking what Windows and your VR runtime leave free; ED-UP's swapchains, UI layer, and DLSS add roughly 2 to 3.5 GB on top.
- **Fixes:**
  1. **Update to v0.23.0+:** Includes automatic VRAM budget protection (`advanced.game_vram = auto`), which reserves headroom for ED-UP and calibrates over your first few **5+ minute** sessions (`%LOCALAPPDATA%\EDUP\game_vram.txt`). *(Short sessions under 5 minutes will not write the learned reserve yet. If you override `game_vram` manually in `edup.ini`, never set it below `4096` MB or Elite drops geometry and textures to very low detail.)*
  2. **Check VRAM live:** Open **F8 -> Monitor**. If VRAM sits at 95 to 100% of budget, lower per-eye resolution (OpenXR or SteamVR), lower **HMD Image Quality** (`0.5` to `0.75`), keep **UI quality** at `100%`, leave **HUD Anti-Aliasing** off, and drop **Texture Quality** to High or Medium.
  3. **Windows 11 HAGS:** If multi-second head-locked freezes persist on Windows 11 with NVIDIA and SteamVR, turn **Hardware-accelerated GPU scheduling** off (*Windows Settings -> System -> Display -> Graphics -> Change default graphics settings*) and reboot.

### Why is FPS low in stations and settlements when Task Manager shows 20 to 35% CPU and under 100% GPU?

You are **CPU-bound**. Elite runs on a single Direct3D 11 render thread with minimal occlusion culling in stations and settlements (often 26,000+ draw calls per frame, doubled in VR), and ED-UP computes per-object motion vectors on that thread so moving NPCs and ships do not smear. Windows moves that thread across cores, so Task Manager's total CPU percentage looks low even when the render thread is saturated.

Check **F8 -> Monitor**: if GPU load is well below 99 to 100% while FPS is below target, lowering resolution will not raise FPS. What helps today:

- Keep **Optimized stations** on (`F8 -> Performance`).
- Lower **Spot Shadow Quality** to Low or Medium (significant CPU relief with minimal visual change).
- Turn **Ambient Occlusion**, **Blur**, and **Depth of Field** off.
- On a **monitor**, **XeSS + XeSS Frame Generation** roughly doubles FPS in CPU-bound stations.
- In **VR**, try **Turbo mode** (`F8 -> Performance`) if half-rate drops feel choppy.

### FPS locks at half refresh (36, 45, or 60) or quarter refresh (30) and stays stuck

Headset runtime reprojection has latched onto a slow frame:

- Disable **SteamVR Motion Smoothing** (per-app for Elite), **Meta ASW**, or **Pimax Motion Smoothing**.
- Mid-session recovery: toggle Motion Smoothing on and back off in SteamVR, or change refresh rate and switch back.
- The older pre-v0.23.0 bug where FPS stayed permanently low after disembarking and re-embarking your ship is fixed in **v0.23.0**.

### Crashes on startup or every 30 to 60 minutes

Check the four most frequent causes seen in logs:

1. **Windows page file (virtual memory) capped too low — especially in v0.23.0 with FSR 4.1:** Because v0.23.0 keeps Elite inside available VRAM, more assets live in system RAM. A manually capped page file (such as 4 GB) triggers `E_OUTOFMEMORY` crashes. Go to **Windows Settings -> System -> About -> Advanced system settings -> Performance Settings -> Advanced -> Virtual memory -> Change**, set `C:` to **System managed size**, click **Set**, click **OK**, and **reboot**.
2. **GPU overclock or undervolt:** ED-UP drives the GPU harder (including during startup) and can expose borderline overclocks (`DXGI_ERROR_DEVICE_REMOVED`) that pass in other games. Drop core/memory clocks by 25 to 50 MHz or test at stock.
3. **AMD Adrenalin overlay or per-game profiles:** Can cause 110 C hotspots, alt-tab crashes, or confused VR/flat profiles. Disable **AMD Adrenalin Overlay** and use one **Global** profile.
4. **ReShade (`dxgi.dll`) or 1 to 2 second launch crash:** Remove `dxgi.dll` to test, update to the latest ED-UP, and if a crash persists right after launch see [the technical troubleshooting notes](https://github.com/characterecho-sean/ed-unofficial-patch/blob/main/docs/troubleshooting.md#if-the-game-dies-a-second-or-two-after-launch) (`context_hook_mode = shared` or `live`).

---

## On foot in VR and visual quirks

### I still see the flat 2D screen on foot — how do I get native 3D on-foot VR?

1. Make sure you are on **v0.22.0+** and on an ED-UP release matched to the current Odyssey build (native on-foot VR stands down on unrecognized builds).
2. Confirm **Native on-foot VR** is on in `F8` and Elite's **3D Mode** is **HMD** (not Off or HMD Cinema).
3. **Non-English Windows or custom Saved Games folder:** ED-UP watches Elite's journal for the disembark event. If your journal directory is not at the default path, set `journal_dir = <path>` under `[d3d11]` in `edup.ini`.

### Can I use VR motion controllers or walk around room-scale on foot?

No. On-foot VR uses keyboard/mouse or a gamepad, and is intentionally a seated experience. Micro-recentering and view fade-out when leaning far exist as an anti-cheat safeguard against peeking through walls or cover; there is no toggle to disable it. Use the external camera to walk around and inspect ships or concourses.

### On-foot aiming, turning, ADS, and HUD tweaks (`F8 -> Native on foot`)

- **Snap vs smooth turning:** Switch **Turn style** between **Snap** (45 degrees by default) and **Smooth**.
- **Look to Aim:** When on, your commander faces and walks where your head points (suited to a gamepad). When off, mouse/stick aim normally while your head looks around freely. You can also toggle **ADS: Look to Aim** and vertical fine-tuning in ADS independently.
- **Weapon sights feel too far from your eye in ADS:** Enable **Developer mode** (`F8 -> Performance`), then open the **Experimental** page and raise **ADS eye forward / back** (positive moves closer to the sight).
- **On-foot HUD sits too far in the corners or shield bar looks stepped:** Adjust HUD distance, size, element scale, and spread on **F8 -> Native on foot**.

### Common visual quirks and quick fixes

- **Flickering cracks on asteroids or mismatched brightness on rocks:** Turn Elite's **Ambient Occlusion** off (vanilla VR stereo bug).
- **Blinding bloom on large beam lasers or ice planets:** Lower or disable **Bloom** in Elite's graphics options.
- **Galaxy Map or System Map cursor misaligned in VR:** Update to **v0.23.0+**, enable **panel-based cursor** in Elite, and verify **VR Galaxy and System Map fixes** is on in `F8 -> Visual`.
- **Missing planets in System Map or missing station hangar walls:** Caused by **Optimized stations** culling — turn **Optimized stations** off in `F8 -> Performance` as a workaround and make sure you are on the latest release.
- **World scale feels too small in VR:** Adjust **World scale** in `F8 -> Visual` (many players prefer `108%` to `120%`).
- **TrackIR feels swimmy or laggy (flat):** Temporal AA camera jitter and frame history can add feel/trail to fast TrackIR pans. Press **F7** to compare; if you prefer zero temporal lag, keep ED-UP AA off (for example with DLDSR) while keeping ED-UP's other fixes.

---

## Flat screen, XeSS Frame Generation, AMD FSR 4, and Linux

### XeSS Frame Generation freezes on a black screen or needs Alt-Tab

- **Flat screen only** (not available in VR) and requires **Anti-aliasing = XeSS-SR**.
- **Use Borderless or Windowed mode:** Intel XeFG does not support Exclusive Fullscreen. Enable Windows **"Optimizations for windowed games"** so Borderless matches fullscreen performance.
- **Restart Elite** after turning Frame Generation on for the first time.
- **OBS or Discord streaming hangs with Frame Generation on:** Use **Game Capture**, not Whole-Window Capture.

### AMD FSR 4.1 does not engage or falls back to FSR 3

- **Windows:** Requires **v0.23.0+** and an **RX 7000 or 9000** GPU. Use one **Global** profile in AMD Adrenalin rather than per-game profiles.
- **Linux / Proton:**
  1. Use **`GE-Proton11-12` or newer** (stock Proton 11 often fails to initialize FSR 4).
  2. Set Steam Launch Options to `PROTON_FSR4_UPGRADE=1 %command%` (add `PROTON_FSR4_INDICATOR=1` to confirm which mode is active).
  3. Keep display resolution at your monitor's native size and Supersampling at `0.65` to `0.75`.
  4. Older RDNA 2 / RDNA 3 / 780M GPUs need INT8 FSR 4 support; if FSR 4 smears or falls back, use **FSR 3.1** or **DittoSR**.

### Linux, hybrid GPUs, and Proton issues

- **Hybrid GPU laptop (Intel iGPU + NVIDIA dGPU) — DLSS missing:** Launch with `prime-run` or `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` (or set `DXVK_FILTER_DEVICE_NAME`).
- **Black HUD ghosting under FSR on Linux:** Switch from Proton 9 to **Proton Experimental** or a recent **GE-Proton**.
- **Epic on Linux (Heroic, Lutris, Legendary):** Run `./ed-up-launcher.sh --store epic` after running Odyssey once through that launcher. [Install on Linux](Install-on-Linux.md).

---

## Other mods and overlays

| Tool or mod | Compatibility | Notes |
| --- | --- | --- |
| **EDHM** | Works (chained) | Both use `d3d11.dll`. Install EDHM first, then run ED-UP or press **Repair** so ED-UP renames EDHM to `d3d11_edhm.dll` and chains it via `real_dll`. Check that `edup-flat.ini` does not have `real_dll = none`. After uninstalling EDHM, press **Repair** in ED-UP (EDHM's uninstaller deletes `d3d11.dll`). If standard 3Dmigoto breaks FSR 3/4 or crashes on Linux, enable the optional **EDHM 2DMigoto** runtime under **Settings -> EDHM compatibility** in the launcher ([The launcher](The-launcher.md#experimental-edhm-2dmigoto-runtime)). See [Other mods](Other-mods.md). |
| **ReShade (`dxgi.dll`)** | Unreliable on flat | Can cause startup crashes ("two beeps and exit"), hard freezes (especially with EDHM or Linux), or block F8. Remove `dxgi.dll` first when troubleshooting. |
| **OpenComposite** | Remove | Conflicts with ED-UP's built-in OpenXR runtime. |
| **vrperfkit / OptiScaler / Lossless Scaling** | Remove | Redundant, suppressed, or causes crashes and black-screen hangs. |
| **OpenXR Toolkit** | Usually unneeded | Foveated rendering yields little gain in CPU-bound stations (FOV cropping works). Disable or remove it if VR fails to launch. |
| **OFXR-Bridge / VectorXR (VR Frame Gen)** | Works | Use the `djules75` OFXR-Bridge fork and set optic flow resolution to `100%` to avoid skybox/star ghosting; FSR pairs cleaner than DLSS with OFXR. QuadViews is not supported in DX11. |
| **RTSS / MSI Afterburner** | Overlay conflict | Often fails to hook. Use ED-UP's **F8 -> Monitor / Frame-rate readout**, or `fpsVR` in VR. |
| **AMD Adrenalin / Steam Overlays** | Disable if crashing | Adrenalin overlay can trigger alt-tab/hotspot crashes; Steam overlay can freeze headsets or crash on exit. |
| **OpenKneeboard / Desktop+ / MFD plugins** | Works | Start **OpenKneeboard** *after* Elite (VDXR/Oculus). Use stable SteamVR for **Desktop+**. Re-enable MFD plugins in `F8 -> Plugins` after updating ED-UP; `edcopilot-mfd` requires EDCoPilot's backend voice service running. |

---

## Still stuck? Collect logs first

Close Elite completely (verify `EliteDangerous64.exe` is gone in Task Manager), open **`ed-up-launcher.exe`**, and click **Save logs**. It writes a `.zip` to your Desktop (Downloads on Linux) and copies it to your clipboard so you can press `Ctrl+V` straight into Discord or a [GitHub issue](https://github.com/characterecho-sean/ed-unofficial-patch/issues/new/choose). Include whether you play **VR or flat**, your **store**, **GPU**, and (for VR) your **headset and OpenXR runtime**. [Ask for help](Ask-for-help.md).
