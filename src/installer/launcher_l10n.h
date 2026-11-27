// The launcher's own words, in the language the top control has selected.
// English is the key. A missing translation stays English.
//
// SECURITY CONTRACT: strings returned by tr() and trLine() come from external
// translation catalogs (src/installer/l10n/*.po). Never pass them as a
// printf-family format string; any placeholder substitution must use literal
// find/replace only.
#pragma once

#include <cstddef>
#include <string>

namespace edvr::installer {

struct L10nLanguage {
    const char* code;
    const wchar_t* name;
};

// Compiled launcher UI languages, starting with "en" followed by every translated
// locale under src/installer/l10n/. Returned pointers have static storage duration.
const L10nLanguage* l10nLanguages(size_t* count);

// Canonical code among compiled launcher UI languages (case-insensitive), or "en".
std::string l10nCanonicalCode(const std::string& code);

// Windows UI language mapped to a compiled launcher UI language, or "en".
std::string l10nSystemCode();

void l10nSet(const std::string& code);
const std::string& l10nCode();

// True when `english` is present in the compiled launcher.pot key table.
bool l10nHasKey(const wchar_t* english);

// The translation of `english` for the selected language. The pointer stays
// valid until the next l10nSet, and is `english` itself when that is what shows.
// Never pass the returned pointer as a printf-family format string.
const wchar_t* tr(const wchar_t* english);

// A launcher status line. Fixed English sentences are translated whole. "ED-UP <version> is available."
// and "ED-UP <version> is ready to install." keep the version and translate the rest.
// Never pass the returned text as a printf-family format string.
std::wstring trLine(const std::string& utf8);

}  // namespace edvr::installer
