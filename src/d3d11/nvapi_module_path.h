#pragma once

#include <string_view>

namespace edvr::nvapi {

inline wchar_t pathFold(wchar_t c) noexcept {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

inline bool sameModulePath(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (pathFold(a[i]) != pathFold(b[i])) return false;
    }
    return true;
}

// Module filenames must be ordinary absolute Windows paths with unambiguous
// components before they can match a System32 or DriverStore location.
inline bool ordinaryModulePath(std::wstring_view path) noexcept {
    if (path.size() < 4 || pathFold(path[0]) < L'a' || pathFold(path[0]) > L'z' ||
        path[1] != L':' || path[2] != L'\\') return false;
    size_t start = 3;
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (i == start || path[i - 1] == L'.' || path[i - 1] == L' ') return false;
            start = i + 1;
            continue;
        }
        const auto c = path[i];
        if (c < 32 || c > 126 || c == L'/' || c == L':' || c == L'<' || c == L'>' ||
            c == L'"' || c == L'|' || c == L'?' || c == L'*') return false;
    }
    return true;
}

inline bool nvapiSystemStubPath(std::wstring_view systemDirectory, std::wstring_view stub) noexcept {
    constexpr std::wstring_view leaf = L"\\nvapi64.dll";
    return ordinaryModulePath(systemDirectory) && ordinaryModulePath(stub) &&
        stub.size() == systemDirectory.size() + leaf.size() &&
        sameModulePath(stub.substr(0, systemDirectory.size()), systemDirectory) &&
        sameModulePath(stub.substr(systemDirectory.size()), leaf);
}

// QueryInterface can resolve functions in the public stub (1) or the separate
// NVIDIA DriverStore implementation (2). Return 0 for other or mixed owners.
// These checks use the loader's module filenames and GetSystemDirectoryW.
inline unsigned nvapiOwnerPathKind(std::wstring_view systemDirectory, std::wstring_view stub,
                                  std::wstring_view firstOwner, std::wstring_view secondOwner) noexcept {
    if (!nvapiSystemStubPath(systemDirectory, stub) || !ordinaryModulePath(firstOwner) ||
        !sameModulePath(firstOwner, secondOwner)) return 0;
    if (sameModulePath(firstOwner, stub)) return 1;
    constexpr std::wstring_view prefix = L"\\driverstore\\filerepository\\";
    constexpr std::wstring_view package = L"nv_dispi.inf_amd64_";
    constexpr std::wstring_view leaf = L"\\nvapi64_impl.dll";
    const size_t begin = systemDirectory.size() + prefix.size();
    if (firstOwner.size() != begin + package.size() + 16 + leaf.size() ||
        !sameModulePath(firstOwner.substr(0, systemDirectory.size()), systemDirectory) ||
        !sameModulePath(firstOwner.substr(systemDirectory.size(), prefix.size()), prefix) ||
        !sameModulePath(firstOwner.substr(begin, package.size()), package) ||
        !sameModulePath(firstOwner.substr(begin + package.size() + 16), leaf)) return 0;
    for (size_t i = begin + package.size(); i < begin + package.size() + 16; ++i) {
        const auto c = pathFold(firstOwner[i]);
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return 0;
    }
    return 2;
}

}  // namespace edvr::nvapi
