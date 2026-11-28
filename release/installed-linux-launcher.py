#!/usr/bin/env python3
"""Start the packaged launcher with a host path visible to Proton."""

import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main(arguments):
    package = Path(__file__).resolve().parent
    command = [sys.executable, str(package / "ed-up-launcher-linux.py"), "--desktop", "--managed-launcher"]
    wrapper_args = arguments[:arguments.index("--")] if "--" in arguments else arguments
    if "--dry-run" in wrapper_args:
        return subprocess.call(command + arguments)

    # Keep a writable copy outside package storage. A package with a different
    # bundled EXE starts from its own copy.
    data = Path(os.environ.get("XDG_DATA_HOME", ""))
    if not data.is_absolute():
        data = Path.home() / ".local/share"
    bundled = package / "ed-up-launcher.exe"
    digest = hashlib.sha256(bundled.read_bytes()).hexdigest()
    folder = data / "ed-up-launcher" / digest
    folder.mkdir(mode=0o700, parents=True, exist_ok=True)
    exe = folder / bundled.name
    backup = exe.with_name(exe.name + ".old")
    command += ["--launcher-exe", str(exe)]

    # Steam and Epic can be in separate prefixes. Serialize access to the
    # executable across their Windows instance locks.
    with (folder / "launch.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        if not exe.is_file():
            if backup.is_file():
                os.replace(backup, exe)
            else:
                with tempfile.TemporaryDirectory(dir=folder) as seed:
                    staged = Path(seed) / exe.name
                    shutil.copyfile(bundled, staged)
                    os.link(staged, exe)

        # The updater leaves .old after a successful signed swap. Resume if
        # the previous process stopped before the host could relaunch.
        updated = list(arguments)
        if "--" not in updated:
            updated.append("--")
        updated.append("--updated")
        if backup.is_file():
            return subprocess.call(command + updated)

        result = subprocess.call(command + arguments)
        if result == 0 and exe.is_file() and backup.is_file():
            return subprocess.call(command + updated)
        return result


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
