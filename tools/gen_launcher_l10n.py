#!/usr/bin/env python3
"""Generate the launcher's compiled localization table from Crowdin .pot/.po catalogs.

WHY THIS EXISTS

The launcher's UI strings live in GNU gettext catalogs under
src/installer/l10n/ so translators can work in Crowdin (see crowdin.yml)
instead of hand-editing C++ unicode escapes:

  * src/installer/l10n/launcher.pot   English source template (owned by the repo)
  * src/installer/l10n/<code>.po      per-language translations (owned by Crowdin)

At build time this tool validates the catalogs against the launcher's C++
sources and generates <gen>/launcher_l10n_table.inc as pure ASCII C++ wide
string literals (\\uXXXX / \\UXXXXXXXX escapes, safe without /utf-8 on any
Windows codepage). ed-up-launcher.exe therefore stays a single, offline,
byte-deterministic binary with no runtime sidecar files.

SINGLE-WRITER MODEL AND LANGUAGE ACTIVATION

The repo writes launcher.pot (--update-pot); Crowdin is the sole writer of
src/installer/l10n/*.po. Because a newly added English UI string only reaches
Crowdin after merging to main, missing or fuzzy translations in the five core
languages (de, es, fr, pt-BR, ru) fall back cleanly to English at runtime with
a build warning rather than breaking the build. Each core catalog file must
still exist and hold at least one valid translation.

Additional Crowdin languages listed in KNOWN_LANGUAGES (zh-CN, cs, it, ja, uk)
are preconfigured for zero-code-change activation:
  * If <code>.po is absent or has no translated strings yet, the language is
    omitted from the launcher's dropdown so users never see an English-only
    entry.
  * As soon as Crowdin syncs approved translations into <code>.po, the language
    compiles into launcher_l10n_table.inc automatically, with any untranslated
    or fuzzy string falling back cleanly to English at runtime.
  * Every translated string in any language (core or optional) is treated as
    untrusted input and checked for NUL/control/bidi/invisible characters,
    whitespace-only text, maximum length, and exact %s placeholder parity.

Usage:
  python tools/gen_launcher_l10n.py --root <repo> --out <gen dir> [--dry-run]
  python tools/gen_launcher_l10n.py --root <repo> --check
  python tools/gen_launcher_l10n.py --root <repo> --update-pot [--dry-run]
  python tools/gen_launcher_l10n.py --self-test
"""

import argparse
import io
import os
import shutil
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

# Keep --dry-run strictly write-free: importing a sibling helper must not write __pycache__.
sys.dont_write_bytecode = True
_TOOLS_DIR = str(Path(__file__).resolve().parent)
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)
import check_agent_material  # noqa: E402

L10N_REL_DIR = Path("src") / "installer" / "l10n"
POT_NAME = "launcher.pot"
TABLE_NAME = "launcher_l10n_table.inc"

# Cap translated string length so worst-case ASCII \UXXXXXXXX escaping (10 bytes/char)
# stays strictly below MSVC's C2026 string literal limit (16,380 bytes).
MAX_TRANSLATION_CHARS = 1500

UI_SOURCE_FILES = (
    Path("src") / "installer" / "gui.cpp",
    Path("src") / "installer" / "launcher_gui.inc",
    Path("src") / "installer" / "launcher_login_dialog.cpp",
    Path("src") / "installer" / "launcher_l10n.cpp",
)

# Ordered language registry: (code, native_display_name, win_locale_prefixes, core)
# Keep "en" first; non-English order is the order shown in the launcher's Language combo.
KNOWN_LANGUAGES = (
    ("en", "English", ("en",), True),
    ("de", "Deutsch", ("de",), True),
    ("es", "Español", ("es",), True),
    ("fr", "Français", ("fr",), True),
    ("pt-BR", "Português", ("pt",), True),
    ("ru", "Русский", ("ru",), True),
    ("zh-CN", "简体中文", ("zh-hans", "zh-cn", "zh-sg"), False),
    ("cs", "Čeština", ("cs",), False),
    ("it", "Italiano", ("it",), False),
    ("ja", "日本語", ("ja",), False),
    ("uk", "Українська", ("uk",), False),
)

KNOWN_BY_CODE = {code: (name, prefixes, core) for code, name, prefixes, core in KNOWN_LANGUAGES}


# Strings legitimately spelled the same as English in a core language.
def same_as_english(english, lang):
    if english == "Elite Dangerous":
        return True
    if english == "VR" and lang in ("de", "ru"):
        return True
    if english == "launcher" and lang not in ("de", "ru"):
        return True
    if english == "Code" and lang in ("de", "fr"):
        return True
    return False


# ---------------------------------------------------------------------------
# C++ launcher UI string scanner (mirrors tools/launcher_test/launcher_test.cpp)
# ---------------------------------------------------------------------------

def _strip_cpp_comments(src):
    out = []
    i = 0
    n = len(src)
    while i < n:
        if src[i:i + 2] == "/*":
            end = src.find("*/", i + 2)
            i = n if end == -1 else end + 2
            out.append("\n")
        elif src[i:i + 2] == "//":
            end = src.find("\n", i + 2)
            i = n if end == -1 else end
        elif src[i] == '"':
            out.append(src[i])
            i += 1
            while i < n and src[i] != '"':
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i])
                    i += 1
                out.append(src[i])
                i += 1
            if i < n:
                out.append(src[i])
                i += 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def _launcher_ifdef_source(src):
    """Keep the EDUP_LAUNCHER build's branches; drop #ifndef EDUP_LAUNCHER-only lines."""
    out = []
    keep = [True]
    for row in src.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        trimmed = row.lstrip(" \t")
        is_ifdef = trimmed.startswith("#ifdef")
        is_ifndef = trimmed.startswith("#ifndef")
        if is_ifdef or is_ifndef:
            launcher = "EDUP_LAUNCHER" in trimmed
            branch = True if not launcher else is_ifdef
            keep.append(keep[-1] and branch)
        elif trimmed.startswith("#else") and len(keep) > 1:
            keep[-1] = keep[-2] and not keep[-1]
        elif trimmed.startswith("#endif") and len(keep) > 1:
            keep.pop()
        elif keep[-1]:
            out.append(row)
    return "\n".join(out)


def _parse_cpp_wide_literal(src, i):
    if src[i:i + 2] != 'L"':
        return None, i
    i += 2
    out = []
    n = len(src)
    while i < n:
        c = src[i]
        i += 1
        if c == '"':
            return "".join(out), i
        if c != "\\" or i >= n:
            out.append(c)
            continue
        e = src[i]
        i += 1
        if e == "n":
            out.append("\n")
        elif e == "r":
            out.append("\r")
        elif e == "t":
            out.append("\t")
        elif e in ("\\", '"'):
            out.append(e)
        elif e in ("x", "u"):
            digits = []
            while len(digits) < 4 and i < n and src[i] in "0123456789abcdefABCDEF":
                digits.append(src[i])
                i += 1
            if digits:
                out.append(chr(int("".join(digits), 16)))
        elif e == "U":
            digits = []
            while len(digits) < 8 and i < n and src[i] in "0123456789abcdefABCDEF":
                digits.append(src[i])
                i += 1
            if digits:
                out.append(chr(int("".join(digits), 16)))
        else:
            out.append(e)
    return None, i


_UI_LINE_TOKENS = (
    "makeButton",
    "makeCheckbox",
    "setControlText",
    "drawText",
    ".label",
    ".value",
    "EM_SETCUEBANNER",
    "kScopeChoices",
    "say(",
    ".title",
    ".body",
)


def scan_ui_strings(root):
    """Return unique English UI strings found in the launcher C++ sources, in source order."""
    root = Path(root)
    found = []
    seen = set()
    for rel in UI_SOURCE_FILES:
        path = root / rel
        if not path.is_file():
            raise ValueError("missing launcher source file: %s" % rel.as_posix())
        src = _launcher_ifdef_source(_strip_cpp_comments(path.read_text(encoding="utf-8")))
        i = 0
        n = len(src)
        while i < n:
            if src[i:i + 2] != 'L"':
                i += 1
                continue
            text, at = _parse_cpp_wide_literal(src, i)
            if text is None:
                i += 1
                continue
            joined = [text]
            while True:
                look = at
                while look < n and src[look] in " \t\n\r":
                    look += 1
                if src[look:look + 2] != 'L"':
                    break
                more, new_at = _parse_cpp_wide_literal(src, look)
                if more is None:
                    break
                joined.append(more)
                at = new_at
            full = "".join(joined)
            pre = i
            while pre > 0 and src[pre - 1] in " \t\n\r":
                pre -= 1
            inside_tr = pre >= 3 and src[pre - 3:pre] == "tr("
            line_start = src.rfind("\n", 0, i) + 1
            line_end = src.find("\n", i)
            line = src[line_start: n if line_end == -1 else line_end]
            is_ui = inside_tr or any(tok in line for tok in _UI_LINE_TOKENS)
            if is_ui and any(("A" <= c <= "Z") or ("a" <= c <= "z") for c in full):
                if full not in seen:
                    seen.add(full)
                    found.append(full)
            i = at
    return found


# ---------------------------------------------------------------------------
# GNU gettext .pot / .po parser and writer (stdlib only)
# ---------------------------------------------------------------------------

def _normalize_key(text):
    """Normalize line endings for msgid lookup so LF-normalized exports match CRLF keys."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def _unescape_po_string(raw, path_label, line_no):
    out = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        i += 1
        if c == '"':
            raise ValueError("%s:%d: unescaped quote inside string" % (path_label, line_no))
        if c != "\\":
            out.append(c)
            continue
        if i >= n:
            raise ValueError("%s:%d: trailing backslash in string" % (path_label, line_no))
        e = raw[i]
        i += 1
        if e == "n":
            out.append("\n")
        elif e == "r":
            out.append("\r")
        elif e == "t":
            out.append("\t")
        elif e in ("\\", '"'):
            out.append(e)
        else:
            raise ValueError("%s:%d: unsupported escape \\%s" % (path_label, line_no, e))
    return "".join(out)


def _escape_po_string(text):
    out = []
    for c in text:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\r":
            out.append("\\r")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        else:
            out.append(c)
    return "".join(out)


def _parse_po_quoted(segment, path_label, line_no):
    s = segment.strip()
    if len(s) < 2 or not (s.startswith('"') and s.endswith('"')):
        raise ValueError("%s:%d: expected quoted string, got %a" % (path_label, line_no, s))
    return _unescape_po_string(s[1:-1], path_label, line_no)


def _read_catalog_text(path, rel_label):
    """Read raw catalog bytes, reject NUL bytes, decode UTF-8 (allowing leading BOM),
    and reject hidden/bidi/zero-width/tag characters via check_agent_material."""
    raw = Path(path).read_bytes()
    if b"\x00" in raw:
        raise ValueError("%s: contains NUL byte" % rel_label)
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError("%s: invalid UTF-8 (%s)" % (rel_label, exc))
    hits = check_agent_material.hidden_chars(text)
    if hits:
        points = sorted(set(cp for _, cp in hits))
        raise ValueError(
            "%s:%d: contains invisible/bidi character(s) (%s)"
            % (rel_label, hits[0][0], ", ".join("U+%04X" % cp for cp in points))
        )
    return text


def parse_po(text, path_label="<po>"):
    """Parse a .pot or .po file into a list of (msgid, msgstr, flags_set) entries.

    Header entry (msgid == "") is excluded from the returned entry list.
    Duplicate non-header msgids (after CRLF/LF normalization) raise ValueError.
    """
    if text.startswith("\ufeff"):
        text = text[1:]
    entries = []
    seen = set()
    flags = set()
    msgid = None
    msgstr = None
    field = None
    entry_line = 0

    def flush():
        nonlocal flags, msgid, msgstr, field, entry_line
        if msgid is None:
            flags = set()
            field = None
            return
        if msgstr is None:
            raise ValueError("%s:%d: msgid without msgstr" % (path_label, entry_line))
        key = "".join(msgid)
        val = "".join(msgstr)
        if key != "":
            norm = _normalize_key(key)
            if norm in seen:
                raise ValueError("%s:%d: duplicate msgid %a" % (path_label, entry_line, key))
            seen.add(norm)
            entries.append((key, val, set(flags)))
        flags = set()
        msgid = None
        msgstr = None
        field = None

    # Split strictly on CR/LF, not Python splitlines(), so U+2028/U+0085 cannot split a line.
    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    for line_no, raw_line in enumerate(lines, 1):
        line = raw_line.strip(" \t")
        if not line:
            flush()
            continue
        if line.startswith("#~"):
            # Obsolete gettext entry (commented out): ignore cleanly.
            continue
        if line.startswith("#,"):
            if msgid is not None:
                flush()
            for tok in line[2:].split(","):
                tok = tok.strip()
                if tok:
                    flags.add(tok)
            continue
        if line.startswith("#"):
            if msgid is not None:
                flush()
            continue
        if line.startswith("msgctxt "):
            raise ValueError("%s:%d: msgctxt is not supported by launcher_l10n" % (path_label, line_no))
        if line.startswith("msgid_plural "):
            raise ValueError("%s:%d: msgid_plural is not supported by launcher_l10n" % (path_label, line_no))
        if line.startswith("msgid "):
            if msgid is not None:
                flush()
            entry_line = line_no
            msgid = [_parse_po_quoted(line[6:], path_label, line_no)]
            field = "msgid"
            continue
        if line.startswith("msgstr "):
            if msgid is None:
                raise ValueError("%s:%d: msgstr without preceding msgid" % (path_label, line_no))
            msgstr = [_parse_po_quoted(line[7:], path_label, line_no)]
            field = "msgstr"
            continue
        if line.startswith('"'):
            part = _parse_po_quoted(line, path_label, line_no)
            if field == "msgid":
                msgid.append(part)
            elif field == "msgstr":
                msgstr.append(part)
            else:
                raise ValueError("%s:%d: unexpected continuation string" % (path_label, line_no))
            continue
        raise ValueError("%s:%d: unexpected line in catalog: %a" % (path_label, line_no, line))

    flush()
    return entries


def format_po(lang_code, native_name, entries):
    """Render a deterministic UTF-8 .pot or .po catalog with LF line endings."""
    is_pot = lang_code is None
    if is_pot:
        header_comment = "# ED-UP launcher localization template.\n# Managed with Crowdin (see crowdin.yml).\n"
        lang_header = "en"
    else:
        header_comment = "# ED-UP launcher localization (%s).\n# Managed with Crowdin (see crowdin.yml).\n" % native_name
        lang_header = lang_code

    lines = [
        header_comment + 'msgid ""',
        'msgstr ""',
        '"Project-Id-Version: ed-up-launcher\\n"',
        '"Content-Type: text/plain; charset=UTF-8\\n"',
        '"Content-Transfer-Encoding: 8bit\\n"',
        '"Language: %s\\n"' % lang_header,
        "",
    ]
    for msgid, msgstr in entries:
        if "%s" in msgid:
            lines.append("#, c-format")
        lines.append('msgid "%s"' % _escape_po_string(msgid))
        lines.append('msgstr "%s"' % _escape_po_string("" if is_pot else msgstr))
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Untrusted content checks, validation, and C++ table generation
# ---------------------------------------------------------------------------

def _scan_percent_tokens(text):
    """Return (valid_specs, invalid_tokens) for every '%' in text.

    Only literal '%s' conversions are permitted; any other '%' sequence ('%n',
    '%d', '%%', trailing '%', etc.) is recorded in invalid_tokens.
    """
    valid = []
    invalid = []
    i = 0
    n = len(text)
    while i < n:
        if text[i] != "%":
            i += 1
            continue
        if i + 1 < n and text[i + 1] == "s":
            valid.append("%s")
            i += 2
        elif i + 1 < n:
            invalid.append(text[i:i + 2])
            i += 2
        else:
            invalid.append("%")
            i += 1
    return valid, invalid


def _disallowed_control_points(text):
    """Sorted unique code points for disallowed control chars (NUL, C0 except tab/CR/LF, DEL, C1)."""
    bad = set()
    for ch in text:
        cp = ord(ch)
        if cp == 0 or (cp < 0x20 and ch not in ("\t", "\r", "\n")) or cp == 0x7F or (0x80 <= cp <= 0x9F):
            bad.add(cp)
    return sorted(bad)


def _check_string_safety(text, rel_label, key_for_msg):
    """Return list of safety errors for a parsed catalog string."""
    errs = []
    ctrl = _disallowed_control_points(text)
    if ctrl:
        errs.append(
            "%s: disallowed control character(s) (%s) for %a"
            % (rel_label, ", ".join("U+%04X" % cp for cp in ctrl), key_for_msg)
        )
    hidden = check_agent_material.hidden_chars(text)
    if hidden:
        points = sorted(set(cp for _, cp in hidden))
        errs.append(
            "%s: invisible/bidi character(s) (%s) for %a"
            % (rel_label, ", ".join("U+%04X" % cp for cp in points), key_for_msg)
        )
    if len(text) > MAX_TRANSLATION_CHARS:
        errs.append(
            "%s: translation exceeds %d-character limit (%d chars) for %a"
            % (rel_label, MAX_TRANSLATION_CHARS, len(text), key_for_msg)
        )
    return errs


def _match_line_endings(source, translated):
    """If the English msgid uses CRLF (for Win32 EDIT controls), normalize bare LF in translation to CRLF."""
    if "\r\n" in source and "\n" in translated:
        return translated.replace("\r\n", "\n").replace("\n", "\r\n")
    return translated


def _cpp_wide_literal(text):
    """Encode text as an ASCII-only C++ L"..." literal using \\uXXXX / \\UXXXXXXXX escapes."""
    out = ['L"']
    for ch in text:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif 0x20 <= code <= 0x7E:
            out.append(ch)
        elif code <= 0xFFFF:
            out.append("\\u%04x" % code)
        else:
            # Standard C++ universal-character-name: MSVC compiles \UXXXXXXXX to UTF-16
            # surrogate pairs cleanly, whereas \uD800..\uDFFF triggers C3850.
            out.append("\\U%08x" % code)
    out.append('"')
    return "".join(out)


def load_and_validate(root):
    """Validate src/installer/l10n/ against launcher C++ sources and return compiled table data.

    Returns (pot_keys, compiled_langs, warnings, errors) where:
      * pot_keys: list of English source strings in launcher.pot order
      * compiled_langs: list of (code, native_name, win_prefixes, translations_list_or_None)
        starting with ("en", "English", ("en",), None)
      * warnings: list of human-readable warning strings (e.g. core language strings falling back to English)
      * errors: list of human-readable error strings (empty when valid)
    """
    root = Path(root)
    l10n_dir = root / L10N_REL_DIR
    pot_path = l10n_dir / POT_NAME
    warnings = []
    errors = []

    if not pot_path.is_file():
        return [], [], [], ["missing template catalog: %s" % (L10N_REL_DIR / POT_NAME).as_posix()]

    pot_rel = pot_path.relative_to(root).as_posix()
    try:
        pot_text = _read_catalog_text(pot_path, pot_rel)
        pot_entries = parse_po(pot_text, pot_rel)
    except (OSError, ValueError) as exc:
        return [], [], [], [str(exc)]

    pot_keys = [msgid for msgid, _, _ in pot_entries]
    pot_set = set(pot_keys)
    if len(pot_keys) < 1:
        errors.append("%s: template catalog has no entries" % pot_rel)

    for key in pot_keys:
        errors.extend(_check_string_safety(key, pot_rel, key))
        _, bad_src_pct = _scan_percent_tokens(key)
        if bad_src_pct:
            errors.append("%s: unsupported percent token(s) %a in msgid %a" % (pot_rel, bad_src_pct, key))

    try:
        ui_strings = scan_ui_strings(root)
    except (OSError, ValueError) as exc:
        errors.append(str(exc))
        ui_strings = []

    for ui in ui_strings:
        if ui not in pot_set:
            errors.append(
                "UI string missing from %s (run `python tools/gen_launcher_l10n.py --root . --update-pot`): %a"
                % (pot_rel, ui)
            )

    # Check every .po file in src/installer/l10n/ is registered in KNOWN_LANGUAGES.
    po_files = {}
    for path in sorted(l10n_dir.glob("*.po")):
        code = path.stem
        if code == "en" or code not in KNOWN_BY_CODE:
            errors.append(
                "%s: unregistered language code %a (add its native name and Windows locale prefix to KNOWN_LANGUAGES in tools/gen_launcher_l10n.py)"
                % (path.relative_to(root).as_posix(), code)
            )
            continue
        po_files[code] = path

    compiled_langs = [("en", "English", ("en",), None)]

    for code, native_name, prefixes, is_core in KNOWN_LANGUAGES:
        if code == "en":
            continue
        path = po_files.get(code)
        if path is None:
            if is_core:
                errors.append("missing required translation catalog: %s" % (L10N_REL_DIR / (code + ".po")).as_posix())
            continue

        rel_label = path.relative_to(root).as_posix()
        try:
            po_text = _read_catalog_text(path, rel_label)
            po_entries = parse_po(po_text, rel_label)
        except (OSError, ValueError) as exc:
            errors.append(str(exc))
            continue

        # Validate every entry in the file (even keys not currently in pot) against safety checks.
        by_norm_key = {}
        for msgid, msgstr, flags in po_entries:
            by_norm_key[_normalize_key(msgid)] = (msgstr, flags)
            if "fuzzy" not in flags and msgstr != "":
                if msgstr.strip() == "":
                    errors.append("%s: whitespace-only translation for %a" % (rel_label, msgid))
                else:
                    errors.extend(_check_string_safety(msgstr, rel_label, msgid))

        translated_row = []
        translated_count = 0
        fallback_count = 0

        for key in pot_keys:
            item = by_norm_key.get(_normalize_key(key))
            if item is None:
                msgstr, flags = "", set()
            else:
                msgstr, flags = item
            is_fuzzy = "fuzzy" in flags
            effective = "" if is_fuzzy else msgstr

            if effective != "":
                if effective.strip() == "":
                    # Already recorded above; do not emit blank text.
                    translated_row.append(None)
                    continue
                expected_specs, _ = _scan_percent_tokens(key)
                actual_specs, bad_specs = _scan_percent_tokens(effective)
                if bad_specs or actual_specs != expected_specs:
                    errors.append(
                        "%s: placeholder mismatch for %a (expected %a, got %a, invalid %a in %a)"
                        % (rel_label, key, expected_specs, actual_specs, bad_specs, effective)
                    )
                if is_core and _normalize_key(effective) == _normalize_key(key) and not same_as_english(key, code):
                    errors.append("%s: translation copies English verbatim for %a" % (rel_label, key))
                translated_row.append(_match_line_endings(key, effective))
                translated_count += 1
            else:
                if is_core:
                    fallback_count += 1
                translated_row.append(None)

        if is_core and translated_count == 0:
            errors.append("%s: required catalog has no valid translations" % rel_label)
        elif is_core and fallback_count > 0:
            warnings.append(
                "%s: %d untranslated/fuzzy string(s) falling back to English"
                % (rel_label, fallback_count)
            )

        if is_core or translated_count > 0:
            compiled_langs.append((code, native_name, prefixes, translated_row))

    return pot_keys, compiled_langs, warnings, errors


def render_cpp_table(pot_keys, compiled_langs):
    """Render deterministic ASCII C++ include file for src/installer/launcher_l10n.cpp."""
    lines = [
        "// Generated by tools/gen_launcher_l10n.py from src/installer/l10n/. Do not edit by hand.",
        "",
        "const L10nLanguage kLanguages[] = {",
    ]
    for code, native_name, _, _ in compiled_langs:
        lines.append('    {"%s", %s},' % (code, _cpp_wide_literal(native_name)))
    lines.append("};")
    lines.append("")

    # Build system-locale prefix rules sorted longest-prefix-first so e.g. zh-hans matches before any shorter prefix.
    locale_rules = []
    for code, _, prefixes, _ in compiled_langs:
        if code == "en":
            continue
        for pfx in prefixes:
            locale_rules.append((pfx, code))
    locale_rules.sort(key=lambda item: (-len(item[0]), item[0], item[1]))

    lines.append("struct SystemLocaleRule {")
    lines.append("    const wchar_t* prefix;")
    lines.append("    size_t prefixLen;")
    lines.append("    const char* code;")
    lines.append("};")
    lines.append("")
    lines.append("const SystemLocaleRule kSystemLocaleRules[] = {")
    if not locale_rules:
        lines.append("    {nullptr, 0, nullptr},")
    else:
        for pfx, code in locale_rules:
            lines.append('    {%s, %d, "%s"},' % (_cpp_wide_literal(pfx), len(pfx), code))
    lines.append("};")
    lines.append("")

    lines.append("constexpr size_t kStringCount = %d;" % len(pot_keys))
    lines.append("const wchar_t* const kEnglish[kStringCount] = {")
    for key in pot_keys:
        lines.append("    %s," % _cpp_wide_literal(key))
    lines.append("};")
    lines.append("")

    non_en = [item for item in compiled_langs if item[0] != "en"]
    lines.append("const wchar_t* const kTranslations[%d][kStringCount] = {" % max(1, len(non_en)))
    if not non_en:
        lines.append("    {nullptr},")
    else:
        for code, _, _, row in non_en:
            lines.append("    {  // %s" % code)
            for entry in row:
                if entry is None:
                    lines.append("        nullptr,")
                else:
                    lines.append("        %s," % _cpp_wide_literal(entry))
            lines.append("    },")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def _write_if_changed(path, text, dry_run=False):
    path = Path(path)
    encoded = text.encode("utf-8")
    if path.is_file() and path.read_bytes() == encoded:
        return False
    if dry_run:
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(encoded)
    return True


def update_pot(root, dry_run=False):
    root = Path(root)
    pot_path = root / L10N_REL_DIR / POT_NAME
    existing_keys = []
    if pot_path.is_file():
        pot_rel = pot_path.relative_to(root).as_posix()
        existing_entries = parse_po(_read_catalog_text(pot_path, pot_rel), pot_rel)
        existing_keys = [k for k, _, _ in existing_entries]

    ui_strings = scan_ui_strings(root)
    existing_norm = {_normalize_key(k) for k in existing_keys}
    added = [s for s in ui_strings if _normalize_key(s) not in existing_norm]
    merged = existing_keys + added
    rendered = format_po(None, "English", [(k, "") for k in merged])
    changed = _write_if_changed(pot_path, rendered, dry_run=dry_run)
    if dry_run:
        if changed:
            print("gen_launcher_l10n: --dry-run would update %s (+%d string(s), %d total)"
                  % ((L10N_REL_DIR / POT_NAME).as_posix(), len(added), len(merged)))
        else:
            print("gen_launcher_l10n: %s is already up to date (%d strings)"
                  % ((L10N_REL_DIR / POT_NAME).as_posix(), len(merged)))
    else:
        print("gen_launcher_l10n: %s %s (+%d string(s), %d total)"
              % ("updated" if changed else "unchanged", (L10N_REL_DIR / POT_NAME).as_posix(), len(added), len(merged)))
    return 0


def run(root, out_dir=None, check_only=False, dry_run=False):
    pot_keys, compiled_langs, warnings, errors = load_and_validate(root)
    for warn in warnings:
        print("gen_launcher_l10n: WARNING: %s" % warn)
    if errors:
        for err in errors:
            print("gen_launcher_l10n: ERROR: %s" % err)
        return 1
    lang_list = ", ".join(code for code, _, _, _ in compiled_langs)
    if check_only:
        print("gen_launcher_l10n: OK (%d strings, %d compiled languages: %s)"
              % (len(pot_keys), len(compiled_langs), lang_list))
        return 0
    if not out_dir:
        print("gen_launcher_l10n: ERROR: --out is required unless --check or --update-pot is given")
        return 2
    rendered = render_cpp_table(pot_keys, compiled_langs)
    out_path = Path(out_dir) / TABLE_NAME
    changed = _write_if_changed(out_path, rendered, dry_run=dry_run)
    if dry_run:
        print("gen_launcher_l10n: --dry-run would %s %s (%d strings, %d languages: %s)"
              % ("write" if changed else "keep", out_path, len(pot_keys), len(compiled_langs), lang_list))
    else:
        print("gen_launcher_l10n: %s %s (%d strings, %d languages: %s)"
              % ("wrote" if changed else "up to date", out_path, len(pot_keys), len(compiled_langs), lang_list))
    return 0


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def self_test():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    def run_cli(argv):
        buf = io.StringIO()
        with redirect_stdout(buf), redirect_stderr(buf):
            rc = main(argv)
        return rc, buf.getvalue()

    base = Path(tempfile.mkdtemp(prefix="edup-launcher-l10n-"))
    try:
        # 1. PO round-trip with CRLF, quotes, backslashes, UTF-8, c-format, BOM, U+2028, and multiline continuation.
        sample_entries = [
            ("Setup", "Einrichtung"),
            ("Line 1\r\n\r\nLine 2 \"quoted\" \\path\\", "Zeile 1\r\n\r\nZeile 2 \"zitiert\" \\pfad\\"),
            ("ED-UP %s is available.", "ED-UP %s ist verfügbar."),
        ]
        po_text = format_po("de", "Deutsch", sample_entries)
        check("#, c-format" in po_text, "format_po marks %s strings with #, c-format")
        parsed = parse_po(po_text, "de.po")
        check([(k, v) for k, v, _ in parsed] == sample_entries, "parse_po round-trips format_po losslessly")

        # Leading BOM is accepted cleanly.
        bom_parsed = parse_po("\ufeff" + po_text, "bom.po")
        check([(k, v) for k, v, _ in bom_parsed] == sample_entries, "parse_po accepts leading UTF-8 BOM")

        # U+2028 in a comment does not split lines as splitlines() would.
        u2028_parsed = parse_po('# note\u2028still comment\nmsgid "A"\nmsgstr "B"\n', "u2028.po")
        check(u2028_parsed == [("A", "B", set())], "U+2028 does not split PO lines")

        multiline_po = (
            'msgid ""\n'
            'msgstr "Content-Type: text/plain; charset=UTF-8\\n"\n\n'
            '#, fuzzy\n'
            'msgid "Hello "\n'
            '"world"\n'
            'msgstr "Hallo "\n'
            '"Welt"\n'
        )
        mp = parse_po(multiline_po, "multi.po")
        check(mp == [("Hello world", "Hallo Welt", {"fuzzy"})], "multiline continuation and fuzzy flag parse")

        # Unescaped inner quote ("x" "y") must be rejected.
        inner_quote_failed = False
        try:
            parse_po('msgid "A"\nmsgstr "x" "y"\n', "badq.po")
        except ValueError:
            inner_quote_failed = True
        check(inner_quote_failed, "unescaped inner quote in msgstr is rejected")

        # Duplicate msgid (including CRLF vs LF normalized duplicate) must fail.
        dup_failed = False
        try:
            parse_po('msgid "A\\r\\nB"\nmsgstr "1"\n\nmsgid "A\\nB"\nmsgstr "2"\n', "dup.po")
        except ValueError:
            dup_failed = True
        check(dup_failed, "duplicate normalized msgid is rejected")

        # Non-BMP characters emit \UXXXXXXXX (valid MSVC universal-character-name, not \uD800..\uDFFF).
        emoji_lit = _cpp_wide_literal("Hi \U0001f600")
        check(emoji_lit == 'L"Hi \\U0001f600"', "non-BMP character encodes as \\U0001f600: %a" % emoji_lit)

        # 2. Full fixture repo for validation, single-writer fallback, untrusted checks, CLI routes, and determinism.
        repo = base / "repo"
        (repo / "src" / "installer" / "l10n").mkdir(parents=True)
        (repo / "src" / "installer" / "gui.cpp").write_text(
            'void f() {\n'
            '    // tr(L"Commented out")\n'
            '#ifndef EDUP_LAUNCHER\n'
            '    tr(L"Installer only");\n'
            '#endif\n'
            '    setControlText(1, L"Setup");\n'
            '    tr(L"VR");\n'
            '    tr(L"Line 1\\r\\n\\r\\nLine 2");\n'
            '}\n',
            encoding="utf-8",
        )
        (repo / "src" / "installer" / "launcher_gui.inc").write_text(
            'void g() { tr(L"Hello " L"world"); }\n',
            encoding="utf-8",
        )
        (repo / "src" / "installer" / "launcher_login_dialog.cpp").write_text(
            'void h() { tr(L"Code"); }\n',
            encoding="utf-8",
        )
        (repo / "src" / "installer" / "launcher_l10n.cpp").write_text(
            'void k() { tr(L"ED-UP %s is available."); }\n',
            encoding="utf-8",
        )

        keys = ["Setup", "VR", "Line 1\r\n\r\nLine 2", "Hello world", "Code", "ED-UP %s is available.", "Runtime only"]
        scanned = scan_ui_strings(repo)
        check(scanned == keys[:-1], "scanner skips comments/#ifndef EDUP_LAUNCHER and joins adjacent literals: %a" % (scanned,))

        (repo / L10N_REL_DIR / POT_NAME).write_text(format_po(None, "English", [(k, "") for k in keys]), encoding="utf-8")

        # Note: de.po uses LF-normalized msgid for the CRLF entry to verify Crowdin LF-normalized lookup matches CRLF pot_key.
        de_entries = [
            ("Setup", "Einrichtung"),
            ("VR", "VR"),
            ("Line 1\n\nLine 2", "Zeile 1\n\nZeile 2"),
            ("Hello world", "Hallo Welt"),
            ("Code", "Code"),
            ("ED-UP %s is available.", "ED-UP %s ist verfügbar."),
            ("Runtime only", "Nur Laufzeit"),
        ]
        # Write de.po with a leading UTF-8 BOM to verify BOM acceptance on disk.
        (repo / L10N_REL_DIR / "de.po").write_bytes(b"\xef\xbb\xbf" + format_po("de", "Deutsch", de_entries).encode("utf-8"))

        req_translations = {
            "es": ["Instalación", "RV", "Línea 1\r\n\r\nLínea 2", "Hola mundo", "Código", "ED-UP %s está disponible.", "Solo tiempo"],
            "fr": ["Installation", "RV", "Ligne 1\r\n\r\nLigne 2", "Bonjour monde", "Code", "ED-UP %s est disponible.", "Exécution"],
            "pt-BR": ["Instalação", "RV", "Linha 1\r\n\r\nLinha 2", "Olá mundo", "Código", "ED-UP %s está disponível.", "Tempo"],
            "ru": ["Установка", "VR", "Строка 1\r\n\r\nСтрока 2", "Привет мир", "Код", "ED-UP %s доступен.", "Время"],
        }
        for code, vals in req_translations.items():
            name = KNOWN_BY_CODE[code][0]
            (repo / L10N_REL_DIR / (code + ".po")).write_text(
                format_po(code, name, list(zip(keys, vals))), encoding="utf-8"
            )

        # Empty optional Crowdin language (0% translated) must NOT appear in compiled table.
        (repo / L10N_REL_DIR / "ja.po").write_text(
            format_po("ja", "日本語", [(k, "") for k in keys]), encoding="utf-8"
        )
        # Partially translated optional Crowdin language (with non-BMP emoji) MUST appear.
        (repo / L10N_REL_DIR / "zh-CN.po").write_text(
            format_po("zh-CN", "简体中文", [("Setup", "安装 \U0001f600"), ("ED-UP %s is available.", "ED-UP %s 可用。")]),
            encoding="utf-8",
        )

        # Drive all CLI routes through main().
        rc, out = run_cli(["--root", str(repo), "--check"])
        check(rc == 0 and "OK (" in out, "CLI --check passes: %a" % out)

        dry_out = base / "dry_out"
        rc, out = run_cli(["--root", str(repo), "--out", str(dry_out), "--dry-run"])
        check(rc == 0 and not dry_out.exists(), "CLI --out --dry-run exits 0 and creates no directory or file")

        gen_out = base / "gen_out"
        rc, out = run_cli(["--root", str(repo), "--out", str(gen_out)])
        check(rc == 0 and (gen_out / TABLE_NAME).is_file(), "CLI --out writes launcher_l10n_table.inc")
        generated = (gen_out / TABLE_NAME).read_text(encoding="utf-8")
        check(generated.isascii(), "generated C++ table is strictly ASCII")
        check('"zh-CN"' in generated, "partially translated optional language zh-CN is compiled")
        check('"ja"' not in generated, "0%-translated optional language ja is omitted")
        check('L"Zeile 1\\r\\n\\r\\nZeile 2"' in generated, "LF-normalized msgid in de.po matches CRLF pot key and emits CRLF")
        check('\\U0001f600' in generated and '\\ud83d' not in generated, "non-BMP emoji uses \\U0001f600 not surrogate escape")
        check("nullptr," in generated, "untranslated entry in partial optional language emits nullptr fallback")

        # Deterministic output across two runs.
        gen_out2 = base / "gen_out2"
        rc2, _ = run_cli(["--root", str(repo), "--out", str(gen_out2)])
        check(rc2 == 0 and (gen_out / TABLE_NAME).read_bytes() == (gen_out2 / TABLE_NAME).read_bytes(),
              "generated table is byte-for-byte deterministic")

        # 3. Single-writer model: missing or fuzzy entry in a core language warns and falls back to English (rc == 0).
        de_path = repo / L10N_REL_DIR / "de.po"
        orig_de = de_path.read_bytes()
        de_path.write_text(
            orig_de.decode("utf-8-sig").replace('msgid "Setup"', '#, fuzzy\nmsgid "Setup"', 1),
            encoding="utf-8",
        )
        _, compiled_fuzzy, warns_fuzzy, errs_fuzzy = load_and_validate(repo)
        check(not errs_fuzzy and any("de.po" in w and "1 untranslated/fuzzy" in w for w in warns_fuzzy),
              "fuzzy entry in core language warns instead of failing: warns=%a errs=%a" % (warns_fuzzy, errs_fuzzy))
        de_row = [row for c, _, _, row in compiled_fuzzy if c == "de"][0]
        check(de_row[0] is None, "fuzzy entry in core language emits nullptr English fallback")
        de_path.write_bytes(orig_de)

        # 4. Untrusted translation content checks.
        uk_path = repo / L10N_REL_DIR / "uk.po"

        # 4a. Placeholder attacks: %s %n, %%s, trailing %, missing %s, extra % in plain string.
        for bad_ph_val, label in (
            ("ED-UP %s %n", "%s %n"),
            ("ED-UP %%s", "%%s"),
            ("ED-UP %s 100%", "trailing %"),
            ("ED-UP без заповнювача", "missing %s"),
        ):
            uk_path.write_text(format_po("uk", "Українська", [("ED-UP %s is available.", bad_ph_val)]), encoding="utf-8")
            _, _, _, errs = load_and_validate(repo)
            check(any("placeholder mismatch" in e and "uk.po" in e for e in errs),
                  "bad placeholder (%s) rejected: %a" % (label, errs))
            # Error output must be strictly cp1252-encodable even when Cyrillic is present.
            for e in errs:
                try:
                    e.encode("cp1252")
                except UnicodeEncodeError:
                    check(False, "error not cp1252-safe: %a" % e)

        uk_path.write_text(format_po("uk", "Українська", [("Setup", "Налаштування %")]), encoding="utf-8")
        _, _, _, errs = load_and_validate(repo)
        check(any("placeholder mismatch" in e for e in errs), "stray trailing % in non-format string rejected")

        # 4b. NUL byte in file (including first 8 KB) rejected.
        uk_path.write_bytes(format_po("uk", "Українська", [("Setup", "Налаштування")]).encode("utf-8").replace(b"Setup", b"Set\x00up"))
        _, _, _, errs = load_and_validate(repo)
        check(any("NUL byte" in e for e in errs), "NUL byte in .po rejected: %a" % errs)

        # 4c. C0 control, DEL, C1 control, bidi override, zero-width space, non-leading BOM rejected.
        for bad_ch, label in (
            ("\x07", "C0 BEL"),
            ("\x7f", "DEL"),
            ("\x85", "C1 NEL"),
            ("\u202e", "bidi RLO"),
            ("\u200b", "zero-width space"),
            ("\ufeff", "mid-file BOM"),
        ):
            uk_path.write_text(format_po("uk", "Українська", [("Setup", "A" + bad_ch + "B")]), encoding="utf-8")
            _, _, _, errs = load_and_validate(repo)
            check(bool(errs), "disallowed character (%s) rejected" % label)

        # 4d. Whitespace-only msgstr rejected (required and optional).
        uk_path.write_text(format_po("uk", "Українська", [("Setup", "   \t ")]), encoding="utf-8")
        _, _, _, errs = load_and_validate(repo)
        check(any("whitespace-only" in e for e in errs), "whitespace-only translation rejected: %a" % errs)

        # 4e. Over-length msgstr rejected (prevents MSVC C2026).
        uk_path.write_text(format_po("uk", "Українська", [("Setup", "x" * (MAX_TRANSLATION_CHARS + 1))]), encoding="utf-8")
        _, _, _, errs = load_and_validate(repo)
        check(any("character limit" in e for e in errs), "over-length translation rejected: %a" % errs)

        uk_path.unlink()

        # 5. Unregistered .po filename must fail.
        zz_path = repo / L10N_REL_DIR / "zz.po"
        zz_path.write_text(format_po("zz", "Test", [("Setup", "X")]), encoding="utf-8")
        _, _, _, unk_errs = load_and_validate(repo)
        check(any("unregistered language code" in e and "zz.po" in e for e in unk_errs),
              "unregistered .po language fails validation")
        zz_path.unlink()

        # 6. Missing UI string in launcher.pot fails validation; --update-pot adds it to .pot only,
        # and then --check succeeds with warnings (no hand-editing of .po required!).
        gui_path = repo / "src" / "installer" / "gui.cpp"
        gui_path.write_text(gui_path.read_text(encoding="utf-8") + '\nvoid extra() { tr(L"Brand new label"); }\n', encoding="utf-8")
        _, _, _, miss_errs = load_and_validate(repo)
        check(any("Brand new label" in e for e in miss_errs), "missing UI string in launcher.pot fails validation")
        pot_before = (repo / L10N_REL_DIR / POT_NAME).read_bytes()
        rc, _ = run_cli(["--root", str(repo), "--update-pot", "--dry-run"])
        check(rc == 0 and (repo / L10N_REL_DIR / POT_NAME).read_bytes() == pot_before, "--update-pot --dry-run writes nothing")
        rc, _ = run_cli(["--root", str(repo), "--update-pot"])
        check(rc == 0, "--update-pot succeeds")
        pot_after = (repo / L10N_REL_DIR / POT_NAME).read_text(encoding="utf-8")
        check('msgid "Brand new label"' in pot_after and 'msgid "Runtime only"' in pot_after,
              "--update-pot appends new UI strings while preserving runtime-only strings")
        rc, out = run_cli(["--root", str(repo), "--check"])
        check(rc == 0 and "WARNING:" in out and "OK (" in out,
              "after --update-pot without editing .po, --check passes with fallback warnings: %a" % out)
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if failures:
        print("gen_launcher_l10n: self-test FAILED")
        for f in failures:
            print("  " + f)
        return 1
    print("gen_launcher_l10n: self-test OK")
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(errors="backslashreplace")
            except (OSError, ValueError):
                pass

    ap = argparse.ArgumentParser(description="Generate and validate the ED-UP launcher localization catalog.")
    ap.add_argument("--root", help="repository root directory")
    ap.add_argument("--out", help="output directory for generated launcher_l10n_table.inc")
    ap.add_argument("--check", action="store_true", help="validate catalogs without writing files")
    ap.add_argument("--update-pot", action="store_true", help="append any new UI strings to src/installer/l10n/launcher.pot")
    ap.add_argument("--dry-run", action="store_true", help="report what would be written without writing anything")
    ap.add_argument("--self-test", action="store_true", help="run fixture self-tests and exit")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.root:
        ap.error("--root is required")
    if args.update_pot:
        return update_pot(args.root, dry_run=args.dry_run)
    return run(args.root, out_dir=args.out, check_only=args.check, dry_run=args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
