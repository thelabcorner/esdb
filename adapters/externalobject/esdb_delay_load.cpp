#if defined(_WIN32)

#include <windows.h>
#include <delayimp.h>

#include <cwchar>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

FARPROC WINAPI esdb_delay_load_hook(
    unsigned notification,
    PDelayLoadInfo info) noexcept {
    if (notification != dliNotePreLoadLibrary ||
        !info ||
        !info->szDll ||
        _stricmp(info->szDll, "ESDBCore.dll") != 0) {
        return nullptr;
    }

    wchar_t module_path[32768]{};
    const DWORD length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase),
        module_path,
        static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0])));
    if (length == 0u ||
        length >= static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0]))) {
        return nullptr;
    }

    wchar_t *separator = std::wcsrchr(module_path, L'\\');
    if (!separator) separator = std::wcsrchr(module_path, L'/');
    if (!separator) return nullptr;
    *(separator + 1) = L'\0';

    static const wchar_t core_name[] = L"ESDBCore.dll";
    const std::size_t prefix =
        static_cast<std::size_t>((separator + 1) - module_path);
    const std::size_t capacity = sizeof(module_path) / sizeof(module_path[0]);
    if (prefix + (sizeof(core_name) / sizeof(core_name[0])) > capacity) {
        return nullptr;
    }
    const std::size_t remaining = capacity - prefix;
    if (wcscpy_s(separator + 1, remaining, core_name) != 0) {
        return nullptr;
    }

    /*
     * ExternalObject's library search folders are not Windows DLL dependency
     * search paths. Resolve ESDBCore relative to the already-loaded adapter so
     * a private side-by-side ESDB deployment works without PATH mutation.
     */
    HMODULE module = LoadLibraryExW(
        module_path,
        nullptr,
        LOAD_WITH_ALTERED_SEARCH_PATH);
    return reinterpret_cast<FARPROC>(module);
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = esdb_delay_load_hook;

#endif
