#!/bin/sh

# Flatpak records where this application's immutable files live on the host.
app_path=$(awk '/^\[Instance\]$/{instance=1; next} /^\[/{instance=0} instance && /^app-path=/{sub(/^app-path=/, ""); print; exit}' /.flatpak-info)
case $app_path in
    /*) ;;
    *) printf '%s\n' 'ED-UP: could not locate the installed launcher.' >&2; exit 1 ;;
esac

# Steam, Proton, and the host VR runtime use the desktop session environment.
exec flatpak-spawn --host --directory="$HOME" \
    /usr/bin/env python3 "$app_path/lib/ed-up-launcher/installed-linux-launcher.py" "$@"
