ED-UP launcher on Linux (Steam and Epic copies)

Extract the Linux ZIP and double-click ED-UP Launcher in a host-installed
file manager, such as GNOME Files or your distribution's Dolphin. If Dolphin
asks whether to run it, choose Launch. Keep the other files beside it. On
Steam Deck, use Desktop Mode.
The file-manager launcher is for x86-64 Linux with glibc 2.34 or newer.

If Flatpak Dolphin shows Open With or No Apps available, open the extracted
folder in a host-installed file manager. That prompt appears before ED-UP
starts.

What it is: the ED-UP launcher, the same Windows program people run on
Windows. For a Steam copy it runs inside Elite's own Proton setup, so there is
nothing else to install. It needs Python 3 and the Proton that Steam already
uses for Elite. It does not need Protontricks or a system Wine. For an Epic
copy it runs under the Wine or Proton that copy already uses (see "Epic
copies" below).

Run Elite once from Steam, then close it. If you change Elite's Proton version
in Steam, launch Elite once again first. ED-UP Launcher opens in that existing
prefix and enables OpenXR import when it starts Steam Linux Runtime.

For command-line options or troubleshooting, use the included shell script:

    ./ed-up-launcher.sh

To see what would run without starting anything or writing any files:

    ./ed-up-launcher.sh --dry-run

Options for the launcher itself go after a lone --, for example:

    ./ed-up-launcher.sh -- --channel rc

The wrapper's own options (--game-dir, --steam-dir, --prefix, --proton, --launcher-exe)
go before the --. Use --game-dir '/path/to/product' for a different product
folder, --steam-dir '/path/to/Steam' or STEAM_DIR for a nonstandard Steam root,
and --proton '/path/to/Proton' if the prefix record is missing or stale. The
shell script works from any directory. You can also run
python3 ed-up-launcher-linux.py directly.

For a custom Steam prefix, use --store steam --prefix with the existing
compatdata directory (the parent of pfx). An inherited prefix from another
Steam shortcut is ignored. Flatpak Steam uses its installed sandbox and the
recorded Proton, including compatibility-tool extensions; missing or ambiguous
tools are refused. An explicit --proton selects a host runner and bypasses
the sandbox.
For Steam, additional named system Flatpak installations require that host
override.

When the launcher uses Steam Linux Runtime, it enables OpenXR runtime import
for the headset check. An explicit PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES
setting takes precedence.

Run through Protontricks from GNOME Files (Steam)

If you use the native Protontricks package, copy ed-up-protontricks.sh from
this download into .local/share/nautilus/scripts in your Home folder. Turn on
Show Hidden Files to see .local, then create the scripts folder if needed.
In the copied file's Properties, make sure Executable as Program is on.
Then right-click ed-up-launcher.exe and choose Scripts > ed-up-protontricks.sh.
The script uses Elite's existing Steam prefix and sets up OpenXR import for
that launch. Protontricks' standard Open With action uses the environment
from the desktop session.

The launcher keeps its downloads and settings inside Elite's Proton prefix, in
drive_c/users/steamuser/AppData/Local/EDVR under
steamapps/compatdata/359320/pfx. For an Epic copy it keeps them in the prefix
that copy runs in.

The launcher window installs, updates, repairs and uninstalls ED-UP for the
selected Elite folder, and its Save logs button writes the log zip to your
Downloads folder, or to your home folder when Downloads is missing.

Play starts Elite through Steam. Starting Elite directly, without Steam, is not
offered on Linux yet.

Epic copies

The wrapper also finds an Epic copy of Elite installed through Epic's native
Linux launcher (the Preview), Heroic, Legendary, Lutris, or the Windows Epic
launcher in an existing Wine prefix. It reads the manager's local records,
takes the Wine or Proton runner and prefix they name, and runs the launcher
under that runner with the game folder and --store epic. It requires
EliteDangerous64.exe in the recorded folder, and it does not create a prefix or
choose a replacement runner. Missing or conflicting records are reported, not
guessed.

Run Elite once through its usual manager, then close it and open ED-UP
Launcher. After you change the manager's runner, launch Elite once again
first. If you have both a Steam and an Epic copy, you can select Epic with
the shell script:

    ./ed-up-launcher.sh --store epic --dry-run
    ./ed-up-launcher.sh --store epic

With only an Epic copy installed, --store epic is chosen for you. When a Steam
copy is installed too, the wrapper asks you to pass --store steam or --store
epic. --game-dir '/path/to/product' picks one copy when several are installed.

When the records do not name the runner, pass the prefix you already play in
and the Wine that runs it, or an installed Proton:

    ./ed-up-launcher.sh --store epic --prefix '/existing/prefix' --wine '/path/to/wine'
    ./ed-up-launcher.sh --store epic --prefix '/existing/compatdata' --proton '/path/to/Proton'

For --proton, the prefix is the compatdata folder that contains pfx. Flatpak
managers run their recorded runner inside their own sandbox; the filesystem
access the launcher needs is granted for that one run only, and --wine or
--proton selects a host runner instead. Keep epic_library.py beside
ed-up-launcher-linux.py: the wrapper reads Epic's records through it.

Options for the launcher itself go after the lone --, as for Steam, for
example ./ed-up-launcher.sh --store epic -- --install --scope both. The
wrapper sets --dir and --store itself and refuses them there.

Play starts Elite through Steam, so it is not for an Epic copy. Start the game
from Heroic, Legendary, Lutris or Epic, so that program signs you in.

Keep any existing required Elite launch options. On hybrid-GPU systems (NVIDIA
+ integrated GPU), if Elite or ED-UP selects the integrated GPU under Proton,
set __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia. Headset and
OpenXR setup and Proton gameplay are not covered by this package.
