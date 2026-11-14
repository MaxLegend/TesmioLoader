#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "tesmio_api.h"

namespace tsm
{
inline constexpr wchar_t kWorkshopSetting[] = L"workshop_path";
inline constexpr wchar_t kWorkshopSubdir[] = L"workshop\\content\\784150";

inline std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf)
{
    return dir + (!dir.empty() && dir.back() == L'\\' ? L"" : L"\\") + leaf;
}

inline std::wstring Parent(const std::wstring& path)
{
    const auto end = path.find_last_of(L"\\");
    return end == std::wstring::npos ? L"" : path.substr(0, end);
}

inline std::wstring ModuleDir(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(module, path, _countof(path));
    return len && len < _countof(path) ? Parent(path) : L"";
}

inline std::wstring WorkshopDefault(std::wstring dir)
{
    while (!dir.empty())
    {
        const auto slash = dir.find_last_of(L'\\');
        if (_wcsicmp(dir.c_str() + (slash == std::wstring::npos ? 0 : slash + 1),
                      L"steamapps") == 0)
            return JoinPath(dir, kWorkshopSubdir);
        dir = Parent(dir);
    }
    return {};
}

inline std::wstring ReadWorkshopOverride(const wchar_t* ini)
{
    wchar_t value[MAX_PATH];
    GetPrivateProfileStringW(L"tesmioloader", kWorkshopSetting, L"",
                             value, _countof(value), ini);
    return value;
}

struct PluginFile
{
    std::wstring path;
    std::wstring file;
    std::wstring key;

    std::wstring IniPath() const { return path.substr(0, path.size() - 4) + L".ini"; }
};

// Read the export table as file data. Scanning must never run DllMain,
// resolve dependencies or call a function from a Workshop DLL.
class PeExports
{
public:
    PeExports(const BYTE* data, size_t size) : data_(data), size_(size) {}

    bool IsPlugin()
    {
        const auto* dos = At<IMAGE_DOS_HEADER>(0);
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
        const size_t ntOffset = dos->e_lfanew;
        const auto* nt = At<IMAGE_NT_HEADERS64>(ntOffset);
        if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;

        sectionCount_ = nt->FileHeader.NumberOfSections;
        sections_ = At<IMAGE_SECTION_HEADER>(ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
                                            + nt->FileHeader.SizeOfOptionalHeader, sectionCount_);
        if (!sections_) return false;
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        const auto* exp = FromRva<IMAGE_EXPORT_DIRECTORY>(dir.VirtualAddress, 1);
        if (!exp || dir.Size < sizeof(*exp)) return false;
        const auto* names = FromRva<DWORD>(exp->AddressOfNames, exp->NumberOfNames);
        const auto* ordinals = FromRva<WORD>(exp->AddressOfNameOrdinals, exp->NumberOfNames);
        const auto* functions = FromRva<DWORD>(exp->AddressOfFunctions, exp->NumberOfFunctions);
        if (!names || !ordinals || !functions) return false;

        bool version = false, init = false;
        for (DWORD i = 0; i < exp->NumberOfNames; ++i)
        {
            if (ordinals[i] >= exp->NumberOfFunctions || !functions[ordinals[i]]) continue;
            if (Matches(names[i], TSM_EXPORT_APIVERSION)) version = true;
            if (Matches(names[i], TSM_EXPORT_INIT)) init = true;
            if (version && init) return true;
        }
        return false;
    }

private:
    template<class T> const T* At(size_t offset, size_t count = 1) const
    {
        if (offset > size_ || count > (size_ - offset) / sizeof(T)) return nullptr;
        return reinterpret_cast<const T*>(data_ + offset);
    }

    template<class T> const T* FromRva(DWORD rva, size_t count) const
    {
        if (!rva || !count) return nullptr;
        for (WORD i = 0; i < sectionCount_; ++i)
        {
            const auto& section = sections_[i];
            if (rva < section.VirtualAddress) continue;
            const DWORD delta = rva - section.VirtualAddress;
            if (delta >= section.SizeOfRawData ||
                count > (section.SizeOfRawData - delta) / sizeof(T)) continue;
            return At<T>(size_t(section.PointerToRawData) + delta, count);
        }
        return nullptr;
    }

    template<size_t N> bool Matches(DWORD rva, const char (&name)[N]) const
    {
        const auto* candidate = FromRva<char>(rva, N);
        return candidate && memcmp(candidate, name, N) == 0;
    }

    const BYTE* data_;
    size_t size_;
    const IMAGE_SECTION_HEADER* sections_ = nullptr;
    WORD sectionCount_ = 0;
};

inline bool HasPluginExports(const wchar_t* path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    HANDLE mapping = GetFileSizeEx(file, &size) && size.QuadPart > 0
        ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
    CloseHandle(file);
    if (!mapping) return false;
    const auto* bytes = static_cast<const BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!bytes) return false;
    bool plugin = false;
    __try { plugin = PeExports(bytes, static_cast<size_t>(size.QuadPart)).IsPlugin(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { plugin = false; }
    UnmapViewOfFile(bytes);
    return plugin;
}

inline void ScanDirectory(const std::wstring& root, bool recursive,
                          std::vector<PluginFile>& plugins)
{
    if (root.empty()) return;
    std::vector<std::wstring> pending{root};
    while (!pending.empty())
    {
        std::wstring dir = std::move(pending.back());
        pending.pop_back();
        WIN32_FIND_DATAW entry;
        HANDLE find = FindFirstFileW(JoinPath(dir, L"*").c_str(), &entry);
        if (find == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            const wchar_t* name = entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (recursive && wcscmp(name, L".") != 0 && wcscmp(name, L"..") != 0)
                    pending.push_back(JoinPath(dir, name));
                continue;
            }
            const wchar_t* dot = wcsrchr(name, L'.');
            if (!dot || dot == name || _wcsicmp(dot, L".dll") != 0) continue;
            auto path = JoinPath(dir, name);
            if (HasPluginExports(path.c_str()))
                plugins.push_back({std::move(path), name, std::wstring(name, dot)});
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
}

inline std::vector<PluginFile> DiscoverPlugins(const std::wstring& local,
                                                const std::wstring& workshop)
{
    std::vector<PluginFile> plugins;
    ScanDirectory(local, false, plugins);
    const auto localCount = plugins.size();
    ScanDirectory(workshop, true, plugins);
    const auto byKey = [](const PluginFile& a, const PluginFile& b) {
        const int key = _wcsicmp(a.key.c_str(), b.key.c_str());
        return key ? key < 0 : a.path < b.path;
    };
    // Local copies win ties. Within Workshop the full path breaks ties
    // deterministically, regardless of directory enumeration order.
    std::sort(plugins.begin(), plugins.begin() + localCount, byKey);
    std::sort(plugins.begin() + localCount, plugins.end(), byKey);
    std::inplace_merge(plugins.begin(), plugins.begin() + localCount, plugins.end(),
                       [](const PluginFile& a, const PluginFile& b) {
                           return _wcsicmp(a.key.c_str(), b.key.c_str()) < 0;
                       });
    plugins.erase(std::unique(plugins.begin(), plugins.end(), [](const PluginFile& a, const PluginFile& b) {
        return _wcsicmp(a.key.c_str(), b.key.c_str()) == 0;
    }), plugins.end());
    return plugins;
}
}
