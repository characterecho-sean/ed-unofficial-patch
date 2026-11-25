#!/usr/bin/env python3
"""Fetch and verify NVIDIA's pinned Reflex PCL header and SDK license.

    python tools/fetch_reflex.py [--dest DIR] [--dry-run]
    python tools/fetch_reflex.py --verify DIR
    python tools/fetch_reflex.py --self-test

Default destination is EDUP_REFLEX_SDK when set, otherwise this checkout's
ignored third_party/reflex directory. --dest (or --into) selects another SDK
directory. The subset preserves upstream Reflex_Stats/pclstats.h and license.txt
paths, with exact byte lengths and SHA-256 pins from one official commit.

The SDK source retains NVIDIA's license; it is not covered by this project's
MIT license. Builds incorporating PCL stage its unchanged license separately
as NVIDIA-REFLEX-LICENSE.txt. No SDK source is included in release archives.

--dry-run performs no network access or writes, including temporary files and
directories. Existing verified SDKs are reused. An unverified existing
destination is refused rather than overwritten; --verify explains mismatches.
"""

import argparse
from contextlib import redirect_stdout
import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import urllib.request


COMMIT = "c2522cee75be7a952ab29281c4cf6d978c5e2e0e"
URL = "https://raw.githubusercontent.com/NVIDIA-RTX/REFLEX"
HEADER = "Reflex_Stats/pclstats.h"
LICENSE = "license.txt"
LICENSE_NAME = "NVIDIA-REFLEX-LICENSE.txt"
PCL_BINARY_MARKER = b"EDUP_REFLEX_PCL_SDK_v1"
FILES = {
    HEADER: (7213, "95267f05f43b79f4b69186125560498d5deb8eeeef3ae26103c1bb5825442d7b"),
    LICENSE: (20504, "ebf83c07fb3b2939908c3795d887afde3161c89a28ba391724efc784ce1bdabe"),
}
MAX_DOWNLOAD = 1024 * 1024


def default_dest():
    return Path(os.environ.get("EDUP_REFLEX_SDK") or Path(__file__).resolve().parents[1] / "third_party" / "reflex")


def payload_problem(name, data):
    """Return a pinned-size/hash mismatch; every consumer uses the same pins."""
    expected_size, expected_hash = FILES[name]
    observed = hashlib.sha256(data).hexdigest()
    if len(data) != expected_size or observed != expected_hash:
        return ("%s is not the pinned Reflex SDK file: size %d (expected %d), SHA-256 %s (pin %s)" %
                (name, len(data), expected_size, observed, expected_hash))
    return None


def verify(dest, quiet=False):
    """Check the exact subset on disk without changing it; return all problems."""
    problems = []
    for name in FILES:
        path = Path(dest) / name
        try:
            data = path.read_bytes()
        except OSError as error:
            problems.append("missing or unreadable: %s (%s)" % (path, error))
            continue
        problem = payload_problem(name, data)
        if problem:
            problems.append(problem)
    if not problems and not quiet:
        print("Reflex PCL SDK verified at %s (commit %s)" % (dest, COMMIT))
    return problems


def pcl_compiled(graphics):
    """Detect the compiled-only wrapper marker in this specific graphics DLL.

    A notice left by an older build is not evidence of PCL incorporation. Read
    with overlap so a marker spanning a chunk boundary is still recognized.
    """
    path = Path(graphics)
    if not path.is_file():
        return False
    tail = b""
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            data = tail + block
            if PCL_BINARY_MARKER in data:
                return True
            tail = data[-(len(PCL_BINARY_MARKER) - 1):]
    return False


def build_license(graphics, build):
    """Return the required verified notice path, or None for a non-PCL binary."""
    if not pcl_compiled(graphics):
        return None
    notice = Path(build) / LICENSE_NAME
    try:
        data = notice.read_bytes()
    except OSError as error:
        raise ValueError("PCL-enabled graphics requires %s: %s" % (notice, error)) from error
    problem = payload_problem(LICENSE, data)
    if problem:
        raise ValueError("PCL-enabled graphics requires the unchanged NVIDIA notice at %s: %s" % (notice, problem))
    return notice


def download_file(source, timeout=60):
    request = urllib.request.Request("%s/%s/%s" % (URL, COMMIT, source),
                                     headers={"User-Agent": "EDUP-pinned-reflex-sdk-fetch/1"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read(MAX_DOWNLOAD + 1)


def fetch(dest, dry_run=False, downloader=None):
    """Verify both downloads before exclusive publication; never replace an SDK."""
    destination = Path(dest)
    if dry_run:
        print("Would fetch NVIDIA Reflex PCL SDK commit %s into %s" % (COMMIT, destination))
        for name, (size, digest) in FILES.items():
            print("Would verify %s (%d bytes, SHA-256 %s)" % (name, size, digest))
        print("(dry run) no network, files, temporary paths, or directories were touched")
        return 0
    if destination.exists() or destination.is_symlink():
        problems = verify(destination, quiet=True)
        if not problems:
            print("Reflex PCL SDK already verified at %s" % destination)
            return 0
        raise ValueError("unverified Reflex SDK destination already exists; choose a fresh --dest or inspect --verify:\n  " +
                         "\n  ".join(problems))
    payload = {}
    for name in FILES:
        data = (downloader or download_file)(name)
        problem = payload_problem(name, data)
        if problem:
            raise ValueError("downloaded " + problem)
        payload[name] = data
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.mkdir()  # Exclusive ownership; a racing fetch cannot replace it.
    owned_files, owned_dirs = [], []
    try:
        for name, data in payload.items():
            path = destination / name
            if path.parent != destination and not path.parent.exists():
                path.parent.mkdir()
                owned_dirs.append(path.parent)
            with path.open("xb") as stream:
                owned_files.append(path)
                stream.write(data)
        problems = verify(destination)
        if problems:
            raise ValueError("published SDK failed verification: " + "; ".join(problems))
    except BaseException:
        # Remove only this attempt's files, never unrelated contents placed by
        # another actor while a failed publication is being cleaned up.
        for path in list(reversed(owned_files)) + list(reversed(owned_dirs)) + [destination]:
            try:
                path.rmdir() if path.is_dir() else path.unlink()
            except OSError:
                pass
        raise
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", "--into", type=Path, help="destination (default: EDUP_REFLEX_SDK or third_party/reflex)")
    parser.add_argument("--verify", type=Path, metavar="DIR", help="verify an existing pinned SDK without writes")
    parser.add_argument("--dry-run", action="store_true", help="show the fetch plan without network access or writes")
    parser.add_argument("--self-test", action="store_true", help="run offline portable fixtures")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.dest or args.verify or args.dry_run:
            parser.error("--self-test must be used alone")
        return self_test()
    if args.dest and args.verify:
        parser.error("--dest and --verify are mutually exclusive")
    try:
        if args.verify:
            problems = verify(args.verify)
            if problems:
                print("fetch_reflex: " + "\nfetch_reflex: ".join(problems), file=sys.stderr)
                return 1
            return 0
        return fetch(args.dest or default_dest(), dry_run=args.dry_run)
    except (OSError, ValueError, urllib.error.URLError) as error:
        print("fetch_reflex: " + str(error), file=sys.stderr)
        return 1


def self_test():
    class Tests(unittest.TestCase):
        def setUp(self):
            self.folder = tempfile.TemporaryDirectory(prefix="edup-reflex-test-")
            self.addCleanup(self.folder.cleanup)
            self.root = Path(self.folder.name)
            self.payload = {HEADER: b"fixture Reflex PCL header", LICENSE: b"fixture NVIDIA SDK license"}
            self.pins = {name: (len(data), hashlib.sha256(data).hexdigest()) for name, data in self.payload.items()}
            self.patch = mock.patch.dict(FILES, self.pins)
            self.patch.start()
            self.addCleanup(self.patch.stop)

        def test_fetch_verify_reuse_and_pins(self):
            target = self.root / "sdk"
            with redirect_stdout(io.StringIO()):
                self.assertEqual(fetch(target, downloader=self.payload.__getitem__), 0)
                self.assertEqual(main(["--verify", str(target)]), 0)
                self.assertEqual(fetch(target, downloader=lambda _: self.fail("verified SDK fetched twice")), 0)
            self.assertEqual((target / HEADER).read_bytes(), self.payload[HEADER])
            self.assertEqual((target / LICENSE).read_bytes(), self.payload[LICENSE])
            (target / HEADER).write_bytes(b"wrong header")
            self.assertIn("SHA-256", "\n".join(verify(target, quiet=True)))
            with self.assertRaisesRegex(ValueError, "already exists"):
                fetch(target, downloader=lambda _: self.fail("unverified existing SDK was fetched over"))

        def test_bad_or_missing_payload_creates_nothing(self):
            target = self.root / "sdk"
            for broken in (HEADER, LICENSE):
                with self.subTest(broken=broken), self.assertRaisesRegex(ValueError, "SHA-256"):
                    fetch(target, downloader=lambda name: b"corrupt" if name == broken else self.payload[name])
                self.assertEqual(list(self.root.iterdir()), [])
            self.assertEqual(len(verify(target, quiet=True)), 2)

        def test_failed_publication_cleans_only_owned_paths(self):
            target = self.root / "sdk"
            with mock.patch(__name__ + ".verify", return_value=["injected verification failure"]), \
                    self.assertRaisesRegex(ValueError, "published SDK failed"):
                fetch(target, downloader=self.payload.__getitem__)
            self.assertFalse(target.exists())

            def foreign_file(dest, quiet=False):
                (Path(dest) / "foreign.txt").write_bytes(b"another actor")
                return ["injected verification failure"]

            with mock.patch(__name__ + ".verify", side_effect=foreign_file), \
                    self.assertRaisesRegex(ValueError, "published SDK failed"):
                fetch(target, downloader=self.payload.__getitem__)
            self.assertEqual((target / "foreign.txt").read_bytes(), b"another actor")
            self.assertFalse((target / HEADER).exists())
            self.assertFalse((target / LICENSE).exists())

        def test_main_dry_run_never_downloads_or_writes(self):
            for extra in ([], ["--into", str(self.root / "explicit")]):
                with mock.patch.dict(os.environ, {"EDUP_REFLEX_SDK": str(self.root / "from-env")}), \
                        mock.patch(__name__ + ".download_file", side_effect=AssertionError("network")), \
                        mock.patch.object(Path, "mkdir", side_effect=AssertionError("directory write")), \
                        mock.patch.object(Path, "open", side_effect=AssertionError("file write")), \
                        redirect_stdout(io.StringIO()):
                    self.assertEqual(main(["--dry-run"] + extra), 0)
            self.assertEqual(list(self.root.iterdir()), [])

        def test_default_and_error_routes(self):
            with mock.patch.dict(os.environ, {"EDUP_REFLEX_SDK": str(self.root / "custom")}):
                self.assertEqual(default_dest(), self.root / "custom")
            with mock.patch.dict(os.environ, {"EDUP_REFLEX_SDK": ""}):
                self.assertEqual(default_dest(), Path(__file__).resolve().parents[1] / "third_party" / "reflex")
            with mock.patch("sys.stderr", io.StringIO()):
                self.assertEqual(main(["--verify", str(self.root / "absent")]), 1)

        def test_build_license_requires_current_binary_and_exact_notice(self):
            graphics, notice = self.root / "graphics.dll", self.root / LICENSE_NAME
            graphics.write_bytes(b"non-PCL graphics")
            notice.write_bytes(b"stale notice")
            self.assertIsNone(build_license(graphics, self.root))
            graphics.write_bytes(b"x" * (1024 * 1024 - 3) + PCL_BINARY_MARKER + b"tail")
            with self.assertRaisesRegex(ValueError, "unchanged NVIDIA notice"):
                build_license(graphics, self.root)
            notice.unlink()
            with self.assertRaisesRegex(ValueError, "requires"):
                build_license(graphics, self.root)
            notice.write_bytes(self.payload[LICENSE])
            self.assertEqual(build_license(graphics, self.root), notice)

    result = unittest.TextTestRunner(stream=sys.stdout, verbosity=1).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
