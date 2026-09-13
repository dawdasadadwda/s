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
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
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
        ComPtr<ID2D1Bitmap1> curBmp;
        ComPtr<ID3D11Texture2D> wtex[RING];
        ComPtr<ID2D1Bitmap1> modeBmp;
        ComPtr<ID3D11Texture2D> modeTex;
        UINT wIdx = (UINT)(RING - 1);
        HICON lastCur = nullptr;
        DecodedCursor dcur;
        LONG orgX = 0, orgY = 0;
        bool cursorInit = false;
        bool cursorVisible = false;
        LONG cursorX = 0, cursorY = 0;
        LONG64 lastMouseStamp = 0;
        bool d2dOk = false, pendingScale = false;
        ComPtr<IMFDXGIDeviceManager> dxgiMan;
        ComPtr<IMFAttributes> attrs;
        ComPtr<IMFSinkWriter> writer;
        ComPtr<IMFMediaType> outType, inType;
        ComPtr<IDXGIResource> res;
        ComPtr<ID3D11Texture2D> frameTex;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
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
        UINT writeCount = 0;
        int segCount = 1;
        double elapsedHns = 0.0, frameDurHns = 0.0;
        DXGI_OUTDUPL_DESC dupDesc{};
        string curFile = cfg.outputFile;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        auto CreateCaptureTextures = [&](UINT w, UINT h) -> HRESULT {
            HRESULT r = S_OK;
            for (int i = 0; i < RING; i++) staging[i].Reset();
            for (int i = 0; i < RING; i++) { wtex[i].Reset(); frameBmp[i].Reset(); }
            modeTex.Reset(); modeBmp.Reset(); cpuStage.Reset(); curBmp.Reset();
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
            }
            for (int i = 0; i < RING; i++) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = w; td.Height = h;
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

        auto CreateVideoWriter = [&](UINT inW, UINT inH, UINT oW, UINT oH, const wstring& file) -> HRESULT {
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
            MFSetAttributeSize(ot.Get(), MF_MT_FRAME_SIZE, oW, oH);
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
            MFSetAttributeSize(it2.Get(), MF_MT_FRAME_SIZE, inW, inH);
            MFSetAttributeRatio(it2.Get(), MF_MT_FRAME_RATE, (UINT32)cfg.fps, 1);
            MFSetAttributeRatio(it2.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            r = w->SetInputMediaType(si, it2.Get(), nullptr);
            if (FAILED(r)) return r;

            audioStreamIdx = 0;
            if (useSysAudio || useMic) {
                if (!AddAacStream(w.Get(), &audioStreamIdx)) {
                    audioStreamIdx = 0;
                }
            }

            r = w->BeginWriting();
            if (FAILED(r)) return r;
            writer.Reset(w.Detach());
            return S_OK;
            };

        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) comHere = true;
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) { err = "MFStartup falhou"; failHR = (DWORD)hr; goto cleanup; }
        mfHere = true;
        useSysAudio = cfg.audioEnabled && ProbeDefaultEndpoint(eRender);
        useMic = cfg.micEnabled && ProbeDefaultEndpoint(eCapture);
        QueryPerformanceFrequency(&qpf);
        {
            UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
            D3D_FEATURE_LEVEL got{};
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3dDev, &got, &ctx);
            if (FAILED(hr)) { err = "D3D11CreateDevice falhou - GPU indisponivel?"; failHR = (DWORD)hr; goto cleanup; }
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
        outW = (cfg.outWidth > 0) ? (UINT)(cfg.outWidth & ~1) : texW;
        outH = (cfg.outHeight > 0) ? (UINT)(cfg.outHeight & ~1) : texH;
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
            wstring wfile = Utf8ToWide(curFile);
            hr = CreateVideoWriter(texW, texH, outW, outH, wfile);
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

        while (!m_stopFlag.load()) {
            if (!dupl.Get()) {
                if (framePending) framePending = false;
                if (FAILED(output1->DuplicateOutput(d3dDev.Get(), &dupl))) {
                    Sleep(20);
                    continue;
                }
                DXGI_OUTDUPL_DESC d2{};
                dupl->GetDesc(&d2);
                UINT nw = d2.ModeDesc.Width, nh = d2.ModeDesc.Height;
                lastMouseStamp = 0;
                cursorInit = false;
                if (nw != texW || nh != texH) {
                    bool autoRes = (cfg.outWidth == 0 && cfg.outHeight == 0);
                    if (autoRes) {
                        if (m_mixer.IsRunning()) m_mixer.Stop();
                        if (writer.Get() && wroteAny) writer->Finalize();
                        writer.Reset();
                        texW = nw; texH = nh;
                        outW = texW; outH = texH;
                        hr = CreateCaptureTextures(texW, texH);
                        if (FAILED(hr)) { err = "CreateTexture2D staging"; failHR = (DWORD)hr; goto cleanup; }
                        pendingScale = false; modeTex.Reset(); modeBmp.Reset();
                        curFile = MakeAutoFilename(cfg.format);
                        g_cfg.outputFile = curFile;
                        hr = CreateVideoWriter(texW, texH, outW, outH, Utf8ToWide(curFile));
                        if (FAILED(hr)) { err = "nao foi possivel reiniciar o encoder"; failHR = (DWORD)hr; goto cleanup; }
                        if ((useSysAudio || useMic) && audioStreamIdx > 0) {
                            if (!m_mixer.Start(writer.Get(), audioStreamIdx, useSysAudio, L"", useMic, cfg.micId)) {
                                useSysAudio = false; useMic = false; audioStreamIdx = 0;
                            }
                        }
                        writeCount = 0;
                        QueryPerformanceCounter(&t0);
                        haveNew = false; wroteAny = false;
                        segCount++;
                        cout << "\n resolucao alterada para " << texW << "x" << texH << " - continuando em novo arquivo: " << curFile << "\n";
                    }
                    else {
                        if (!d2dOk) { err = "resolucao do monitor mudou durante a gravacao"; goto cleanup; }
                        D3D11_TEXTURE2D_DESC md{};
                        md.Width = nw; md.Height = nh; md.MipLevels = 1; md.ArraySize = 1;
                        md.Format = DXGI_FORMAT_B8G8R8A8_UNORM; md.SampleDesc.Count = 1;
                        md.Usage = D3D11_USAGE_DEFAULT;
                        md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        modeTex.Reset(); modeBmp.Reset();
                        hr = d3dDev->CreateTexture2D(&md, nullptr, &modeTex);
                        if (FAILED(hr)) { err = "CreateTexture2D modeTex"; failHR = (DWORD)hr; goto cleanup; }
                        ComPtr<IDXGISurface> msurf;
                        if (FAILED(modeTex->QueryInterface(__uuidof(IDXGISurface), (void**)&msurf))) { err = "modeTex surface"; goto cleanup; }
                        D2D1_BITMAP_PROPERTIES1 mbp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                        if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(msurf.Get(), &mbp, &modeBmp))) { err = "modeBmp"; goto cleanup; }
                        pendingScale = true;
                        haveNew = false;
                        cout << "\n resolucao do monitor mudou para " << nw << "x" << nh << " - saida mantida em " << outW << "x" << outH << "\n";
                    }
                }
            }

            hr = dupl->AcquireNextFrame(1, &frameInfo, &res);
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
                    if (pendingScale && modeTex.Get()) ctx->CopyResource(modeTex.Get(), frameTex.Get());
                }
                frameTex.Reset();
                res.Reset();
                dupl->ReleaseFrame();
                framePending = false;
                haveNew = true;
            }
            else if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
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
            elapsedHns = (double)(now.QuadPart - t0.QuadPart) * 10'000'000.0 / (double)qpf.QuadPart;
            double dueTs = (double)writeCount * frameDurHns;

            if (elapsedHns >= dueTs && (haveNew || wroteAny)) {
                wIdx = (wIdx + 1) % RING;
                ctx->CopyResource(wtex[wIdx].Get(), (pendingScale && modeTex.Get()) ? modeTex.Get() : staging[stageIdx].Get());
                if (d2dOk) {
                    d2dCtx->SetTarget(frameBmp[wIdx].Get());
                    d2dCtx->BeginDraw();
                    if (pendingScale && modeBmp.Get()) {
                        D2D1_RECT_F full = D2D1::RectF(0.0f, 0.0f, (float)texW, (float)texH);
                        d2dCtx->DrawBitmap(modeBmp.Get(), &full);
                    }
                    if (cfg.showCursor && cursorInit && cursorVisible && dcur.w > 0 && curBmp.Get()) {
                        float dx = (float)(cursorX - orgX - dcur.hx);
                        float dy = (float)(cursorY - orgY - dcur.hy);
                        D2D1_RECT_F dr = D2D1::RectF(dx, dy, dx + (float)dcur.w, dy + (float)dcur.h);
                        d2dCtx->DrawBitmap(curBmp.Get(), &dr);
                    }
                    HRESULT hd = d2dCtx->EndDraw();
                    if (FAILED(hd)) {
                        d2dCtx.Reset();
                        d2dDevice.Reset();
                        d2dFactory.Reset();
                        curBmp.Reset();
                        d2dOk = false;
                        do {
                            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), (void**)&d2dFactory))) break;
                            if (FAILED(d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice))) break;
                            if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx))) break;
                            d2dOk = true;
                        } while (false);
                        if (!d2dOk) cout << "\n cursor: falha de dispositivo D2D - gravando sem cursor\n";
                    }
                }
                ID3D11Texture2D* srcTex = wtex[wIdx].Get();
                ComPtr<IMFMediaBuffer> outBuf;
                if (!useRam) {
                    hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), srcTex, 0, FALSE, &outBuf);
                    if (FAILED(hr)) { err = "MFCreateDXGISurfaceBuffer"; failHR = (DWORD)hr; goto cleanup; }
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
                        cd.Width = texW; cd.Height = texH; cd.MipLevels = 1; cd.ArraySize = 1;
                        cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; cd.SampleDesc.Count = 1;
                        cd.Usage = D3D11_USAGE_STAGING;
                        cd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        hr = d3dDev->CreateTexture2D(&cd, nullptr, &cpuStage);
                        if (FAILED(hr)) { err = "CreateTexture2D staging RAM"; failHR = (DWORD)hr; goto cleanup; }
                    }
                    ctx->CopyResource(cpuStage.Get(), srcTex);
                    mp = {};
                    if (FAILED(ctx->Map(cpuStage.Get(), 0, D3D11_MAP_READ, 0, &mp))) { err = "Map staging RAM"; goto cleanup; }
                    rowBytes = texW * 4;
                    frameSize = rowBytes * texH;
                    ComPtr<IMFMediaBuffer> mem;
                    hr = MFCreateMemoryBuffer(frameSize, &mem);
                    if (SUCCEEDED(hr)) {
                        dstp = nullptr;
                        hr = mem->Lock(&dstp, nullptr, nullptr);
                        if (SUCCEEDED(hr)) {
                            BYTE* srcp = (BYTE*)mp.pData;
                            for (UINT r = 0; r < texH; r++) memcpy(dstp + (size_t)r * rowBytes, srcp + (size_t)r * mp.RowPitch, rowBytes);
                            mem->Unlock();
                            mem->SetCurrentLength(frameSize);
                        }
                    }
                    ctx->Unmap(cpuStage.Get(), 0);
                    if (FAILED(hr)) { err = "buffer RAM"; failHR = (DWORD)hr; goto cleanup; }
                    outBuf.Reset(mem.Detach());
                }
                double ts = dueTs;
                sample.Reset();
                hr = MFCreateSample(&sample);
                if (SUCCEEDED(hr)) hr = sample->AddBuffer(outBuf.Get());
                if (SUCCEEDED(hr)) hr = sample->SetSampleTime((LONGLONG)ts);
                if (SUCCEEDED(hr)) hr = sample->SetSampleDuration((LONGLONG)frameDurHns);
                if (FAILED(hr)) { err = "criacao do sample"; failHR = (DWORD)hr; goto cleanup; }
                hr = writer->WriteSample(videoStreamIdx, sample.Get());
                if (FAILED(hr)) {
                    if (!wroteAny) {
                        useRam = true;
                        cout << "\n encoder recusou textura de GPU - usando modo de compatibilidade em RAM\n";
                        err.clear();
                        failHR = 0;
                    }
                    else {
                        err = "WriteSample";
                        failHR = (DWORD)hr;
                        goto cleanup;
                    }
                }
                else {
                    wroteAny = true;
                    writeCount++;
                    haveNew = false;
                }
            }
            else if (elapsedHns < dueTs) {
                double waitMs = (dueTs - elapsedHns) / 10000.0;
                if (waitMs > 2.0) waitMs = 2.0;
                if (waitMs > 0.3) Sleep((DWORD)waitMs);
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
        sample.Reset(); buffer.Reset();
        inType.Reset(); outType.Reset();
        attrs.Reset();
        writer.Reset();
        dxgiMan.Reset();
        for (int i = 0; i < RING; i++) staging[i].Reset();
        cpuStage.Reset();
        for (int i = 0; i < RING; i++) { frameBmp[i].Reset(); wtex[i].Reset(); }
        modeTex.Reset(); modeBmp.Reset();
        curBmp.Reset();
        d2dCtx.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        dupl.Reset(); output1.Reset(); output.Reset(); adapter.Reset(); dxgiDev.Reset();
        mt.Reset(); ctx.Reset(); d3dDev.Reset();
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
};

ScreenRecorder g_recorder;
static BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) g_recorder.Stop();
    return TRUE;
}

void startrecord();
void openconfig();
static string ResLabel() {
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
    cout << " Config : " << g_cfg.fps << " fps | " << g_cfg.bitrateKbps << " kbps | " << g_cfg.format << " | " << ResLabel()
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
    while (g_recorder.IsRecording()) {
        int c = -1;
        if (_kbhit()) {
            c = _getch();
            if (c == 0 || c == 224) { if (_kbhit()) (void)_getch(); c = -1; }
        }
        if (IsEnter(c) || IsEsc(c)) break;
        int sec = (int)g_recorder.ElapsedSeconds();
        if (sec != lastSec) {
            lastSec = sec;
            if (hasPos) {
                SetConsoleCursorPosition(hConsole(), timerPos);
                SetColor(kAccent);
                cout << setw(2) << setfill('0') << (sec / 60) << ":" << setw(2) << setfill('0') << (sec % 60) << "  " << flush;
                SetColor(kGray);
            }
        }
        Sleep(50);
    }
    g_recorder.Stop();
}
void startrecord() {
    if (g_recorder.IsRecording()) { MessageScreen("Recording is already running.", kAccent); return; }
    vector<MonitorInfo> mons = ListMonitors();
    if (g_cfg.monitor < 0 || g_cfg.monitor >= (int)mons.size()) {
        MessageScreen("Monitor invalido: " + to_string(g_cfg.monitor) + ". Configure em Open Configuration.", kErr);
        return;
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
    const int N = 8;
    int sel = 0;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kTitle); cout << "   CONFIGURATION  -  dev - kernel11\n";
        SetColor(kTitle); cout << "==================================================\n\n";
        vector<string> items;
        items.push_back("FPS                :  " + to_string(g_cfg.fps));
        items.push_back("Bitrate (kbps)     :  " + to_string(g_cfg.bitrateKbps));
        items.push_back("Monitor            :  " + to_string(g_cfg.monitor));
        items.push_back("Resolucao saida    :  " + ResLabel());
        items.push_back("Formato de video   :  " + g_cfg.format);
        items.push_back(string("Mostrar Cursor     :  ") + (g_cfg.showCursor ? "Sim" : "Nao"));
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
            case 6: ConfigAudioMenu(); break;
            case 7: return;
            }
        }
    }
}
void mainmenu() {
    const int N = 3;
    int sel = 0;
    while (true) {
        ClearScreen();
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kTitle); cout << "     GRAVADOR DE TELA  -  dev - kernel11\n";
        SetColor(kTitle); cout << "==================================================\n";
        SetColor(kGray);
        cout << " Status : " << (g_recorder.IsRecording() ? "RECORDING" : "STOPPED") << "\n";
        cout << " Config : " << g_cfg.fps << " fps | " << g_cfg.bitrateKbps << " kbps | monitor " << g_cfg.monitor << " | " << ResLabel() << " | " << g_cfg.format
            << " | cursor " << (g_cfg.showCursor ? "Sim" : "Nao")
            << " | audio " << (g_cfg.audioEnabled ? "Sim" : "Nao")
            << " | mic " << (g_cfg.micEnabled ? "Sim" : "Nao") << "\n";
        cout << " Arquivo: " << MakeAutoFilename(g_cfg.format) << "\n";
        if (!g_recorder.LastError().empty()) { SetColor(kErr); cout << " Ultimo erro: " << g_recorder.LastError() << "\n"; SetColor(kGray); }
        SetColor(kTitle); cout << "--------------------------------------------------\n\n";
        vector<string> items;
        items.push_back("Start Recording");
        items.push_back("Open Configuration");
        items.push_back("Exit");
        DrawItems(items, sel);
        DrawFooter("[ESC] sair");
        int c = ReadKey();
        if (c >= '1' && c <= '3') { sel = c - '1'; c = '\r'; }
        if (IsUp(c)) sel = (sel + N - 1) % N;
        else if (IsDown(c)) sel = (sel + 1) % N;
        else if (IsEsc(c)) { if (g_recorder.IsRecording()) g_recorder.Stop(); return; }
        else if (IsEnter(c)) {
            switch (sel) {
            case 0: startrecord(); break;
            case 1: openconfig(); break;
            case 2: if (g_recorder.IsRecording()) g_recorder.Stop(); return;
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
