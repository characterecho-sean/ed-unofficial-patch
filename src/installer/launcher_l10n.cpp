#include "launcher_l10n.h"

#include <windows.h>

#include <cstring>

namespace edvr::installer {
namespace {

#include "launcher_l10n_table.inc"

constexpr size_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

size_t g_langIndex = 0;
std::string g_code = "en";

size_t findLanguageIndex(const std::string& code) {
    if (code.empty()) return 0;
    for (size_t i = 0; i < kLanguageCount; ++i) {
        if (_stricmp(kLanguages[i].code, code.c_str()) == 0) return i;
    }
    return 0;
}

std::wstring widenUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::wstring fillVersion(const wchar_t* pattern, const std::wstring& version) {
    std::wstring text = pattern ? pattern : L"";
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, version);
    return text;
}

bool takeMarked(const std::string& line, const char* head, const char* tail, std::string* middle) {
    const size_t h = std::strlen(head);
    const size_t t = std::strlen(tail);
    if (line.size() < h + t || line.compare(0, h, head) != 0 || line.compare(line.size() - t, t, tail) != 0) return false;
    *middle = line.substr(h, line.size() - h - t);
    return true;
}

}  // namespace

const L10nLanguage* l10nLanguages(size_t* count) {
    if (count) *count = kLanguageCount;
    return kLanguages;
}

std::string l10nCanonicalCode(const std::string& code) {
    return kLanguages[findLanguageIndex(code)].code;
}

std::string l10nSystemCode() {
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) == 0) return "en";
    for (const SystemLocaleRule& rule : kSystemLocaleRules) {
        if (!rule.prefix || !rule.code) continue;
        if (_wcsnicmp(name, rule.prefix, rule.prefixLen) == 0 &&
            (name[rule.prefixLen] == L'\0' || name[rule.prefixLen] == L'-' || name[rule.prefixLen] == L'_')) {
            return rule.code;
        }
    }
    return "en";
}

void l10nSet(const std::string& code) {
    g_langIndex = findLanguageIndex(code);
    g_code = kLanguages[g_langIndex].code;
}

const std::string& l10nCode() { return g_code; }

bool l10nHasKey(const wchar_t* english) {
    if (!english) return false;
    for (size_t i = 0; i < kStringCount; ++i) {
        if (wcscmp(kEnglish[i], english) == 0) return true;
    }
    return false;
}

const wchar_t* tr(const wchar_t* english) {
    if (!english || g_langIndex == 0) return english;
    for (size_t i = 0; i < kStringCount; ++i) {
        if (wcscmp(kEnglish[i], english) == 0) {
            const wchar_t* translated = kTranslations[g_langIndex - 1][i];
            return (translated && *translated) ? translated : english;
        }
    }
    return english;
}

std::wstring trLine(const std::string& utf8) {
    std::string version;
    if (takeMarked(utf8, "ED-UP ", " is available.", &version))
        return fillVersion(tr(L"ED-UP %s is available."), widenUtf8(version));
    if (takeMarked(utf8, "ED-UP ", " package available for repair and updates.", &version))
        return fillVersion(tr(L"ED-UP %s package available for repair and updates."), widenUtf8(version));
    if (takeMarked(utf8, "ED-UP ", " is ready to install.", &version))
        return fillVersion(tr(L"ED-UP %s is ready to install."), widenUtf8(version));
    const std::wstring wide = widenUtf8(utf8);
    const wchar_t* hit = tr(wide.c_str());
    return hit == wide.c_str() ? wide : std::wstring(hit);
}

}  // namespace edvr::installer
