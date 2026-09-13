#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <d2d1_1.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <propidl.h>
#include <dwmapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <conio.h>
#include <timeapi.h>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "dwmapi.lib")
#endif
using namespace std;

static const GUID kGuidSubtypeH264 = { 0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71} };
static const GUID kGuidVideoWMV3 = { 0x334D5637, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71} };

template <class T>
class ComPtr {
    T* m_p = nullptr;
public:
    ComPtr() {}
    ~ComPtr() { Reset(); }
    T** operator&() { Reset(); return &m_p; }
    T* operator->() const { return m_p; }
    T* Get() const { return m_p; }
    operator T* () const { return m_p; }
    void Reset(T* pNew = nullptr) { if (m_p) m_p->Release(); m_p = pNew; }
    T* Detach() { T* p = m_p; m_p = nullptr; return p; }
private:
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

struct Config {
    int fps = 30;
    int bitrateKbps = 8000;
    int monitor = 0;
    int outWidth = 0;
    int outHeight = 0;
    string format = "mp4";
    bool showCursor = true;
    bool audioEnabled = true;
    bool micEnabled = true;
    wstring micId;
    int captureMode = 0;
    wstring windowTitle;
    string outputFile;
};
Config g_cfg;
const char* kConfigFile = "gravador.cfg";

static string MakeAutoFilename(const string& ext) {
    time_t t = time(nullptr);
    tm lt{};
#ifdef _MSC_VER
    localtime_s(&lt, &t);
#else
    tm* plt = localtime(&t);
    if (plt) lt = *plt;
#endif
    ostringstream os;
    os << "recorderkernel11-" << setw(2) << setfill('0') << lt.tm_hour << "-" << setw(2) << setfill('0') << lt.tm_min << "-" << setw(2) << setfill('0') << lt.tm_sec << "-" << setw(2) << setfill('0') << lt.tm_mday << "-" << setw(2) << setfill('0') << (lt.tm_mon + 1) << "-" << (lt.tm_year + 1900) << "." << ext;
    return os.str();
}

static string WideToUtf8(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring Utf8ToWide(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static const WORD kGray = 0x0F, kDim = 0x05, kHover = 0x5F, kTitle = 0x0D, kAccent = 0x0F, kRec = 0x0D, kErr = 0x0D, kOk = 0x0F;
static HANDLE hConsole() { return GetStdHandle(STD_OUTPUT_HANDLE); }
static void SetColor(WORD attr) { SetConsoleTextAttribute(hConsole(), attr); }
static void ClearScreen() { system("cls"); }

enum { KEY_UP = 0x1000, KEY_DOWN = 0x1001, KEY_LEFT = 0x1002, KEY_RIGHT = 0x1003 };
static int ReadKey() {
    for (;;) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 0 || c == 224) {
                int c2 = _kbhit() ? _getch() : -1;
                switch (c2) {
                case 72: return KEY_UP;
                case 80: return KEY_DOWN;
                case 75: return KEY_LEFT;
                case 77: return KEY_RIGHT;
                default: continue;
                }
            }
            return c;
        }
        Sleep(10);
    }
}
static void SleepUntilQpc(LARGE_INTEGER target, LARGE_INTEGER freq) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    if (c.QuadPart >= target.QuadPart) return;
    double ms = (double)(target.QuadPart - c.QuadPart) * 1000.0 / (double)freq.QuadPart;
    if (ms > 2.0) Sleep((DWORD)(ms - 1.0));
    for (;;) {
        QueryPerformanceCounter(&c);
        if (c.QuadPart >= target.QuadPart) break;
        double remMs = (double)(target.QuadPart - c.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (remMs > 0.5) Sleep(0);
        else YieldProcessor();
    }
}
static bool IsUp(int c) { return c == KEY_UP; }
static bool IsDown(int c) { return c == KEY_DOWN; }
static bool IsEnter(int c) { return c == '\r' || c == '\n'; }
static bool IsEsc(int c) { return c == 27; }

static void DrawItems(const vector<string>& items, int sel) {
    const int largura = 46;
    for (size_t i = 0; i < items.size(); i++) {
        bool s = ((int)i == sel);
        SetColor(s ? kHover : kGray);
        string t = "  " + items[i];
        if ((int)t.size() < largura) t += string(largura - (int)t.size(), ' ');
        cout << t << "\n";
    }
    SetColor(kGray);
}
static void DrawFooter(const string& extra = "") {
    cout << "\n";
    SetColor(kDim);
    cout << " [setas] mover   [ENTER] selecionar";
    if (!extra.empty()) cout << "   " << extra;
    cout << "\n";
    SetColor(kGray);
}
static void MessageScreen(const string& msg, WORD color = kGray) {
    ClearScreen();
    cout << "\n\n ";
    SetColor(color);
    cout << msg << "\n\n";
    SetColor(kDim);
    cout << " Press any key...";
    SetColor(kGray);
    cout << flush;
    (void)ReadKey();
}
static int SelectFromList(const string& titulo, const vector<string>& items, int selIni) {
    int n = (int)items.size(), sel = max(0, min(n - 1, selIni));
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "=== " << titulo << " ===\n\n";
        DrawItems(items, sel);
        DrawFooter("[ESC] voltar");
        int c = ReadKey();
        if (IsUp(c)) sel = (sel + n - 1) % n;
        else if (IsDown(c)) sel = (sel + 1) % n;
        else if (IsEnter(c)) return sel;
        else if (IsEsc(c)) return -1;
    }
}
static bool ReadIntConsole(const string& titulo, const string& prompt, int atual, int minV, int maxV, int& novo) {
    string buf, erro;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "=== " << titulo << " ===\n\n";
        SetColor(kGray); cout << " " << prompt << "\n";
        cout << " Atual: " << atual << "\n\n";
        SetColor(kAccent); cout << " > " << buf << "_\n";
        if (!erro.empty()) { SetColor(kErr); cout << " " << erro << "\n"; }
        SetColor(kDim);
        cout << "\n [0-9] digitar   [BACKSPACE] apagar   [ENTER] confirmar   [ESC] cancelar\n";
        SetColor(kGray);
        int c = ReadKey();
        erro.clear();
        if (IsEsc(c)) return false;
        if (IsEnter(c)) {
            if (buf.empty()) return false;
            int v = atoi(buf.c_str());
            if (v < minV || v > maxV) { erro = "Valor fora da faixa: " + to_string(minV) + " a " + to_string(maxV) + "."; continue; }
            novo = v;
            return true;
        }
        if (c == 8) { if (!buf.empty()) buf.pop_back(); continue; }
        if (c >= '0' && c <= '9' && buf.size() < 9) { buf += (char)c; continue; }
    }
}

struct MonitorInfo {
    wstring deviceName;
    string nameUtf8;
};
static vector<MonitorInfo> ListMonitors() {
    vector<MonitorInfo> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return out;
    ComPtr<IDXGIAdapter> adapter;
    for (UINT a = 0; factory->EnumAdapters(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
            DXGI_OUTPUT_DESC d{};
            if (FAILED(output->GetDesc(&d))) continue;
            if (!d.AttachedToDesktop) continue;
            MonitorInfo mi;
            mi.deviceName = d.DeviceName;
            mi.nameUtf8 = WideToUtf8(mi.deviceName);
            out.push_back(mi);
        }
    }
    return out;
}

struct WindowInfo {
    HWND hwnd;
    wstring title;
};
struct FindWindowData {
    const wstring* target;
    HWND found;
};
static BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lp) {
    FindWindowData* d = reinterpret_cast<FindWindowData*>(lp);
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return TRUE;
    wstring title(len + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    title.resize(len);
    if (title == *d->target) {
        d->found = hwnd;
        return FALSE;
    }
    return TRUE;
}
static BOOL CALLBACK EnumWindowsListProc(HWND hwnd, LPARAM lp) {
    vector<WindowInfo>* out = reinterpret_cast<vector<WindowInfo>*>(lp);
    if (!IsWindow(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return TRUE;
    wstring title(len + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], len + 1);
    title.resize(len);
    if (title.empty()) return TRUE;
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, L"ConsoleWindowClass") == 0) return TRUE;
    if (wcscmp(cls, L"Progman") == 0) return TRUE;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0) return TRUE;
    for (auto& w : *out) {
        if (w.title == title) return TRUE;
    }
    WindowInfo wi;
    wi.hwnd = hwnd;
    wi.title = title;
    out->push_back(wi);
    return TRUE;
}
static vector<WindowInfo> ListWindows() {
    vector<WindowInfo> out;
    EnumWindows(EnumWindowsListProc, (LPARAM)&out);
    return out;
}
static HWND FindWindowByTitle(const wstring& title) {
    FindWindowData d{ &title, nullptr };
    EnumWindows(FindWindowProc, (LPARAM)&d);
    return d.found;
}

struct AudioDeviceInfo {
    wstring id;
    string nameUtf8;
    bool isDefault;
};
static vector<AudioDeviceInfo> ListAudioDevices(EDataFlow flow) {
    vector<AudioDeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return out;
    ComPtr<IMMDevice> defDev;
    wstring defId;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &defDev))) {
        LPWSTR did = nullptr;
        if (SUCCEEDED(defDev->GetId(&did))) { defId = did; CoTaskMemFree(did); }
    }
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &col))) return out;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        LPWSTR id = nullptr;
        if (FAILED(d->GetId(&id))) continue;
        ComPtr<IPropertyStore> props;
        string name = "Desconhecido";
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                name = WideToUtf8(pv.pwszVal);
            }
            PropVariantClear(&pv);
        }
        AudioDeviceInfo info;
        info.id = id;
        info.nameUtf8 = name;
        info.isDefault = (defId == id);
        out.push_back(info);
        CoTaskMemFree(id);
    }
    return out;
}

static void SaveConfig() {
    ofstream f(kConfigFile);
    if (!f) return;
    f << "fps=" << g_cfg.fps << "\n";
    f << "bitrate=" << g_cfg.bitrateKbps << "\n";
    f << "monitor=" << g_cfg.monitor << "\n";
    f << "width=" << g_cfg.outWidth << "\n";
    f << "height=" << g_cfg.outHeight << "\n";
    f << "format=" << g_cfg.format << "\n";
    f << "cursor=" << (g_cfg.showCursor ? 1 : 0) << "\n";
    f << "audio=" << (g_cfg.audioEnabled ? 1 : 0) << "\n";
    f << "mic=" << (g_cfg.micEnabled ? 1 : 0) << "\n";
    f << "micid=" << WideToUtf8(g_cfg.micId) << "\n";
    f << "capmode=" << g_cfg.captureMode << "\n";
    f << "wintitle=" << WideToUtf8(g_cfg.windowTitle) << "\n";
}
static void LoadConfig() {
    ifstream f(kConfigFile);
    if (!f) return;
    string line;
    while (getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == string::npos) continue;
        string key = line.substr(0, eq), val = line.substr(eq + 1);
        try {
            if (key == "fps") g_cfg.fps = max(1, min(240, stoi(val)));
            else if (key == "bitrate") g_cfg.bitrateKbps = max(500, min(200000, stoi(val)));
            else if (key == "monitor") g_cfg.monitor = max(0, stoi(val));
            else if (key == "width") g_cfg.outWidth = max(0, stoi(val));
            else if (key == "height") g_cfg.outHeight = max(0, stoi(val));
            else if (key == "format" && (val == "mp4" || val == "wmv")) g_cfg.format = val;
            else if (key == "cursor") g_cfg.showCursor = (val == "1");
            else if (key == "audio") g_cfg.audioEnabled = (val == "1");
            else if (key == "mic") g_cfg.micEnabled = (val == "1");
            else if (key == "micid") g_cfg.micId = Utf8ToWide(val);
            else if (key == "capmode") g_cfg.captureMode = max(0, min(1, stoi(val)));
            else if (key == "wintitle") g_cfg.windowTitle = Utf8ToWide(val);
        }
        catch (...) {}
    }
}

static bool ProbeDefaultEndpoint(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return false;
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev))) return false;
    return dev.Get() != nullptr;
}

struct DecodedCursor {
    vector<BYTE> px;
    int w = 0, h = 0, hx = 0, hy = 0;
};
static bool DecodeCursor(HICON hc, DecodedCursor& dc) {
    ICONINFO ii{};
    if (!GetIconInfo(hc, &ii)) return false;
    int w = 0, h = 0;
    BITMAP bm{};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(BITMAP), &bm)) { w = bm.bmWidth; h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight; }
    else if (ii.hbmMask && GetObject(ii.hbmMask, sizeof(BITMAP), &bm)) { w = bm.bmWidth; h = bm.bmHeight / 2; }
    if (w <= 0 || h <= 0 || w > 256 || h > 256) {
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* pb = nullptr;
    void* pw = nullptr;
    HDC wdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP dibB = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP dibW = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    bool ok = false;
    if (dibB && dibW && pb && pw) {
        HGDIOBJ old = SelectObject(mdc, dibB);
        PatBlt(mdc, 0, 0, w, h, BLACKNESS);
        DrawIconEx(mdc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(mdc, dibW);
        PatBlt(mdc, 0, 0, w, h, WHITENESS);
        DrawIconEx(mdc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(mdc, old);
        const BYTE* B = (const BYTE*)pb;
        const BYTE* W = (const BYTE*)pw;
        dc.px.assign((size_t)w * h * 4, 0);
        for (int i = 0; i < w * h; i++) {
            const BYTE* b = B + i * 4;
            const BYTE* ww = W + i * 4;
            int dif = 0;
            for (int c = 0; c < 3; c++) { int d = ww[c] - b[c]; if (d > dif) dif = d; }
            int a = 255 - dif;
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            BYTE* o = &dc.px[(size_t)i * 4];
            o[0] = b[0]; o[1] = b[1]; o[2] = b[2]; o[3] = (BYTE)a;
        }
        dc.w = w; dc.h = h; dc.hx = (int)ii.xHotspot; dc.hy = (int)ii.yHotspot;
        ok = true;
    }
    if (dibB) DeleteObject(dibB);
    if (dibW) DeleteObject(dibW);
    DeleteDC(mdc);
    ReleaseDC(nullptr, wdc);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return ok;
}

struct AudioBuffer {
    vector<BYTE> data;
    mutex mtx;
    void Push(const BYTE* src, size_t bytes) {
        lock_guard<mutex> lk(mtx);
        data.insert(data.end(), src, src + bytes);
        const size_t MAX = 48000 * 4 * 2;
        if (data.size() > MAX) data.erase(data.begin(), data.begin() + (data.size() - MAX));
    }
    size_t Available() {
        lock_guard<mutex> lk(mtx);
        return data.size();
    }
    size_t Pop(BYTE* dst, size_t bytes) {
        lock_guard<mutex> lk(mtx);
        size_t n = min(bytes, data.size());
        if (n > 0) {
            memcpy(dst, data.data(), n);
            data.erase(data.begin(), data.begin() + n);
        }
        return n;
    }
    void Clear() { lock_guard<mutex> lk(mtx); data.clear(); }
};

class AudioCapture {
public:
    explicit AudioCapture(bool isMic) : m_isMic(isMic) {}
    bool Start(IMFSinkWriter* writer, DWORD streamIdx, const wstring& deviceId, AudioBuffer* buffer = nullptr) {
        if (m_running.load()) return false;
        if (m_thread.joinable()) m_thread.join();
        m_stopFlag.store(false);
        m_writer = writer;
        m_streamIdx = streamIdx;
        m_deviceId = deviceId;
        m_buffer = buffer;
        m_lastError.clear();
        try {
            m_thread = thread(&AudioCapture::ThreadProc, this);
        }
        catch (...) {
            m_lastError = "falha ao criar thread de audio";
            return false;
        }
        for (int i = 0; i < 200; i++) {
            if (m_running.load() || !m_lastError.empty()) break;
            Sleep(10);
        }
        return m_lastError.empty();
    }
    void Stop() {
        m_stopFlag.store(true);
        if (m_thread.joinable()) m_thread.join();
        m_running.store(false);
    }
    bool IsRunning() const { return m_running.load(); }
    string LastError() const { return m_lastError; }
private:
    void ThreadProc() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool comHere = SUCCEEDED(hr);
        DWORD taskIndex = 0;
        HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> audioClient;
        ComPtr<IAudioCaptureClient> captureClient;
        HANDLE hEvent = nullptr;

        do {
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
            if (FAILED(hr)) { m_lastError = "MMDeviceEnumerator falhou"; break; }

            if (m_deviceId.empty()) {
                hr = enumerator->GetDefaultAudioEndpoint(m_isMic ? eCapture : eRender, eConsole, &device);
            }
            else {
                hr = enumerator->GetDevice(m_deviceId.c_str(), &device);
                if (FAILED(hr)) hr = enumerator->GetDefaultAudioEndpoint(m_isMic ? eCapture : eRender, eConsole, &device);
            }
            if (FAILED(hr)) { m_lastError = m_isMic ? "nenhum microfone padrao" : "nenhum dispositivo de saida"; break; }

            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
            if (FAILED(hr)) { m_lastError = "IAudioClient Activate falhou"; break; }

            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = 48000;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = (WORD)((wf.nChannels * wf.wBitsPerSample) / 8);
            wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            wf.cbSize = 0;

            DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (!m_isMic) initFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

            hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags, 1000000, 0, &wf, nullptr);
            if (FAILED(hr)) { m_lastError = "IAudioClient Initialize falhou"; break; }

            hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!hEvent) { m_lastError = "CreateEvent falhou"; break; }

            hr = audioClient->SetEventHandle(hEvent);
            if (FAILED(hr)) { m_lastError = "SetEventHandle falhou"; break; }

            hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
            if (FAILED(hr)) { m_lastError = "GetService(IAudioCaptureClient) falhou"; break; }

            hr = audioClient->Start();
            if (FAILED(hr)) { m_lastError = "IAudioClient Start falhou"; break; }

            m_running.store(true);
            LONGLONG samplePos = 0;

            while (!m_stopFlag.load()) {
                DWORD wait = WaitForSingleObject(hEvent, 100);
                if (wait != WAIT_OBJECT_0) continue;
                if (m_stopFlag.load()) break;

                UINT32 packetLength = 0;
                hr = captureClient->GetNextPacketSize(&packetLength);
                while (SUCCEEDED(hr) && packetLength > 0 && !m_stopFlag.load()) {
                    BYTE* data = nullptr;
                    UINT32 numFrames = 0;
                    DWORD flags = 0;
                    hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
                    if (FAILED(hr)) break;

                    UINT32 bytes = numFrames * 4;

                    if (m_buffer) {
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            vector<BYTE> zeros(bytes, 0);
                            m_buffer->Push(zeros.data(), bytes);
                        }
                        else {
                            m_buffer->Push(data, bytes);
                        }
                    }
                    else if (m_writer) {
                        ComPtr<IMFMediaBuffer> buf;
                        if (SUCCEEDED(MFCreateMemoryBuffer(bytes, &buf))) {
                            BYTE* dst = nullptr;
                            if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
                                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(dst, 0, bytes);
                                else memcpy(dst, data, bytes);
                                buf->Unlock();
                                buf->SetCurrentLength(bytes);

                                ComPtr<IMFSample> sample;
                                if (SUCCEEDED(MFCreateSample(&sample))) {
                                    sample->AddBuffer(buf.Get());
                                    LONGLONG ts = (samplePos * 10000000LL) / 48000;
                                    LONGLONG dur = ((LONGLONG)numFrames * 10000000LL) / 48000;
                                    sample->SetSampleTime(ts);
                                    sample->SetSampleDuration(dur);
                                    m_writer->WriteSample(m_streamIdx, sample.Get());
                                }
                            }
                        }
                    }

                    samplePos += numFrames;
                    captureClient->ReleaseBuffer(numFrames);
                    hr = captureClient->GetNextPacketSize(&packetLength);
                }
            }

            audioClient->Stop();
        } while (false);

        if (hEvent) CloseHandle(hEvent);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (comHere) CoUninitialize();
        m_running.store(false);
    }

    thread m_thread;
    atomic<bool> m_stopFlag{ false };
    atomic<bool> m_running{ false };
    IMFSinkWriter* m_writer = nullptr;
    DWORD m_streamIdx = 0;
    bool m_isMic = false;
    wstring m_deviceId;
    AudioBuffer* m_buffer = nullptr;
    string m_lastError;
};

class AudioMixer {
public:
    bool Start(IMFSinkWriter* writer, DWORD streamIdx, bool useSys, const wstring& sysId, bool useMic, const wstring& micId) {
        if (m_running.load()) return false;
        m_writer = writer;
        m_streamIdx = streamIdx;
        m_useSys = useSys;
        m_useMic = useMic;
        m_stopFlag.store(false);
        m_lastError.clear();
        m_sysBuf.Clear();
        m_micBuf.Clear();

        if (useSys) {
            if (!m_sys.Start(nullptr, 0, sysId, &m_sysBuf)) {
                m_lastError = "sys: " + m_sys.LastError();
                return false;
            }
        }
        if (useMic) {
            if (!m_mic.Start(nullptr, 0, micId, &m_micBuf)) {
                if (useSys) m_sys.Stop();
                m_lastError = "mic: " + m_mic.LastError();
                return false;
            }
        }

        m_running.store(true);
        try {
            m_mixThread = thread(&AudioMixer::MixLoop, this);
        }
        catch (...) {
            m_stopFlag.store(true);
            m_sys.Stop();
            m_mic.Stop();
            m_running.store(false);
            m_lastError = "falha ao criar thread de mixagem";
            return false;
        }
        return true;
    }

    void Stop() {
        if (!m_running.load()) return;
        m_stopFlag.store(true);
        if (m_mixThread.joinable()) m_mixThread.join();
        m_sys.Stop();
        m_mic.Stop();
        m_running.store(false);
    }

    bool IsRunning() const { return m_running.load(); }
    string LastError() const { return m_lastError; }

private:
    void MixLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

        const int FRAMES = 480;
        const int BYTES = FRAMES * 4;

        vector<BYTE> sysChunk(BYTES);
        vector<BYTE> micChunk(BYTES);
        vector<BYTE> mixed(BYTES);
        LONGLONG samplePos = 0;

        while (!m_stopFlag.load()) {
            bool sysReady = !m_useSys || m_sysBuf.Available() >= (size_t)BYTES;
            bool micReady = !m_useMic || m_micBuf.Available() >= (size_t)BYTES;

            if (!sysReady || !micReady) {
                Sleep(1);
                continue;
            }

            if (m_useSys) m_sysBuf.Pop(sysChunk.data(), BYTES);
            if (m_useMic) m_micBuf.Pop(micChunk.data(), BYTES);

            if (m_useSys && m_useMic) {
                int16_t* a = (int16_t*)sysChunk.data();
                int16_t* b = (int16_t*)micChunk.data();
                int16_t* o = (int16_t*)mixed.data();
                for (int i = 0; i < FRAMES * 2; i++) {
                    int v = (int)a[i] + (int)b[i];
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    o[i] = (int16_t)v;
                }
            }
            else if (m_useSys) {
                memcpy(mixed.data(), sysChunk.data(), BYTES);
            }
            else if (m_useMic) {
                memcpy(mixed.data(), micChunk.data(), BYTES);
            }
            else {
                Sleep(5);
                continue;
            }

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(BYTES, &buf))) continue;
            BYTE* dst = nullptr;
            if (FAILED(buf->Lock(&dst, nullptr, nullptr))) continue;
            memcpy(dst, mixed.data(), BYTES);
            buf->Unlock();
            buf->SetCurrentLength(BYTES);

            ComPtr<IMFSample> sample;
            if (FAILED(MFCreateSample(&sample))) continue;
            sample->AddBuffer(buf.Get());
            LONGLONG ts = (samplePos * 10000000LL) / 48000;
            LONGLONG dur = ((LONGLONG)FRAMES * 10000000LL) / 48000;
            sample->SetSampleTime(ts);
            sample->SetSampleDuration(dur);
            m_writer->WriteSample(m_streamIdx, sample.Get());

            samplePos += FRAMES;
        }

        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        CoUninitialize();
    }

    IMFSinkWriter* m_writer = nullptr;
    DWORD m_streamIdx = 0;
    bool m_useSys = false;
    bool m_useMic = false;
    atomic<bool> m_stopFlag{ false };
    atomic<bool> m_running{ false };
    AudioCapture m_sys{ false };
    AudioCapture m_mic{ true };
    AudioBuffer m_sysBuf;
    AudioBuffer m_micBuf;
    thread m_mixThread;
    string m_lastError;
};

class ScreenRecorder {
public:
    bool Start(const Config& cfg) {
        if (m_running.load()) return false;
        if (m_thread.joinable()) m_thread.join();
        m_lastError.clear();
        m_stopFlag.store(false);
        m_startTime = chrono::steady_clock::now();
        m_droppedFrames.store(0);
        m_duplicatedFrames.store(0);
        m_running.store(true);
        try { m_thread = thread(&ScreenRecorder::RecordLoop, this, cfg); }
        catch (...) { m_running.store(false); m_lastError = "falha ao criar a thread de gravacao"; return false; }
        return true;
    }
    void Stop() {
        if (!m_running.load()) return;
        m_stopFlag.store(true);
        if (m_thread.joinable()) m_thread.join();
        m_running.store(false);
    }
    bool IsRecording() const { return m_running.load(); }
    double ElapsedSeconds() const {
        if (!m_running.load()) return 0.0;
        return chrono::duration<double>(chrono::steady_clock::now() - m_startTime).count();
    }
    const string& LastError() const { return m_lastError; }
    UINT64 DroppedFrames() const { return m_droppedFrames.load(); }
    UINT64 DuplicatedFrames() const { return m_duplicatedFrames.load(); }

private:
    static const int RING = 32;

    void RecordLoop(Config cfg) {
        HRESULT hr = S_OK;
        string err;
        DWORD failHR = 0;
        ComPtr<ID3D11Device> d3dDev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<ID3D10Multithread> mt;
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIOutput> output;
        ComPtr<IDXGIOutput1> output1;
        ComPtr<IDXGIOutputDuplication> dupl;
        ComPtr<ID3D11Texture2D> staging[RING];
        ComPtr<ID2D1Factory1> d2dFactory;
        ComPtr<ID2D1Device> d2dDevice;
        ComPtr<ID2D1DeviceContext> d2dCtx;
        ComPtr<ID2D1Bitmap1> frameBmp[RING];
        ComPtr<ID2D1Bitmap1> srcBmp[RING];
        ComPtr<ID2D1Bitmap1> curBmp;
        ComPtr<ID3D11Texture2D> wtex[RING];
        UINT wIdx = (UINT)(RING - 1);
        HICON lastCur = nullptr;
        DecodedCursor dcur;
        LONG orgX = 0, orgY = 0;
        bool cursorInit = false;
        bool cursorVisible = false;
        LONG cursorX = 0, cursorY = 0;
        LONG64 lastMouseStamp = 0;
        bool d2dOk = false;
        ComPtr<IMFDXGIDeviceManager> dxgiMan;
        ComPtr<IMFSinkWriter> writer;
        ComPtr<IDXGIResource> res;
        ComPtr<ID3D11Texture2D> frameTex;
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        LARGE_INTEGER qpf{}, t0{}, now{};
        UINT texW = 0, texH = 0, outW = 0, outH = 0, stageIdx = 0;
        bool haveNew = false, wroteAny = false, comHere = false, mfHere = false, useRam = false;
        bool framePending = false;
        DWORD rowBytes = 0, frameSize = 0;
        ComPtr<ID3D11Texture2D> cpuStage;
        D3D11_MAPPED_SUBRESOURCE mp{};
        BYTE* dstp = nullptr;
        DWORD videoStreamIdx = 0;
        DWORD audioStreamIdx = 0;
        bool useSysAudio = false;
        bool useMic = false;
        UINT64 writeCount = 0;
        double frameDurHns = 0.0;
        DXGI_OUTDUPL_DESC dupDesc{};
        string curFile = cfg.outputFile;
        HWND targetHwnd = nullptr;
        bool windowMode = (cfg.captureMode == 1);
        bool haveLastGood = false;
        bool windowFrozen = false;
        chrono::steady_clock::time_point lastDuplAttempt = chrono::steady_clock::now() - chrono::seconds(10);
        DWORD acquireTimeoutMs = 8;
        HANDLE hMmcssCapture = nullptr;
        DWORD mmcssTaskIndex = 0;

        auto CreateCaptureTextures = [&](UINT w, UINT h) -> HRESULT {
            HRESULT r = S_OK;
            for (int i = 0; i < RING; i++) { staging[i].Reset(); srcBmp[i].Reset(); }
            for (int i = 0; i < RING; i++) { wtex[i].Reset(); frameBmp[i].Reset(); }
            cpuStage.Reset(); curBmp.Reset();
            for (int i = 0; i < RING; i++) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w; td.Height = h;
                td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                r = d3dDev->CreateTexture2D(&td, nullptr, &staging[i]);
                if (FAILED(r)) return r;
                if (d2dOk) {
                    // Bitmap D2D que "embrulha" a textura de staging (frame cheio do
                    // monitor, sempre em texW x texH). Como ele compartilha a memoria
                    // da GPU com a textura, qualquer CopyResource para dentro de
                    // staging[i] atualiza automaticamente o conteudo deste bitmap -
                    // nao e preciso recriar por frame. E a origem usada para o recorte
                    // de janela (DrawBitmap com srcRect = retangulo da janela).
                    ComPtr<IDXGISurface> ssurf;
                    if (FAILED(staging[i]->QueryInterface(__uuidof(IDXGISurface), (void**)&ssurf))) return E_FAIL;
                    D2D1_BITMAP_PROPERTIES1 sbp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                    if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(ssurf.Get(), &sbp, &srcBmp[i]))) return E_FAIL;
                }
            }
            for (int i = 0; i < RING; i++) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = outW; td.Height = outH;
                td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                r = d3dDev->CreateTexture2D(&td, nullptr, &wtex[i]);
                if (FAILED(r)) return r;
                if (d2dOk) {
                    ComPtr<IDXGISurface> surf;
                    if (FAILED(wtex[i]->QueryInterface(__uuidof(IDXGISurface), (void**)&surf))) return E_FAIL;
                    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                    if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &frameBmp[i]))) return E_FAIL;
                }
            }
            return S_OK;
            };

        auto AddAacStream = [&](IMFSinkWriter* w, DWORD* outIdx) -> bool {
            ComPtr<IMFMediaType> ao;
            MFCreateMediaType(&ao);
            ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
            ao->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            DWORD idx = 0;
            if (FAILED(w->AddStream(ao.Get(), &idx))) return false;
            ComPtr<IMFMediaType> ai;
            MFCreateMediaType(&ai);
            ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
            ai->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
            if (FAILED(w->SetInputMediaType(idx, ai.Get(), nullptr))) return false;
            *outIdx = idx;
            return true;
            };

        auto CreateVideoWriter = [&](const wstring& file) -> HRESULT {
            ComPtr<IMFAttributes> at;
            MFCreateAttributes(&at, 4);
            at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            at->SetUINT32(MF_LOW_LATENCY, TRUE);
            at->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgiMan.Get());
            ComPtr<IMFSinkWriter> w;
            HRESULT r = MFCreateSinkWriterFromURL(file.c_str(), nullptr, at.Get(), &w);
            if (FAILED(r)) return r;

            ComPtr<IMFMediaType> ot;
            MFCreateMediaType(&ot);
            ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            if (cfg.format == "wmv") ot->SetGUID(MF_MT_SUBTYPE, kGuidVideoWMV3);
            else { ot->SetGUID(MF_MT_SUBTYPE, kGuidSubtypeH264); ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High); }
            ot->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)cfg.bitrateKbps * 1000);
            ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, outW, outH);
            MFSetAttributeRatio(ot.Get(), MF_MT_FRAME_RATE, (UINT32)cfg.fps, 1);
            MFSetAttributeRatio(ot.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            DWORD si = 0;
            r = w->AddStream(ot.Get(), &si);
            if (FAILED(r)) return r;
            videoStreamIdx = si;

            ComPtr<IMFMediaType> it2;
            MFCreateMediaType(&it2);
            it2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            it2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            it2->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(it2.Get(), MF_MT_FRAME_SIZE, outW, outH);
            MFSetAttributeRatio(it2.Get(), MF_MT_FRAME_RATE, (UINT32)cfg.fps, 1);
            MFSetAttributeRatio(it2.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            r = w->SetInputMediaType(si, it2.Get(), nullptr);
            if (FAILED(r)) return r;

            audioStreamIdx = 0;
            if (useSysAudio || useMic) {
                if (!AddAacStream(w.Get(), &audioStreamIdx)) audioStreamIdx = 0;
            }

            r = w->BeginWriting();
            if (FAILED(r)) return r;
            writer.Reset(w.Detach());
            return S_OK;
            };

        // Modo janela: sempre compomos via D2D (crop + eventual escala), nunca com
        // um "fast path" de copia direta baseado em suposicoes de tamanho identico.
        // Isso elimina o bug de cair sem querer no ramo de tela inteira quando o
        // tamanho do cliente da janela nao bate exatamente com outW/outH.
        auto WriteFrame = [&](double ts) -> HRESULT {
            bool wroteContent = false;

            if (windowMode) {
                bool winOk = targetHwnd && IsWindow(targetHwnd) && !IsIconic(targetHwnd);
                bool cloaked = false;
                if (winOk) {
                    DWORD cloakedVal = 0;
                    if (SUCCEEDED(DwmGetWindowAttribute(targetHwnd, DWMWA_CLOAKED, &cloakedVal, sizeof(cloakedVal)))) {
                        cloaked = (cloakedVal != 0);
                    }
                }
                if (winOk && !cloaked) {
                    RECT cr;
                    GetClientRect(targetHwnd, &cr);
                    POINT tl = { 0, 0 };
                    ClientToScreen(targetHwnd, &tl);

                    LONG lx = tl.x - orgX;
                    LONG ly = tl.y - orgY;
                    LONG lw = cr.right - cr.left;
                    LONG lh = cr.bottom - cr.top;
                    if (lx < 0) { lw += lx; lx = 0; }
                    if (ly < 0) { lh += ly; ly = 0; }
                    if (lx + lw > (LONG)texW) lw = (LONG)texW - lx;
                    if (ly + lh > (LONG)texH) lh = (LONG)texH - ly;

                    if (lw > 0 && lh > 0 && d2dOk && srcBmp[stageIdx].Get()) {
                        wIdx = (wIdx + 1) % RING;
                        d2dCtx->SetTarget(frameBmp[wIdx].Get());
                        d2dCtx->BeginDraw();
                        d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 1.0f));
                        D2D1_RECT_F srcRect = D2D1::RectF((float)lx, (float)ly, (float)(lx + lw), (float)(ly + lh));
                        D2D1_RECT_F dstRect = D2D1::RectF(0, 0, (float)outW, (float)outH);
                        d2dCtx->DrawBitmap(srcBmp[stageIdx].Get(), &dstRect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, &srcRect);
                        if (cfg.showCursor && cursorInit && cursorVisible && dcur.w > 0 && curBmp.Get()) {
                            float sx = (float)outW / (float)max<LONG>(1, lw);
                            float sy = (float)outH / (float)max<LONG>(1, lh);
                            float dx = ((float)(cursorX - orgX - lx - dcur.hx)) * sx;
                            float dy = ((float)(cursorY - orgY - ly - dcur.hy)) * sy;
                            D2D1_RECT_F dr = D2D1::RectF(dx, dy, dx + (float)dcur.w * sx, dy + (float)dcur.h * sy);
                            d2dCtx->DrawBitmap(curBmp.Get(), &dr);
                        }
                        HRESULT hd = d2dCtx->EndDraw();
                        if (FAILED(hd)) {
                            d2dCtx.Reset(); d2dDevice.Reset(); d2dFactory.Reset(); curBmp.Reset();
                            d2dOk = false;
                        }
                        wroteContent = true;
                        haveLastGood = true;
                        windowFrozen = false;
                    }
                    else if (lw > 0 && lh > 0 && !d2dOk) {
                        wIdx = (wIdx + 1) % RING;
                        D3D11_BOX box = {};
                        box.left = (UINT)lx; box.top = (UINT)ly; box.front = 0;
                        box.right = (UINT)min<LONG>(lx + lw, (LONG)texW);
                        box.bottom = (UINT)min<LONG>(ly + lh, (LONG)texH);
                        box.back = 1;
                        UINT cw = box.right - box.left, ch = box.bottom - box.top;
                        cw = min(cw, outW); ch = min(ch, outH);
                        box.right = box.left + cw; box.bottom = box.top + ch;
                        ctx->CopySubresourceRegion(wtex[wIdx].Get(), 0, 0, 0, 0, staging[stageIdx].Get(), 0, &box);
                        wroteContent = true;
                        haveLastGood = true;
                        windowFrozen = false;
                    }
                }
                else {
                    // Janela minimizada/oculta (ex.: alt-tab em fullscreen exclusivo).
                    // Nao tentamos compor lixo: apenas repetimos o ultimo frame bom
                    // (congelamento controlado), sem alterar buffers/estado de cursor.
                    windowFrozen = true;
                }
            }
            else {
                wIdx = (wIdx + 1) % RING;
                ctx->CopyResource(wtex[wIdx].Get(), staging[stageIdx].Get());

                if (cfg.showCursor && d2dOk && cursorInit && cursorVisible && dcur.w > 0 && curBmp.Get() && frameBmp[wIdx].Get()) {
                    d2dCtx->SetTarget(frameBmp[wIdx].Get());
                    d2dCtx->BeginDraw();
                    float dx = (float)(cursorX - orgX - dcur.hx);
                    float dy = (float)(cursorY - orgY - dcur.hy);
                    D2D1_RECT_F dr = D2D1::RectF(dx, dy, dx + (float)dcur.w, dy + (float)dcur.h);
                    d2dCtx->DrawBitmap(curBmp.Get(), &dr);
                    HRESULT hd = d2dCtx->EndDraw();
                    if (FAILED(hd)) {
                        d2dCtx.Reset(); d2dDevice.Reset(); d2dFactory.Reset(); curBmp.Reset();
                        d2dOk = false;
                    }
                }
                wroteContent = true;
            }

            if (!wroteContent && !haveLastGood) {
                wIdx = (wIdx + 1) % RING;
                ctx->CopyResource(wtex[wIdx].Get(), staging[stageIdx].Get());
            }
            else if (!wroteContent && haveLastGood) {
                wIdx = (wIdx == 0) ? (RING - 1) : (wIdx - 1);
                m_duplicatedFrames.fetch_add(1);
            }

            ID3D11Texture2D* srcTex = wtex[wIdx].Get();
            ComPtr<IMFMediaBuffer> outBuf;
            if (!useRam) {
                HRESULT r = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), srcTex, 0, FALSE, &outBuf);
                if (FAILED(r)) return r;
                IMF2DBuffer* p2d = nullptr;
                if (SUCCEEDED(outBuf->QueryInterface(__uuidof(IMF2DBuffer), (void**)&p2d))) {
                    DWORD c2 = 0;
                    if (SUCCEEDED(p2d->GetContiguousLength(&c2))) outBuf->SetCurrentLength(c2);
                    p2d->Release();
                }
            }
            else {
                if (!cpuStage.Get()) {
                    D3D11_TEXTURE2D_DESC cd{};
                    cd.Width = outW; cd.Height = outH; cd.MipLevels = 1; cd.ArraySize = 1;
                    cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
                    cd.Usage = D3D11_USAGE_STAGING;
                    cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    HRESULT r = d3dDev->CreateTexture2D(&cd, nullptr, &cpuStage);
                    if (FAILED(r)) return r;
                }
                ctx->CopyResource(cpuStage.Get(), srcTex);
                mp = {};
                if (FAILED(ctx->Map(cpuStage.Get(), 0, D3D11_MAP_READ, 0, &mp))) return E_FAIL;
                rowBytes = outW * 4;
                frameSize = rowBytes * outH;
                ComPtr<IMFMediaBuffer> mem;
                HRESULT r = MFCreateMemoryBuffer(frameSize, &mem);
                if (FAILED(r)) { ctx->Unmap(cpuStage.Get(), 0); return r; }
                dstp = nullptr;
                r = mem->Lock(&dstp, nullptr, nullptr);
                if (SUCCEEDED(r)) {
                    BYTE* srcp = (BYTE*)mp.pData;
                    for (UINT rr = 0; rr < outH; rr++) memcpy(dstp + (size_t)rr * rowBytes, srcp + (size_t)rr * mp.RowPitch, rowBytes);
                    mem->Unlock();
                    mem->SetCurrentLength(frameSize);
                }
                ctx->Unmap(cpuStage.Get(), 0);
                if (FAILED(r)) return r;
                outBuf.Reset(mem.Detach());
            }
            ComPtr<IMFSample> s;
            HRESULT r = MFCreateSample(&s);
            if (FAILED(r)) return r;
            r = s->AddBuffer(outBuf.Get());
            if (FAILED(r)) return r;
            r = s->SetSampleTime((LONGLONG)ts);
            if (FAILED(r)) return r;
            r = s->SetSampleDuration((LONGLONG)frameDurHns);
            if (FAILED(r)) return r;
            return writer->WriteSample(videoStreamIdx, s.Get());
            };

        if (windowMode) {
            targetHwnd = FindWindowByTitle(cfg.windowTitle);
            if (!targetHwnd) {
                err = "janela alvo nao encontrada: " + WideToUtf8(cfg.windowTitle);
                goto cleanup;
            }
        }

        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) comHere = true;
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) { err = "MFStartup falhou"; failHR = (DWORD)hr; goto cleanup; }
        mfHere = true;
        useSysAudio = cfg.audioEnabled && ProbeDefaultEndpoint(eRender);
        useMic = cfg.micEnabled && ProbeDefaultEndpoint(eCapture);
        QueryPerformanceFrequency(&qpf);
        // Prioridade normal + classe MMCSS "Capture": garante boa prioridade de
        // agendamento para a thread de captura sem brigar por CPU com o jogo do
        // jeito que THREAD_PRIORITY_ABOVE_NORMAL fazia sob carga alta.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        hMmcssCapture = AvSetMmThreadCharacteristicsW(L"Capture", &mmcssTaskIndex);
        if (!hMmcssCapture) hMmcssCapture = AvSetMmThreadCharacteristicsW(L"Games", &mmcssTaskIndex);
        {
            UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            D3D_FEATURE_LEVEL got{};
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDev, &got, &ctx);
            if (FAILED(hr)) { err = "D3D11CreateDevice falhou"; failHR = (DWORD)hr; goto cleanup; }
        }
        hr = ctx->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt);
        if (SUCCEEDED(hr)) mt->SetMultithreadProtected(TRUE);
        hr = d3dDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
        if (FAILED(hr)) { err = "IDXGIDevice"; failHR = (DWORD)hr; goto cleanup; }
        dxgiDev->GetAdapter(&adapter);
        hr = adapter->EnumOutputs((UINT)cfg.monitor, &output);
        if (FAILED(hr)) { err = "monitor invalido"; failHR = (DWORD)hr; goto cleanup; }
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (FAILED(hr)) { err = "IDXGIOutput1"; failHR = (DWORD)hr; goto cleanup; }
        hr = output1->DuplicateOutput(d3dDev.Get(), &dupl);
        if (FAILED(hr)) { err = "DuplicateOutput falhou"; failHR = (DWORD)hr; goto cleanup; }
        dupl->GetDesc(&dupDesc);
        texW = dupDesc.ModeDesc.Width;
        texH = dupDesc.ModeDesc.Height;
        if (texW == 0 || texH == 0) { err = "resolucao do desktop invalida"; goto cleanup; }

        if (windowMode) {
            RECT cr;
            GetClientRect(targetHwnd, &cr);
            UINT wcw = (UINT)((cr.right - cr.left) & ~1);
            UINT wch = (UINT)((cr.bottom - cr.top) & ~1);
            if (wcw >= 64 && wch >= 64) { outW = wcw; outH = wch; }
            else { outW = texW; outH = texH; }
        }
        else {
            outW = (cfg.outWidth > 0) ? (UINT)(cfg.outWidth & ~1) : texW;
            outH = (cfg.outHeight > 0) ? (UINT)(cfg.outHeight & ~1) : texH;
        }
        if (outW < 2) outW = 2;
        if (outH < 2) outH = 2;

        {
            UINT resetToken = 0;
            hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiMan);
            if (FAILED(hr)) { err = "MFCreateDXGIDeviceManager"; failHR = (DWORD)hr; goto cleanup; }
            hr = dxgiMan->ResetDevice(d3dDev.Get(), resetToken);
            if (FAILED(hr)) { err = "ResetDevice"; failHR = (DWORD)hr; goto cleanup; }
        }
        {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(output->GetDesc(&od))) { orgX = od.DesktopCoordinates.left; orgY = od.DesktopCoordinates.top; }
            do {
                if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), (void**)&d2dFactory))) break;
                if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice))) break;
                if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx))) break;
                d2dOk = true;
            } while (false);
            hr = CreateCaptureTextures(texW, texH);
            if (FAILED(hr)) { err = "CreateTexture2D staging"; failHR = (DWORD)hr; goto cleanup; }
            if (!d2dOk && cfg.showCursor) cout << "\n cursor: falha ao iniciar composicao - gravando sem cursor\n";
            if (windowMode && !d2dOk) cout << "\n aviso: composicao D2D indisponivel - recorte de janela em modo de compatibilidade\n";
            wstring wfile = Utf8ToWide(curFile);
            hr = CreateVideoWriter(wfile);
            if (FAILED(hr)) { err = "nao foi possivel iniciar o encoder"; failHR = (DWORD)hr; goto cleanup; }
            if ((useSysAudio || useMic) && audioStreamIdx > 0) {
                if (!m_mixer.Start(writer.Get(), audioStreamIdx, useSysAudio, L"", useMic, cfg.micId)) {
                    cout << "\n mixer de audio: falha (" << m_mixer.LastError() << ") - continuando sem audio\n";
                    useSysAudio = false;
                    useMic = false;
                    audioStreamIdx = 0;
                }
            }
        }
        QueryPerformanceCounter(&t0);
        frameDurHns = 10'000'000.0 / (double)cfg.fps;
        // Timeout de aquisicao proporcional ao periodo do frame (limitado entre 2 e 16ms)
        // em vez de fixo em 1ms - reduz polling excessivo sem atrasar o pacing.
        acquireTimeoutMs = (DWORD)max(2.0, min(16.0, (frameDurHns / 10000.0) * 0.5));

        while (!m_stopFlag.load()) {
            if (!dupl.Get()) {
                if (framePending) framePending = false;
                auto nowSteady = chrono::steady_clock::now();
                auto sinceAttempt = chrono::duration_cast<chrono::milliseconds>(nowSteady - lastDuplAttempt).count();
                if (sinceAttempt < 1500) {
                    Sleep(20);
                    continue;
                }
                lastDuplAttempt = nowSteady;
                if (FAILED(output1->DuplicateOutput(d3dDev.Get(), &dupl))) {
                    Sleep(20);
                    continue;
                }
                DXGI_OUTDUPL_DESC d2{};
                dupl->GetDesc(&d2);
                UINT nw = d2.ModeDesc.Width, nh = d2.ModeDesc.Height;
                lastMouseStamp = 0;
                cursorInit = false;

                bool resChanged = (nw != texW || nh != texH);
                if (!windowMode && resChanged && cfg.outWidth == 0 && cfg.outHeight == 0) {
                    if (m_mixer.IsRunning()) m_mixer.Stop();
                    if (writer.Get() && wroteAny) writer->Finalize();
                    writer.Reset();
                    texW = nw; texH = nh;
                    outW = texW; outH = texH;
                    hr = CreateCaptureTextures(texW, texH);
                    if (FAILED(hr)) { err = "CreateTexture2D staging"; failHR = (DWORD)hr; goto cleanup; }
                    curFile = MakeAutoFilename(cfg.format);
                    g_cfg.outputFile = curFile;
                    hr = CreateVideoWriter(Utf8ToWide(curFile));
                    if (FAILED(hr)) { err = "nao foi possivel reiniciar o encoder"; failHR = (DWORD)hr; goto cleanup; }
                    if ((useSysAudio || useMic) && audioStreamIdx > 0) {
                        if (!m_mixer.Start(writer.Get(), audioStreamIdx, useSysAudio, L"", useMic, cfg.micId)) {
                            useSysAudio = false; useMic = false; audioStreamIdx = 0;
                        }
                    }
                    writeCount = 0;
                    QueryPerformanceCounter(&t0);
                    haveNew = false; wroteAny = false;
                    cout << "\n resolucao alterada para " << texW << "x" << texH << " - continuando em novo arquivo: " << curFile << "\n";
                }
                else if (windowMode) {
                    texW = nw; texH = nh;
                }
            }

            hr = dupl->AcquireNextFrame(acquireTimeoutMs, &frameInfo, &res);
            if (hr == S_OK) {
                framePending = true;
                if (d2dOk && cfg.showCursor) {
                    bool timeChanged = (frameInfo.LastMouseUpdateTime.QuadPart != lastMouseStamp);
                    bool posChanged = (frameInfo.PointerPosition.Position.x != cursorX) ||
                        (frameInfo.PointerPosition.Position.y != cursorY);
                    bool visChanged = ((frameInfo.PointerPosition.Visible != FALSE) != cursorVisible);
                    if (timeChanged || posChanged || visChanged) {
                        if (timeChanged) lastMouseStamp = frameInfo.LastMouseUpdateTime.QuadPart;
                        CURSORINFO ci{};
                        ci.cbSize = sizeof(CURSORINFO);
                        bool win32Visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
                        bool dxgiVisible = frameInfo.PointerPosition.Visible != FALSE;
                        if (win32Visible && dxgiVisible) {
                            cursorInit = true;
                            cursorVisible = true;
                            cursorX = frameInfo.PointerPosition.Position.x;
                            cursorY = frameInfo.PointerPosition.Position.y;
                            if (ci.hCursor != lastCur) {
                                lastCur = ci.hCursor;
                                if (!DecodeCursor(ci.hCursor, dcur)) dcur.w = 0;
                                if (dcur.w > 0) {
                                    D2D1_BITMAP_PROPERTIES1 cbp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                                    D2D1_SIZE_U sz = { (UINT)dcur.w, (UINT)dcur.h };
                                    curBmp.Reset();
                                    d2dCtx->CreateBitmap(sz, dcur.px.data(), dcur.w * 4, &cbp, &curBmp);
                                }
                            }
                        }
                        else if (!win32Visible && !dxgiVisible) {
                            cursorVisible = false;
                        }
                    }
                }
                stageIdx = (stageIdx + 1) % RING;
                frameTex.Reset();
                res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameTex);
                if (frameTex.Get()) {
                    ctx->CopyResource(staging[stageIdx].Get(), frameTex.Get());
                    // srcBmp[stageIdx] ja embrulha staging[stageIdx] (criado em
                    // CreateCaptureTextures) e reflete este CopyResource automaticamente,
                    // pois compartilha a mesma superficie DXGI - nada mais a fazer aqui.
                }
                frameTex.Reset();
                res.Reset();
                dupl->ReleaseFrame();
                framePending = false;
                haveNew = true;
            }
            else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                if (haveNew == false) m_droppedFrames.fetch_add(0); // sem novidade; nao conta como drop, apenas idle
            }
            else if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == E_ACCESSDENIED) {
                if (framePending && dupl.Get()) {
                    dupl->ReleaseFrame();
                    framePending = false;
                }
                dupl.Reset();
            }
            else {
                if (framePending && dupl.Get()) {
                    dupl->ReleaseFrame();
                    framePending = false;
                }
                err = "AcquireNextFrame falhou";
                failHR = (DWORD)hr;
                goto cleanup;
            }

            QueryPerformanceCounter(&now);
            double elapsedHns = (double)(now.QuadPart - t0.QuadPart) * 10'000'000.0 / (double)qpf.QuadPart;
            double dueTs = (double)writeCount * frameDurHns;

            // CORRECAO CRITICA (stutter / aceleracao subita / FPS instavel):
            // em vez de escrever no maximo 1 frame por iteracao do loop mesmo
            // quando ja estamos muito atrasados em relacao ao relogio real,
            // fazemos "catch-up": escrevemos quantos frames (duplicando o
            // ultimo conteudo disponivel) forem necessarios para que o numero
            // de amostras gravadas acompanhe o tempo de parede real. Sem isso,
            // qualquer atraso (WriteSample lento, troca de encoder, disco lento)
            // fazia o video final ter menos frames do que o tempo real decorrido,
            // o que se manifesta como "o video acelera sozinho".
            const UINT64 MAX_CATCHUP_PER_TICK = (UINT64)max(1, cfg.fps * 2);
            UINT64 catchup = 0;
            while (elapsedHns >= dueTs && (haveNew || wroteAny) && catchup < MAX_CATCHUP_PER_TICK) {
                hr = WriteFrame(dueTs);
                if (FAILED(hr)) {
                    if (!wroteAny && !useRam) {
                        useRam = true;
                        cout << "\n encoder recusou textura de GPU - usando modo de compatibilidade em RAM\n";
                        continue;
                    }
                    else {
                        err = "WriteSample";
                        failHR = (DWORD)hr;
                        goto cleanup;
                    }
                }
                wroteAny = true;
                writeCount++;
                haveNew = false;
                catchup++;
                dueTs = (double)writeCount * frameDurHns;
            }
            if (catchup > 1) {
                m_duplicatedFrames.fetch_add(catchup - 1);
            }
            if (catchup >= MAX_CATCHUP_PER_TICK) {
                // atraso maior que 2x a duracao de um segundo de frames: nao tenta
                // recuperar tudo de uma vez (evita travar o loop), realinha o
                // relogio de referencia para o instante atual e conta como perda.
                m_droppedFrames.fetch_add(1);
                QueryPerformanceCounter(&t0);
                writeCount = 0;
            }

            QueryPerformanceCounter(&now);
            elapsedHns = (double)(now.QuadPart - t0.QuadPart) * 10'000'000.0 / (double)qpf.QuadPart;
            double nextDue = (double)writeCount * frameDurHns;
            if (nextDue > elapsedHns) {
                LARGE_INTEGER target;
                target.QuadPart = t0.QuadPart + (LONGLONG)(nextDue * (double)qpf.QuadPart / 10'000'000.0);
                SleepUntilQpc(target, qpf);
            }
        }
    cleanup:
        if (m_mixer.IsRunning()) m_mixer.Stop();
        if (!wroteAny) {
            if (err.empty()) err = "nenhum frame capturado - gravacao parada antes do primeiro frame";
            writer.Reset();
            DeleteFileW(Utf8ToWide(curFile).c_str());
        }
        else if (writer.Get()) {
            HRESULT hf = writer->Finalize();
            if (FAILED(hf) && err.empty()) { err = "Finalize"; failHR = (DWORD)hf; }
        }
        writer.Reset();
        dxgiMan.Reset();
        for (int i = 0; i < RING; i++) { staging[i].Reset(); srcBmp[i].Reset(); }
        cpuStage.Reset();
        for (int i = 0; i < RING; i++) { frameBmp[i].Reset(); wtex[i].Reset(); }
        curBmp.Reset();
        d2dCtx.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        dupl.Reset(); output1.Reset(); output.Reset(); adapter.Reset(); dxgiDev.Reset();
        mt.Reset(); ctx.Reset(); d3dDev.Reset();
        if (hMmcssCapture) AvRevertMmThreadCharacteristics(hMmcssCapture);
        if (mfHere) MFShutdown();
        if (comHere) CoUninitialize();
        if (!err.empty()) {
            ostringstream os;
            os << hex << uppercase << setw(8) << setfill('0') << failHR;
            m_lastError = err + " | hr=0x" + os.str();
        }
        m_running.store(false);
    }

    thread m_thread;
    atomic<bool> m_running{ false };
    atomic<bool> m_stopFlag{ false };
    chrono::steady_clock::time_point m_startTime;
    string m_lastError;
    AudioMixer m_mixer;
    atomic<UINT64> m_droppedFrames{ 0 };
    atomic<UINT64> m_duplicatedFrames{ 0 };
};

ScreenRecorder g_recorder;
static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) g_recorder.Stop();
    return TRUE;
}

void startrecord();
void openconfig();
static string ResLabel() {
    if (g_cfg.captureMode == 1) return "Janela";
    if (g_cfg.outWidth > 0 && g_cfg.outHeight > 0) return to_string(g_cfg.outWidth) + "x" + to_string(g_cfg.outHeight);
    return "AUTO";
}
static void RecordingScreen() {
    ClearScreen();
    SetColor(kTitle); cout << "==================================================\n";
    SetColor(kRec); cout << "   [REC]  GRAVANDO TELA  -  dev - kernel11\n";
    SetColor(kTitle); cout << "==================================================\n";
    SetColor(kGray);
    cout << " Arquivo: " << g_cfg.outputFile << "\n";
    cout << " Alvo   : " << (g_cfg.captureMode == 1 ? ("Janela: " + WideToUtf8(g_cfg.windowTitle)) : ("Monitor " + to_string(g_cfg.monitor))) << "\n";
    cout << " Config : " << g_cfg.fps << " fps | " << g_cfg.bitrateKbps << " kbps | " << g_cfg.format
        << " | audio " << (g_cfg.audioEnabled ? "Sim" : "Nao")
        << " | mic " << (g_cfg.micEnabled ? "Sim" : "Nao") << "\n";
    SetColor(kTitle); cout << "--------------------------------------------------\n";
    SetColor(kGray); cout << " Duracao: " << flush;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    COORD timerPos{};
    bool hasPos = GetConsoleScreenBufferInfo(hConsole(), &csbi) != 0;
    if (hasPos) timerPos = csbi.dwCursorPosition;
    SetColor(kAccent);
    cout << "00:00\n";
    SetColor(kTitle); cout << "--------------------------------------------------\n";
    SetColor(kAccent); cout << "\n   >>> Press ENTER to finish record. <<<\n\n";
    SetColor(kGray);
    int lastSec = -1;
    UINT64 lastDrop = (UINT64)-1, lastDup = (UINT64)-1;
    while (g_recorder.IsRecording()) {
        int c = -1;
        if (_kbhit()) {
            c = _getch();
            if (c == 0 || c == 224) { if (_kbhit()) (void)_getch(); c = -1; }
        }
        if (IsEnter(c) || IsEsc(c)) break;
        int sec = (int)g_recorder.ElapsedSeconds();
        UINT64 drop = g_recorder.DroppedFrames();
        UINT64 dup = g_recorder.DuplicatedFrames();
        if (sec != lastSec || drop != lastDrop || dup != lastDup) {
            lastSec = sec; lastDrop = drop; lastDup = dup;
            if (hasPos) {
                SetConsoleCursorPosition(hConsole(), timerPos);
                SetColor(kAccent);
                cout << setw(2) << setfill('0') << (sec / 60) << ":" << setw(2) << setfill('0') << (sec % 60) << "  ";
                SetColor(kDim);
                cout << " (perdidos: " << drop << " | duplicados: " << dup << ")   " << flush;
                SetColor(kGray);
            }
        }
        Sleep(50);
    }
    g_recorder.Stop();
}
void startrecord() {
    if (g_recorder.IsRecording()) { MessageScreen("Recording is already running.", kAccent); return; }
    if (g_cfg.captureMode == 1) {
        if (g_cfg.windowTitle.empty()) {
            MessageScreen("Nenhuma janela selecionada. Va em Configuracao -> Capture Target.", kErr);
            return;
        }
        HWND h = FindWindowByTitle(g_cfg.windowTitle);
        if (!h) {
            MessageScreen("Janela nao encontrada: " + WideToUtf8(g_cfg.windowTitle), kErr);
            return;
        }
    }
    else {
        vector<MonitorInfo> mons = ListMonitors();
        if (g_cfg.monitor < 0 || g_cfg.monitor >= (int)mons.size()) {
            MessageScreen("Monitor invalido: " + to_string(g_cfg.monitor) + ". Configure em Open Configuration.", kErr);
            return;
        }
    }
    g_cfg.outputFile = MakeAutoFilename(g_cfg.format);
    if (!g_recorder.Start(g_cfg)) { MessageScreen("Nao foi possivel iniciar: " + g_recorder.LastError(), kErr); return; }
    RecordingScreen();
}
static void ConfigEditFps() {
    int v;
    if (ReadIntConsole("FPS", "Frames por segundo", g_cfg.fps, 5, 240, v) && v != g_cfg.fps) { g_cfg.fps = v; SaveConfig(); }
}
static void ConfigEditBitrate() {
    int v;
    if (ReadIntConsole("BITRATE", "Bitrate de video em kbps", g_cfg.bitrateKbps, 500, 200000, v) && v != g_cfg.bitrateKbps) { g_cfg.bitrateKbps = v; SaveConfig(); }
}
static void ConfigPickMonitor() {
    vector<MonitorInfo> mons = ListMonitors();
    if (mons.empty()) { MessageScreen("Nenhum monitor detectado.", kErr); return; }
    vector<string> items;
    for (size_t i = 0; i < mons.size(); i++) items.push_back("Monitor " + to_string(i) + "  -  " + mons[i].nameUtf8);
    int r = SelectFromList("SELECIONE O MONITOR", items, g_cfg.monitor);
    if (r >= 0 && r != g_cfg.monitor) { g_cfg.monitor = r; SaveConfig(); }
}
static void ConfigCustomResolution() {
    string wd, ht;
    bool phaseH = false, err = false;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "=== RESOLUCAO PERSONALIZADA ===\n\n";
        SetColor(kGray); cout << " Digite a largura e depois a altura - max. 4 digitos cada\n";
        cout << " Atual: " << ResLabel() << "\n\n";
        SetColor(kAccent); cout << " > " << wd << (phaseH ? "x" : "") << ht << "_\n";
        if (err) { SetColor(kErr); cout << " Resolucao invalida - 64 a 8192 em cada lado.\n"; }
        SetColor(kDim);
        cout << "\n [0-9] digitar   [X] pular pra altura   [BACKSPACE] apagar   [ENTER] confirmar   [ESC] cancelar\n";
        SetColor(kGray);
        int c = ReadKey();
        err = false;
        if (IsEsc(c)) return;
        if (IsEnter(c)) {
            if (wd.empty() || ht.empty()) { err = true; continue; }
            int w = atoi(wd.c_str()), h = atoi(ht.c_str());
            if (w < 64 || h < 64 || w > 8192 || h > 8192) { err = true; continue; }
            g_cfg.outWidth = w & ~1;
            g_cfg.outHeight = h & ~1;
            SaveConfig();
            return;
        }
        if (c == 8) {
            if (!ht.empty()) ht.pop_back();
            else if (phaseH) phaseH = false;
            else if (!wd.empty()) wd.pop_back();
            continue;
        }
        if (c >= '0' && c <= '9') {
            if (!phaseH) { if (wd.size() < 4) { wd += (char)c; if (wd.size() == 4) phaseH = true; } }
            else if (ht.size() < 4) ht += (char)c;
            continue;
        }
        if ((c == 'x' || c == 'X') && !phaseH && !wd.empty()) phaseH = true;
    }
}
static void ConfigPickResolution() {
    static const int RW[] = { 0, 640, 800, 1024, 1280, 1280, 1366, 1600, 1920, 1920, 2560, 2560, 3840 };
    static const int RH[] = { 0, 480, 600, 768, 720, 800, 768, 900, 1080, 1200, 1080, 1440, 2160 };
    const int n = (int)(sizeof(RW) / sizeof(RW[0]));
    vector<string> items;
    for (int i = 0; i < n; i++) items.push_back(RW[i] == 0 ? string("AUTO - acompanha o monitor") : to_string(RW[i]) + "x" + to_string(RH[i]));
    items.push_back("Personalizado...");
    int cur = n;
    for (int i = 0; i < n; i++) if (RW[i] == g_cfg.outWidth && RH[i] == g_cfg.outHeight) { cur = i; break; }
    int r = SelectFromList("SELECIONE A RESOLUCAO", items, cur);
    if (r < 0) return;
    if (r == n) { ConfigCustomResolution(); return; }
    g_cfg.outWidth = RW[r];
    g_cfg.outHeight = RH[r];
    SaveConfig();
}

static void ConfigPickCaptureTarget() {
    vector<string> items;
    items.push_back("Monitor inteiro (modo padrao)");
    auto wins = ListWindows();
    for (auto& w : wins) items.push_back(WideToUtf8(w.title));
    int cur = 0;
    if (g_cfg.captureMode == 1 && !g_cfg.windowTitle.empty()) {
        for (size_t i = 0; i < wins.size(); i++) {
            if (wins[i].title == g_cfg.windowTitle) { cur = (int)i + 1; break; }
        }
    }
    int r = SelectFromList("SELECIONE O ALVO", items, cur);
    if (r < 0) return;
    if (r == 0) {
        g_cfg.captureMode = 0;
        g_cfg.windowTitle.clear();
    }
    else {
        g_cfg.captureMode = 1;
        g_cfg.windowTitle = wins[r - 1].title;
    }
    SaveConfig();
}

static void ConfigAudioMenu() {
    int sel = 0;
    const int N = 5;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "=== AUDIO ===\n\n";
        string micLabel = "Padrao do Windows";
        if (!g_cfg.micId.empty()) {
            auto mics = ListAudioDevices(eCapture);
            bool found = false;
            for (auto& m : mics) {
                if (m.id == g_cfg.micId) { micLabel = m.nameUtf8; found = true; break; }
            }
            if (!found) micLabel = "(desconectado)";
        }
        vector<string> items;
        items.push_back(string("Audio do Sistema   :  ") + (g_cfg.audioEnabled ? "Sim" : "Nao"));
        items.push_back(string("Gravar Microfone   :  ") + (g_cfg.micEnabled ? "Sim" : "Nao"));
        items.push_back("Dispositivo Mic    :  " + micLabel);
        items.push_back("Listar dispositivos");
        items.push_back("Voltar");
        DrawItems(items, sel);
        DrawFooter("[ESC] voltar");
        int c = ReadKey();
        if (IsUp(c)) sel = (sel + N - 1) % N;
        else if (IsDown(c)) sel = (sel + 1) % N;
        else if (IsEsc(c)) return;
        else if (IsEnter(c)) {
            switch (sel) {
            case 0: g_cfg.audioEnabled = !g_cfg.audioEnabled; SaveConfig(); break;
            case 1: g_cfg.micEnabled = !g_cfg.micEnabled; SaveConfig(); break;
            case 2: {
                auto mics = ListAudioDevices(eCapture);
                if (mics.empty()) { MessageScreen("Nenhum microfone detectado.", kErr); break; }
                vector<string> mlist;
                mlist.push_back("Padrao do Windows");
                for (auto& m : mics) mlist.push_back(m.nameUtf8 + (m.isDefault ? "  (padrao)" : ""));
                int r = SelectFromList("SELECIONE O MICROFONE", mlist, 0);
                if (r >= 0) {
                    if (r == 0) g_cfg.micId.clear();
                    else g_cfg.micId = mics[r - 1].id;
                    SaveConfig();
                }
                break;
            }
            case 3: {
                auto mics = ListAudioDevices(eCapture);
                auto spks = ListAudioDevices(eRender);
                ClearScreen();
                SetColor(kTitle); cout << "=== DISPOSITIVOS DE AUDIO ===\n\n";
                SetColor(kGray);
                cout << " Microfones (" << mics.size() << "):\n";
                for (size_t i = 0; i < mics.size(); i++) cout << "   [" << i << "] " << mics[i].nameUtf8 << (mics[i].isDefault ? "  (padrao)" : "") << "\n";
                cout << "\n Saidas (" << spks.size() << "):\n";
                for (size_t i = 0; i < spks.size(); i++) cout << "   [" << i << "] " << spks[i].nameUtf8 << (spks[i].isDefault ? "  (padrao)" : "") << "\n";
                SetColor(kDim);
                cout << "\n Press any key...";
                SetColor(kGray);
                cout << flush;
                (void)ReadKey();
                break;
            }
            case 4: return;
            }
        }
    }
}

void openconfig() {
    const int N = 9;
    int sel = 0;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kTitle); cout << "   CONFIGURATION  -  dev - kernel11\n";
        SetColor(kTitle); cout << "==================================================\n\n";
        string targetLabel;
        if (g_cfg.captureMode == 1) targetLabel = "Janela: " + WideToUtf8(g_cfg.windowTitle);
        else targetLabel = "Monitor " + to_string(g_cfg.monitor);
        vector<string> items;
        items.push_back("FPS                :  " + to_string(g_cfg.fps));
        items.push_back("Bitrate (kbps)     :  " + to_string(g_cfg.bitrateKbps));
        items.push_back("Monitor            :  " + to_string(g_cfg.monitor));
        items.push_back("Resolucao saida    :  " + ResLabel());
        items.push_back("Formato de video   :  " + g_cfg.format);
        items.push_back(string("Mostrar Cursor     :  ") + (g_cfg.showCursor ? "Sim" : "Nao"));
        items.push_back("Capture Target     :  " + targetLabel);
        items.push_back("Configurar Audio...");
        items.push_back("Voltar");
        DrawItems(items, sel);
        DrawFooter("[ESC] voltar");
        int c = ReadKey();
        if (IsUp(c)) sel = (sel + N - 1) % N;
        else if (IsDown(c)) sel = (sel + 1) % N;
        else if (IsEsc(c)) return;
        else if (IsEnter(c)) {
            switch (sel) {
            case 0: ConfigEditFps(); break;
            case 1: ConfigEditBitrate(); break;
            case 2: ConfigPickMonitor(); break;
            case 3: ConfigPickResolution(); break;
            case 4: g_cfg.format = (g_cfg.format == "mp4") ? "wmv" : "mp4"; SaveConfig(); break;
            case 5: g_cfg.showCursor = !g_cfg.showCursor; SaveConfig(); break;
            case 6: ConfigPickCaptureTarget(); break;
            case 7: ConfigAudioMenu(); break;
            case 8: return;
            }
        }
    }
}
void mainmenu() {
    const int N = 4;
    int sel = 0;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kTitle); cout << "     GRAVADOR DE TELA  -  dev - kernel11\n";
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kGray);
        cout << " Status : " << (g_recorder.IsRecording() ? "RECORDING" : "STOPPED") << "\n";
        cout << " Config : " << g_cfg.fps << " fps | " << g_cfg.bitrateKbps << " kbps | " << ResLabel() << " | " << g_cfg.format
            << " | cursor " << (g_cfg.showCursor ? "Sim" : "Nao")
            << " | audio " << (g_cfg.audioEnabled ? "Sim" : "Nao")
            << " | mic " << (g_cfg.micEnabled ? "Sim" : "Nao") << "\n";
        if (g_cfg.captureMode == 1) {
            cout << " Alvo   : Janela: " << WideToUtf8(g_cfg.windowTitle) << "\n";
        }
        else {
            cout << " Alvo   : Monitor " << g_cfg.monitor << "\n";
        }
        if (!g_recorder.LastError().empty()) { SetColor(kErr); cout << " Ultimo erro: " << g_recorder.LastError() << "\n"; SetColor(kGray); }
        SetColor(kTitle); cout << "--------------------------------------------------\n\n";
        vector<string> items;
        items.push_back("Record Monitor");
        items.push_back("Record Window");
        items.push_back("Open Configuration");
        items.push_back("Exit");
        DrawItems(items, sel);
        DrawFooter("[ESC] sair");
        int c = ReadKey();
        if (c >= '1' && c <= '4') { sel = c - '1'; c = '\r'; }
        if (IsUp(c)) sel = (sel + N - 1) % N;
        else if (IsDown(c)) sel = (sel + 1) % N;
        else if (IsEsc(c)) { if (g_recorder.IsRecording()) g_recorder.Stop(); return; }
        else if (IsEnter(c)) {
            switch (sel) {
            case 0: {
                g_cfg.captureMode = 0;
                startrecord();
                break;
            }
            case 1: {
                auto wins = ListWindows();
                if (wins.empty()) { MessageScreen("Nenhuma janela detectada.", kErr); break; }
                vector<string> wlist;
                for (auto& w : wins) wlist.push_back(WideToUtf8(w.title));
                int r = SelectFromList("SELECIONE A JANELA", wlist, 0);
                if (r < 0) break;
                g_cfg.captureMode = 1;
                g_cfg.windowTitle = wins[r].title;
                SaveConfig();
                startrecord();
                break;
            }
            case 2: openconfig(); break;
            case 3: if (g_recorder.IsRecording()) g_recorder.Stop(); return;
            }
        }
    }
}
int main() {
    timeBeginPeriod(1);
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadConfig();
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    mainmenu();
    timeEndPeriod(1);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}
