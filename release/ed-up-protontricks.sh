#!/bin/sh

show_error() {
    printf '%s\n' "$1" >&2
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title 'ED-UP via Protontricks' --text "$1"
    fi
    exit 1
}

# GNOME Files passes local selections as arguments, relative to the open folder.
if [ "$#" -ne 1 ] || [ "${1##*/}" != ed-up-launcher.exe ] || [ ! -f "$1" ]; then
    show_error 'Select ed-up-launcher.exe in the file manager.'
fi
if ! command -v protontricks-launch >/dev/null 2>&1; then
    show_error 'Install the native Protontricks package to use this shortcut.'
fi

case $1 in
    /*) launcher=$1 ;;
    *) launcher=$PWD/$1 ;;
esac

# Protontricks forwards the caller's XDG_CONFIG_DIRS into Pressure Vessel. Let
# the container supply its own value so OpenXR can find the imported manifest.
exec env -u XDG_CONFIG_DIRS PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 \
    protontricks-launch --no-term --appid 359320 "$launcher"
