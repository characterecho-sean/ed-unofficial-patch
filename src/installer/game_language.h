// Elite's language, the way min-ed-launcher chooses it (Cobra.getGameLang).
//
// A settings code is "en" or a language folder beside EDLaunch.exe (de, es, fr,
// pt-BR, ru). "en" needs no folder. Any other code is used only when that folder
// is there. The game argument is the GameLanguage string in LocalResources, which
// keeps the two backslashes the resource stores (English\\UK). An empty code means
// the Windows UI language, and a code this install does not have falls back to that.
#pragma once

#include <string>

namespace edvr::installer {

// "en", "de", "es", "fr", "pt-BR" or "ru". Anything else, including empty, is "en"
// only when `orEnglish` is set; otherwise it is returned unchanged and empty stays empty.
std::string canonicalGameLanguage(const std::string& code);

// The Windows UI language, as one of the codes Elite ships a folder for. Portuguese
// is pt-BR, which is the satellite Elite installs.
std::string systemGameLanguageCode();

// Maps any saved launcher language code to one of Elite's six supported codes
// ("en", "de", "es", "fr", "pt-BR", "ru"). Empty codes and launcher-only UI
// languages outside Elite's six fall back to systemGameLanguageCode().
std::string resolvedGameLanguageCode(const std::string& code);

// The name shown in the language control, in that language.
const wchar_t* gameLanguageName(const std::string& code);

// The first Name\\CC run in a LocalResources image, backslashes kept. Empty when
// the image has none.
std::string gameLanguageInBytes(const void* data, size_t size);

// The /language argument for `code` (empty code: the system language), read from
// `edLaunchDir` when that folder has the satellite, else the string Elite's
// resources carry for that language.
std::string gameLanguageArgument(const std::wstring& edLaunchDir, const std::string& code);

}  // namespace edvr::installer
