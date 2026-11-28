#!/usr/bin/env python3
"""Build installable Linux launchers from the existing ED-UP Windows launcher."""

import argparse
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from textwrap import dedent

sys.dont_write_bytecode = True  # --dry-run must leave a clean checkout untouched.
from gen_installer_rc import launcher_icon_png  # noqa: E402
from package_native import LAUNCHER_ASSET, LAUNCHER_LINUX_WRAPPER, LINUX_EPIC_LIBRARY  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
APP_ID = "io.github.characterecho_sean.EDUP"
COMMAND = "ed-up-launcher"
APP_DIR = Path("lib/ed-up-launcher")
INSTALLER = "installed-linux-launcher.py"
RUNTIME = "org.freedesktop.Platform/x86_64/25.08"
RUNTIME_REPO = "https://dl.flathub.org/repo/flathub.flatpakrepo"


def stage_application(destination, exe, *, flatpak):
    """Write the app menu entry, matching artwork, and launcher payload."""
    library = destination / APP_DIR
    library.mkdir(parents=True)
    for source, name in ((exe, LAUNCHER_ASSET), (ROOT / "tools/install_proton.py", LAUNCHER_LINUX_WRAPPER),
                         (ROOT / "tools" / LINUX_EPIC_LIBRARY, LINUX_EPIC_LIBRARY),
                         (ROOT / "release" / INSTALLER, INSTALLER), (ROOT / "LICENSE", "LICENSE")):
        shutil.copyfile(source, library / name)

    menu = destination / "share/applications" / (APP_ID + ".desktop")
    menu.parent.mkdir(parents=True)
    entry = COMMAND if flatpak else f"/usr/bin/{COMMAND}"
    menu.write_text(f"[Desktop Entry]\nType=Application\nName=ED-UP Launcher\n"
                    f"Comment=Launch ED Unofficial Patch\nExec={entry}\nIcon={APP_ID}\n"
                    "Terminal=false\nCategories=Game;\n", encoding="utf-8")
    icon = destination / "share/icons/hicolor/128x128/apps" / (APP_ID + ".png")
    icon.parent.mkdir(parents=True)
    icon.write_bytes(launcher_icon_png())

    metainfo = destination / "share/metainfo" / (APP_ID + ".metainfo.xml")
    metainfo.parent.mkdir(parents=True)
    metainfo.write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>{APP_ID}</id>
  <name>ED-UP Launcher</name>
  <summary>Launch ED Unofficial Patch for Elite Dangerous</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
  <description>
    <p>Opens the ED-UP launcher using Elite Dangerous's configured Wine or Proton runner.</p>
  </description>
  <launchable type="desktop-id">{APP_ID}.desktop</launchable>
  <url type="homepage">https://github.com/characterecho-sean/ed-unofficial-patch</url>
</component>
""", encoding="utf-8")

    binary = destination / "bin" / COMMAND
    binary.parent.mkdir()
    if flatpak:
        shutil.copyfile(ROOT / "release/ed-up-flatpak-launcher.sh", binary)
    else:
        binary.write_text(f'#!/bin/sh\nexec /usr/bin/python3 /usr/{APP_DIR}/{INSTALLER} "$@"\n', encoding="utf-8")
    binary.chmod(0o755)


def build_deb(tree, output, exe, version):
    stage_application(tree / "usr", exe, flatpak=False)
    upstream, separator, prerelease = version.partition("-")
    if separator and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", upstream):
        # Debian's tilde orders the prerelease before its stable update.
        version = upstream + "~" + prerelease.replace("-", ".")
    control = tree / "DEBIAN/control"
    control.parent.mkdir()
    control.write_text(f"Package: ed-up-launcher\nVersion: {version}\nArchitecture: amd64\n"
                       "Maintainer: ED-UP contributors\nSection: games\nPriority: optional\nDepends: python3\n"
                       "Description: ED Unofficial Patch launcher\n"
                       " Starts ED-UP using Elite Dangerous's configured Wine or Proton runner.\n", encoding="utf-8")
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", str(tree), str(output)], check=True)


def build_flatpak(tree, output, exe):
    stage_application(tree / "files", exe, flatpak=True)
    (tree / "var").mkdir()
    (tree / "metadata").write_text(f"[Application]\nname={APP_ID}\nruntime={RUNTIME}\n"
                                   "sdk=org.freedesktop.Sdk/x86_64/25.08\n", encoding="utf-8")
    repo = tree.parent / "repo"
    subprocess.run(["flatpak", "build-finish", "--no-inherit-permissions", f"--command={COMMAND}",
                    "--talk-name=org.freedesktop.Flatpak", str(tree)], check=True)
    subprocess.run(["flatpak", "build-export", str(repo), str(tree), "stable"], check=True)
    subprocess.run(["flatpak", "build-bundle", f"--runtime-repo={RUNTIME_REPO}", str(repo), str(output),
                    APP_ID, "stable"], check=True)


def package(exe, version, output, formats, dry_run):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[~+.-][A-Za-z0-9.+~.-]+)?", version):
        raise ValueError("use a version such as 0.3.1 or 0.3.1~preview1")
    with exe.open("rb") as handle:
        if handle.read(2) != b"MZ":
            raise ValueError(f"expected the Windows launcher executable: {exe}")
    for kind in formats:
        if not shutil.which("dpkg-deb" if kind == "deb" else "flatpak"):
            raise ValueError(f"install the Linux build tool for {kind} packaging")
        destination = output / f"ed-up-launcher-linux-{version}.{kind}"
        print(destination, flush=True)
        if dry_run:
            continue
        output.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            raise ValueError(f"output already exists: {destination}")
        with tempfile.TemporaryDirectory(prefix=".edup-linux-", dir=output) as tmp:
            directory = Path(tmp)
            staged = directory / destination.name
            if kind == "deb":
                build_deb(directory / "app", staged, exe, version)
            else:
                build_flatpak(directory / "app", staged, exe)
            try:
                os.link(staged, destination)
            except FileExistsError:
                if not filecmp.cmp(staged, destination, shallow=False):
                    raise ValueError(f"output already exists: {destination}") from None
    return 0


def self_test():
    with tempfile.TemporaryDirectory(prefix="edup-packaging-test-") as tmp:
        root = Path(tmp)
        exe = root / "launcher.exe"
        exe.write_bytes(b"MZ preview fixture")
        for flatpak in (False, True):
            stage = root / ("flatpak" if flatpak else "native")
            stage_application(stage, exe, flatpak=flatpak)
            entry = (stage / "share/applications" / (APP_ID + ".desktop")).read_text(encoding="utf-8")
            assert "Terminal=false\n" in entry and f"Icon={APP_ID}\n" in entry
            assert (f"Exec={COMMAND}\n" if flatpak else f"Exec=/usr/bin/{COMMAND}\n") in entry
            icon = stage / "share/icons/hicolor/128x128/apps" / (APP_ID + ".png")
            assert icon.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n"
            assert (stage / APP_DIR / LAUNCHER_ASSET).read_bytes() == b"MZ preview fixture"
            assert (stage / APP_DIR / INSTALLER).is_file()
        if sys.platform == "linux":
            for package in ("native", "flatpak"):
                installed_update_self_test(root / package / APP_DIR, root / f"{package} data")
        if shutil.which("dpkg-deb"):
            output = root / "installers"
            command = ["0.23.0-rc1", "--launcher-exe", str(exe), "--output", str(output), "--format", "deb"]
            assert main(command + ["--dry-run"]) == 0
            assert not output.exists()
            assert main(command) == 0
            deb = output / "ed-up-launcher-linux-0.23.0-rc1.deb"
            version = subprocess.check_output(["dpkg-deb", "-f", str(deb), "Version"], text=True).strip()
            assert version == "0.23.0~rc1"
            subprocess.run(["dpkg", "--compare-versions", version, "lt", "0.23.0"], check=True)
    print("package_linux_launcher: self-test passed")
    return 0


def installed_update_self_test(library, data):
    # Stand in for Proton and the updater's successful swap. The Windows
    # launcher rig exercises downloading and verifying the signed release.
    (library / LAUNCHER_LINUX_WRAPPER).write_text(dedent("""\
        import os
        import sys
        from pathlib import Path

        assert sys.argv[1:3] == ['--desktop', '--managed-launcher']
        if '--dry-run' in sys.argv:
            assert '--launcher-exe' not in sys.argv
            sys.exit(0)
        exe = Path(sys.argv[sys.argv.index('--launcher-exe') + 1])
        assert exe != Path(__file__).with_name('ed-up-launcher.exe')
        assert '--channel' in sys.argv and sys.argv[sys.argv.index('--channel') + 1] == 'rc'
        backup = exe.with_name(exe.name + '.old')
        started = exe.read_bytes().decode('ascii')
        if '--updated' in sys.argv:
            backup.unlink()
            print('resumed', started)
        else:
            print('started', started)
            replacement = os.environ.get('EDUP_TEST_LAUNCHER_UPDATE')
            if replacement:
                exe.replace(backup)
                exe.write_bytes(replacement.encode('ascii'))
            sys.exit(int(os.environ.get('EDUP_TEST_LAUNCHER_EXIT', '0')))
        """), encoding="utf-8")
    env = dict(os.environ, XDG_DATA_HOME=str(data), EDUP_TEST_LAUNCHER_UPDATE="", EDUP_TEST_LAUNCHER_EXIT="0")
    launch = [sys.executable, str(library / INSTALLER)]
    subprocess.run(launch + ["--dry-run"], env=env, check=True)
    assert not data.exists()
    launch += ["--", "--channel", "rc"]
    updated = subprocess.check_output(launch, env=dict(env, EDUP_TEST_LAUNCHER_UPDATE="MZ v2"), text=True)
    assert updated.splitlines() == ["started MZ preview fixture", "resumed MZ v2"]
    reopened = subprocess.check_output(launch, env=env, text=True)
    assert reopened.splitlines() == ["started MZ v2"]
    stopped = subprocess.run(launch, env=dict(env, EDUP_TEST_LAUNCHER_UPDATE="MZ v3", EDUP_TEST_LAUNCHER_EXIT="37"),
                             capture_output=True, text=True)
    assert stopped.returncode == 37 and stopped.stdout.splitlines() == ["started MZ v2"]
    recovered = subprocess.check_output(launch, env=env, text=True)
    assert recovered.splitlines() == ["resumed MZ v3"]
    assert (library / LAUNCHER_ASSET).read_bytes() == b"MZ preview fixture"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", nargs="?")
    parser.add_argument("--launcher-exe", type=Path, default=ROOT / "build" / LAUNCHER_ASSET)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--format", choices=("deb", "flatpak"), action="append")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.version is None:
        parser.error("version is required")
    try:
        return package(args.launcher_exe, args.version, args.output.resolve(), args.format or ("deb", "flatpak"),
                       args.dry_run)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"package_linux_launcher: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
