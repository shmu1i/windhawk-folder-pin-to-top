// ==WindhawkMod==
// @id              folder-pin-to-top
// @name            Pin Folders to Top in File Explorer
// @description     Right-click any file or folder in File Explorer to pin/unpin it to the top of its parent. Pinned items report a future Date Modified so they sort/group to the top.
// @version         0.5
// @author          shmu1i
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lshlwapi -lshell32 -luuid -ladvapi32 -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Pin Folders to Top in File Explorer

Right-click any file or folder in File Explorer and pick **"📌 Pin to top"**.
Right-click it again to **"📌 Unpin from top"**. Pinned items appear at the
top of their parent folder in views sorted or grouped by Date Modified.

Designed for Windows 11's new XAML File Explorer.

## How it works

The mod rewrites the timestamps a pinned item appears to have, so the shell
treats the pinned item as though it were modified about a week in the future.
With Date Modified sorted/grouped in descending order (the default for
Downloads), this lands the pinned item at the top.

Pin state is stored in the registry under
`HKCU\Software\WindhawkFolderPin\Pins` (REG_MULTI_SZ). No Windhawk setting to
edit — the context menu is the only UI.

Three hook surfaces:

1. **`NtQueryDirectoryFile` / `NtQueryDirectoryFileEx`** in `ntdll.dll`
   rewrites timestamps in the kernel-level directory enumeration, beating any
   shell-level cache. This is what actually moves pinned items in the view.
2. **`TrackPopupMenuEx`** in `user32.dll` injects the Pin/Unpin item into
   Explorer's classic context menu. On Windows 11 with the modern context
   menu enabled by default, the item appears under "Show more options". If
   you've made the classic menu the default (e.g. via the "Classic context
   menu on Windows 11" mod), the item appears directly.
3. **`CFSFolder::GetDetailsEx`** in `windows.storage.dll` is also hooked as
   belt-and-suspenders for any shell callers that re-fetch timestamps after
   enumeration.

## Visible side effect

The Date Modified column shows a date ~7 days in the future for pinned items.
This is intrinsic to the mechanism.

## Not covered

- Sorting/grouping by Name, Size, Type — pinned items appear in their natural
  position there.
- Ascending sort by Date — future dates are "largest", so ASC puts them last.
- Virtual locations (Home, Quick Access, Gallery, libraries).
- Items not directly inside the folder you're viewing.

## Building / installing

Open Windhawk → Create new mod → paste this file → Compile (Ctrl+B) → Save
and enable. Pin state survives mod reloads (stored in the registry).
*/
// ==/WindhawkModReadme==

#include <windhawk_utils.h>

#include <initguid.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propkey.h>
#include <oleauto.h>
#include <winternl.h>
#include <exdisp.h>
#include <shldisp.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ============================================================================
// Pin state (in memory + registry)
// ============================================================================

constexpr const wchar_t* kRegSubKey = L"Software\\WindhawkFolderPin";
constexpr const wchar_t* kRegValueName = L"Pins";

std::mutex g_pinsMutex;
// parent path (lowercase, no trailing slash) -> set of child names (lowercase)
std::unordered_map<std::wstring, std::unordered_set<std::wstring>> g_pinsByParent;
// All pinned items as absolute lowercase paths, for serialization.
std::unordered_set<std::wstring> g_pinsFullPaths;
std::atomic<bool> g_havePins{false};

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

std::wstring NormalizePath(const wchar_t* raw) {
    std::wstring s(raw ? raw : L"");
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/' || s.back() == L' '))
        s.pop_back();
    return ToLower(std::move(s));
}

// Split a full path into (parent, child). Returns false if no separator.
bool SplitPath(const std::wstring& full, std::wstring& parent, std::wstring& child) {
    size_t sep = full.find_last_of(L'\\');
    if (sep == std::wstring::npos || sep + 1 >= full.size()) return false;
    parent = full.substr(0, sep);
    child = full.substr(sep + 1);
    return !parent.empty() && !child.empty();
}

// Re-derive g_pinsByParent and g_havePins from g_pinsFullPaths. Caller holds g_pinsMutex.
void RebuildIndex_locked() {
    g_pinsByParent.clear();
    for (const auto& full : g_pinsFullPaths) {
        std::wstring parent, child;
        if (SplitPath(full, parent, child)) {
            g_pinsByParent[parent].insert(child);
        }
    }
    g_havePins.store(!g_pinsFullPaths.empty());
}

void LoadPinsFromRegistry() {
    std::lock_guard<std::mutex> lk(g_pinsMutex);
    g_pinsFullPaths.clear();

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        RebuildIndex_locked();
        Wh_Log(L"No pin registry key yet");
        return;
    }

    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(hKey, kRegValueName, nullptr, &type, nullptr, &cb) == ERROR_SUCCESS &&
        type == REG_MULTI_SZ && cb >= sizeof(wchar_t)) {
        std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(hKey, kRegValueName, nullptr, &type,
                             (LPBYTE)buf.data(), &cb) == ERROR_SUCCESS) {
            const wchar_t* p = buf.data();
            const wchar_t* end = p + (cb / sizeof(wchar_t));
            while (p < end && *p) {
                std::wstring entry = NormalizePath(p);
                if (!entry.empty()) g_pinsFullPaths.insert(entry);
                p += wcslen(p) + 1;
            }
        }
    }
    RegCloseKey(hKey);
    RebuildIndex_locked();
    Wh_Log(L"Loaded %zu pin(s) from registry", g_pinsFullPaths.size());
}

// Caller holds g_pinsMutex.
void SavePinsToRegistry_locked() {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        Wh_Log(L"Failed to open registry for write");
        return;
    }
    // Build double-null-terminated string.
    std::wstring buf;
    for (const auto& p : g_pinsFullPaths) {
        buf.append(p);
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0');  // final null terminator (REG_MULTI_SZ requires double-null at end)
    RegSetValueExW(hKey, kRegValueName, 0, REG_MULTI_SZ,
                   (const BYTE*)buf.data(),
                   (DWORD)(buf.size() * sizeof(wchar_t)));
    RegCloseKey(hKey);
    Wh_Log(L"Wrote %zu pin(s) to registry", g_pinsFullPaths.size());
}

bool IsPathPinned(const std::wstring& fullPathLower) {
    std::lock_guard<std::mutex> lk(g_pinsMutex);
    return g_pinsFullPaths.count(fullPathLower) > 0;
}

// Returns the new pinned state.
bool TogglePin(const std::wstring& fullPathLower) {
    std::lock_guard<std::mutex> lk(g_pinsMutex);
    bool wasPinned = g_pinsFullPaths.count(fullPathLower) > 0;
    if (wasPinned) g_pinsFullPaths.erase(fullPathLower);
    else g_pinsFullPaths.insert(fullPathLower);
    RebuildIndex_locked();
    SavePinsToRegistry_locked();
    return !wasPinned;
}

void SetPin(const std::wstring& fullPathLower, bool pinned) {
    std::lock_guard<std::mutex> lk(g_pinsMutex);
    bool was = g_pinsFullPaths.count(fullPathLower) > 0;
    if (was == pinned) return;
    if (pinned) g_pinsFullPaths.insert(fullPathLower);
    else g_pinsFullPaths.erase(fullPathLower);
    RebuildIndex_locked();
    SavePinsToRegistry_locked();
}

bool LookupPinSet(const std::wstring& parentPath, std::unordered_set<std::wstring>& out) {
    std::lock_guard<std::mutex> lk(g_pinsMutex);
    auto it = g_pinsByParent.find(parentPath);
    if (it == g_pinsByParent.end() || it->second.empty()) return false;
    out = it->second;
    return true;
}

// ============================================================================
// Path / shell helpers
// ============================================================================

bool TryGetParentPath(IShellFolder* psf, std::wstring& out) {
    IPersistFolder2* ppf = nullptr;
    if (FAILED(psf->QueryInterface(IID_IPersistFolder2, (void**)&ppf)) || !ppf) return false;
    LPITEMIDLIST pidl = nullptr;
    HRESULT hr = ppf->GetCurFolder(&pidl);
    ppf->Release();
    if (FAILED(hr) || !pidl) return false;
    wchar_t buf[MAX_PATH];
    BOOL ok = SHGetPathFromIDListW(pidl, buf);
    CoTaskMemFree(pidl);
    if (!ok) return false;
    out = NormalizePath(buf);
    return !out.empty();
}

bool TryGetChildName(IShellFolder* psf, PCUITEMID_CHILD pidl, std::wstring& out) {
    STRRET sr{};
    if (FAILED(psf->GetDisplayNameOf(pidl, SHGDN_INFOLDER | SHGDN_FORPARSING, &sr))) return false;
    wchar_t buf[MAX_PATH];
    if (FAILED(StrRetToBufW(&sr, pidl, buf, ARRAYSIZE(buf)))) return false;
    out = ToLower(buf);
    return !out.empty();
}

bool TryGetHandlePath(HANDLE h, std::wstring& out) {
    wchar_t buf[MAX_PATH + 16] = {};
    DWORD len = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= ARRAYSIZE(buf)) return false;
    const wchar_t* p = buf;
    if (len > 4 && wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    out = NormalizePath(p);
    return !out.empty();
}

// ============================================================================
// Time spoofing
// ============================================================================

// Computed fresh on every call (not cached): the pinned timestamp must always
// be ~7 days ahead of *now*, otherwise a long-running explorer.exe eventually
// outlives a once-cached value and pinned items sink back into the listing.
DATE GetPinnedDateModified() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    DATE d = 0;
    if (SystemTimeToVariantTime(&st, &d)) return d + 7.0;
    return 73050.0;
}

FILETIME GetPinnedFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ull;
    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    ull.QuadPart += 7ULL * 24 * 60 * 60 * 10000000ULL;
    ft.dwLowDateTime = ull.LowPart;
    ft.dwHighDateTime = ull.HighPart;
    return ft;
}

static const PROPERTYKEY PKEY_FindData = {
    {0x28636AA6, 0x953D, 0x11D2,
     {0xB5, 0xD6, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0xD0}},
    0};

// ============================================================================
// NtQueryDirectoryFile{Ex} hooks -- primary mechanism for moving pinned items
// ============================================================================

typedef NTSTATUS(NTAPI* NtQueryDirectoryFile_t)(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
    FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);
NtQueryDirectoryFile_t NtQueryDirectoryFile_Original;

typedef NTSTATUS(NTAPI* NtQueryDirectoryFileEx_t)(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
    FILE_INFORMATION_CLASS, ULONG, PUNICODE_STRING);
NtQueryDirectoryFileEx_t NtQueryDirectoryFileEx_Original;

template <typename T>
void RewriteTimestamps(BYTE* base, ULONG_PTR bytesReturned,
                       const std::unordered_set<std::wstring>& pinSet,
                       const LARGE_INTEGER& ft) {
    BYTE* cur = base;
    BYTE* end = base + bytesReturned;
    while (cur >= base && cur < end) {
        auto* entry = reinterpret_cast<T*>(cur);
        ULONG nameLen = entry->FileNameLength / sizeof(WCHAR);
        if (nameLen > 0) {
            std::wstring lower(entry->FileName, nameLen);
            CharLowerBuffW(lower.data(), (DWORD)lower.size());
            if (pinSet.count(lower)) {
                entry->CreationTime = ft;
                entry->LastAccessTime = ft;
                entry->LastWriteTime = ft;
                entry->ChangeTime = ft;
            }
        }
        if (entry->NextEntryOffset == 0) break;
        cur += entry->NextEntryOffset;
    }
}

void ProcessDirectoryListing(HANDLE FileHandle, PVOID FileInformation,
                             FILE_INFORMATION_CLASS fic, ULONG_PTR bytesReturned) {
    if (!g_havePins.load() || !FileInformation || bytesReturned == 0) return;

    std::wstring parent;
    if (!TryGetHandlePath(FileHandle, parent)) return;

    std::unordered_set<std::wstring> pinSet;
    if (!LookupPinSet(parent, pinSet)) return;

    FILETIME pinned = GetPinnedFileTime();
    LARGE_INTEGER li;
    li.LowPart = pinned.dwLowDateTime;
    li.HighPart = pinned.dwHighDateTime;

    auto* base = static_cast<BYTE*>(FileInformation);
    switch (fic) {
        case FileDirectoryInformation:
            RewriteTimestamps<FILE_DIRECTORY_INFORMATION>(base, bytesReturned, pinSet, li);
            break;
        case FileFullDirectoryInformation:
            RewriteTimestamps<FILE_FULL_DIR_INFORMATION>(base, bytesReturned, pinSet, li);
            break;
        case FileBothDirectoryInformation:
            RewriteTimestamps<FILE_BOTH_DIR_INFORMATION>(base, bytesReturned, pinSet, li);
            break;
        case FileIdBothDirectoryInformation:
            RewriteTimestamps<FILE_ID_BOTH_DIR_INFORMATION>(base, bytesReturned, pinSet, li);
            break;
        case FileIdFullDirectoryInformation:
            RewriteTimestamps<FILE_ID_FULL_DIR_INFORMATION>(base, bytesReturned, pinSet, li);
            break;
        default:
            return;
    }
}

NTSTATUS NTAPI NtQueryDirectoryFile_Hook(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS fic, BOOLEAN ReturnSingleEntry,
    PUNICODE_STRING FileName, BOOLEAN RestartScan) {
    NTSTATUS s = NtQueryDirectoryFile_Original(
        FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
        FileInformation, Length, fic, ReturnSingleEntry, FileName, RestartScan);
    if (NT_SUCCESS(s) && IoStatusBlock && FileInformation) {
        ProcessDirectoryListing(FileHandle, FileInformation, fic, IoStatusBlock->Information);
    }
    return s;
}

NTSTATUS NTAPI NtQueryDirectoryFileEx_Hook(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
    FILE_INFORMATION_CLASS fic, ULONG QueryFlags, PUNICODE_STRING FileName) {
    NTSTATUS s = NtQueryDirectoryFileEx_Original(
        FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
        FileInformation, Length, fic, QueryFlags, FileName);
    if (NT_SUCCESS(s) && IoStatusBlock && FileInformation) {
        ProcessDirectoryListing(FileHandle, FileInformation, fic, IoStatusBlock->Information);
    }
    return s;
}

// ============================================================================
// CFSFolder::GetDetailsEx hook (belt-and-suspenders)
// ============================================================================

using CFSFolder_GetDetailsEx_t = HRESULT(WINAPI*)(void*, PCUITEMID_CHILD,
                                                  const PROPERTYKEY*, VARIANT*);
CFSFolder_GetDetailsEx_t CFSFolder_GetDetailsEx_Original;

HRESULT WINAPI CFSFolder_GetDetailsEx_Hook(void* pThis, PCUITEMID_CHILD pidl,
                                           const PROPERTYKEY* scid, VARIANT* value) {
    HRESULT hr = CFSFolder_GetDetailsEx_Original(pThis, pidl, scid, value);
    if (FAILED(hr) || !pThis || !pidl || !scid || !value || !g_havePins.load()) return hr;

    auto* psf = reinterpret_cast<IShellFolder*>(pThis);
    std::wstring parentPath;
    if (!TryGetParentPath(psf, parentPath)) return hr;

    std::unordered_set<std::wstring> pinSet;
    if (!LookupPinSet(parentPath, pinSet)) return hr;

    std::wstring name;
    if (!TryGetChildName(psf, pidl, name)) return hr;
    if (!pinSet.count(name)) return hr;

    if (IsEqualPropertyKey(*scid, PKEY_DateModified) && value->vt == VT_DATE) {
        value->date = GetPinnedDateModified();
    } else if (IsEqualPropertyKey(*scid, PKEY_FindData) &&
               value->vt == (VT_ARRAY | VT_UI1) && value->parray) {
        BYTE* pData = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(value->parray, (void**)&pData))) {
            LONG lower = 0, upper = 0;
            SafeArrayGetLBound(value->parray, 1, &lower);
            SafeArrayGetUBound(value->parray, 1, &upper);
            size_t bytes = (size_t)(upper - lower + 1);
            if (bytes >= sizeof(WIN32_FIND_DATAW) && pData) {
                auto* fd = reinterpret_cast<WIN32_FIND_DATAW*>(pData);
                FILETIME ft = GetPinnedFileTime();
                fd->ftLastWriteTime = ft;
                fd->ftCreationTime = ft;
                fd->ftLastAccessTime = ft;
            }
            SafeArrayUnaccessData(value->parray);
        }
    }
    return hr;
}

// ============================================================================
// Selected-files retrieval via IShellWindows automation
// Adapted from remove-context-menu-items.wh.cpp's GetSelectedFilesViaAutomation.
// ============================================================================

std::vector<std::wstring> GetSelectedFilesAtCursor() {
    std::vector<std::wstring> files;

    POINT cursorPos;
    GetCursorPos(&cursorPos);
    HWND hwndAtCursor = WindowFromPoint(cursorPos);
    HWND hwndTopLevel = hwndAtCursor;
    while (hwndTopLevel && GetParent(hwndTopLevel)) {
        hwndTopLevel = GetParent(hwndTopLevel);
    }
    if (!hwndTopLevel) return files;

    IShellWindows* pShellWindows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_IShellWindows, (void**)&pShellWindows)) ||
        !pShellWindows) {
        return files;
    }

    long count = 0;
    pShellWindows->get_Count(&count);
    for (long i = 0; i < count; i++) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;

        IDispatch* pDisp = nullptr;
        if (SUCCEEDED(pShellWindows->Item(v, &pDisp)) && pDisp) {
            IWebBrowserApp* pWeb = nullptr;
            if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowserApp, (void**)&pWeb)) && pWeb) {
                HWND hwnd = nullptr;
                pWeb->get_HWND((SHANDLE_PTR*)&hwnd);
                if (hwnd == hwndTopLevel) {
                    IDispatch* pDoc = nullptr;
                    if (SUCCEEDED(pWeb->get_Document(&pDoc)) && pDoc) {
                        IShellFolderViewDual* pView = nullptr;
                        if (SUCCEEDED(pDoc->QueryInterface(IID_IShellFolderViewDual,
                                                           (void**)&pView)) &&
                            pView) {
                            FolderItems* pItems = nullptr;
                            if (SUCCEEDED(pView->SelectedItems(&pItems)) && pItems) {
                                long itemCount = 0;
                                pItems->get_Count(&itemCount);
                                for (long j = 0; j < itemCount; j++) {
                                    VARIANT vIdx;
                                    VariantInit(&vIdx);
                                    vIdx.vt = VT_I4;
                                    vIdx.lVal = j;
                                    FolderItem* pItem = nullptr;
                                    if (SUCCEEDED(pItems->Item(vIdx, &pItem)) && pItem) {
                                        BSTR path = nullptr;
                                        if (SUCCEEDED(pItem->get_Path(&path)) && path) {
                                            files.emplace_back(path);
                                            SysFreeString(path);
                                        }
                                        pItem->Release();
                                    }
                                }
                                pItems->Release();
                            }
                            pView->Release();
                        }
                        pDoc->Release();
                    }
                }
                pWeb->Release();
            }
            pDisp->Release();
        }
        if (!files.empty()) break;
    }
    pShellWindows->Release();
    return files;
}

// ============================================================================
// TrackPopupMenu{Ex} hooks -- context menu injection
// ============================================================================

// Pick a menu item ID that's vanishingly unlikely to collide with shell
// extensions. Standard shell extensions use IDs in the range passed via
// idCmdFirst..idCmdLast which is typically <= 0x7FFF.
constexpr UINT kPinCmdId = 0xFADE;

using TrackPopupMenuEx_t = decltype(&TrackPopupMenuEx);
TrackPopupMenuEx_t TrackPopupMenuEx_Original;

// Notify the shell that the parents of `paths` should be re-enumerated, so
// the new (spoofed) timestamps propagate to any open view immediately.
void FireRefreshForPaths(const std::vector<std::wstring>& paths) {
    std::set<std::wstring> parents;
    for (const auto& p : paths) {
        size_t sep = p.find_last_of(L'\\');
        if (sep != std::wstring::npos) parents.insert(p.substr(0, sep));
    }
    for (const auto& parent : parents) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, parent.c_str(), nullptr);
    }
}

// Returns true if we added our item to hMenu. Stores chosen label state and
// the captured paths for the click handler.
struct MenuInjection {
    std::vector<std::wstring> selectedPaths;  // original case from BSTR
    bool allPinned = false;                   // true => label was "Unpin"
};

bool InjectPinMenuItem(HMENU hMenu, MenuInjection& out) {
    auto paths = GetSelectedFilesAtCursor();
    if (paths.empty()) return false;

    bool allPinned = true;
    for (const auto& p : paths) {
        if (!IsPathPinned(NormalizePath(p.c_str()))) {
            allPinned = false;
            break;
        }
    }

    int itemCount = GetMenuItemCount(hMenu);
    // Separator first (only if menu isn't already empty)
    if (itemCount > 0) {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    const wchar_t* label = allPinned ? L"\U0001F4CC Unpin from top"
                                      : L"\U0001F4CC Pin to top";
    AppendMenuW(hMenu, MF_STRING, kPinCmdId, label);

    out.selectedPaths = std::move(paths);
    out.allPinned = allPinned;
    return true;
}

void HandlePinClick(const MenuInjection& inj) {
    Wh_Log(L"Pin/Unpin invoked: %d item(s), allPinned=%d",
           (int)inj.selectedPaths.size(), inj.allPinned ? 1 : 0);
    for (const auto& path : inj.selectedPaths) {
        std::wstring norm = NormalizePath(path.c_str());
        if (norm.empty()) continue;
        // If all were pinned, the click means unpin everything.
        // Otherwise (some or none were pinned), the click means pin all.
        SetPin(norm, /*pinned=*/ !inj.allPinned);
    }
    FireRefreshForPaths(inj.selectedPaths);
}

BOOL WINAPI TrackPopupMenuEx_Hook(HMENU hMenu, UINT uFlags, int x, int y,
                                  HWND hWnd, LPTPMPARAMS lptpm) {
    HRESULT hrCom = CoInitialize(nullptr);
    bool comInit = SUCCEEDED(hrCom);

    MenuInjection inj;
    bool injected = InjectPinMenuItem(hMenu, inj);

    UINT effFlags = uFlags;
    bool addedReturnCmd = false;
    if (injected && !(effFlags & TPM_RETURNCMD)) {
        effFlags |= TPM_RETURNCMD;
        addedReturnCmd = true;
    }

    LRESULT ret = TrackPopupMenuEx_Original(hMenu, effFlags, x, y, hWnd, lptpm);

    if (injected) {
        if ((UINT)ret == kPinCmdId) {
            HandlePinClick(inj);
            ret = addedReturnCmd ? TRUE : 0;
        } else if (addedReturnCmd) {
            // Caller didn't ask for TPM_RETURNCMD; restore the BOOL semantics
            // and post WM_COMMAND for the chosen item (if any) so the shell's
            // dispatch path still works.
            if (ret && hWnd) {
                PostMessageW(hWnd, WM_COMMAND, (WPARAM)ret, 0);
            }
            ret = ret ? TRUE : FALSE;
        }
    }

    if (comInit) CoUninitialize();
    return (BOOL)ret;
}

}  // namespace

// ============================================================================
// Mod entry points
// ============================================================================

BOOL Wh_ModInit() {
    Wh_Log(L"Init");
    LoadPinsFromRegistry();

    // --- ntdll: NtQueryDirectoryFile{Ex} (primary timestamp rewrite) ---
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) { Wh_Log(L"ntdll.dll not loaded"); return FALSE; }
    NtQueryDirectoryFile_Original = (NtQueryDirectoryFile_t)
        GetProcAddress(hNtdll, "NtQueryDirectoryFile");
    NtQueryDirectoryFileEx_Original = (NtQueryDirectoryFileEx_t)
        GetProcAddress(hNtdll, "NtQueryDirectoryFileEx");
    if (!NtQueryDirectoryFile_Original) {
        Wh_Log(L"NtQueryDirectoryFile not found");
        return FALSE;
    }
    Wh_SetFunctionHook((void*)NtQueryDirectoryFile_Original,
                       (void*)NtQueryDirectoryFile_Hook,
                       (void**)&NtQueryDirectoryFile_Original);
    if (NtQueryDirectoryFileEx_Original) {
        Wh_SetFunctionHook((void*)NtQueryDirectoryFileEx_Original,
                           (void*)NtQueryDirectoryFileEx_Hook,
                           (void**)&NtQueryDirectoryFileEx_Original);
    }

    // --- user32: TrackPopupMenu{Ex} (context menu injection) ---
    Wh_SetFunctionHook((void*)TrackPopupMenuEx,
                       (void*)TrackPopupMenuEx_Hook,
                       (void**)&TrackPopupMenuEx_Original);

    // --- windows.storage: CFSFolder::GetDetailsEx (belt-and-suspenders) ---
    HMODULE hStorage = LoadLibraryExW(L"windows.storage.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hStorage) {
        WindhawkUtils::SYMBOL_HOOK hooks[] = {
            {
                {
                    LR"(public: virtual long __cdecl CFSFolder::GetDetailsEx(struct _ITEMID_CHILD const __unaligned *,struct _tagpropertykey const *,struct tagVARIANT *))",
                },
                &CFSFolder_GetDetailsEx_Original,
                CFSFolder_GetDetailsEx_Hook,
                true,
            },
        };
        WindhawkUtils::HookSymbols(hStorage, hooks, ARRAYSIZE(hooks));
    }

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L"Uninit");
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = TRUE;
    return TRUE;
}
