// Vault Commander - two-pane file manager with time-stamped backup copy.
//
// Evolved from the "Directory tree viewer" project.

#include "framework.h"
#include "Commander.h"
#include "Lang.h"

// ===========================================================================
//  Version / platform (shown in the title bar)
// ===========================================================================
#define APP_VERSION L"2.2"
#if defined(_M_ARM64)
#define APP_PLATFORM L"ARM64"
#elif defined(_WIN64)
#define APP_PLATFORM L"x64"
#else
#define APP_PLATFORM L"x32"
#endif

static std::wstring MainTitle()
{
    return std::wstring(T(S_APP_TITLE)) + L" " + APP_VERSION + L" " + APP_PLATFORM;
}

// ===========================================================================
//  IDs
//  (prefixed CMD_/CTL_ to avoid clashing with the macros in Resource.h)
// ===========================================================================
enum {
    CTL_COMBO0 = 301, CTL_COMBO1 = 302,
    CTL_LIST0 = 311, CTL_LIST1 = 312,
    CTL_STATUS = 320,

    CMD_RENAME = 201, CMD_EDIT, CMD_COPY, CMD_MOVE, CMD_NEWFOLDER, CMD_DELETE,
    CMD_BACKUP, CMD_EXIT, CMD_REFRESH, CMD_SAVETREE,
    CMD_LANG_EN, CMD_LANG_HU, CMD_ABOUT
};

const UINT     WM_APP_REFRESH = WM_APP + 1;
const UINT     WM_APP_DROP = WM_APP + 2;
const UINT_PTR TIMER_STATUS = 1;
const UINT     CTX_BACKUP = 0x8001;     // our own item inside the shell context menu

// Bottom function-key buttons
const int      kButtonCount = 8;
const int      kButtonCmds[kButtonCount] = { CMD_RENAME, CMD_EDIT, CMD_COPY, CMD_MOVE, CMD_NEWFOLDER, CMD_DELETE, CMD_BACKUP, CMD_EXIT };
const Str      kButtonStr[kButtonCount] = { S_RENAME, S_EDIT, S_COPY, S_MOVE, S_NEWFOLDER, S_DELETE, S_BACKUPCOPY, S_EXIT };
const wchar_t* const kButtonKeys[kButtonCount] = { L"F2", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"Alt+F4" };

// List columns: 0 = Name, 1 = Ext, 2 = Size, 3 = Date
const int      kColCount = 4;

// ===========================================================================
//  Data
// ===========================================================================
// Shell id list (ITEMIDLIST) stored as plain bytes, so it can be copied around safely
typedef std::vector<BYTE> PidlBytes;

struct Entry {
    std::wstring name;
    std::wstring ext;          // without the dot, empty for folders
    PidlBytes    pidl;         // shell id list (mobile device items only)
    bool         isDir = false;
    ULONGLONG    size = 0;
    FILETIME     time = {};
};

struct Pane {
    HWND hCombo = nullptr;     // drive selector
    HWND hPath = nullptr;      // current path label
    HWND hList = nullptr;      // file list
    HWND hInfo = nullptr;      // "selected / total" info line under the list
    std::wstring path;
    std::vector<Entry> items;  // item index in the list view == index in this vector
    int  sortCol = 0;
    bool sortAsc = true;
    std::wstring selectName;   // name to put the cursor on after the next reload
    bool shell = false;        // true while browsing a mobile device (MTP/PTP) through the Windows shell
    PidlBytes shellRoot;       // id list of the device
    PidlBytes shellCur;        // id list of the current folder on the device
    std::wstring lastFsPath;   // last real folder (used when leaving the device)
};

struct DropJob {               // file operation requested by a drop, executed after the drag loop ends
    std::vector<std::wstring> files;
    std::wstring dest;
    bool move;
};

static Pane         g_pane[2];
static int          g_active = 0;
static bool         g_loading = false;
static HWND         g_hMain = nullptr;
static HWND         g_hStatus = nullptr;
static HWND         g_hButtons[kButtonCount] = {};
static std::wstring g_iniPath;
static IContextMenu2* g_pcm2 = nullptr;   // alive only while a shell context menu is shown
static IContextMenu3* g_pcm3 = nullptr;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static bool LoadPane(int p, std::wstring path);
static void RefreshBoth();
static void UpdateInfo(int p);

// ===========================================================================
//  Small helpers
// ===========================================================================
static std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

static void Msg(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
    MessageBoxW(g_hMain, text.c_str(), T(S_APP_TITLE), MB_OK | icon);
}

// Message in the status bar; clears itself after a few seconds
static void SetStatus(const std::wstring& s)
{
    SendMessageW(g_hStatus, SB_SETTEXT, 0, (LPARAM)s.c_str());
    if (!s.empty()) SetTimer(g_hMain, TIMER_STATUS, 8000, nullptr);
}

static void SetGuiFont(HWND h)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

// "C:\Dir\" -> "C:\Dir",  "C:" -> "C:\"
static std::wstring NormalizePath(std::wstring p)
{
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    return p;
}

static bool IsRoot(const std::wstring& p) { return PathIsRootW(p.c_str()) != FALSE; }

static std::wstring ParentOf(const std::wstring& p)
{
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return p;
    if (pos <= 2) return p.substr(0, pos + 1);   // "C:\"
    return p.substr(0, pos);
}

static std::wstring LastComponent(const std::wstring& p)
{
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

static bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(NormalizePath(a).c_str(), NormalizePath(b).c_str()) == 0;
}

// true if 'child' is the same as 'parent' or lies somewhere inside it
static bool IsSubPathOrSame(const std::wstring& parent, const std::wstring& child)
{
    std::wstring a = NormalizePath(parent), b = NormalizePath(child);
    if (a.empty()) return false;
    if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;
    if (a.back() != L'\\') a += L'\\';
    return b.size() > a.size() && _wcsnicmp(a.c_str(), b.c_str(), a.size()) == 0;
}

static bool SameVolume(const std::wstring& a, const std::wstring& b)
{
    int da = PathGetDriveNumberW(a.c_str()), db = PathGetDriveNumberW(b.c_str());
    return da >= 0 && da == db;
}

// Extension without the dot ("a.txt" -> "txt"). ".gitignore" and "name." have none.
static std::wstring ExtOf(const std::wstring& name, bool isDir)
{
    if (isDir) return L"";
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
    return name.substr(dot + 1);
}

static std::wstring FormatSize(ULONGLONG sz)
{
    wchar_t buf[64];
    StrFormatByteSizeW((LONGLONG)sz, buf, 64);
    return buf;
}

static std::wstring FormatDate(const FILETIME& ft)
{
    FILETIME lft;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return L"";
    wchar_t d[64] = {}, t[64] = {};
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return std::wstring(d) + L" " + t;
}

// ===========================================================================
//  Settings (language + last folders), stored in %APPDATA%\VaultCommander
// ===========================================================================
static void InitIni()
{
    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring dir = JoinPath(appdata, L"VaultCommander");
        CreateDirectoryW(dir.c_str(), nullptr);
        g_iniPath = JoinPath(dir, L"settings.ini");
    }
}

static std::wstring ReadIni(const wchar_t* key, const wchar_t* def)
{
    if (g_iniPath.empty()) return def;
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"Settings", key, def, buf, _countof(buf), g_iniPath.c_str());
    return buf;
}

static void WriteIni(const wchar_t* key, const std::wstring& val)
{
    if (!g_iniPath.empty())
        WritePrivateProfileStringW(L"Settings", key, val.c_str(), g_iniPath.c_str());
}

static std::wstring StartPath(const wchar_t* key, int csidl)
{
    std::wstring p = ReadIni(key, L"");
    if (!p.empty() && PathIsDirectoryW(p.c_str())) return p;
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf))) return buf;
    return L"C:\\";
}

// ===========================================================================
//  Pane handling
// ===========================================================================
// ---- Mobile devices (phones/tablets connected over USB, MTP/PTP) ----------
//  They have no drive letter, so they are browsed through the Windows shell namespace.
//  Items are tracked by their shell id lists (not by text names), exactly like Explorer does,
//  because the text form of MTP items cannot always be turned back into an item.
struct Device { std::wstring name; PidlBytes pidl; };
static std::vector<Device> g_devices;

// property keys written out by hand (System.Size, System.DateModified, System.FileName)
static const PROPERTYKEY kKeySize     = { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 12 };
static const PROPERTYKEY kKeyModified = { { 0xB725F130, 0x47EF, 0x101A, { 0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC } }, 14 };
static const PROPERTYKEY kKeyFileName = { { 0x41CF5AE0, 0xF75A, 0x4806, { 0xBD, 0x87, 0x59, 0xC7, 0xD9, 0x24, 0x8E, 0xB9 } }, 100 };

static PidlBytes PidlFromAbsolute(PCIDLIST_ABSOLUTE pidl)
{
    PidlBytes r;
    if (pidl) {
        UINT sz = ILGetSize(pidl);
        r.assign((const BYTE*)pidl, (const BYTE*)pidl + sz);
    }
    return r;
}

static PidlBytes PidlFromItem(IShellItem* item)
{
    PidlBytes r;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHGetIDListFromObject(item, &pidl)) && pidl) {
        r = PidlFromAbsolute(pidl);
        CoTaskMemFree(pidl);
    }
    return r;
}

static IShellItem* ItemFromPidl(const PidlBytes& b)
{
    IShellItem* it = nullptr;
    if (b.empty() || FAILED(SHCreateItemFromIDList((PCIDLIST_ABSOLUTE)b.data(), IID_PPV_ARGS(&it)))) return nullptr;
    return it;
}

static bool PidlEqual(const PidlBytes& a, const PidlBytes& b)
{
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return ILIsEqual((PCIDLIST_ABSOLUTE)a.data(), (PCIDLIST_ABSOLUTE)b.data()) != FALSE;
}

static void ScanDevices()
{
    g_devices.clear();
    IShellItem* pc = nullptr;                                   // "This PC"
    if (FAILED(SHCreateItemFromParsingName(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", nullptr, IID_PPV_ARGS(&pc))))
        return;

    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(pc->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            LPWSTR parse = nullptr, name = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parse)) &&
                (StrStrIW(parse, L"usb#vid_") || StrStrIW(parse, L"6ac27878-a6fa-4155-ba85-f98f491d4f33")))  // USB / WPD device
            {
                SFGAOF attr = 0;
                it->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attr);
                if ((attr & SFGAO_FOLDER) && !(attr & SFGAO_FILESYSTEM) &&
                    SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
                {
                    Device d;
                    d.name = name;
                    d.pidl = PidlFromItem(it);
                    if (!d.pidl.empty()) g_devices.push_back(d);
                }
            }
            if (parse) CoTaskMemFree(parse);
            if (name) CoTaskMemFree(name);
            it->Release();
            it = nullptr;
        }
        en->Release();
    }
    pc->Release();
}

static void SelectDrive(int p)
{
    Pane& P = g_pane[p];
    int n = (int)SendMessageW(P.hCombo, CB_GETCOUNT, 0, 0);
    int sel = -1;
    for (int i = 0; i < n; ++i) {
        LRESULT data = SendMessageW(P.hCombo, CB_GETITEMDATA, i, 0);
        if (P.shell) {
            if (data > 0 && (size_t)data <= g_devices.size() && PidlEqual(g_devices[data - 1].pidl, P.shellRoot))
                sel = i;
        }
        else if (data == 0) {
            wchar_t buf[16] = {};
            SendMessageW(P.hCombo, CB_GETLBTEXT, i, (LPARAM)buf);
            if (P.path.size() >= 2 && P.path[1] == L':' && _wcsnicmp(buf, P.path.c_str(), 2) == 0)
                sel = i;
        }
    }
    SendMessageW(P.hCombo, CB_SETCURSEL, sel, 0);
}

// Drive letters first, then the connected mobile devices (item data = index in g_devices + 1)
static void FillAllDrives()
{
    ScanDevices();

    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(_countof(buf), buf);
    if (n > _countof(buf)) n = 0;

    for (int p = 0; p < 2; ++p) {
        HWND c = g_pane[p].hCombo;
        SendMessageW(c, CB_RESETCONTENT, 0, 0);
        for (wchar_t* d = buf; n && *d; d += wcslen(d) + 1) {
            LRESULT idx = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)d);
            SendMessageW(c, CB_SETITEMDATA, idx, 0);
        }
        for (size_t i = 0; i < g_devices.size(); ++i) {
            LRESULT idx = SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_devices[i].name.c_str());
            SendMessageW(c, CB_SETITEMDATA, idx, (LPARAM)(i + 1));
        }
        SendMessageW(c, CB_SETDROPPEDWIDTH, 260, 0);
        SelectDrive(p);
    }
}

static void SortItems(Pane& P)
{
    std::stable_sort(P.items.begin(), P.items.end(), [&P](const Entry& a, const Entry& b) {
        bool aUp = (a.name == L".."), bUp = (b.name == L"..");
        if (aUp != bUp) return aUp;                     // ".." always first
        if (a.isDir != b.isDir) return a.isDir;         // folders before files
        int c = 0;
        if (P.sortCol == 1 && !a.isDir)
            c = _wcsicmp(a.ext.c_str(), b.ext.c_str());
        else if (P.sortCol == 2 && !a.isDir)
            c = a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
        else if (P.sortCol == 3)
            c = CompareFileTime(&a.time, &b.time);
        if (c == 0)
            c = StrCmpLogicalW(a.name.c_str(), b.name.c_str());
        if (c == 0) return false;
        return P.sortAsc ? c < 0 : c > 0;
    });
}

// Arrow in the header of the column the list is sorted by (up = ascending)
static void UpdateSortArrow(int p)
{
    Pane& P = g_pane[p];
    HWND hdr = ListView_GetHeader(P.hList);
    if (!hdr) return;
    for (int i = 0; i < kColCount; ++i) {
        HDITEMW hd = {};
        hd.mask = HDI_FORMAT;
        if (!Header_GetItem(hdr, i, &hd)) continue;
        hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == P.sortCol) hd.fmt |= P.sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hd);
    }
}

static void Populate(int p)
{
    Pane& P = g_pane[p];
    g_loading = true;
    SendMessageW(P.hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(P.hList);

    int focusIdx = 0;
    for (int i = 0; i < (int)P.items.size(); ++i) {
        const Entry& e = P.items[i];

        SHFILEINFOW sfi = {};
        SHGetFileInfoW(e.name.c_str(),
            e.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
            &sfi, sizeof(sfi),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);

        // name column shows the name without the extension (extension has its own column)
        std::wstring shown = e.name;
        if (!e.ext.empty()) shown.resize(e.name.size() - e.ext.size() - 1);

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_IMAGE;
        lvi.iItem = i;
        lvi.pszText = (LPWSTR)shown.c_str();
        lvi.iImage = sfi.iIcon;
        ListView_InsertItem(P.hList, &lvi);

        std::wstring sizeText = e.isDir ? L"<DIR>" : FormatSize(e.size);
        std::wstring dateText = (e.name == L"..") ? L"" : FormatDate(e.time);
        ListView_SetItemText(P.hList, i, 1, (LPWSTR)e.ext.c_str());
        ListView_SetItemText(P.hList, i, 2, (LPWSTR)sizeText.c_str());
        ListView_SetItemText(P.hList, i, 3, (LPWSTR)dateText.c_str());

        if (!P.selectName.empty() && _wcsicmp(e.name.c_str(), P.selectName.c_str()) == 0)
            focusIdx = i;
    }

    if (!P.items.empty()) {
        ListView_SetItemState(P.hList, focusIdx, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        ListView_EnsureVisible(P.hList, focusIdx, FALSE);
    }
    P.selectName.clear();

    SendMessageW(P.hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(P.hList, nullptr, TRUE);
    g_loading = false;
    UpdateSortArrow(p);
    UpdateInfo(p);
}

static bool LoadPane(int p, std::wstring path)
{
    Pane& P = g_pane[p];
    path = NormalizePath(path);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Msg(Fmt(T(S_ERR_OPEN_FOLDER), path.c_str()), MB_ICONWARNING);
        return false;
    }

    std::vector<Entry> tmp;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        Entry e;
        e.name = name;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.ext = ExtOf(name, e.isDir);
        e.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.time = fd.ftLastWriteTime;
        tmp.push_back(e);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (!IsRoot(path)) {
        Entry up;
        up.name = L"..";
        up.isDir = true;
        tmp.push_back(up);
    }

    // On a plain refresh keep the cursor on the same item
    if (P.selectName.empty() && !P.shell && SamePath(P.path, path)) {
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)P.items.size()) P.selectName = P.items[f].name;
    }

    P.shell = false;
    P.lastFsPath = path;
    P.path = path;
    P.items.swap(tmp);
    SortItems(P);
    Populate(p);
    SetWindowTextW(P.hPath, path.c_str());
    SelectDrive(p);
    return true;
}

// "ZenFone7 Pro\Internal storage\DCIM" style path for the label above a device pane
static std::wstring ShellDisplayPath(IShellItem* item, const PidlBytes& root)
{
    std::wstring result;
    IShellItem* cur = item;
    cur->AddRef();
    for (int guard = 0; guard < 64 && cur; ++guard) {
        LPWSTR nm = nullptr;
        if (SUCCEEDED(cur->GetDisplayName(SIGDN_NORMALDISPLAY, &nm))) {
            result = result.empty() ? std::wstring(nm) : std::wstring(nm) + L"\\" + result;
            CoTaskMemFree(nm);
        }
        if (PidlEqual(PidlFromItem(cur), root)) { cur->Release(); cur = nullptr; break; }
        IShellItem* par = nullptr;
        HRESULT hr = cur->GetParent(&par);
        cur->Release();
        cur = SUCCEEDED(hr) ? par : nullptr;
    }
    if (cur) cur->Release();
    return result;
}

// List a folder of a mobile device. 'root' = id list of the device (pass an empty one to keep the current device).
static bool LoadShellPane(int p, const PidlBytes& pidl, const PidlBytes& root)
{
    Pane& P = g_pane[p];

    IShellFolder* psf = nullptr;
    IEnumIDList* en = nullptr;
    HRESULT hr = E_FAIL;
    if (!pidl.empty()) {
        hr = SHBindToObject(nullptr, (PCUIDLIST_RELATIVE)pidl.data(), nullptr, IID_PPV_ARGS(&psf));
        if (SUCCEEDED(hr))
            hr = psf->EnumObjects(g_hMain, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &en);
    }
    if (FAILED(hr)) {
        if (psf) psf->Release();
        Msg(Fmt(T(S_ERR_OPEN_FOLDER), P.path.c_str()) + Fmt(L"\n(0x%08X)", (unsigned)hr), MB_ICONWARNING);
        return false;
    }

    std::vector<Entry> tmp;
    if (en) {                                                   // S_FALSE = empty folder, no enumerator
        PITEMID_CHILD cp = nullptr;
        while (en->Next(1, &cp, nullptr) == S_OK) {
            Entry e;
            IShellItem2* c2 = nullptr;
            if (SUCCEEDED(SHCreateItemWithParent((PCIDLIST_ABSOLUTE)pidl.data(), psf, cp, IID_PPV_ARGS(&c2))) && c2) {
                LPWSTR s = nullptr;
                // real file name incl. extension (the display name may hide it)
                if (SUCCEEDED(c2->GetString(kKeyFileName, &s)) && s && *s) e.name = s;
                if (s) { CoTaskMemFree(s); s = nullptr; }
                if (e.name.empty() && SUCCEEDED(c2->GetDisplayName(SIGDN_NORMALDISPLAY, &s))) {
                    e.name = s;
                    CoTaskMemFree(s);
                    s = nullptr;
                }

                SFGAOF attr = 0;
                c2->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attr);
                e.isDir = ((attr & SFGAO_FOLDER) != 0) && ((attr & SFGAO_STREAM) == 0);
                e.ext = ExtOf(e.name, e.isDir);

                ULONGLONG sz = 0;
                FILETIME ft = {};
                if (!e.isDir && SUCCEEDED(c2->GetUInt64(kKeySize, &sz))) e.size = sz;
                if (SUCCEEDED(c2->GetFileTime(kKeyModified, &ft))) e.time = ft;
                c2->Release();
            }
            PIDLIST_ABSOLUTE abs = ILCombine((PCIDLIST_ABSOLUTE)pidl.data(), cp);
            e.pidl = PidlFromAbsolute(abs);
            if (abs) ILFree(abs);
            CoTaskMemFree(cp);
            cp = nullptr;
            if (!e.name.empty() && !e.pidl.empty()) tmp.push_back(e);
        }
        en->Release();
    }
    psf->Release();

    if (tmp.empty() && !root.empty() && PidlEqual(pidl, root))
        Msg(T(S_ERR_DEVICE_EMPTY), MB_ICONINFORMATION);        // locked phone / charging-only USB mode

    Entry up;
    up.name = L"..";
    up.isDir = true;
    tmp.push_back(up);

    if (P.selectName.empty() && P.shell && PidlEqual(P.shellCur, pidl)) {
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)P.items.size()) P.selectName = P.items[f].name;
    }

    P.shell = true;
    if (!root.empty()) P.shellRoot = root;
    P.shellCur = pidl;
    IShellItem* folder = ItemFromPidl(pidl);
    if (folder) {
        P.path = ShellDisplayPath(folder, P.shellRoot);        // display text only while in device mode
        folder->Release();
    }

    P.items.swap(tmp);
    SortItems(P);
    Populate(p);
    SetWindowTextW(P.hPath, P.path.c_str());
    SelectDrive(p);
    return true;
}

static void RefreshPane(int p)
{
    Pane& P = g_pane[p];
    if (P.shell) {
        PidlBytes cur = P.shellCur;
        if (LoadShellPane(p, cur, PidlBytes())) return;
        LoadPane(p, P.lastFsPath.empty() ? std::wstring(L"C:\\") : P.lastFsPath);   // device was unplugged
        return;
    }
    std::wstring path = P.path;
    while (!IsRoot(path) && !PathIsDirectoryW(path.c_str()))   // folder vanished -> go up
        path = ParentOf(path);
    LoadPane(p, path);
}

static void RefreshBoth()
{
    RefreshPane(0);
    RefreshPane(1);
}

static void GoUp(int p)
{
    Pane& P = g_pane[p];

    if (P.shell) {
        if (PidlEqual(P.shellCur, P.shellRoot)) {              // top of the device -> back to the file system
            P.selectName.clear();
            LoadPane(p, P.lastFsPath.empty() ? std::wstring(L"C:\\") : P.lastFsPath);
            return;
        }
        PidlBytes parent = P.shellCur;
        if (!ILRemoveLastID((PUIDLIST_RELATIVE)parent.data())) return;
        parent.resize(ILGetSize((PCUIDLIST_RELATIVE)parent.data()));
        P.selectName = LastComponent(P.path);
        if (!LoadShellPane(p, parent, PidlBytes())) P.selectName.clear();
        return;
    }

    if (IsRoot(P.path)) return;
    P.selectName = LastComponent(P.path);
    if (!LoadPane(p, ParentOf(P.path))) P.selectName.clear();
}

static void OpenItem(int p, int idx)
{
    Pane& P = g_pane[p];
    if (idx < 0 || idx >= (int)P.items.size()) return;
    const Entry e = P.items[idx];
    if (e.name == L"..") { GoUp(p); return; }

    if (P.shell) {                                         // item on a mobile device
        if (e.isDir) {
            P.selectName.clear();
            LoadShellPane(p, e.pidl, PidlBytes());
        }
        else {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_IDLIST;
            sei.hwnd = g_hMain;
            sei.lpIDList = (LPVOID)e.pidl.data();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        return;
    }

    std::wstring full = JoinPath(P.path, e.name);
    if (e.isDir) {
        P.selectName.clear();
        LoadPane(p, full);
    }
    else {
        ShellExecuteW(g_hMain, L"open", full.c_str(), nullptr, P.path.c_str(), SW_SHOWNORMAL);
    }
}

// id used by the file operations: real path (for device items only the name; they are handled via id lists)
static std::wstring ItemId(const Pane& P, const Entry& e)
{
    return P.shell ? e.name : JoinPath(P.path, e.name);
}

// Selected items (full paths). With nothing selected the cursor item is used.
static std::vector<std::wstring> GetSelection(int p)
{
    Pane& P = g_pane[p];
    std::vector<std::wstring> r;
    int i = -1;
    while ((i = ListView_GetNextItem(P.hList, i, LVNI_SELECTED)) != -1) {
        if (i < (int)P.items.size() && P.items[i].name != L"..")
            r.push_back(ItemId(P, P.items[i]));
    }
    if (r.empty()) {
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)P.items.size() && P.items[f].name != L"..")
            r.push_back(ItemId(P, P.items[f]));
    }
    return r;
}

// Info line under each pane:  <selected size> / <total size> - <sel files> / <files>, <sel folders> / <folders>
static void UpdateInfo(int p)
{
    Pane& P = g_pane[p];
    if (g_loading || !P.hInfo) return;

    int files = 0, dirs = 0;
    ULONGLONG total = 0;
    for (const Entry& e : P.items) {
        if (e.name == L"..") continue;
        if (e.isDir) dirs++; else { files++; total += e.size; }
    }

    int selFiles = 0, selDirs = 0;
    ULONGLONG selSize = 0;
    int i = -1;
    while ((i = ListView_GetNextItem(P.hList, i, LVNI_SELECTED)) != -1) {
        if (i >= (int)P.items.size()) break;
        const Entry& e = P.items[i];
        if (e.name == L"..") continue;
        if (e.isDir) selDirs++; else { selFiles++; selSize += e.size; }
    }

    std::wstring text = Fmt(T(S_INFO),
        FormatSize(selSize).c_str(), FormatSize(total).c_str(),
        selFiles, files, selDirs, dirs);
    SetWindowTextW(P.hInfo, text.c_str());
}

static void SetActive(int p)
{
    if (g_active != p) {
        g_active = p;
        InvalidateRect(g_pane[0].hPath, nullptr, TRUE);
        InvalidateRect(g_pane[1].hPath, nullptr, TRUE);
    }
}

// ===========================================================================
//  File operations
// ===========================================================================
static std::wstring DoubleNull(const std::vector<std::wstring>& v)
{
    std::wstring s;
    for (const auto& item : v) { s += item; s.push_back(L'\0'); }
    s.push_back(L'\0');
    return s;
}

// Wrapper around SHFileOperation (progress dialog, recycle bin, conflict prompts)
static bool ShellOp(UINT func, const std::vector<std::wstring>& src, const std::wstring& dest, FILEOP_FLAGS flags)
{
    std::wstring from = DoubleNull(src);
    std::wstring to = dest;
    to.push_back(L'\0');

    SHFILEOPSTRUCTW op = {};
    op.hwnd = g_hMain;
    op.wFunc = func;
    op.pFrom = from.c_str();
    op.pTo = dest.empty() ? nullptr : to.c_str();
    op.fFlags = flags;
    int r = SHFileOperationW(&op);
    return r == 0 && !op.fAnyOperationsAborted;
}

// Where files come from / go to: a real path, or a shell id list for items on a mobile device
struct Loc {
    std::wstring path;
    PidlBytes    pidl;
};

static IShellItem* ItemFromLoc(const Loc& l)
{
    if (!l.pidl.empty()) return ItemFromPidl(l.pidl);
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(l.path.c_str(), nullptr, IID_PPV_ARGS(&it)))) return nullptr;
    return it;
}

// Same selection as GetSelection(), but as Loc (id lists for device items)
static std::vector<Loc> GetSelectionLocs(int p)
{
    Pane& P = g_pane[p];
    std::vector<Loc> r;
    auto add = [&](const Entry& e) {
        Loc l;
        if (P.shell) l.pidl = e.pidl; else l.path = JoinPath(P.path, e.name);
        r.push_back(l);
    };
    int i = -1;
    while ((i = ListView_GetNextItem(P.hList, i, LVNI_SELECTED)) != -1)
        if (i < (int)P.items.size() && P.items[i].name != L"..") add(P.items[i]);
    if (r.empty()) {
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)P.items.size() && P.items[f].name != L"..") add(P.items[f]);
    }
    return r;
}

static Loc LocOf(int p)
{
    Loc l;
    if (g_pane[p].shell) l.pidl = g_pane[p].shellCur; else l.path = g_pane[p].path;
    return l;
}

static bool SameLoc(const Loc& a, const Loc& b)
{
    if (!a.pidl.empty() || !b.pidl.empty())
        return !a.pidl.empty() && !b.pidl.empty() && PidlEqual(a.pidl, b.pidl);
    return SamePath(a.path, b.path);
}

// Copy / move / delete through IFileOperation. Needed when a mobile device is involved,
// because its items have no file-system path.   func: 0 = copy, 1 = move, 2 = delete
static bool ItemOp(int func, const std::vector<Loc>& src, const Loc& dest, DWORD flags = 0)
{
    IFileOperation* pfo = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pfo)))) return false;
    pfo->SetOwnerWindow(g_hMain);
    pfo->SetOperationFlags(flags);

    IShellItem* pdest = nullptr;
    if (func != 2) {
        pdest = ItemFromLoc(dest);
        if (!pdest) { pfo->Release(); return false; }
    }
    for (const auto& l : src) {
        IShellItem* psi = ItemFromLoc(l);
        if (!psi) continue;
        if (func == 0)      pfo->CopyItem(psi, pdest, nullptr, nullptr);
        else if (func == 1) pfo->MoveItem(psi, pdest, nullptr, nullptr);
        else                pfo->DeleteItem(psi, nullptr);
        psi->Release();
    }
    HRESULT hr = pfo->PerformOperations();
    BOOL aborted = FALSE;
    pfo->GetAnyOperationsAborted(&aborted);
    if (pdest) pdest->Release();
    pfo->Release();
    return SUCCEEDED(hr) && !aborted;
}

// ---- Plain copy / move ----------------------------------------------------
static void DoCopyMove(bool move)
{
    auto sel = GetSelection(g_active);
    if (sel.empty()) { Msg(T(S_NOTHING_SELECTED)); return; }

    const int other = 1 - g_active;
    const bool viaItems = g_pane[g_active].shell || g_pane[other].shell;   // phone involved
    if (SameLoc(LocOf(g_active), LocOf(other))) { Msg(T(S_SAME_FOLDER), MB_ICONWARNING); return; }
    if (!viaItems)
        for (const auto& s : sel)
            if (IsSubPathOrSame(s, g_pane[other].path)) { Msg(T(S_INTO_ITSELF), MB_ICONWARNING); return; }

    std::wstring q = Fmt(move ? T(S_CONFIRM_MOVE) : T(S_CONFIRM_COPY), (int)sel.size(), g_pane[other].path.c_str());
    if (MessageBoxW(g_hMain, q.c_str(), T(S_APP_TITLE), MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    if (viaItems) ItemOp(move ? 1 : 0, GetSelectionLocs(g_active), LocOf(other));
    else          ShellOp(move ? FO_MOVE : FO_COPY, sel, g_pane[other].path, 0);
    RefreshBoth();
}

static void DoDelete()
{
    auto sel = GetSelection(g_active);
    if (sel.empty()) { Msg(T(S_NOTHING_SELECTED)); return; }

    bool permanent = GetKeyState(VK_SHIFT) < 0;          // Shift = skip the Recycle Bin
    DWORD flags = permanent ? 0 : FOF_ALLOWUNDO;
    if (g_pane[g_active].shell) ItemOp(2, GetSelectionLocs(g_active), Loc(), flags);
    else                        ShellOp(FO_DELETE, sel, L"", (FILEOP_FLAGS)flags);
    RefreshBoth();
}

// ---- Backup copy ----------------------------------------------------------
//  Creates a NEW folder named <YYYY-MM-DD_HH-MM-SS> in the destination and copies
//  the selected items into it. If the name already exists (two backups within
//  the same second) a _2, _3 ... suffix is added. Existing folders are never reused,
//  so earlier backups can never be overwritten (CreateDirectory is atomic).
//  The source may be a mobile device; the destination must be a normal folder.
static std::wstring TimestampName()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Fmt(L"%04d-%02d-%02d_%02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static std::wstring CreateBackupFolder(const std::wstring& destDir, DWORD& err)
{
    std::wstring base = TimestampName();
    for (int n = 1; n < 1000; ++n) {
        std::wstring name = (n == 1) ? base : base + L"_" + std::to_wstring(n);
        std::wstring full = JoinPath(destDir, name);
        if (CreateDirectoryW(full.c_str(), nullptr)) return full;
        err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) return L"";
    }
    return L"";
}

static void DoBackup()
{
    auto sel = GetSelection(g_active);
    if (sel.empty()) { Msg(T(S_NOTHING_SELECTED)); return; }

    int other = 1 - g_active;
    if (g_pane[other].shell) { Msg(T(S_ERR_DEVICE_UNSUPPORTED), MB_ICONWARNING); return; }
    std::wstring dest = g_pane[other].path;

    // the backup folder would end up inside a folder that is being copied
    if (!g_pane[g_active].shell)
        for (const auto& s : sel)
            if (IsSubPathOrSame(s, dest)) { Msg(T(S_INTO_ITSELF), MB_ICONWARNING); return; }

    std::wstring q = Fmt(T(S_CONFIRM_BACKUP), (int)sel.size(), dest.c_str());
    if (MessageBoxW(g_hMain, q.c_str(), T(S_APP_TITLE), MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    DWORD err = 0;
    std::wstring folder = CreateBackupFolder(dest, err);
    if (folder.empty()) { Msg(Fmt(T(S_ERR_CREATE_BACKUP), (unsigned long)err), MB_ICONERROR); return; }

    bool ok;
    if (g_pane[g_active].shell) {
        Loc target;
        target.path = folder;
        ok = ItemOp(0, GetSelectionLocs(g_active), target);
    }
    else {
        ok = ShellOp(FO_COPY, sel, folder, 0);
    }
    if (!ok) RemoveDirectoryW(folder.c_str());            // only succeeds if still empty
    else     g_pane[other].selectName = LastComponent(folder);

    RefreshBoth();
    if (ok) SetStatus(Fmt(T(S_BACKUP_DONE), LastComponent(folder).c_str()));
}

// ---- Edit / rename / new folder ------------------------------------------
static void DoEdit()
{
    auto sel = GetSelection(g_active);
    if (sel.empty() || g_pane[g_active].shell) return;
    DWORD attr = GetFileAttributesW(sel[0].c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    HINSTANCE r = ShellExecuteW(g_hMain, L"edit", sel[0].c_str(), nullptr,
        g_pane[g_active].path.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        std::wstring arg = L"\"" + sel[0] + L"\"";
        ShellExecuteW(g_hMain, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

static void DoRename()
{
    Pane& P = g_pane[g_active];
    if (P.shell) { Msg(T(S_ERR_DEVICE_UNSUPPORTED), MB_ICONWARNING); return; }
    int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
    if (f < 0 || f >= (int)P.items.size() || P.items[f].name == L"..") return;
    SetFocus(P.hList);
    ListView_EditLabel(P.hList, f);                       // in-place rename, finished in LVN_ENDLABELEDIT
}

static void DoNewFolder()
{
    Pane& P = g_pane[g_active];
    if (P.shell) { Msg(T(S_ERR_DEVICE_UNSUPPORTED), MB_ICONWARNING); return; }
    std::wstring base = T(S_NEW_FOLDER_NAME), name = base;
    for (int n = 2; n < 1000 && GetFileAttributesW(JoinPath(P.path, name).c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
        name = base + L" (" + std::to_wstring(n) + L")";

    if (!CreateDirectoryW(JoinPath(P.path, name).c_str(), nullptr)) {
        Msg(Fmt(T(S_ERR_CREATE_FOLDER), (unsigned long)GetLastError()), MB_ICONERROR);
        return;
    }
    P.selectName = name;
    RefreshBoth();
    SetFocus(P.hList);
    int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
    if (f >= 0) ListView_EditLabel(P.hList, f);           // let the user type the real name
}

// ---- Directory tree export (the original tree-viewer feature) -------------
static void BuildTree(const std::wstring& path, const std::wstring& prefix, std::wstring& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> dirs, files;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(name);
        else files.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (size_t i = 0; i < dirs.size(); i++) {
        bool last = (i == dirs.size() - 1 && files.empty());
        out += prefix + (last ? L"└── " : L"├── ") + dirs[i] + L"/\r\n";
        BuildTree(JoinPath(path, dirs[i]), prefix + (last ? L"    " : L"│   "), out);
    }
    for (size_t i = 0; i < files.size(); i++) {
        bool last = (i == files.size() - 1);
        out += prefix + (last ? L"└── " : L"├── ") + files[i] + L"\r\n";
    }
}

static void SaveTree()
{
    if (g_pane[g_active].shell) { Msg(T(S_ERR_DEVICE_UNSUPPORTED), MB_ICONWARNING); return; }
    const std::wstring root = g_pane[g_active].path;
    std::wstring title = IsRoot(root) ? root : LastComponent(root);
    std::wstring text = title + L"/\r\n";
    BuildTree(root, L"", text);

    wchar_t fileName[MAX_PATH];
    std::wstring def = (IsRoot(root) ? std::wstring(L"tree") : LastComponent(root)) + L".txt";
    wcscpy_s(fileName, def.c_str());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"Text (*.txt)\0*.txt\0";
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = root.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size > 0 ? size - 1 : 0, 0);
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8[0], size, nullptr, nullptr);

    std::ofstream file(fileName, std::ios::binary);
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };   // UTF-8 BOM so Notepad shows it correctly
    file.write((const char*)bom, 3);
    file.write(utf8.c_str(), utf8.size());
}

// ===========================================================================
//  Windows shell integration: Explorer context menu + drag & drop
// ===========================================================================

// Shell item ids for a list of paths that all live in the same folder
struct ShellSel {
    IShellFolder* folder = nullptr;
    std::vector<PIDLIST_ABSOLUTE> full;
    std::vector<PCUITEMID_CHILD>  child;

    ~ShellSel()
    {
        for (auto pidl : full) CoTaskMemFree(pidl);
        if (folder) folder->Release();
    }

    bool Init(const std::vector<std::wstring>& paths)
    {
        for (const auto& s : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(s.c_str(), nullptr, &pidl, 0, nullptr)))
                full.push_back(pidl);
        }
        for (auto pidl : full) {
            IShellFolder* f = nullptr;
            PCUITEMID_CHILD c = nullptr;
            if (FAILED(SHBindToParent(pidl, IID_PPV_ARGS(&f), &c))) continue;
            if (!folder) folder = f; else f->Release();
            child.push_back(c);
        }
        return folder && !child.empty();
    }
};

// ---- Explorer context menu (right click / Shift+F10 / Menu key) -----------
//  background == true : menu of the current folder itself (New, Paste, Properties...)
//  background == false: menu of the selected files/folders (Open, Send to, Properties...)
static void ShowShellMenu(int p, POINT pt, bool background)
{
    Pane& P = g_pane[p];
    if (P.shell) return;                                  // no Explorer menu for mobile devices
    ShellSel ss;
    IShellFolder* bg = nullptr;
    IContextMenu* pcm = nullptr;
    HRESULT hr = E_FAIL;

    if (background) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(P.path.c_str(), nullptr, &pidl, 0, nullptr))) {
            if (SUCCEEDED(SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&bg))))
                hr = bg->CreateViewObject(g_hMain, IID_PPV_ARGS(&pcm));
            CoTaskMemFree(pidl);
        }
    }
    else {
        auto sel = GetSelection(p);
        if (!sel.empty() && ss.Init(sel))
            hr = ss.folder->GetUIObjectOf(g_hMain, (UINT)ss.child.size(), ss.child.data(),
                __uuidof(IContextMenu), nullptr, (void**)&pcm);
    }

    if (SUCCEEDED(hr) && pcm) {
        HMENU hm = CreatePopupMenu();
        UINT flags = CMF_NORMAL;
        if (GetKeyState(VK_SHIFT) < 0) flags |= CMF_EXTENDEDVERBS;

        if (SUCCEEDED(pcm->QueryContextMenu(hm, 0, 1, 0x7FFF, flags))) {
            if (!background) {      // our own entry on top of the Explorer menu
                InsertMenuW(hm, 0, MF_BYPOSITION | MF_STRING, CTX_BACKUP, T(S_BACKUPCOPY));
                InsertMenuW(hm, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            }

            // needed so "Send to", "Open with", "New" ... submenus are filled and drawn
            pcm->QueryInterface(IID_PPV_ARGS(&g_pcm2));
            pcm->QueryInterface(IID_PPV_ARGS(&g_pcm3));

            UINT cmd = (UINT)TrackPopupMenuEx(hm, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, g_hMain, nullptr);

            if (g_pcm2) { g_pcm2->Release(); g_pcm2 = nullptr; }
            if (g_pcm3) { g_pcm3->Release(); g_pcm3 = nullptr; }

            if (cmd == CTX_BACKUP) {
                DoBackup();
            }
            else if (cmd > 0) {
                CMINVOKECOMMANDINFOEX ci = {};
                ci.cbSize = sizeof(ci);
                ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                ci.hwnd = g_hMain;
                ci.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                ci.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                ci.nShow = SW_SHOWNORMAL;
                ci.ptInvoke = pt;
                pcm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci);
                RefreshBoth();
            }
        }
        DestroyMenu(hm);
    }

    if (pcm) pcm->Release();
    if (bg) bg->Release();
}

// ---- Drag source ----------------------------------------------------------
class SimpleDropSource : public IDropSource {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropSource))) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP QueryContinueDrag(BOOL escPressed, DWORD keyState) override
    {
        if (escPressed) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
private:
    LONG m_ref = 1;
};

static void BeginDrag(int p)
{
    if (g_pane[p].shell) return;
    auto sel = GetSelection(p);
    if (sel.empty()) return;

    ShellSel ss;
    if (!ss.Init(sel)) return;

    IDataObject* pdo = nullptr;
    if (FAILED(ss.folder->GetUIObjectOf(g_hMain, (UINT)ss.child.size(), ss.child.data(),
        __uuidof(IDataObject), nullptr, (void**)&pdo)) || !pdo)
        return;

    SimpleDropSource* src = new SimpleDropSource();
    DWORD effect = 0;
    DoDragDrop(pdo, src, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    src->Release();
    pdo->Release();
    RefreshBoth();
}

// ---- Drop target (one per pane) -------------------------------------------
static bool GetDropFiles(IDataObject* pdo, std::vector<std::wstring>& out)
{
    FORMATETC fe = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm = {};
    if (!pdo || FAILED(pdo->GetData(&fe, &sm))) return false;

    HDROP hd = (HDROP)sm.hGlobal;
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(hd, i, nullptr, 0);
        std::wstring s(len, L'\0');
        DragQueryFileW(hd, i, &s[0], len + 1);
        out.push_back(s);
    }
    ReleaseStgMedium(&sm);
    return !out.empty();
}

class PaneDropTarget : public IDropTarget {
public:
    explicit PaneDropTarget(int pane) : m_pane(pane) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDropTarget))) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP DragEnter(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* eff) override
    {
        m_files.clear();
        GetDropFiles(pdo, m_files);
        return DragOver(keys, pt, eff);
    }

    STDMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* eff) override
    {
        int hl = -1;
        std::wstring dest = DestAt(pt, hl);
        SetHilite(hl);
        AutoScroll(pt);
        *eff = ChooseEffect(keys, dest, *eff);
        return S_OK;
    }

    STDMETHODIMP DragLeave() override
    {
        SetHilite(-1);
        m_files.clear();
        return S_OK;
    }

    STDMETHODIMP Drop(IDataObject* pdo, DWORD keys, POINTL pt, DWORD* eff) override
    {
        m_files.clear();
        GetDropFiles(pdo, m_files);
        int hl = -1;
        std::wstring dest = DestAt(pt, hl);
        SetHilite(-1);

        DWORD effect = ChooseEffect(keys, dest, *eff);
        if (effect != DROPEFFECT_NONE && !m_files.empty()) {
            // do the (possibly long) file operation after the drag loop has finished
            DropJob* job = new DropJob{ m_files, dest, effect == DROPEFFECT_MOVE };
            PostMessageW(g_hMain, WM_APP_DROP, 0, (LPARAM)job);
        }
        // We move the files ourselves, so tell the source not to delete anything.
        *eff = (effect == DROPEFFECT_MOVE) ? DROPEFFECT_NONE : effect;
        m_files.clear();
        return S_OK;
    }

private:
    LONG m_ref = 1;
    int  m_pane;
    int  m_hilite = -1;
    std::vector<std::wstring> m_files;

    // Folder under the cursor (if any) or the pane's current folder
    std::wstring DestAt(POINTL pt, int& hl)
    {
        Pane& P = g_pane[m_pane];
        hl = -1;
        POINT cp = { pt.x, pt.y };
        ScreenToClient(P.hList, &cp);
        LVHITTESTINFO ht = {};
        ht.pt = cp;
        int i = ListView_HitTest(P.hList, &ht);
        if (i >= 0 && i < (int)P.items.size() && P.items[i].isDir) {
            hl = i;
            return P.items[i].name == L".." ? ParentOf(P.path) : JoinPath(P.path, P.items[i].name);
        }
        return P.path;
    }

    void SetHilite(int idx)
    {
        if (idx == m_hilite) return;
        HWND h = g_pane[m_pane].hList;
        if (m_hilite >= 0) ListView_SetItemState(h, m_hilite, 0, LVIS_DROPHILITED);
        if (idx >= 0)      ListView_SetItemState(h, idx, LVIS_DROPHILITED, LVIS_DROPHILITED);
        m_hilite = idx;
    }

    void AutoScroll(POINTL pt)
    {
        HWND h = g_pane[m_pane].hList;
        POINT cp = { pt.x, pt.y };
        ScreenToClient(h, &cp);
        RECT rc, hr = {};
        GetClientRect(h, &rc);
        HWND hdr = ListView_GetHeader(h);
        if (hdr) GetWindowRect(hdr, &hr);
        int hh = hr.bottom - hr.top;
        if (cp.x < 0 || cp.x > rc.right || cp.y < 0 || cp.y > rc.bottom) return;
        if (cp.y < hh + 12)             ListView_Scroll(h, 0, -16);
        else if (cp.y > rc.bottom - 12) ListView_Scroll(h, 0, 16);
    }

    // No key: same drive = move, other drive = copy (like Explorer). Ctrl = copy, Shift = move.
    DWORD ChooseEffect(DWORD keys, const std::wstring& dest, DWORD allowed)
    {
        if (m_files.empty() || g_pane[m_pane].shell) return DROPEFFECT_NONE;
        for (const auto& f : m_files) {
            if (IsSubPathOrSame(f, dest)) return DROPEFFECT_NONE;       // into itself
            if (SamePath(ParentOf(f), dest)) return DROPEFFECT_NONE;    // already there
        }
        DWORD e;
        if (keys & MK_CONTROL)    e = DROPEFFECT_COPY;
        else if (keys & MK_SHIFT) e = DROPEFFECT_MOVE;
        else                      e = SameVolume(m_files[0], dest) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        e &= allowed;
        if (!e) e = allowed & DROPEFFECT_COPY;
        return e;
    }
};

// ===========================================================================
//  Menu / language / layout
// ===========================================================================
static void BuildMenu()
{
    auto add = [](HMENU m, UINT id, Str s, const wchar_t* key) {
        std::wstring t = T(s);
        if (key) { t += L"\t"; t += key; }
        AppendMenuW(m, MF_STRING, id, t.c_str());
    };

    HMENU files = CreatePopupMenu();
    add(files, CMD_RENAME, S_RENAME, L"F2");
    add(files, CMD_EDIT, S_EDIT, L"F4");
    add(files, CMD_COPY, S_COPY, L"F5");
    add(files, CMD_MOVE, S_MOVE, L"F6");
    add(files, CMD_NEWFOLDER, S_NEWFOLDER, L"F7");
    add(files, CMD_DELETE, S_DELETE, L"F8");
    AppendMenuW(files, MF_SEPARATOR, 0, nullptr);
    add(files, CMD_REFRESH, S_REFRESH, L"Ctrl+R");
    AppendMenuW(files, MF_SEPARATOR, 0, nullptr);
    add(files, CMD_EXIT, S_EXIT, L"Alt+F4");

    HMENU backup = CreatePopupMenu();
    add(backup, CMD_BACKUP, S_BACKUPCOPY, L"F9");

    HMENU tools = CreatePopupMenu();
    add(tools, CMD_SAVETREE, S_SAVETREE, nullptr);

    HMENU lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING | (g_lang == APPLANG_EN ? MF_CHECKED : 0), CMD_LANG_EN, L"English");
    AppendMenuW(lang, MF_STRING | (g_lang == APPLANG_HU ? MF_CHECKED : 0), CMD_LANG_HU, L"Magyar");

    HMENU help = CreatePopupMenu();
    add(help, CMD_ABOUT, S_ABOUT, nullptr);

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)files, T(S_MENU_FILES));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)backup, T(S_MENU_BACKUP));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, T(S_MENU_TOOLS));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)lang, T(S_MENU_LANG));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, T(S_MENU_HELP));

    HMENU old = GetMenu(g_hMain);
    SetMenu(g_hMain, bar);
    if (old) DestroyMenu(old);
    DrawMenuBar(g_hMain);
}

static void ApplyLanguage()
{
    SetWindowTextW(g_hMain, MainTitle().c_str());
    BuildMenu();

    for (int i = 0; i < kButtonCount; ++i) {
        std::wstring t = std::wstring(kButtonKeys[i]) + L" " + T(kButtonStr[i]);
        SetWindowTextW(g_hButtons[i], t.c_str());
    }

    const Str cols[kColCount] = { S_COL_NAME, S_COL_EXT, S_COL_SIZE, S_COL_DATE };
    for (int p = 0; p < 2; ++p) {
        for (int c = 0; c < kColCount; ++c) {
            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT;
            col.pszText = (LPWSTR)T(cols[c]);
            ListView_SetColumn(g_pane[p].hList, c, &col);
        }
        UpdateSortArrow(p);
        UpdateInfo(p);
    }
}

static void Layout()
{
    if (!g_hStatus) return;
    RECT rc;
    GetClientRect(g_hMain, &rc);
    const int W = rc.right, H = rc.bottom;

    SendMessageW(g_hStatus, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_hStatus, &sr);
    const int sh = sr.bottom - sr.top;

    const int M = 4, btnH = 28, topH = 24, infoH = 20;
    const int btnY = H - sh - btnH - M;
    const int infoY = btnY - M - infoH;
    const int paneW = (W - 3 * M) / 2;
    const int listY = M + topH + M;
    const int listH = std::max(50, infoY - 2 - listY);

    for (int p = 0; p < 2; ++p) {
        Pane& P = g_pane[p];
        const int x = M + p * (paneW + M);
        MoveWindow(P.hCombo, x, M, 120, 300, TRUE);
        MoveWindow(P.hPath, x + 124, M, paneW - 124, topH, TRUE);
        MoveWindow(P.hList, x, listY, paneW, listH, TRUE);
        MoveWindow(P.hInfo, x, infoY, paneW, infoH, TRUE);
        ListView_SetColumnWidth(P.hList, 0, std::max(100, paneW - 60 - 90 - 130 - 28));
    }

    const int bw = (W - M * (kButtonCount + 1)) / kButtonCount;
    for (int i = 0; i < kButtonCount; ++i)
        MoveWindow(g_hButtons[i], M + i * (bw + M), btnY, bw, btnH, TRUE);
}

// Ctrl+Up / Ctrl+Down: step through the list and SELECT the items we pass (instead of only moving the cursor).
// Stepping back the other way takes the selection back again.
static int  g_walkDir[2] = { 0, 0 };
static bool g_walkShrink[2] = { false, false };

static LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR ref)
{
    const int p = (int)ref;

    if ((msg == WM_KEYUP && wParam == VK_CONTROL) || msg == WM_LBUTTONDOWN) {
        g_walkDir[p] = 0;
        g_walkShrink[p] = false;
    }

    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN) &&
        GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_MENU) >= 0)
    {
        Pane& P = g_pane[p];
        const int cnt = (int)P.items.size();
        const int f = ListView_GetNextItem(hwnd, -1, LVNI_FOCUSED);
        if (cnt > 0 && f >= 0 && f < cnt) {
            const int dir = (wParam == VK_DOWN) ? 1 : -1;
            const int n = f + dir;
            if (n >= 0 && n < cnt) {
                auto isSel = [hwnd](int i) { return (ListView_GetItemState(hwnd, i, LVIS_SELECTED) & LVIS_SELECTED) != 0; };
                auto setSel = [hwnd, &P](int i, bool on) {
                    if (P.items[i].name != L"..")
                        ListView_SetItemState(hwnd, i, on ? LVIS_SELECTED : 0, LVIS_SELECTED);
                };

                bool shrink;
                if (g_walkDir[p] != 0 && dir != g_walkDir[p]) shrink = isSel(n);   // direction reversed over selected items
                else                                          shrink = g_walkShrink[p] && isSel(n);

                if (shrink) {
                    setSel(f, false);
                }
                else {
                    setSel(f, true);
                    setSel(n, true);
                }
                ListView_SetItemState(hwnd, n, LVIS_FOCUSED, LVIS_FOCUSED);
                ListView_EnsureVisible(hwnd, n, FALSE);
                g_walkShrink[p] = shrink;
                g_walkDir[p] = dir;
            }
            return 0;           // handled: skip the default "move cursor only" behaviour
        }
    }

    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ListSubclassProc, id);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void OnCreate(HWND hwnd)
{
    g_hMain = hwnd;
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)CTL_STATUS, hInst, nullptr);

    SHFILEINFOW sfi = {};
    HIMAGELIST himl = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);

    const int colFmt[kColCount] = { LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT };
    const int colWidth[kColCount] = { 200, 60, 90, 130 };

    for (int p = 0; p < 2; ++p) {
        Pane& P = g_pane[p];

        P.hCombo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 64, 300, hwnd, (HMENU)(INT_PTR)(CTL_COMBO0 + p), hInst, nullptr);
        P.hPath = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_CENTERIMAGE,
            0, 0, 100, 24, hwnd, nullptr, hInst, nullptr);
        P.hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS | LVS_SHAREIMAGELISTS,
            0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)(CTL_LIST0 + p), hInst, nullptr);
        P.hInfo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_SUNKEN,
            0, 0, 100, 20, hwnd, nullptr, hInst, nullptr);

        ListView_SetExtendedListViewStyle(P.hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        if (himl) ListView_SetImageList(P.hList, himl, LVSIL_SMALL);

        for (int c = 0; c < kColCount; ++c) {
            LVCOLUMNW col = {};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
            col.pszText = (LPWSTR)L"";
            col.fmt = colFmt[c];
            col.cx = colWidth[c];
            col.iSubItem = c;
            ListView_InsertColumn(P.hList, c, &col);
        }

        SetGuiFont(P.hCombo);
        SetGuiFont(P.hPath);
        SetGuiFont(P.hList);
        SetGuiFont(P.hInfo);
        SetWindowSubclass(P.hList, ListSubclassProc, 1, (DWORD_PTR)p);

        // each list accepts files dropped from Explorer or from the other pane
        PaneDropTarget* dt = new PaneDropTarget(p);
        RegisterDragDrop(P.hList, dt);
        dt->Release();
    }

    FillAllDrives();                                      // drive letters + connected phones

    for (int i = 0; i < kButtonCount; ++i) {
        g_hButtons[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 80, 28, hwnd, (HMENU)(INT_PTR)kButtonCmds[i], hInst, nullptr);
        SetGuiFont(g_hButtons[i]);
    }
    SetGuiFont(g_hStatus);

    ApplyLanguage();

    if (!LoadPane(0, StartPath(L"LeftPath", CSIDL_PROFILE)))            LoadPane(0, L"C:\\");
    if (!LoadPane(1, StartPath(L"RightPath", CSIDL_DESKTOPDIRECTORY)))  LoadPane(1, L"C:\\");
}

// ===========================================================================
//  Message handling
// ===========================================================================
static void OnListKey(int p, WORD vk)
{
    SetActive(p);
    Pane& P = g_pane[p];
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;

    switch (vk) {
    case VK_RETURN: OpenItem(p, ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED)); break;
    case VK_BACK:   GoUp(p); break;
    case VK_TAB:    SetFocus(g_pane[1 - p].hList); break;
    case VK_F2:     DoRename(); break;
    case VK_F4:     DoEdit(); break;
    case VK_F5:     DoCopyMove(false); break;
    case VK_F6:     DoCopyMove(true); break;
    case VK_F7:     DoNewFolder(); break;
    case VK_F8:
    case VK_DELETE: DoDelete(); break;
    case VK_F9:     DoBackup(); break;
    case 'R':       if (ctrl) RefreshBoth(); break;
    case VK_INSERT: {   // toggle selection and move down (Total Commander style)
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        if (f >= 0 && f < (int)P.items.size()) {
            if (P.items[f].name != L"..") {
                bool sel = (ListView_GetItemState(P.hList, f, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                ListView_SetItemState(P.hList, f, sel ? 0 : LVIS_SELECTED, LVIS_SELECTED);
            }
            if (f + 1 < (int)P.items.size()) {
                ListView_SetItemState(P.hList, f + 1, LVIS_FOCUSED, LVIS_FOCUSED);
                ListView_EnsureVisible(P.hList, f + 1, FALSE);
            }
        }
        break;
    }
    }
}

static LRESULT OnNotify(LPARAM lParam)
{
    NMHDR* nm = (NMHDR*)lParam;
    int p = (nm->idFrom == CTL_LIST0) ? 0 : (nm->idFrom == CTL_LIST1) ? 1 : -1;
    if (p < 0) return 0;
    Pane& P = g_pane[p];

    switch (nm->code) {
    case NM_SETFOCUS:
        SetActive(p);
        break;

    case NM_DBLCLK: {
        NMITEMACTIVATE* ia = (NMITEMACTIVATE*)lParam;
        SetActive(p);
        OpenItem(p, ia->iItem);
        break;
    }

    case LVN_KEYDOWN:
        OnListKey(p, ((NMLVKEYDOWN*)lParam)->wVKey);
        break;

    case LVN_BEGINDRAG:
        SetActive(p);
        BeginDrag(p);
        break;

    case LVN_ITEMCHANGED: {
        NMLISTVIEW* nl = (NMLISTVIEW*)lParam;
        if (!g_loading && (nl->uChanged & LVIF_STATE) && ((nl->uOldState ^ nl->uNewState) & LVIS_SELECTED))
            UpdateInfo(p);
        break;
    }

    case LVN_COLUMNCLICK: {
        NMLISTVIEW* nl = (NMLISTVIEW*)lParam;
        if (P.sortCol == nl->iSubItem) P.sortAsc = !P.sortAsc;
        else { P.sortCol = nl->iSubItem; P.sortAsc = true; }
        LoadPane(p, P.path);                                       // re-sorts and updates the header arrow
        break;
    }

    case LVN_BEGINLABELEDIT: {
        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
        int i = di->item.iItem;
        if (i < 0 || i >= (int)P.items.size() || P.items[i].name == L"..") return TRUE;   // cancel
        if (P.shell) return TRUE;                                  // no renaming on mobile devices

        // The list shows the name without extension; edit the FULL name, pre-select just the base name
        const Entry& e = P.items[i];
        HWND ed = ListView_GetEditControl(P.hList);
        if (ed) {
            SetWindowTextW(ed, e.name.c_str());
            size_t baseLen = e.ext.empty() ? e.name.size() : e.name.size() - e.ext.size() - 1;
            SendMessageW(ed, EM_SETSEL, 0, (LPARAM)baseLen);
        }
        return FALSE;
    }

    case LVN_ENDLABELEDIT: {
        NMLVDISPINFOW* di = (NMLVDISPINFOW*)lParam;
        if (!di->item.pszText) return FALSE;                       // edit cancelled
        int i = di->item.iItem;
        if (i < 0 || i >= (int)P.items.size()) return FALSE;

        std::wstring newName = di->item.pszText;
        const std::wstring oldName = P.items[i].name;
        if (newName == oldName) return FALSE;
        if (newName.empty() || newName.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
            Msg(T(S_ERR_INVALID_NAME), MB_ICONWARNING);
            return FALSE;
        }
        if (!MoveFileW(JoinPath(P.path, oldName).c_str(), JoinPath(P.path, newName).c_str())) {
            Msg(Fmt(T(S_ERR_RENAME), (unsigned long)GetLastError()), MB_ICONERROR);
            return FALSE;
        }
        P.selectName = newName;
        PostMessageW(g_hMain, WM_APP_REFRESH, 0, 0);               // reload after the notification returns
        return FALSE;
    }
    }
    return 0;
}

static void OnCommand(WPARAM wParam)
{
    const int id = LOWORD(wParam), code = HIWORD(wParam);

    // drive combo boxes
    if (id == CTL_COMBO0 || id == CTL_COMBO1) {
        int p = id - CTL_COMBO0;
        if (code == CBN_DROPDOWN) {
            FillAllDrives();                                  // re-scan: drives and phones may have been (un)plugged
        }
        else if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(g_pane[p].hCombo, CB_GETCURSEL, 0, 0);
            LRESULT data = (i >= 0) ? SendMessageW(g_pane[p].hCombo, CB_GETITEMDATA, i, 0) : 0;
            SetActive(p);
            if (data > 0 && (size_t)data <= g_devices.size()) {          // a mobile device
                Device d = g_devices[data - 1];
                g_pane[p].selectName.clear();
                if (!LoadShellPane(p, d.pidl, d.pidl)) SelectDrive(p);
            }
            else if (i >= 0) {                                           // a drive letter
                wchar_t buf[16] = {};
                SendMessageW(g_pane[p].hCombo, CB_GETLBTEXT, i, (LPARAM)buf);
                if (buf[0] && !LoadPane(p, buf)) SelectDrive(p);
            }
            SetFocus(g_pane[p].hList);
        }
        return;
    }

    bool refocus = true;
    switch (id) {
    case CMD_RENAME:    DoRename();  refocus = false; break;
    case CMD_NEWFOLDER: DoNewFolder(); refocus = false; break;
    case CMD_EDIT:      DoEdit(); break;
    case CMD_COPY:      DoCopyMove(false); break;
    case CMD_MOVE:      DoCopyMove(true); break;
    case CMD_DELETE:    DoDelete(); break;
    case CMD_BACKUP:    DoBackup(); break;
    case CMD_REFRESH:   RefreshBoth(); break;
    case CMD_SAVETREE:  SaveTree(); break;
    case CMD_EXIT:      DestroyWindow(g_hMain); return;
    case CMD_LANG_EN:   g_lang = APPLANG_EN; ApplyLanguage(); break;
    case CMD_LANG_HU:   g_lang = APPLANG_HU; ApplyLanguage(); break;
    case CMD_ABOUT:     Msg(MainTitle() + L"\n\n" + T(S_ABOUT_TEXT)); break;
    default:            refocus = false; break;
    }
    if (refocus) SetFocus(g_pane[g_active].hList);
}

// Right click / Shift+F10 / Menu key on a list -> Explorer context menu
static void OnContextMenu(HWND src, LPARAM lParam)
{
    int p = (src == g_pane[0].hList) ? 0 : (src == g_pane[1].hList) ? 1 : -1;
    if (p < 0) return;
    Pane& P = g_pane[p];
    SetActive(p);

    POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
    bool background;

    if (lParam == (LPARAM)-1) {                       // keyboard: open at the cursor item
        int f = ListView_GetNextItem(P.hList, -1, LVNI_FOCUSED);
        RECT r = {};
        pt.x = 0; pt.y = 0;
        if (f >= 0 && ListView_GetItemRect(P.hList, f, &r, LVIR_BOUNDS)) { pt.x = r.left + 24; pt.y = r.bottom; }
        ClientToScreen(P.hList, &pt);
        background = (f < 0 || f >= (int)P.items.size() || P.items[f].name == L"..");
    }
    else {
        POINT cp = pt;
        ScreenToClient(P.hList, &cp);
        LVHITTESTINFO ht = {};
        ht.pt = cp;
        int i = ListView_HitTest(P.hList, &ht);
        background = (i < 0 || i >= (int)P.items.size() || P.items[i].name == L"..");
        if (!background) {
            // right-clicking an unselected item selects just that item (like Explorer)
            if (!(ListView_GetItemState(P.hList, i, LVIS_SELECTED) & LVIS_SELECTED)) {
                ListView_SetItemState(P.hList, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(P.hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
    }
    ShowShellMenu(p, pt, background);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = 720;
        mm->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_SETFOCUS:
        SetFocus(g_pane[g_active].hList);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HWND h = (HWND)lParam;
        for (int p = 0; p < 2; ++p) {
            if (h == g_pane[p].hPath) {
                bool act = (p == g_active);
                HDC dc = (HDC)wParam;
                SetTextColor(dc, GetSysColor(act ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT));
                SetBkColor(dc, GetSysColor(act ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
                return (LRESULT)GetSysColorBrush(act ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
            }
        }
        break;
    }

    // Messages the shell context menu needs forwarded (owner-drawn items, submenus)
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR:
        if (g_pcm3) {
            LRESULT res = 0;
            if (SUCCEEDED(g_pcm3->HandleMenuMsg2(msg, wParam, lParam, &res))) return res;
        }
        else if (g_pcm2) {
            if (SUCCEEDED(g_pcm2->HandleMenuMsg(msg, wParam, lParam))) return (msg == WM_INITMENUPOPUP) ? 0 : TRUE;
        }
        break;

    case WM_CONTEXTMENU:
        if ((HWND)wParam == g_pane[0].hList || (HWND)wParam == g_pane[1].hList) {
            OnContextMenu((HWND)wParam, lParam);
            return 0;
        }
        break;

    case WM_APP_REFRESH:
        RefreshBoth();
        return 0;

    case WM_APP_DROP: {
        DropJob* job = (DropJob*)lParam;
        if (job) {
            ShellOp(job->move ? FO_MOVE : FO_COPY, job->files, job->dest, 0);
            delete job;
            RefreshBoth();
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TIMER_STATUS) {
            KillTimer(hwnd, TIMER_STATUS);
            SetStatus(L"");
            return 0;
        }
        break;

    case WM_COMMAND:
        OnCommand(wParam);
        return 0;

    case WM_NOTIFY:
        return OnNotify(lParam);

    case WM_DESTROY:
        RevokeDragDrop(g_pane[0].hList);
        RevokeDragDrop(g_pane[1].hList);
        WriteIni(L"Language", g_lang == APPLANG_HU ? L"hu" : L"en");
        WriteIni(L"LeftPath", g_pane[0].shell ? g_pane[0].lastFsPath : g_pane[0].path);
        WriteIni(L"RightPath", g_pane[1].shell ? g_pane[1].lastFsPath : g_pane[1].path);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ===========================================================================
//  Entry point
// ===========================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow)
{
    InitIni();

    std::wstring l = ReadIni(L"Language", L"");
    if (l == L"hu")      g_lang = APPLANG_HU;
    else if (l == L"en") g_lang = APPLANG_EN;
    else g_lang = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_HUNGARIAN) ? APPLANG_HU : APPLANG_EN;

    if (FAILED(OleInitialize(nullptr))) return 1;          // needed for drag & drop and the shell context menu

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"VaultCommanderWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_ICON1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, MainTitle().c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        100, 100, 1100, 650,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { OleUninitialize(); return 1; }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}
