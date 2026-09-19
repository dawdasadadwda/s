#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winreg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wininet.h>
#include <objbase.h>   // COM (WIC do logo)
#include <wincodec.h>  // WIC: decodifica o PNG/JPG da memória
#include <pdh.h>       // contadores de performance (uso de GPU)
#include <pdhmsg.h>    // codigos de status do PDH (PDH_MORE_DATA etc.)
#include <cmath>
#include <cfloat>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")          // CoInitializeEx/CoCreateInstance
#pragma comment(lib, "windowscodecs.lib")  // WIC
#pragma comment(lib, "pdh.lib")            // contadores de performance

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "IconsFontAwesome6.h"
#include "fa_solid_900_ttf.h"
#include "logovitin.h"   // logo no canto superior esquerdo

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT  g_resizeW = 0, g_resizeH = 0;
static bool  g_close = false;

static const float WIN_W = 650.0f;
static const float WIN_H = 400.0f;
static const float ROUND = 14.0f;     // arredondamento do fundo (bipas.cpp)
static const float FONT_SIZE = 16.0f;

// ---- estado do login ----
static char s_user[128] = {};
static char s_pass[128] = {};
static char s_otp[8] = {};
static int   s_otpCode = 0;          // código gerado (0 = nenhum enviado)
static bool  s_error = false;      // login errado / bloqueio
static char  s_errorMsg[128] = "Usuario, senha ou OTP incorretos";
static double s_errorAt = -1000.0;    // quando o erro ocorreu
static int   s_focus = 0;             // 0 = nada | 1 = Usuario | 2 = Senha | 3 = OTP
static bool  s_showPass = false;      // eye: true = senha visível
static std::atomic<bool> s_loggedIn{ false }; // thread do auto-login escreve
static std::atomic<bool> s_loginAnim{ false }; // login manual OK (check animado)

// ---- rate limit do OTP/login (persistido em vitin_lock.dat) ----
struct OtpLock {
    long long lastSend;   // unix sec do último Send
    long long window;     // unix sec do início da janela de 60s
    int       attempts;   // tentativas (send + login) na janela
};
static OtpLock g_lock = { 0, 0, 0 };
static const int kMaxAttempts = 7;    // 7 tentativas por minuto

// ---- animação (bipas.cpp) ----
static float s_xGlow = 0.0f;

static float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float EaseSmooth(float x) { return x * x * (3.0f - 2.0f * x); }
static float AnimDeltaTime() { return fminf(ImGui::GetIO().DeltaTime, 1.0f / 30.0f); }

static void CreateRTV()
{
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) {
        g_device->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}

static void ReleaseRTV()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

// D3D do bipas.cpp: swap chain bitblt clássica (DISCARD)
static bool CreateD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL lvl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, _countof(levels), D3D11_SDK_VERSION,
        &sd, &g_swap, &g_device, &lvl, &g_context);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, _countof(levels), D3D11_SDK_VERSION,
            &sd, &g_swap, &g_device, &lvl, &g_context);
    if (FAILED(hr)) return false;

    CreateRTV();
    return g_rtv != nullptr;
}

// ---------------------------------------------------------------------------
// LOGO (logovitin.h) — decodifica a imagem direto da memória com WIC
// (detecta o formato sozinho: PNG/JPG/BMP...) e sobe pra uma textura D3D11
// que o ImGui desenha via AddImage. Carregado 1x, na primeira frame.
// ---------------------------------------------------------------------------
static ID3D11ShaderResourceView* g_logoSRV = nullptr;
static int g_logoW = 0, g_logoH = 0;

static bool LoadLogoTexture(ID3D11Device* dev)
{
    if (!dev) return false;

    // COM é necessário pra criar a factory do WIC
    const bool needCo =
        SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    IWICImagingFactory* wic = nullptr;
    IWICStream* stm = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    std::vector<unsigned char> px;
    bool ok = false;

    do {
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (FAILED(hr)) break;

        hr = wic->CreateStream(&stm);
        if (FAILED(hr)) break;
        // const_cast: a WIC só LÊ o buffer (o array é const no header)
        hr = stm->InitializeFromMemory(
            const_cast<BYTE*>(vitinlogo), (DWORD)sizeof(vitinlogo));
        if (FAILED(hr)) break;

        // sniff automático do formato (PNG/JPG/BMP/GIF/TIFF)
        hr = wic->CreateDecoderFromStream(stm, nullptr,
            WICDecodeMetadataCacheOnLoad, &dec);
        if (FAILED(hr)) break;

        hr = dec->GetFrame(0, &fr);
        if (FAILED(hr)) break;

        // converte pra RGBA 32bpp reto (o blend do backend ImGui espera isso)
        hr = wic->CreateFormatConverter(&cv);
        if (FAILED(hr)) break;
        hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) break;

        UINT w = 0, h = 0;
        hr = cv->GetSize(&w, &h);
        if (FAILED(hr) || !w || !h) break;

        px.resize((size_t)w * h * 4u);
        hr = cv->CopyPixels(nullptr, w * 4u, (UINT)px.size(), px.data());
        if (FAILED(hr)) break;

        // ---- mips gerados NA CPU (box filter 2x2) ----
        // Passar UM único subresource com MipLevels=0 é comportamento
        // indefinido: o runtime lê lixo da pilha como se fossem os outros
        // níveis (era o estouro de memória ao abrir). Geramos todos os
        // níveis aqui e entregamos o array completo pro CreateTexture2D.
        UINT levels = 1, mw = w, mh = h;
        while (mw > 1u || mh > 1u) {
            if (mw > 1u) mw /= 2u;
            if (mh > 1u) mh /= 2u;
            ++levels;
        }
        std::vector<unsigned char> mips(px.begin(), px.end());
        px.clear();
        px.shrink_to_fit();                    // nível 0 já foi pra mips
        mips.reserve(mips.size() + mips.size() / 2u); // total ~4/3 (sem realloc)
        std::vector<D3D11_SUBRESOURCE_DATA> sds(levels);
        {
            UINT pw = w, ph = h;               // dimensões do nível anterior
            size_t pbase = 0;                  // offset do nível anterior
            for (UINT lv = 0; lv < levels; ++lv) {
                const UINT cw = (lv == 0u) ? w : ((pw > 1u) ? pw / 2u : 1u);
                const UINT ch = (lv == 0u) ? h : ((ph > 1u) ? ph / 2u : 1u);
                const size_t rowSz = (size_t)cw * 4u;
                const size_t lvlSz = rowSz * ch;
                size_t base = 0;
                if (lv > 0u) {
                    base = mips.size();
                    mips.resize(base + lvlSz);
                    const unsigned char* s = mips.data() + pbase;
                    unsigned char* d = mips.data() + base;
                    for (UINT y = 0; y < ch; ++y) {
                        const UINT sy0 = y * 2u;
                        const UINT sy1 = (sy0 + 1u < ph) ? sy0 + 1u : sy0;
                        for (UINT x = 0; x < cw; ++x) {
                            const UINT sx0 = x * 2u;
                            const UINT sx1 = (sx0 + 1u < pw) ? sx0 + 1u : sx0;
                            const unsigned char* q00 = s + ((size_t)sy0 * pw + sx0) * 4u;
                            const unsigned char* q01 = s + ((size_t)sy0 * pw + sx1) * 4u;
                            const unsigned char* q10 = s + ((size_t)sy1 * pw + sx0) * 4u;
                            const unsigned char* q11 = s + ((size_t)sy1 * pw + sx1) * 4u;
                            for (int c = 0; c < 4; ++c)
                                d[c] = (unsigned char)((q00[c] + q01[c] + q10[c] + q11[c] + 2) / 4);
                            d += 4;
                        }
                    }
                }
                sds[lv].pSysMem = mips.data() + base;
                sds[lv].SysMemPitch = (UINT)rowSz;
                sds[lv].SysMemSlicePitch = 0;
                pw = cw; ph = ch; pbase = base;
            }
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = levels;            // cadeia completa, dados de todos
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = dev->CreateTexture2D(&td, sds.data(), &tex);
        if (FAILED(hr)) break;

        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = td.Format;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = levels;
        hr = dev->CreateShaderResourceView(tex, &sv, &g_logoSRV);
        if (FAILED(hr)) break;

        g_logoW = (int)w;
        g_logoH = (int)h;
        ok = true;
    } while (false);

    if (tex) tex->Release();
    if (cv)  cv->Release();
    if (fr)  fr->Release();
    if (dec) dec->Release();
    if (stm) stm->Release();
    if (wic) wic->Release();
    if (needCo) ::CoUninitialize();
    return ok;
}

static void DestroyD3D()
{
    ReleaseRTV();
    if (g_logoSRV) { g_logoSRV->Release(); g_logoSRV = nullptr; }
    if (g_swap) { g_swap->Release();      g_swap = nullptr; }
    if (g_context) { g_context->Release();   g_context = nullptr; }
    if (g_device) { g_device->Release();    g_device = nullptr; }
}

// Transparência por pixel do bipas.cpp: DWM glass cobrindo a janela inteira.
static void EnableTransparency(HWND hwnd)
{
    MARGINS margins = { -1, -1, -1, -1 };
    ::DwmExtendFrameIntoClientArea(hwnd, &margins);
}

// ---------------------------------------------------------------------------
// IP PÚBLICO — usado junto do fingerprint pra amarrar o login ao PC E à
// rede. Copiar o .dat pra outro PC não resolve: IP (ou hardware) diferente
// não bate.
// ---------------------------------------------------------------------------
static bool HttpGetText(const char* host, const char* path,
    char* out, size_t cap, DWORD timeoutMs)
{
    out[0] = '\0';
    HINTERNET net = InternetOpenA("VitinOptimizer", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!net) return false;

    HINTERNET conn = InternetConnectA(net, host, INTERNET_DEFAULT_HTTPS_PORT,
        nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (conn)
    {
        HINTERNET req = HttpOpenRequestA(conn, "GET", path,
            nullptr, nullptr, nullptr,
            INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE |
            INTERNET_FLAG_RELOAD, 0);
        if (req)
        {
            InternetSetOptionA(req, INTERNET_OPTION_CONNECT_TIMEOUT,
                &timeoutMs, sizeof(timeoutMs));
            InternetSetOptionA(req, INTERNET_OPTION_SEND_TIMEOUT,
                &timeoutMs, sizeof(timeoutMs));
            InternetSetOptionA(req, INTERNET_OPTION_RECEIVE_TIMEOUT,
                &timeoutMs, sizeof(timeoutMs));

            if (HttpSendRequestA(req, nullptr, 0, nullptr, 0))
            {
                DWORD rd = 0;
                size_t total = 0;
                while (total + 1 < cap &&
                    InternetReadFile(req, out + total,
                        (DWORD)(cap - 1 - total), &rd) && rd > 0)
                {
                    total += rd;
                    out[total] = '\0';
                }
                // corta whitespace do fim (\n do checkip etc.)
                while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r' ||
                    out[total - 1] == ' ' || out[total - 1] == '\t'))
                    out[--total] = '\0';
            }
            InternetCloseHandle(req);
        }
        InternetCloseHandle(conn);
    }
    InternetCloseHandle(net);
    return out[0] != '\0';
}

// aceita só texto com cara de IPv4/IPv6 (dígitos, pontos, :, hex)
static bool IsValidIpText(const char* s)
{
    const size_t n = strlen(s);
    if (n < 3 || n > 45) return false;
    for (const char* p = s; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
            (*p >= 'A' && *p <= 'F') || *p == '.' || *p == ':'))
            return false;
    return true;
}

static bool GetPublicIp(char* out, size_t cap)
{
    if (HttpGetText("api.ipify.org", "/", out, cap, 3000) && IsValidIpText(out))
        return true;
    if (HttpGetText("checkip.amazonaws.com", "/", out, cap, 3000) && IsValidIpText(out))
        return true;
    out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------
// FINGERPRINT do PC (CPU + RAM + GPU + IP) — ao logar com sucesso, o hash é
// salvo num arquivo ao lado do EXE. Se o PC E a rede baterem com o salvo,
// entra direto, sem precisar logar com OTP de novo.
// ---------------------------------------------------------------------------
static const unsigned long long kFpXor = 0x564954494E314F50ULL; // "VITIN1OP"

static unsigned long long HashStr(const char* s)
{
    unsigned long long h = 1469598103934665603ULL; // FNV-1a 64
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long GetFingerprint(const char* ip)
{
    // CPU (nome do processador)
    char cpu[256] = "?";
    DWORD cpuSz = sizeof(cpu) - 1;
    if (FAILED(RegGetValueA(HKEY_LOCAL_MACHINE,
        "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        "ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cpuSz)))
        lstrcpyA(cpu, "?");

    // Memória RAM total (MB)
    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    char ram[32];
    snprintf(ram, sizeof(ram), "%llu",
        (unsigned long long)(ms.ullTotalPhys / (1024ULL * 1024ULL)));

    // GPU (adaptador primário via DXGI)
    char gpu[256] = "?";
    {
        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(fac->EnumAdapters(0, &ad)) && ad) {
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(ad->GetDesc(&d)) && d.Description[0])
                    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1,
                        gpu, sizeof(gpu), nullptr, nullptr);
                ad->Release();
            }
            fac->Release();
        }
    }

    char info[1280];
    snprintf(info, sizeof(info), "%s|%s|%s|%s", cpu, ram, gpu, ip);
    return HashStr(info);
}

// caminho de um arquivo de dados ao lado do EXE
static void AppFilePath(char* path, DWORD cap, const char* name)
{
    path[0] = '\0';
    char mod[MAX_PATH] = {};
    if (::GetModuleFileNameA(nullptr, mod, MAX_PATH)) {
        if (char* slash = strrchr(mod, '\\')) *slash = '\0';
        snprintf(path, cap, "%s\\%s", mod, name);
    }
}

static bool LoadSavedFingerprint(unsigned long long& out)
{
    char path[MAX_PATH];
    AppFilePath(path, sizeof(path), "vitin_pc.dat");
    HANDLE f = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD rd = 0;
    unsigned long long v = 0;
    const BOOL ok = ReadFile(f, &v, sizeof(v), &rd, nullptr) && rd == sizeof(v);
    ::CloseHandle(f);
    if (!ok) return false;
    out = v ^ kFpXor;
    return true;
}

static void SaveFingerprint(unsigned long long fp)
{
    char path[MAX_PATH];
    AppFilePath(path, sizeof(path), "vitin_pc.dat");
    HANDLE f = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const unsigned long long v = fp ^ kFpXor;
    DWORD wr = 0;
    ::WriteFile(f, &v, sizeof(v), &wr, nullptr);
    ::CloseHandle(f);
}

// Auto-login em background: só entra direto se CPU+RAM+GPU e o IP público
// baterem com o salvo. Sem IP (offline) NÃO auto-loga — bloquear a rede
// não pode virar forma de burlar a checagem.
static void TryAutoLogin()
{
    std::thread([]()
        {
            char ip[64] = {};
            if (!GetPublicIp(ip, sizeof(ip)))
                return;
            unsigned long long saved = 0;
            if (LoadSavedFingerprint(saved) && saved == GetFingerprint(ip))
                s_loggedIn = true; // entra direto, sem animação
        }).detach();
}

// ---------------------------------------------------------------------------
// RATE LIMIT persistido (vitin_lock.dat) — o contador de 30s e o limite de
// 4 tentativas por minuto sobrevivem ao fechar/abrir o app, então não dá
// pra floodar o webhook nem bruteforcar o OTP reiniciando o programa.
// ---------------------------------------------------------------------------
static const unsigned long long kLockXor = 0x4F54504C4F434B31ULL; // "OTPLOCK1"

static long long NowUnixSec()
{
    FILETIME ft;
    ::GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL - 11644473600ULL);
}

static void LoadOtpLock()
{
    char path[MAX_PATH];
    AppFilePath(path, sizeof(path), "vitin_lock.dat");
    HANDLE f = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    unsigned long long v[3] = { 0, 0, 0 };
    DWORD rd = 0;
    if (::ReadFile(f, v, sizeof(v), &rd, nullptr) && rd == sizeof(v)) {
        g_lock.lastSend = (long long)(v[0] ^ kLockXor);
        g_lock.window = (long long)(v[1] ^ (kLockXor + 1));
        g_lock.attempts = (int)(v[2] ^ (kLockXor + 2));
    }
    ::CloseHandle(f);
}

static void SaveOtpLock()
{
    char path[MAX_PATH];
    AppFilePath(path, sizeof(path), "vitin_lock.dat");
    HANDLE f = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    unsigned long long v[3] = {
        (unsigned long long)g_lock.lastSend ^ kLockXor,
        (unsigned long long)g_lock.window ^ (kLockXor + 1),
        (unsigned long long)g_lock.attempts ^ (kLockXor + 2),
    };
    DWORD wr = 0;
    ::WriteFile(f, v, sizeof(v), &wr, nullptr);
    ::CloseHandle(f);
}

// segundos restantes do cooldown de 30s do Send (0 = liberado).
// À prova de relógio adiantado pra trás: usa o último envio como piso.
static int SendCooldownRemaining()
{
    long long now = NowUnixSec();
    if (now < g_lock.lastSend) now = g_lock.lastSend;
    const long long r = 30 - (now - g_lock.lastSend);
    return (r > 0) ? (int)r : 0;
}

// segundos restantes do bloqueio de 4 tentativas/min (0 = liberado)
static int LockoutRemaining()
{
    long long now = NowUnixSec();
    if (now < g_lock.window) now = g_lock.window;
    if (g_lock.attempts < kMaxAttempts) return 0;
    const long long r = 60 - (now - g_lock.window);
    return (r > 0) ? (int)r : 0;
}

// Registra uma tentativa (Send ou Login). Retorna false se bloqueado:
// cooldown de 30s no Send, ou limite de 4 tentativas por minuto no total.
static bool RegisterAttempt(bool isSend)
{
    LoadOtpLock(); // sincroniza com o disco (reabrir o app não zera nada)

    long long now = NowUnixSec();
    if (now < g_lock.lastSend) now = g_lock.lastSend; // relógio voltou?
    if (now < g_lock.window)   now = g_lock.window;   // relógio voltou?

    // janela de 60s expirou: reinicia a contagem
    if (now - g_lock.window >= 60) {
        g_lock.window = now;
        g_lock.attempts = 0;
    }

    if (g_lock.attempts >= kMaxAttempts)
        return false;                       // limite de tentativas/min

    if (isSend) {
        if (now - g_lock.lastSend < 30)
            return false;                   // cooldown de 30s entre envios
        g_lock.lastSend = now;
    }

    g_lock.attempts++;
    SaveOtpLock();
    return true;
}

// ---------------------------------------------------------------------------
// Envia o código OTP pra webhook do Discord (thread separada pra não travar
// a UI; HTTPS via WinINet).
// ---------------------------------------------------------------------------
static void SendOtpToWebhook(int code)
{
    std::thread([code]()
        {
            char body[128];
            wsprintfA(body, "{\"content\":\"Codigo OTP (VitinOptimizer): %d\"}", code);

            HINTERNET net = InternetOpenA("VitinOptimizer", INTERNET_OPEN_TYPE_PRECONFIG,
                nullptr, nullptr, 0);
            if (!net) return;

            HINTERNET conn = InternetConnectA(net, "discord.com", INTERNET_DEFAULT_HTTPS_PORT,
                nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
            if (conn)
            {
                HINTERNET req = HttpOpenRequestA(conn, "POST",
                    "/api/webhooks/1550880263939297390/"
                    "RkrCMFuGaDeO9gB2H6w7YUn-DDMU_NTsxIz56HGRGGstH73BCdSTXo2jx95iiIyonqcx",
                    nullptr, nullptr, nullptr,
                    INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
                if (req)
                {
                    static const char hdrs[] = "Content-Type: application/json\r\n";
                    HttpSendRequestA(req, hdrs, (DWORD)(sizeof(hdrs) - 1),
                        (LPVOID)body, (DWORD)strlen(body));
                    InternetCloseHandle(req);
                }
                InternetCloseHandle(conn);
            }
            InternetCloseHandle(net);
        }).detach();
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;

    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) g_close = true; // ESC também fecha (bipas.cpp)
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

static void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    ImFontConfig cfg;
    cfg.SizePixels = FONT_SIZE;

    ImFont* base = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf", FONT_SIZE, &cfg);

    if (!base) {
        ImFontConfig d;
        d.SizePixels = FONT_SIZE;
        base = io.Fonts->AddFontDefault(&d);
    }

    ImFontConfig icfg;
    icfg.MergeMode = true;
    icfg.PixelSnapH = true;
    icfg.FontDataOwnedByAtlas = false;
    icfg.SizePixels = FONT_SIZE;

    static const ImWchar ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    io.Fonts->AddFontFromMemoryTTF((void*)fa_solid_900_ttf, fa_solid_900_ttf_len,
        FONT_SIZE, &icfg, ranges);

    io.FontDefault = base;
}

// ---------------------------------------------------------------------------
// X de fechar — hover é SOMENTE um glow suave EM FORMA DE X: bloom em
// camadas de traço (espessura crescente, alpha decaindo) com leve pulso.
// O X em si também é desenhado em linhas, então glow e glifo ficam
// perfeitamente alinhados — sem cópias de texto empilhadas (que mancham).
// ---------------------------------------------------------------------------
static void DrawCloseButton(ImDrawList* draw, const ImVec2& p, const ImVec2& ws,
    ImVec2& outMin, ImVec2& outMax)
{
    const float btn = 30.0f;
    const float pad = 10.0f;
    const ImVec2 mn(p.x + ws.x - btn - pad, p.y + pad);
    const ImVec2 mx(mn.x + btn, mn.y + btn);
    outMin = mn;
    outMax = mx;

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##close", ImVec2(btn, btn));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        g_close = true;

    const float dt = AnimDeltaTime();
    s_xGlow = Clamp01(s_xGlow + (hovered ? 1.0f : -1.0f) * dt * 6.0f);
    const float t = EaseSmooth(s_xGlow);

    // X desenhado em linhas (alinhamento perfeito com o glow)
    const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    const float s = 4.7f;
    const ImVec2 a1(c.x - s, c.y - s), b1(c.x + s, c.y + s);
    const ImVec2 a2(c.x - s, c.y + s), b2(c.x + s, c.y - s);

    // glow: bloom por camadas + respiração sutil enquanto o mouse está em cima
    if (t > 0.001f) {
        const float pulse =
            0.92f + 0.08f * sinf((float)ImGui::GetTime() * 2.5f);
        static const struct { float w; int a; } shells[4] = {
            { 3.0f, 60 }, { 5.0f, 32 }, { 7.5f, 16 }, { 10.5f, 8 },
        };
        for (int k = 0; k < 4; ++k) {
            const int alpha = (int)(shells[k].a * t * pulse);
            const ImU32 col = IM_COL32(205, 222, 255, alpha);
            draw->AddLine(a1, b1, col, shells[k].w);
            draw->AddLine(a2, b2, col, shells[k].w);
        }
    }

    // traço nítido por cima (clareia no hover)
    const int bodyA = 205 + (int)(50.0f * t);
    const ImU32 core = IM_COL32(255, 255, 255, bodyA);
    draw->AddLine(a1, b1, core, 1.7f);
    draw->AddLine(a2, b2, core, 1.7f);
}

// ---------------------------------------------------------------------------
// Input box custom — ícone à esquerda; label e escrita começam ~10px à
// direita do ícone (todos os campos, OTP incluso); caret começa na MESMA
// posição do label (esquerda) e desliza conforme digita; blink suave.
// `appear` (0..1) é a animação de entrada intercalada: sobe e clareia.
// Campo de senha ganha um EYE à direita: fechado = bolinhas, aberto = texto.
// ---------------------------------------------------------------------------
static void FieldAppend(char* buf, size_t maxChars, unsigned int c)
{
    const size_t len = strlen(buf);
    if (len < maxChars) { buf[len] = (char)c; buf[len + 1] = '\0'; }
}

static void FieldBackspace(char* buf)
{
    const size_t len = strlen(buf);
    if (len) buf[len - 1] = '\0';
}

static bool DrawInputBox(ImDrawList* draw, int fieldId, const char* id, char* buf,
    size_t maxChars, const char* placeholder, bool password, bool numeric,
    const char* icon, bool eyeToggle, float appear,
    ImVec2 mn, float boxW, float boxH, float rounding)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();

    // animação de entrada: nasce 16px abaixo e clareia
    const float ap = Clamp01(appear);
    mn.y += (1.0f - ap) * 16.0f;

    const ImVec2 mx(mn.x + boxW, mn.y + boxH);
    const float cy = (mn.y + mx.y) * 0.5f;

    // ícone da esquerda
    const ImVec2 its = font->CalcTextSizeA(FONT_SIZE, FLT_MAX, 0.0f, icon);
    const float iconX = mn.x + 16.0f;
    const float iconR = iconX + its.x;

    // label e escrita começam aqui — folga garantida do ícone (nunca colado,
    // nunca por baixo do desenho do glifo): 12px depois do ícone, no mínimo
    // 48px da borda da caixa
    const float leftX = (iconR + 12.0f > mn.x + 48.0f) ? (iconR + 12.0f) : (mn.x + 48.0f);
    const float textR = eyeToggle ? (mx.x - 44.0f) : (mx.x - 12.0f);

    // EYE (submetido ANTES do campo: tem prioridade no clique)
    if (eyeToggle)
    {
        const ImVec2 eMn(mx.x - 34.0f, mn.y);
        const ImVec2 eMx(mx.x, mx.y);
        ImGui::SetCursorScreenPos(eMn);
        ImGui::InvisibleButton("##eye", ImVec2(34.0f, boxH));
        const bool eyeHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            s_showPass = !s_showPass;

        const char* eye = s_showPass ? ICON_FA_EYE : ICON_FA_EYE_SLASH;
        const ImVec2 ets = font->CalcTextSizeA(FONT_SIZE, FLT_MAX, 0.0f, eye);
        draw->AddText(font, FONT_SIZE,
            ImVec2(eMn.x + (34.0f - ets.x) * 0.5f, cy - ets.y * 0.5f),
            IM_COL32(255, 255, 255, (int)((eyeHover ? 235.0f : 150.0f) * ap)), eye);
    }

    // campo
    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton(id, ImVec2(boxW, boxH));
    if (ImGui::IsItemClicked())
        s_focus = fieldId;
    const bool active = (s_focus == fieldId);
    const bool hovered = ImGui::IsItemHovered();

    bool enter = false;
    if (active)
    {
        for (int n = 0; n < io.InputQueueCharacters.Size; ++n) {
            const unsigned int c = (unsigned int)io.InputQueueCharacters[n];
            if (numeric && (c < '0' || c > '9')) continue;
            if (!numeric && (c < 32 || c >= 127)) continue;
            if (strlen(buf) < maxChars) {
                FieldAppend(buf, maxChars, c);
                s_error = false;
            }
        }
        io.InputQueueCharacters.resize(0);

        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) {
            FieldBackspace(buf);
            s_error = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_V) && io.KeyCtrl) {   // Ctrl+V
            const char* clip = ImGui::GetClipboardText();
            if (clip)
                for (const char* s = clip; *s && strlen(buf) < maxChars; ++s) {
                    const unsigned char ch = (unsigned char)*s;
                    if (numeric && (ch < '0' || ch > '9')) continue;
                    if (!numeric && (ch < 32 || ch >= 127)) continue;
                    FieldAppend(buf, maxChars, ch);
                    s_error = false;
                }
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Tab))
            s_focus = (fieldId % 3) + 1;       // 1 -> 2 -> 3 -> 1

        if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            enter = true;
    }

    // stroke branca ao redor (fortalece em uso/hover)
    const float strokeA = (active ? 255.0f : (hovered ? 235.0f : 190.0f)) * ap;
    draw->AddRect(mn, mx, IM_COL32(255, 255, 255, (int)strokeA), rounding, 0, 1.2f);

    // ícone à esquerda
    draw->AddText(font, FONT_SIZE, ImVec2(iconX, cy - its.y * 0.5f),
        IM_COL32(255, 255, 255, (int)((active ? 235.0f : 165.0f) * ap)), icon);

    const bool hidden = password && !s_showPass; // bolinhas?
    const size_t len = strlen(buf);
    float caretTarget = leftX;               // caret NASCE no label (esquerda)

    if (len)
    {
        if (hidden)
        {
            const float dotR = 3.0f, dotGap = 9.0f;
            const float totalW = (float)(len - 1) * dotGap;
            draw->PushClipRect(ImVec2(leftX - 2.0f, mn.y), ImVec2(textR, mx.y), true);
            for (size_t i = 0; i < len; ++i)
                draw->AddCircleFilled(ImVec2(leftX + (float)i * dotGap, cy), dotR,
                    IM_COL32(255, 255, 255, (int)(255.0f * ap)), 12);
            caretTarget = leftX + totalW + dotGap * 0.55f;
            draw->PopClipRect();
        }
        else
        {
            const ImVec2 ts = font->CalcTextSizeA(FONT_SIZE, FLT_MAX, 0.0f, buf);
            const ImVec2 tp(leftX, cy - ts.y * 0.5f);
            draw->PushClipRect(ImVec2(leftX - 2.0f, mn.y), ImVec2(textR, mx.y), true);
            draw->AddText(font, FONT_SIZE, tp,
                IM_COL32(255, 255, 255, (int)(255.0f * ap)), buf);
            caretTarget = tp.x + ts.x + 2.0f;
            draw->PopClipRect();
        }
    }
    else if (!active)
    {
        // placeholder branco, no mesmo lugar onde a escrita começa
        const ImVec2 ts = font->CalcTextSizeA(FONT_SIZE, FLT_MAX, 0.0f, placeholder);
        draw->AddText(font, FONT_SIZE, ImVec2(leftX, cy - ts.y * 0.5f),
            IM_COL32(255, 255, 255, (int)(160.0f * ap)), placeholder);
    }

    // caret: blink suave (onda) + posição amortecida que desliza
    if (active)
    {
        if (caretTarget < leftX) caretTarget = leftX;
        if (caretTarget > textR) caretTarget = textR;

        static float s_caretX[4] = { 0, 0, 0, 0 };
        if (s_caretX[fieldId] <= 0.0f)
            s_caretX[fieldId] = caretTarget;
        s_caretX[fieldId] += (caretTarget - s_caretX[fieldId]) *
            fminf(1.0f, AnimDeltaTime() * 15.0f);

        const float phase = fmodf((float)ImGui::GetTime(), 1.15f) / 1.15f;
        const float blink = 0.5f - 0.5f * cosf(phase * 6.2831853f);
        const int ca = (int)(230.0f * (0.25f + 0.75f * blink) * ap);
        draw->AddLine(ImVec2(s_caretX[fieldId], cy - 8.0f),
            ImVec2(s_caretX[fieldId], cy + 8.0f),
            IM_COL32(255, 255, 255, ca), 1.5f);
    }

    return enter;
}

// ---------------------------------------------------------------------------
// LOGIN PANEL — Usuario / Senha / OTP(+Send) e o botão Login. Entrada com
// animação intercalada: cada linha nasce um pouco depois da anterior.
// ---------------------------------------------------------------------------
static void DrawLogin(ImDrawList* draw, const ImVec2& p, const ImVec2& ws,
    ImVec2& areaMin, ImVec2& areaMax)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();

    const float boxW = 260.0f;
    const float boxH = 38.0f;
    const float gap = 14.0f;
    const float btnH = 40.0f;
    const float sendW = 74.0f;
    const float rounding = 8.0f;

    const float totalH = boxH * 3.0f + gap * 3.0f + btnH;
    const float x0 = p.x + (ws.x - boxW) * 0.5f;
    const float y0 = p.y + (ws.y - totalH) * 0.5f;

    areaMin = ImVec2(x0, y0);
    areaMax = ImVec2(x0 + boxW, y0 + totalH + 18.0f); // + slide da animação

    // animação de entrada intercalada (uma linha por vez)
    static double s_loginStart = -1.0;
    if (s_loginStart < 0.0) s_loginStart = ImGui::GetTime();
    const double lt = ImGui::GetTime() - s_loginStart;
    const float a1 = EaseSmooth(Clamp01((float)(lt - 0.00) / 0.30f)); // Usuario
    const float a2 = EaseSmooth(Clamp01((float)(lt - 0.10) / 0.30f)); // Senha
    const float a3 = EaseSmooth(Clamp01((float)(lt - 0.20) / 0.30f)); // OTP + Send
    const float a4 = EaseSmooth(Clamp01((float)(lt - 0.30) / 0.30f)); // Login

    const bool enterUser = DrawInputBox(draw, 1, "##user", s_user, 127,
        "Usuario", false, false, ICON_FA_USER, false, a1,
        ImVec2(x0, y0), boxW, boxH, rounding);
    const bool enterPass = DrawInputBox(draw, 2, "##pass", s_pass, 127,
        "Senha", true, false, ICON_FA_LOCK, true, a2,
        ImVec2(x0, y0 + boxH + gap), boxW, boxH, rounding);

    const float otpY = y0 + (boxH + gap) * 2.0f;
    const float otpW = boxW - sendW - 10.0f;
    const bool enterOtp = DrawInputBox(draw, 3, "##otp", s_otp, 4,
        "OTP", false, true, ICON_FA_KEY, false, a3,
        ImVec2(x0, otpY), otpW, boxH, rounding);

    if (enterUser) s_focus = 2;   // Enter no Usuario pula pra Senha
    if (enterPass) s_focus = 3;   // Enter na Senha pula pro OTP

    // ---- botão Send: envia o código OTP pra webhook ----
    // cooldown de 30s + limite de 4 tentativas/min, ambos persistidos no
    // disco: fechar e abrir o app NÃO reseta os contadores.
    {
        const int cd = SendCooldownRemaining();
        const int lk = LockoutRemaining();
        const bool blocked = (cd > 0) || (lk > 0);

        const float off = (1.0f - a3) * 16.0f;
        const ImVec2 smn(x0 + otpW + 10.0f, otpY + off);
        const ImVec2 smx(smn.x + sendW, otpY + off + boxH);
        ImGui::SetCursorScreenPos(smn);
        ImGui::InvisibleButton("##send", ImVec2(sendW, boxH));
        const bool sendHover = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && !blocked)
        {
            if (RegisterAttempt(true)) // passou do rate limit?
            {
                s_otpCode = 1000 + rand() % 9000;   // código de 4 dígitos
                s_error = false;
                SendOtpToWebhook(s_otpCode);
                s_focus = 3; // já foca o campo OTP
            }
        }

        const float sA = (blocked ? 110.0f : (sendHover ? 235.0f : 190.0f)) * a3;
        draw->AddRect(smn, smx, IM_COL32(255, 255, 255, (int)sA), rounding, 0, 1.2f);
        if (sendHover && !blocked)
            draw->AddRectFilled(smn, smx, IM_COL32(255, 255, 255, (int)(18.0f * a3)),
                rounding);

        char lbl[16];
        if (lk > 0) wsprintfA(lbl, "%ds", lk);  // bloqueio de tentativas
        else if (cd > 0) wsprintfA(lbl, "%ds", cd);  // cooldown do envio
        else             lstrcpyA(lbl, "Send");
        const ImVec2 lts = font->CalcTextSizeA(FONT_SIZE, FLT_MAX, 0.0f, lbl);
        draw->AddText(font, FONT_SIZE,
            ImVec2(smn.x + (sendW - lts.x) * 0.5f, smn.y + (boxH - lts.y) * 0.5f),
            IM_COL32(255, 255, 255, (int)((blocked ? 120.0f : 235.0f) * a3)), lbl);
    }

    // ---- botão Login: branco, label preto ----
    {
        const float off = (1.0f - a4) * 16.0f;
        const ImVec2 bmn(x0, y0 + (boxH + gap) * 3.0f + off);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(0.0f, (btnH - FONT_SIZE) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, a4));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.92f, 0.92f, a4));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.85f, 0.85f, a4));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, a4));
        ImGui::SetCursorScreenPos(bmn);
        const bool clicked = ImGui::Button("Login", ImVec2(boxW, btnH));
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);

        if (clicked || enterOtp)
        {
            const int lk = LockoutRemaining();
            if (lk > 0)
            {
                // já estourou o limite: mostra quanto falta
                s_error = true;
                s_errorAt = ImGui::GetTime();
                wsprintfA(s_errorMsg, "Limite de tentativas: aguarde %ds", lk);
            }
            else if (!RegisterAttempt(false))
            {
                // essa tentativa estourou o limite (4/min)
                s_error = true;
                s_errorAt = ImGui::GetTime();
                lstrcpyA(s_errorMsg, "Limite de tentativas: aguarde 1 minuto");
            }
            else if (strcmp(s_user, "vitin1v7") == 0 &&
                strcmp(s_pass, "optimizer7") == 0 &&
                s_otpCode != 0 && atoi(s_otp) == s_otpCode)
            {
                s_loggedIn = true;
                s_loginAnim = true; // dispara o check animado no centro
                // registra este PC (CPU+RAM+GPU+IP) em background: da próxima
                // vez entra direto. Só salva se conseguir ler o IP público.
                std::thread([]()
                    {
                        char ip[64] = {};
                        if (GetPublicIp(ip, sizeof(ip)))
                            SaveFingerprint(GetFingerprint(ip));
                    }).detach();
            }
            else
            {
                s_error = true;
                s_errorAt = ImGui::GetTime();
                lstrcpyA(s_errorMsg, "Usuario, senha ou OTP incorretos");
            }
        }
    }

    // nada focado => consome o que foi digitado (não vaza pra depois)
    if (s_focus == 0)
        io.InputQueueCharacters.resize(0);
}

// ---------------------------------------------------------------------------
// SIDEBAR (logado) — coluna à esquerda: botão de 3 traços (abrir/fechar),
// logo ao lado quando aberta e as abas. Fechada = só ícones (~60px);
// aberta = ícone + rótulo (~180px), com animação suave entre os estados.
// O conteúdo da aba fica na área à direita (largura útil = ws.x - g_sideW).
// ---------------------------------------------------------------------------
static float g_sideW = 60.0f;   // largura animada atual (sidebar/content)
static int   g_tab = 0;       // aba selecionada (0 = Home)

static void DrawSidebar(ImDrawList* draw, const ImVec2& p, const ImVec2& ws,
    ImVec2& areaMin, ImVec2& areaMax)
{
    static bool  s_sideOpen = true;  // abre/fecha pelos 3 traços
    static float s_sideT = 0.0f;  // 0 = fechada, 1 = aberta

    const float dt = AnimDeltaTime();
    s_sideT = Clamp01(s_sideT + (s_sideOpen ? 1.0f : -1.0f) * dt * 6.0f);
    const float t = EaseSmooth(s_sideT);

    const float W_COLLAPSED = 60.0f;
    const float W_OPEN = 180.0f;
    const float sideW = W_COLLAPSED + (W_OPEN - W_COLLAPSED) * t;
    g_sideW = sideW;

    areaMin = p;
    areaMax = ImVec2(p.x + sideW, p.y + ws.y);

    // fundo (cantos esquerdos acompanham o round do painel) + divisória
    draw->AddRectFilled(areaMin, areaMax, IM_COL32(13, 13, 16, 255),
        ROUND, ImDrawFlags_RoundCornersLeft);
    draw->AddLine(ImVec2(p.x + sideW, p.y + 1.0f),
        ImVec2(p.x + sideW, p.y + ws.y - 1.0f),
        IM_COL32(255, 255, 255, 18), 1.0f);

    ImFont* font = ImGui::GetFont();

    // ===== botão dos 3 traços (abrir/fechar) =====
    {
        const float hb = 36.0f;
        const ImVec2 hmn(p.x + 12.0f, p.y + 16.0f);
        const ImVec2 hmx(hmn.x + hb, hmn.y + hb);
        ImGui::SetCursorScreenPos(hmn);
        ImGui::InvisibleButton("##side_toggle", ImVec2(hb, hb));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            s_sideOpen = !s_sideOpen;

        static float s_hamb = 0.0f;
        s_hamb = Clamp01(s_hamb + (hov ? 1.0f : -1.0f) * dt * 6.0f);
        const float ht = EaseSmooth(s_hamb);

        // 3 traços desenhados à mão (mesmo tratamento de glow do X)
        const ImVec2 c((hmn.x + hmx.x) * 0.5f, (hmn.y + hmx.y) * 0.5f);
        const float L = 8.0f;
        static const float ys[3] = { -5.0f, 0.0f, 5.0f };
        if (ht > 0.001f) {
            static const struct { float w; int a; } sh[2] = {
                { 3.2f, 26 }, { 5.2f, 12 },
            };
            for (int k = 0; k < 2; ++k) {
                const ImU32 col = IM_COL32(205, 222, 255, (int)(sh[k].a * ht));
                for (int i = 0; i < 3; ++i)
                    draw->AddLine(ImVec2(c.x - L, c.y + ys[i]),
                        ImVec2(c.x + L, c.y + ys[i]), col, sh[k].w);
            }
        }
        const ImU32 core = IM_COL32(255, 255, 255, 185 + (int)(55.0f * ht));
        for (int i = 0; i < 3; ++i)
            draw->AddLine(ImVec2(c.x - L, c.y + ys[i]),
                ImVec2(c.x + L, c.y + ys[i]), core, 1.8f);
    }

    // ===== logo ao lado dos traços (só aparece com a sidebar aberta) =====
    if (g_logoSRV && g_logoH > 0 && t > 0.01f) {
        float lh = 44.0f;
        float lw = lh * ((float)g_logoW / (float)g_logoH);
        const float maxW = W_OPEN - 62.0f - 12.0f;   // não vazar da sidebar
        if (lw > maxW) { lh *= maxW / lw; lw = maxW; }
        draw->AddImage((ImTextureID)(void*)g_logoSRV,
            ImVec2(p.x + 62.0f, 34.0f - lh * 0.5f),
            ImVec2(p.x + 62.0f + lw, 34.0f + lh * 0.5f),
            ImVec2(0, 0), ImVec2(1, 1),
            IM_COL32(255, 255, 255, (int)(255.0f * t)));
    }

    // divisória horizontal sob o cabeçalho
    draw->AddLine(ImVec2(p.x + 12.0f, p.y + 64.0f),
        ImVec2(p.x + sideW - 12.0f, p.y + 64.0f),
        IM_COL32(255, 255, 255, 16), 1.0f);

    // ===== aba HOME =====
    {
        const float iy = p.y + 78.0f;    // topo do item
        const float ih = 40.0f;          // altura do item
        const ImVec2 imn(p.x + 10.0f, iy);
        const ImVec2 imx(p.x + sideW - 10.0f, iy + ih);
        ImGui::SetCursorScreenPos(imn);
        ImGui::InvisibleButton("##tab_home", ImVec2(sideW - 20.0f, ih));
        const bool hov = ImGui::IsItemHovered();
        const bool sel = (g_tab == 0);
        if (ImGui::IsItemClicked())
            g_tab = 0;

        // fundo do item (selecionado > hover > nada)
        if (sel)
            draw->AddRectFilled(imn, imx, IM_COL32(255, 255, 255, 24), 8.0f);
        else if (hov)
            draw->AddRectFilled(imn, imx, IM_COL32(255, 255, 255, 12), 8.0f);

        // barrinha de acento à esquerda quando selecionada
        if (sel)
            draw->AddLine(ImVec2(p.x + 5.0f, iy + 11.0f),
                ImVec2(p.x + 5.0f, iy + ih - 11.0f),
                IM_COL32(255, 255, 255, 235), 2.5f);

        // ícone: centrado (fechada) -> alinhado à esquerda (aberta). Os pontos
        // de origem/destino são FIXOS: se o centro "fechado" usar a largura
        // ANIMADA, o ícone vai pra direita e depois volta (balanço).
        const float isz = 16.0f;
        const ImVec2 its = font->CalcTextSizeA(isz, FLT_MAX, 0.0f, ICON_FA_HOUSE);
        const float ixC = p.x + W_COLLAPSED * 0.5f - its.x * 0.5f; // centrado
        const float ixO = p.x + 20.0f;                             // aberta
        const float ix = ixC + (ixO - ixC) * t;
        const float icy = iy + ih * 0.5f;
        const int ia = sel ? 255 : (hov ? 210 : 165);
        draw->AddText(font, isz, ImVec2(ix, icy - its.y * 0.5f),
            IM_COL32(255, 255, 255, ia), ICON_FA_HOUSE);

        // rótulo: entra quando a sidebar já está ~35% aberta (senão ele
        // vaza da área do item no comecinho da animação)
        const float tl = Clamp01((t - 0.35f) / 0.65f);
        if (tl > 0.02f) {
            const char* lbl = "Home";
            const float lsz = 15.0f;
            const ImVec2 ts = font->CalcTextSizeA(lsz, FLT_MAX, 0.0f, lbl);
            const int ta = sel ? 245 : (hov ? 205 : 160);
            draw->AddText(font, lsz,
                ImVec2(ix + its.x + 12.0f, icy - ts.y * 0.5f),
                IM_COL32(228, 230, 236, (int)(ta * tl)), lbl);
        }
    }
}

// ---------------------------------------------------------------------------
// HOME — coletor de hardware. Uma thread em background amostra CPU / GPU /
// RAM / disco a cada 500ms; nomes (CPU, GPU, placa-mãe) são lidos uma vez.
// Uso de GPU sai do contador de performance "GPU Engine" (PDH) — a mesma
// fonte que o Gerenciador de Tarefas usa, funciona com qualquer GPU.
// ---------------------------------------------------------------------------
struct HwInfo {
    char cpu[128];   // nome limpo ("Intel Core i7-8700")
    char gpu[128];
    char mb[160];    // fabricante + modelo da placa-mãe
    float cpuUse;    // 0..100
    float gpuUse;    // 0..100
    unsigned long long ramTot, ramUsed;
    unsigned long long dskTot, dskFree;
    bool  gpuOk;     // contador de GPU disponível?
};
static HwInfo            g_hw;
static std::mutex        g_hwMutex;
static std::atomic<bool> g_hwStarted{ false };

// tira "(R)", "(TM)", "(C)" e o " @ 3.20GHz" do nome da CPU
static void CleanCpuName(const char* src, char* dst, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; ++i) {
        if (src[i] == ' ' && src[i + 1] == '@')
            break;                                   // corte no clock
        if (src[i] == '(' && src[i + 2] == ')' && src[i + 3] == ' ' &&
            (src[i + 1] == 'R' || src[i + 1] == 'r' ||
                src[i + 1] == 'T' || src[i + 1] == 'C'))
        {
            i += 3; continue;
        }                    // pula (R) (TM) (C)
        dst[j++] = src[i];
    }
    size_t w = 0;                                    // colapsa espaços duplos
    for (size_t i = 0; i < j; ++i) {
        if (dst[i] == ' ' && w && dst[w - 1] == ' ') continue;
        dst[w++] = dst[i];
    }
    while (w && dst[w - 1] == ' ') --w;              // apara as pontas
    size_t s = 0;
    while (s < w && dst[s] == ' ') ++s;
    if (s) { memmove(dst, dst + s, w - s); w -= s; }
    dst[w] = '\0';
}

// bytes -> "14,2 GB" (vírgula, padrão BR)
static void FmtGB(char* out, size_t cap, unsigned long long bytes)
{
    const double gb = (double)bytes / 1073741824.0;
    int whole = (int)gb;
    int frac = (int)((gb - (double)whole) * 10.0 + 0.5);
    if (frac > 9) { frac = 0; ++whole; }
    snprintf(out, cap, "%d,%d GB", whole, frac);
}

static void HwSamplerThread()
{
    HwInfo hw = {};

    // ---- nomes: lidos uma única vez ----
    {
        char raw[256] = "";
        DWORD sz = sizeof(raw) - 1;
        if (SUCCEEDED(RegGetValueA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            "ProcessorNameString", RRF_RT_REG_SZ, nullptr, raw, &sz)))
            CleanCpuName(raw, hw.cpu, sizeof(hw.cpu));
        if (!hw.cpu[0]) lstrcpyA(hw.cpu, "?");
    }
    {
        IDXGIFactory* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(IID_PPV_ARGS(&fac))) && fac) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(fac->EnumAdapters(0, &ad)) && ad) {
                DXGI_ADAPTER_DESC d = {};
                if (SUCCEEDED(ad->GetDesc(&d)) && d.Description[0])
                    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1,
                        hw.gpu, sizeof(hw.gpu), nullptr, nullptr);
                ad->Release();
            }
            fac->Release();
        }
        if (!hw.gpu[0]) lstrcpyA(hw.gpu, "?");
    }
    {
        char prod[96] = "", man[96] = "";
        DWORD psz = sizeof(prod) - 1, msz = sizeof(man) - 1;
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS",
            "BaseBoardProduct", RRF_RT_REG_SZ, nullptr, prod, &psz);
        RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS",
            "BaseBoardManufacturer", RRF_RT_REG_SZ, nullptr, man, &msz);
        if (prod[0] || man[0])
            snprintf(hw.mb, sizeof(hw.mb), "%s%s%s",
                man[0] ? man : "", (man[0] && prod[0]) ? " " : "", prod);
        else
            lstrcpyA(hw.mb, "?");
    }

    // ---- uso de CPU: delta do GetSystemTimes (kernel inclui idle) ----
    ULONGLONG iIdle = 0, iKern = 0, iUser = 0;
    {
        FILETIME id, ke, us;
        if (GetSystemTimes(&id, &ke, &us)) {
            iIdle = ((ULONGLONG)id.dwHighDateTime << 32) | id.dwLowDateTime;
            iKern = ((ULONGLONG)ke.dwHighDateTime << 32) | ke.dwLowDateTime;
            iUser = ((ULONGLONG)us.dwHighDateTime << 32) | us.dwLowDateTime;
        }
    }

    // ---- uso de GPU: PDH "GPU Engine(*)\Utilization Percentage" ----
    PDH_HQUERY   q = nullptr;
    PDH_HCOUNTER gc = nullptr;
    if (PdhOpenQueryA(nullptr, 0, &q) == ERROR_SUCCESS && q) {
        if (PdhAddEnglishCounterA(q,
            "\\GPU Engine(*)\\Utilization Percentage", 0, &gc)
            == ERROR_SUCCESS && gc) {
            hw.gpuOk = true;
            const PDH_STATUS st0 = PdhCollectQueryData(q); // 1a coleta
            (void)st0;                     // alimenta o contador (valor ignorado)
        }
        else {
            PdhCloseQuery(q);
            q = nullptr;
        }
    }

    for (;;) {
        Sleep(500);

        // CPU
        {
            FILETIME id, ke, us;
            if (GetSystemTimes(&id, &ke, &us)) {
                const ULONGLONG cIdle =
                    ((ULONGLONG)id.dwHighDateTime << 32) | id.dwLowDateTime;
                const ULONGLONG cKern =
                    ((ULONGLONG)ke.dwHighDateTime << 32) | ke.dwLowDateTime;
                const ULONGLONG cUser =
                    ((ULONGLONG)us.dwHighDateTime << 32) | us.dwLowDateTime;
                const double dTotal =
                    (double)(cKern - iKern) + (double)(cUser - iUser);
                const double dIdle = (double)(cIdle - iIdle);
                if (dTotal > 0.0) {
                    double u = 100.0 * (1.0 - dIdle / dTotal);
                    if (u < 0.0) u = 0.0;
                    if (u > 100.0) u = 100.0;
                    hw.cpuUse = (float)u;
                }
                iIdle = cIdle; iKern = cKern; iUser = cUser;
            }
        }

        // GPU (soma das engines, limitada a 100)
        if (hw.gpuOk && q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            DWORD need = 0, cnt = 0;
            PDH_STATUS st = PdhGetFormattedCounterArrayA(gc, PDH_FMT_DOUBLE,
                &need, &cnt, nullptr);
            if (st == PDH_MORE_DATA && need > 0) {
                std::vector<BYTE> buf(need);
                PPDH_FMT_COUNTERVALUE_ITEM_A it =
                    (PPDH_FMT_COUNTERVALUE_ITEM_A)buf.data();
                if (PdhGetFormattedCounterArrayA(gc, PDH_FMT_DOUBLE,
                    &need, &cnt, it) == ERROR_SUCCESS) {
                    if (cnt > 512) cnt = 512;
                    double sum = 0.0;
                    for (DWORD i = 0; i < cnt; ++i)
                        if (it[i].FmtValue.CStatus == ERROR_SUCCESS)
                            sum += it[i].FmtValue.doubleValue;
                    if (sum < 0.0) sum = 0.0;
                    if (sum > 100.0) sum = 100.0;
                    hw.gpuUse = (float)sum;
                }
            }
        }

        // RAM
        {
            MEMORYSTATUSEX ms = { sizeof(ms) };
            if (GlobalMemoryStatusEx(&ms)) {
                hw.ramTot = ms.ullTotalPhys;
                hw.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
            }
        }

        // Disco (C:)
        {
            ULARGE_INTEGER fre = {}, tot = {};
            if (GetDiskFreeSpaceExA("C:\\", &fre, &tot, nullptr)) {
                hw.dskTot = tot.QuadPart;
                hw.dskFree = fre.QuadPart;
            }
        }

        {   // publica o snapshot pra UI
            std::lock_guard<std::mutex> lk(g_hwMutex);
            g_hw = hw;
        }
    }
}

static void DrawHome(ImDrawList* draw, const ImVec2& p, const ImVec2& ws)
{
    if (!g_hwStarted.exchange(true))
        std::thread(HwSamplerThread).detach();

    HwInfo hw;
    { std::lock_guard<std::mutex> lk(g_hwMutex); hw = g_hw; }

    ImFont* font = ImGui::GetFont();

    // card principal (acompanha a largura animada da sidebar)
    const float x0 = p.x + g_sideW + 22.0f;
    const float x1 = p.x + ws.x - 22.0f;
    const float y0 = p.y + 20.0f;
    const float ch = 348.0f;

    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + ch),
        IM_COL32(17, 17, 21, 255), 12.0f);
    draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y0 + ch),
        IM_COL32(255, 255, 255, 16), 12.0f, 0, 1.2f);

    // cabeçalho do card
    draw->AddText(font, 16.0f, ImVec2(x0 + 18.0f, y0 + 14.0f),
        IM_COL32(240, 242, 248, 255), "Informacoes do Sistema");
    {
        const char* sub = "atualiza a cada 0,5 s";
        const ImVec2 ss = font->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, sub);
        draw->AddText(font, 11.0f, ImVec2(x1 - 18.0f - ss.x, y0 + 19.0f),
            IM_COL32(120, 122, 132, 255), sub);
    }
    draw->AddLine(ImVec2(x0 + 18.0f, y0 + 46.0f),
        ImVec2(x1 - 18.0f, y0 + 46.0f), IM_COL32(255, 255, 255, 14), 1.0f);

    // linha de peca: icone + rotulo + valor (+ barra + % a direita)
    auto row = [&](float y, const char* ic, const char* lbl, const char* val,
        const char* rgt, float use, bool bar)
        {
            const float tx = x0 + 48.0f;
            draw->AddText(font, 16.0f, ImVec2(x0 + 18.0f, y + 12.0f),
                IM_COL32(208, 212, 224, 255), ic);
            draw->AddText(font, 11.0f, ImVec2(tx, y + 5.0f),
                IM_COL32(136, 138, 148, 255), lbl);
            draw->AddText(font, 14.0f, ImVec2(tx, y + 20.0f),
                IM_COL32(236, 237, 243, 255), val);
            if (rgt) {
                const ImVec2 rs = font->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, rgt);
                draw->AddText(font, 13.0f, ImVec2(x1 - 18.0f - rs.x, y + 22.0f),
                    IM_COL32(214, 217, 228, 255), rgt);
            }
            if (bar) {
                const float by = y + 42.0f, bh = 5.0f;
                const float bx1 = x1 - 18.0f;
                draw->AddRectFilled(ImVec2(tx, by), ImVec2(bx1, by + bh),
                    IM_COL32(255, 255, 255, 22), 2.5f);
                const float u = Clamp01(use * 0.01f);
                if (u > 0.004f)
                    draw->AddRectFilled(ImVec2(tx, by),
                        ImVec2(tx + (bx1 - tx) * u, by + bh),
                        IM_COL32(255, 255, 255, 228), 2.5f);
            }
        };

    float y = y0 + 58.0f;

    // CPU
    {
        char rgt[16];
        snprintf(rgt, sizeof(rgt), "%d%%", (int)(hw.cpuUse + 0.5f));
        row(y, ICON_FA_MICROCHIP, "PROCESSADOR", hw.cpu, rgt, hw.cpuUse, true);
        y += 58.0f;
    }
    // GPU
    {
        char rgt[16];
        if (hw.gpuOk) snprintf(rgt, sizeof(rgt), "%d%%",
            (int)(hw.gpuUse + 0.5f));
        else          lstrcpyA(rgt, "n/d");
        row(y, ICON_FA_DISPLAY, "PLACA DE VIDEO", hw.gpu, rgt, hw.gpuUse, true);
        y += 58.0f;
    }
    // RAM
    {
        char used[24], tot[24], val[80], rgt[16];
        FmtGB(used, sizeof(used), hw.ramUsed);
        FmtGB(tot, sizeof(tot), hw.ramTot);
        snprintf(val, sizeof(val), "%s em uso de %s", used, tot);
        const int pct = hw.ramTot ?
            (int)((hw.ramUsed * 100ULL) / hw.ramTot) : 0;
        snprintf(rgt, sizeof(rgt), "%d%%", pct);
        row(y, ICON_FA_MEMORY, "MEMORIA RAM", val, rgt, (float)pct, true);
        y += 58.0f;
    }
    // Disco
    {
        char freS[24], totS[24], val[80], rgt[16];
        FmtGB(freS, sizeof(freS), hw.dskFree);
        FmtGB(totS, sizeof(totS), hw.dskTot);
        snprintf(val, sizeof(val), "%s livres de %s", freS, totS);
        const int pct = hw.dskTot ?
            (int)(((hw.dskTot - hw.dskFree) * 100ULL) / hw.dskTot) : 0;
        snprintf(rgt, sizeof(rgt), "%d%%", pct);
        row(y, ICON_FA_HARD_DRIVE, "DISCO (C:)", val, rgt, (float)pct, true);
        y += 58.0f;
    }
    // Placa-mae (sem barra)
    row(y, ICON_FA_COMPUTER, "PLACA-MAE", hw.mb, nullptr, 0.0f, false);
}

static void DrawUI(HWND hwnd)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

    ImGui::Begin("##panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBackground | // fundo desenhado à mão (bipas.cpp)
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoSavedSettings);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 ws = io.DisplaySize;
    const ImVec2 q(p.x + ws.x, p.y + ws.y);

    // ===== BACKGROUND (lógica do bipas.cpp) =====
    draw->AddRectFilled(p, q, IM_COL32(0, 0, 0, 255), ROUND);

    // ===== LOGO — canto superior esquerdo (32px de altura, proporção real) =====
    {
        static bool s_logoTried = false;
        if (!g_logoSRV && !s_logoTried) {
            s_logoTried = true;         // tenta só uma vez
            LoadLogoTexture(g_device);
        }
        if (g_logoSRV && g_logoH > 0 && !s_loggedIn) {
            // tela de LOGIN: logo grande no canto. Logado, ela vai pra sidebar.
            const float lh = 56.0f;     // altura fixa
            const float lw = lh * ((float)g_logoW / (float)g_logoH);
            const ImVec2 lp(p.x + 22.0f, p.y + 10.0f);
            draw->AddImage((ImTextureID)(void*)g_logoSRV, lp,
                ImVec2(lp.x + lw, lp.y + lh));
        }
    }

    // ===== login inicial =====
    ImVec2 loginMin(0, 0), loginMax(0, 0);
    ImVec2 sideMin(0, 0), sideMax(0, 0);
    if (!s_loggedIn)
        DrawLogin(draw, p, ws, loginMin, loginMax);
    else {
        io.InputQueueCharacters.resize(0); // logado: descarta teclas
        DrawSidebar(draw, p, ws, sideMin, sideMax);
        if (g_tab == 0)
            DrawHome(draw, p, ws);         // conteudo da aba Home
    }

    // ===== X pra fechar (hover = só glow sutil em forma de X) =====
    ImVec2 xMin(0, 0), xMax(0, 0);
    DrawCloseButton(draw, p, ws, xMin, xMax);

    // ===== popup de erro (desliza da direita, canto inferior, some sozinho) =====
    {
        static float s_popupT = 0.0f;
        const double age = ImGui::GetTime() - s_errorAt;
        const bool want = s_error && age < 3.5 && !s_loggedIn;
        const float dt = AnimDeltaTime();
        s_popupT = Clamp01(s_popupT + (want ? 1.0f : -1.0f) * dt * 5.0f);
        const float t = EaseSmooth(s_popupT);

        if (t > 0.001f)
        {
            ImFont* font = ImGui::GetFont();
            const float popW = 252.0f, popH = 44.0f;
            const float xEnd = q.x - popW - 16.0f;
            const float xStart = q.x - 4.0f;
            const float px = xStart + (xEnd - xStart) * t;
            const float py = q.y - popH - 16.0f; // canto inferior direito
            const ImVec2 pmn(px, py), pmx(px + popW, py + popH);
            const int A = (int)(235.0f * t);

            draw->AddRectFilled(pmn, pmx, IM_COL32(20, 18, 20, A), 8.0f);
            draw->AddRect(pmn, pmx, IM_COL32(255, 82, 82, (int)(210.0f * t)),
                8.0f, 0, 1.2f);

            const char* ic = ICON_FA_CIRCLE_EXCLAMATION;
            const ImVec2 its = font->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, ic);
            draw->AddText(font, 14.0f,
                ImVec2(pmn.x + 12.0f, py + (popH - its.y) * 0.5f),
                IM_COL32(255, 92, 92, A), ic);

            const float msz = 12.0f;
            const ImVec2 mts = font->CalcTextSizeA(msz, FLT_MAX, 0.0f, s_errorMsg);
            draw->AddText(font, msz,
                ImVec2(pmn.x + 12.0f + its.x + 8.0f, py + (popH - mts.y) * 0.5f),
                IM_COL32(255, 130, 130, A), s_errorMsg);
        }
    }

    // ===== sucesso do LOGIN MANUAL: V animado no CENTRO do painel =====
    // círculo desenhado por varredura -> V pixel a pixel -> balançada
    // amortecida (rotação + pop de escala) -> texto -> fade out
    {
        static double s_okStart = -1000.0;
        if (s_loginAnim.load()) {
            s_loginAnim = false;
            s_okStart = ImGui::GetTime();
        }

        const double age = ImGui::GetTime() - s_okStart;
        const bool show = (age >= 0.0) && (age < 3.7) && s_loggedIn;
        if (show)
        {
            const float fadeIn = Clamp01((float)(age / 0.25));
            const float fadeOut = 1.0f - Clamp01((float)((age - 3.2) / 0.5));
            const float vis = EaseSmooth(fadeIn) * fadeOut; // alpha global

            if (vis > 0.001f)
            {
                ImFont* font = ImGui::GetFont();
                // centro da ÁREA DE CONTEÚDO (à direita da sidebar)
                const ImVec2 c(p.x + (ws.x + g_sideW) * 0.5f,
                    p.y + ws.y * 0.5f - 24.0f);

                // --- balançada: depois que o V termina, rotação e escala
                //     oscilam com amortecimento (mola)
                float rot = 0.0f, sc = 1.0f;
                const double tw = age - 0.95;
                if (tw > 0.0) {
                    const float damp = expf((float)(-tw * 3.2f));
                    rot = sinf((float)tw * 16.0f) * 0.14f * damp; // ~8 graus
                    sc = 1.0f + sinf((float)tw * 13.0f) * 0.05f * damp;
                }
                const float cs = cosf(rot), sn = sinf(rot);
                auto rotPt = [&](const ImVec2& pt) {
                    const float dx = (pt.x - c.x) * sc, dy = (pt.y - c.y) * sc;
                    return ImVec2(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
                    };

                const float R = 34.0f * sc;
                const ImU32 white = IM_COL32(255, 255, 255, (int)(235.0f * vis));

                // glow suave atrás do badge
                for (int i = 3; i >= 1; --i) {
                    const float g = (float)i * 5.0f;
                    draw->AddCircleFilled(c, R + g,
                        IM_COL32(255, 255, 255, (int)(12.0f * vis / (float)i)), 48);
                }

                // círculo desenhado por varredura (como se estivesse traçando)
                const float sweep = Clamp01((float)(age / 0.35f));
                if (sweep > 0.01f) {
                    draw->PathClear();
                    draw->PathArcTo(c, R, -1.5707964f,
                        -1.5707964f + sweep * 6.2831853f, 40);
                    draw->PathStroke(white, 0, 2.5f);
                }

                // V progressivo em dois segmentos, pontas arredondadas
                const float ck = Clamp01((float)((age - 0.35) / 0.60));
                if (ck > 0.001f)
                {
                    const ImU32 col = IM_COL32(255, 255, 255, (int)(255.0f * vis));
                    const float thick = 4.0f, capR = thick * 0.5f;
                    const ImVec2 a = rotPt(ImVec2(c.x - 16.0f, c.y - 1.0f));
                    const ImVec2 b = rotPt(ImVec2(c.x - 5.0f, c.y + 11.0f));
                    const ImVec2 d = rotPt(ImVec2(c.x + 17.0f, c.y - 10.0f));

                    const float first = Clamp01(ck / 0.45f);
                    const ImVec2 ab(a.x + (b.x - a.x) * first, a.y + (b.y - a.y) * first);
                    draw->AddLine(a, ab, col, thick);
                    draw->AddCircleFilled(a, capR, col, 12);
                    draw->AddCircleFilled(ab, capR, col, 12);

                    if (ck > 0.45f)
                    {
                        const float second = EaseSmooth(Clamp01((ck - 0.45f) / 0.55f));
                        const ImVec2 b2(b.x + (d.x - b.x) * second,
                            b.y + (d.y - b.y) * second);
                        draw->AddLine(b, b2, col, thick);
                        draw->AddCircleFilled(b, capR, col, 12);
                        draw->AddCircleFilled(b2, capR, col, 12);

                        if (ck > 0.985f) { // traço final contínuo, sem emenda
                            const ImVec2 pts[3] = { a, b, d };
                            draw->AddPolyline(pts, 3, col, 0, thick);
                        }
                    }
                }

                // texto embaixo: fade + sobe um pouquinho
                const float tIn = EaseSmooth(Clamp01((float)((age - 0.9) / 0.4)));
                if (tIn > 0.001f)
                {
                    const char* msg = "Logado Com Sucesso!";
                    const float msz = 17.0f;
                    const ImVec2 ts = font->CalcTextSizeA(msz, FLT_MAX, 0.0f, msg);
                    const float ty = c.y + 34.0f + 26.0f - (1.0f - tIn) * 8.0f;
                    draw->AddText(font, msz, ImVec2(c.x - ts.x * 0.5f, ty),
                        IM_COL32(240, 240, 245, (int)(235.0f * tIn * vis)), msg);
                }
            }
        }
    }

    // ===== arraste (pulando o X, o login e a sidebar) =====
    static bool s_dragging = false;
    const bool overX = ImGui::IsMouseHoveringRect(xMin, xMax);
    const bool overLogin = !s_loggedIn && ImGui::IsMouseHoveringRect(loginMin, loginMax);
    const bool overSide = s_loggedIn && ImGui::IsMouseHoveringRect(sideMin, sideMax);
    if ((!overX && !overLogin && !overSide) || s_dragging) {
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("##drag", ws);
        s_dragging = ImGui::IsItemActive();
        if (s_dragging) {
            static POINT  s_start = {};
            static ImVec2 s_grab(0, 0);
            if (ImGui::IsItemActivated()) {
                POINT m; ::GetCursorPos(&m);
                s_grab = ImVec2((float)m.x, (float)m.y);
                RECT rc; ::GetWindowRect(hwnd, &rc);
                s_start.x = rc.left;
                s_start.y = rc.top;
            }
            POINT m; ::GetCursorPos(&m);
            ::SetWindowPos(hwnd, nullptr,
                s_start.x + (int)((float)m.x - s_grab.x),
                s_start.y + (int)((float)m.y - s_grab.y),
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }
    else {
        s_dragging = false;
    }

    ImGui::End();
}

int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int)
{
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst,
                       nullptr, nullptr, nullptr, nullptr, L"ImGuiPanel", nullptr };
    ::RegisterClassExW(&wc);

    int sx = (::GetSystemMetrics(SM_CXSCREEN) - (int)WIN_W) / 2;
    int sy = (::GetSystemMetrics(SM_CYSCREEN) - (int)WIN_H) / 2;

    HWND hwnd = ::CreateWindowExW(
        0, wc.lpszClassName, L"Panel",
        WS_POPUP,
        sx, sy, (int)WIN_W, (int)WIN_H,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd || !CreateD3D(hwnd)) {
        DestroyD3D();
        if (hwnd) ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    EnableTransparency(hwnd);

    // auto-login em background: CPU+RAM+GPU e IP público têm que bater com
    // o salvo no .dat (arquivo copiado pra outro PC/rede não funciona)
    TryAutoLogin();

    // rate limit do OTP/login do disco: fechar e abrir não zera os contadores
    LoadOtpLock();

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0.0f;
    st.WindowRounding = 0.0f;   // o fundo é desenhado à mão
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    LoadFonts();

    srand((unsigned)::GetTickCount64()); // semente do código OTP (64 bits)

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // alfa 0 = transparente

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW && g_resizeH) {
            ReleaseRTV();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRTV();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI(hwnd);

        if (g_close) done = true;

        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
