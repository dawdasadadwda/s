#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>

#include <cfloat>
#include <cmath>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "fa_solid_900_ttf.h"   // array embutido: fa_solid_900_ttf[] / fa_solid_900_ttf_len
#include "IconsFontAwesome6.h"
#include "logo.h"               // PNG embutido: array aura[] (tamanho via sizeof)

#include <wincodec.h>           // WIC: decodifica o PNG sem stb_image
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib") // GetUserNameA

#ifndef ICON_MAX_16_FA
#define ICON_MAX_16_FA ICON_MAX_FA
#endif

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static ImFont* g_fontIcons = nullptr;
static ImFont* g_fontText = nullptr; // Arial Bold para o rótulo "Unload"

// Logo (PNG embutido em logo.h) decodificada para uma textura D3D11
static ID3D11ShaderResourceView* g_logoSRV = nullptr;
static int g_logoW = 0, g_logoH = 0;
static ImVec2 g_logoUV0(0.0f, 0.0f), g_logoUV1(1.0f, 1.0f); // recorte sem bordas transparentes

static bool g_running = true;

static const float kPanelW = 600.0f;
static const float kPanelH = 320.0f;
static const float kRounding = 14.0f;
static const float kIconSize = 17.0f;         // tamanho do ícone no botão
static const float kIconAtlasSize = 22.0f;    // rasterização da fonte de ícones
static const float kLabelSize = 15.0f;        // tamanho do rótulo "Unload" (Arial Bold)
static const float kLogoSize = 32.0f;         // altura da logo no topo (mude aqui)
static const float kTitlePadX = 14.0f;        // padding da logo/nome na esquerda (eixo X)
static const float kTitleGap = 10.0f;         // espaço entre a logo e o nome

static bool  s_dragging = false;
static POINT s_cursorStart = {};
static RECT  s_windowStart = {};

LRESULT WINAPI WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// FONTES — FontAwesome embutido + Arial Bold do sistema para o rótulo.
// ---------------------------------------------------------------------------
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

    ImFontConfig cfg;
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = kIconAtlasSize;
    cfg.FontDataOwnedByAtlas = false; // dado estático: o atlas não pode dar free

    if (g_fontIcons) return;

    // Fonte de texto: tenta Arial BOLD (arialbd.ttf); se não houver, usa o
    // Arial normal; se nem esse houver, cai na fonte padrão do ImGui.
    char winDir[MAX_PATH] = {};
    if (::GetWindowsDirectoryA(winDir, MAX_PATH)) {
        char fontPath[MAX_PATH];
        wsprintfA(fontPath, "%s\\Fonts\\arialbd.ttf", winDir); // Arial Bold
        g_fontText = io.Fonts->AddFontFromFileTTF(fontPath, kLabelSize, nullptr,
            io.Fonts->GetGlyphRangesDefault());
        if (!g_fontText) {
            wsprintfA(fontPath, "%s\\Fonts\\arial.ttf", winDir); // Arial normal
            g_fontText = io.Fonts->AddFontFromFileTTF(fontPath, kLabelSize, nullptr,
                io.Fonts->GetGlyphRangesDefault());
        }
    }
    if (!g_fontText) {
        ImFontConfig defCfg;
        defCfg.SizePixels = kLabelSize;
        g_fontText = io.Fonts->AddFontDefault(&defCfg);
    }

    // 1) TTF embutido (fa_solid_900_ttf.h)
    g_fontIcons = io.Fonts->AddFontFromMemoryTTF(
        (void*)fa_solid_900_ttf, (int)fa_solid_900_ttf_len,
        kIconAtlasSize, &cfg, iconRanges);

    // 2) Fallback: fa-solid-900.ttf na pasta do EXE (caminho absoluto)
    if (!g_fontIcons) {
        char modPath[MAX_PATH] = {};
        if (::GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
            if (char* slash = strrchr(modPath, '\\')) *slash = '\0';
            char fullPath[MAX_PATH];
            wsprintfA(fullPath, "%s\\fa-solid-900.ttf", modPath);
            g_fontIcons = io.Fonts->AddFontFromFileTTF(fullPath, kIconAtlasSize, &cfg, iconRanges);
        }
    }

    // 3) Nunca deixar o atlas vazio
    if (!g_fontIcons)
        io.Fonts->AddFontDefault();
}

// ---------------------------------------------------------------------------
// LOGO — decodifica o PNG embutido (aura[]) com WIC (codec nativo do Windows,
// sem stb_image) e cria a textura D3D11.
// ---------------------------------------------------------------------------
void LoadLogoTexture() {
    HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInitCom = SUCCEEDED(hrCom); // se o COM já estava ativo, não fazemos uninit

    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (SUCCEEDED(hr)) {
        IWICStream* pStream = nullptr;
        hr = pFactory->CreateStream(&pStream);
        if (SUCCEEDED(hr)) {
            hr = pStream->InitializeFromMemory((BYTE*)aura, (DWORD)sizeof(aura));
            if (SUCCEEDED(hr)) {
                IWICBitmapDecoder* pDecoder = nullptr;
                hr = pFactory->CreateDecoderFromStream(
                    pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder);
                if (SUCCEEDED(hr)) {
                    IWICBitmapFrameDecode* pFrame = nullptr;
                    hr = pDecoder->GetFrame(0, &pFrame);
                    if (SUCCEEDED(hr)) {
                        IWICFormatConverter* pConv = nullptr;
                        hr = pFactory->CreateFormatConverter(&pConv);
                        if (SUCCEEDED(hr)) {
                            hr = pConv->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom);
                            if (SUCCEEDED(hr)) {
                                UINT w = 0, h = 0;
                                pConv->GetSize(&w, &h);
                                std::vector<BYTE> buf((size_t)w * h * 4);
                                hr = pConv->CopyPixels(nullptr, w * 4,
                                    (UINT)buf.size(), buf.data());
                                if (SUCCEEDED(hr)) {
                                    // Auto-crop: muitas logos vêm com margem
                                    // transparente no canvas do PNG; aqui a gente
                                    // acha o retângulo visível e recorta via UV.
                                    {
                                        UINT minX = w, minY = h, maxX = 0, maxY = 0;
                                        for (UINT y = 0; y < h; ++y)
                                            for (UINT x = 0; x < w; ++x)
                                                if (buf[(y * w + x) * 4 + 3] > 8) {
                                                    if (x < minX) minX = x;
                                                    if (x > maxX) maxX = x;
                                                    if (y < minY) minY = y;
                                                    if (y > maxY) maxY = y;
                                                }
                                        if (maxX >= minX && maxY >= minY) {
                                            g_logoUV0 = ImVec2((float)minX / (float)w,
                                                (float)minY / (float)h);
                                            g_logoUV1 = ImVec2((float)(maxX + 1) / (float)w,
                                                (float)(maxY + 1) / (float)h);
                                            g_logoW = (int)(maxX - minX + 1);
                                            g_logoH = (int)(maxY - minY + 1);
                                        }
                                        else {
                                            g_logoUV0 = ImVec2(0.0f, 0.0f);
                                            g_logoUV1 = ImVec2(1.0f, 1.0f);
                                            g_logoW = (int)w;
                                            g_logoH = (int)h;
                                        }
                                    }

                                    // Cadeia de mips gerada NA CPU (box filter 2x2):
                                    // o downscale fica suave, sem serrilhado/moiré,
                                    // sem depender do GenerateMips do driver.
                                    struct Mip { std::vector<BYTE> px; UINT w, h; };
                                    auto m2 = [](UINT a, UINT b) { return a < b ? a : b; };
                                    std::vector<Mip> mips;
                                    mips.push_back(Mip{ buf, w, h });
                                    while (mips.back().w > 1 || mips.back().h > 1) {
                                        const Mip& prev = mips.back();
                                        Mip m;
                                        m.w = prev.w > 1 ? prev.w / 2 : 1;
                                        m.h = prev.h > 1 ? prev.h / 2 : 1;
                                        m.px.resize((size_t)m.w * m.h * 4);
                                        for (UINT y = 0; y < m.h; ++y) {
                                            for (UINT x = 0; x < m.w; ++x) {
                                                const UINT y0 = y * 2, y1 = m2(y * 2 + 1, prev.h - 1);
                                                const UINT x0 = x * 2, x1 = m2(x * 2 + 1, prev.w - 1);
                                                const BYTE* a = &prev.px[(y0 * prev.w + x0) * 4];
                                                const BYTE* b = &prev.px[(y0 * prev.w + x1) * 4];
                                                const BYTE* c = &prev.px[(y1 * prev.w + x0) * 4];
                                                const BYTE* d = &prev.px[(y1 * prev.w + x1) * 4];
                                                BYTE* o = &m.px[(y * m.w + x) * 4];
                                                for (int k = 0; k < 4; ++k)
                                                    o[k] = (BYTE)(((int)a[k] + b[k] + c[k] + d[k] + 2) / 4);
                                            }
                                        }
                                        mips.push_back(std::move(m));
                                    }

                                    D3D11_TEXTURE2D_DESC desc = {};
                                    desc.Width = w;
                                    desc.Height = h;
                                    desc.MipLevels = (UINT)mips.size();
                                    desc.ArraySize = 1;
                                    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                    desc.SampleDesc.Count = 1;
                                    desc.Usage = D3D11_USAGE_DEFAULT;
                                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                                    std::vector<D3D11_SUBRESOURCE_DATA> init(mips.size());
                                    for (size_t i = 0; i < mips.size(); ++i) {
                                        init[i].pSysMem = mips[i].px.data();
                                        init[i].SysMemPitch = mips[i].w * 4;
                                        init[i].SysMemSlicePitch = 0;
                                    }

                                    ID3D11Texture2D* tex = nullptr;
                                    hr = g_pd3dDevice->CreateTexture2D(&desc, init.data(), &tex);
                                    if (SUCCEEDED(hr) && tex) {
                                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                                        srvDesc.Format = desc.Format;
                                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                        srvDesc.Texture2D.MostDetailedMip = 0;
                                        srvDesc.Texture2D.MipLevels = (UINT)mips.size();
                                        hr = g_pd3dDevice->CreateShaderResourceView(
                                            tex, &srvDesc, &g_logoSRV);
                                        tex->Release();
                                    }
                                }
                            }
                            pConv->Release();
                        }
                        pFrame->Release();
                    }
                    pDecoder->Release();
                }
            }
            pStream->Release();
        }
        pFactory->Release();
    }

    if (weInitCom) ::CoUninitialize();
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

    // Antisserrilhamento: tenta MSAA 4x; se o driver recusar, volta para 1x
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
        if (wParam == VK_ESCAPE) { ::PostQuitMessage(0); return 0; } // ESC fecha
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void HandleDrag(HWND hWnd, const ImVec2& p, const ImVec2& q, const ImVec2& bp, const ImVec2& bq) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool overBtn = InRect(mouse, bp, bq);

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

// ---------------------------------------------------------------------------
// Botão Power estilo controle de TV: círculo com símbolo de energia, sempre
// branco. No hover ele desliza para a esquerda (expandindo um "pill") com
// suavidade, mostrando o rótulo "Unload". Tem backlight atrás do botão e
// claridade (glow) no ícone e no nome. Clique fecha o programa.
// ---------------------------------------------------------------------------
static float s_pillAnim = 0.0f; // 0 = círculo, 1 = pill aberto

static float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static float EaseSmooth(float x) { return x * x * (3.0f - 2.0f * x); }

// Texto com leve "claridade": halo BEM sutil ao redor + texto nítido por cima.
// Deslocamento mínimo (0.5px) e alpha baixo para NÃO parecer contorno grosso.
static void DrawTextGlow(ImDrawList* draw, ImFont* font, float size, const ImVec2& pos,
    ImU32 col, const char* text) {
    const int colA = (int)((col >> 24) & 0xFF);
    const int haloA = (colA * 14) / 100; // halo fraco (~14% do alpha do texto)
    const ImU32 halo = (col & 0x00FFFFFF) | ((ImU32)haloA << 24);
    static const float offs[4][2] = {   // só 4 amostras, deslocamento de 0.5px
        { 0.5f, 0.0f }, { -0.5f, 0.0f }, { 0.0f, 0.5f }, { 0.0f, -0.5f }
    };
    for (int i = 0; i < 4; ++i)
        draw->AddText(font, size, ImVec2(pos.x + offs[i][0], pos.y + offs[i][1]), halo, text);
    draw->AddText(font, size, pos, col, text);
}

void DrawPowerButton(ImDrawList* draw, const ImVec2& p, const ImVec2& q,
    ImVec2& exMin, ImVec2& exMax) {
    const float radius = 14.0f;
    const float margin = 10.0f;
    const float cy = p.y + margin + radius;      // centro vertical
    const float xRight = q.x - margin;           // borda direita do pill (fixa)

    // Métricas do rótulo "Unload" (Arial Bold)
    ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();
    const char* label = "Unload";
    const ImVec2 ts = textFont->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label);

    const float pillWCollapsed = radius * 2.0f;                       // só o círculo
    const float pillWExpanded = pillWCollapsed + 8.0f + ts.x + 14.0f; // + rótulo

    // Área de clique usa a largura do frame anterior (evita flicker)
    const float pillWPrev = pillWCollapsed + (pillWExpanded - pillWCollapsed) * EaseSmooth(s_pillAnim);
    const ImVec2 pillMinPrev(xRight - pillWPrev, cy - radius);
    ImGui::SetCursorScreenPos(pillMinPrev);
    ImGui::InvisibleButton("##power", ImVec2(pillWPrev, radius * 2.0f));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;

    if (ImGui::IsItemClicked())
        g_running = false; // power fecha o programa

    // Animação: só um deslizar para a esquerda, com suavidade (smoothstep)
    const float dt = ImGui::GetIO().DeltaTime;
    const float target = hovered ? 1.0f : 0.0f;
    if (s_pillAnim < target) {
        s_pillAnim += dt * 4.0f; // expansão mais lenta
        if (s_pillAnim > 1.0f) s_pillAnim = 1.0f;
    }
    else if (s_pillAnim > target) {
        s_pillAnim -= dt * 8.0f;
        if (s_pillAnim < 0.0f) s_pillAnim = 0.0f;
    }

    const float t = EaseSmooth(s_pillAnim);
    const float pillW = pillWCollapsed + (pillWExpanded - pillWCollapsed) * t;
    const ImVec2 pillMin(xRight - pillW, cy - radius);
    const ImVec2 pillMax(xRight, cy + radius);
    const ImVec2 iconCenter(pillMin.x + radius, cy);

    exMin = pillMin; // área excluída do drag
    exMax = pillMax;

    // --- Sombra simples embaixo do pill ---
    draw->AddRectFilled(ImVec2(pillMin.x, pillMin.y + 1.5f),
        ImVec2(pillMax.x, pillMax.y + 1.5f),
        IM_COL32(0, 0, 0, 120), radius);

    // --- Backlight ATRÁS do botão: camadas translúcidas cada vez menores;
    //     o corpo opaco cobre o centro e sobra só o halo suave ao redor. ---
    {
        const int base = hovered ? 12 : 6; // brilho baixo: halo bem sutil
        for (int i = 3; i >= 1; --i) {
            const float g = (float)i * 2.5f;
            draw->AddRectFilled(ImVec2(pillMin.x - g, pillMin.y - g),
                ImVec2(pillMax.x + g, pillMax.y + g),
                IM_COL32(255, 255, 255, base / i), radius + g);
        }
    }

    // --- Corpo (plástico escuro). Largura 2r + rounding r = círculo perfeito. ---
    const ImU32 face = hovered ? IM_COL32(45, 45, 45, 255) : IM_COL32(30, 30, 30, 255);
    draw->AddRectFilled(pillMin, pillMax, face, radius);

    // --- Borda ---
    const ImU32 rim = hovered ? IM_COL32(255, 255, 255, 140) : IM_COL32(85, 85, 85, 255);
    draw->AddRect(pillMin, pillMax, rim, radius, 0, 1.5f);

    // --- Ícone de energia com claridade (halo branco + glifo nítido) ---
    {
        ImFont* iconFont = g_fontIcons ? g_fontIcons : ImGui::GetFont();
        const float iconSize = g_fontIcons ? kIconSize : ImGui::GetFontSize();
        const ImVec2 its = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, ICON_FA_POWER_OFF);
        const ImVec2 ipos(iconCenter.x - its.x * 0.5f, iconCenter.y - its.y * 0.5f);
        DrawTextGlow(draw, iconFont, iconSize, ipos, IM_COL32(255, 255, 255, 255), ICON_FA_POWER_OFF);
    }

    // --- Rótulo "Unload": posição final FIXA, fade suave no fim do slide ---
    {
        const float labelT = EaseSmooth(Clamp01((s_pillAnim - 0.60f) / 0.40f));
        if (labelT > 0.0f) {
            const float textX = xRight - 14.0f - ts.x;
            const float textY = cy - ts.y * 0.5f;
            draw->PushClipRect(pillMin, pillMax);
            DrawTextGlow(draw, textFont, kLabelSize, ImVec2(textX, textY),
                IM_COL32(255, 255, 255, (int)(255.0f * labelT)), label);
            draw->PopClipRect();
        }
    }

    ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow); // cursor padrão, sempre
}

// ---------------------------------------------------------------------------
// Cards: fundo na MESMA cor do botão Unload.
// ---------------------------------------------------------------------------
static char s_userName[256] = { 0 };
static char s_pcName[256] = { 0 };

// Pega o usuário do Windows e o nome do PC (chamar uma vez, antes do loop).
void FetchSystemInfo() {
    DWORD n = sizeof(s_userName);
    if (!::GetUserNameA(s_userName, &n))
        wsprintfA(s_userName, "-");
    n = sizeof(s_pcName);
    if (!::GetComputerNameA(s_pcName, &n))
        wsprintfA(s_pcName, "-");
}

// Fundo + borda de card (mesma cor do corpo do botão Unload).
void DrawCardBase(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx) {
    const float rounding = 14.0f;
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), rounding);
    draw->AddRect(mn, mx, IM_COL32(85, 85, 85, 80), rounding, 0, 1.0f);
}

// Linha de info: ícone à esquerda, rótulo apagado em cima, valor branco embaixo.
void DrawInfoRow(ImDrawList* draw, ImFont* font, float x, float& y,
    const char* icon, const char* label, const char* value) {
    ImFont* iconFont = g_fontIcons ? g_fontIcons : ImGui::GetFont();
    const float iconSize = 16.0f;
    const float textX = x + iconSize + 10.0f;

    const float labelH = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label).y;
    const float valueH = font->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, value).y;
    const float rowH = labelH + 2.0f + valueH;

    // Ícone centralizado na altura da linha
    const ImVec2 its = iconFont->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
    draw->AddText(iconFont, iconSize,
        ImVec2(x, y + (rowH - its.y) * 0.5f),
        IM_COL32(255, 255, 255, 200), icon);

    // Rótulo (apagado) e valor (branco)
    draw->AddText(font, kLabelSize, ImVec2(textX, y), IM_COL32(255, 255, 255, 110), label);
    draw->AddText(font, kLabelSize, ImVec2(textX, y + labelH + 2.0f),
        IM_COL32(255, 255, 255, 240), value);

    y += rowH + 12.0f;
}

// ---------------------------------------------------------------------------
// Header animado yin-yang (fundo branco + onda S alternando lados a cada
// 1.5s) com o label tendo a cor "varrida" pela divisória. Usado pelos dois
// cards. Retorna o Y da divisória (logo abaixo do header).
// ---------------------------------------------------------------------------
float DrawAnimatedHeader(ImDrawList* draw, ImFont* textFont,
    const ImVec2& mn, const ImVec2& mx, const char* label,
    float gx, float flip, float amp, float side) {
    const float padX = 16.0f;
    const float rounding = 14.0f;
    const float headerH = 28.0f;
    const ImU32 headBg = IM_COL32(255, 255, 255, 255);
    draw->AddRectFilled(mn, ImVec2(mx.x, mn.y + headerH), headBg, rounding);
    draw->AddRectFilled(ImVec2(mn.x, mn.y + headerH - rounding),
        ImVec2(mx.x, mn.y + headerH), headBg); // cantos de baixo retos

    // gx = posição GLOBAL da fronteira (sincronizada entre os dois cards)
    const float bx = gx;

    for (int i = 0; i < (int)headerH; ++i) {
        const float v = ((float)i + 0.5f) / headerH; // 0..1 na altura
        const float xx = bx + flip * sinf(v * 6.2831853f) * amp;
        if (side > 0.0f) {
            // preto à ESQUERDA da fronteira
            draw->AddRectFilled(ImVec2(mn.x - 8.0f, mn.y + (float)i),
                ImVec2(xx, mn.y + (float)i + 1.0f),
                IM_COL32(0, 0, 0, 255));
        }
        else {
            // preto à DIREITA da fronteira
            draw->AddRectFilled(ImVec2(xx, mn.y + (float)i),
                ImVec2(mx.x + 8.0f, mn.y + (float)i + 1.0f),
                IM_COL32(0, 0, 0, 255));
        }
    }

    // Label com a cor acompanhando a divisória (2 desenhos com clip)
    {
        const ImVec2 ts = textFont->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label);
        const ImVec2 tpos(mn.x + padX, mn.y + (headerH - ts.y) * 0.5f);
        const float vT = ((tpos.y + ts.y * 0.5f) - mn.y) / headerH;
        const float bxT = bx + flip * sinf(vT * 6.2831853f) * amp;

        if (side > 0.0f) {
            // preto à esquerda: letras brancas até a fronteira, pretas depois
            draw->PushClipRect(ImVec2(mn.x, mn.y), ImVec2(bxT, mn.y + headerH));
            draw->AddText(textFont, kLabelSize, tpos,
                IM_COL32(255, 255, 255, 255), label);
            draw->PopClipRect();
            draw->PushClipRect(ImVec2(bxT, mn.y), ImVec2(mx.x, mn.y + headerH));
            draw->AddText(textFont, kLabelSize, tpos,
                IM_COL32(0, 0, 0, 255), label);
            draw->PopClipRect();
        }
        else {
            // preto à direita: letras pretas até a fronteira, brancas depois
            draw->PushClipRect(ImVec2(mn.x, mn.y), ImVec2(bxT, mn.y + headerH));
            draw->AddText(textFont, kLabelSize, tpos,
                IM_COL32(0, 0, 0, 255), label);
            draw->PopClipRect();
            draw->PushClipRect(ImVec2(bxT, mn.y), ImVec2(mx.x, mn.y + headerH));
            draw->AddText(textFont, kLabelSize, tpos,
                IM_COL32(255, 255, 255, 255), label);
            draw->PopClipRect();
        }
    }

    // Divisória: traço cobrindo TODO o eixo X do card
    const float divY = mn.y + headerH;
    draw->AddLine(ImVec2(mn.x, divY), ImVec2(mx.x, divY), IM_COL32(0, 0, 0, 180), 1.0f);
    return divY;
}

// Card esquerdo: header "INFOS" + usuário, nome do PC, plano e data/hora.
void DrawInfosCard(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    float gx, float flip, float amp, float side) {
    const float padX = 16.0f;
    const float rounding = 14.0f;
    ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();

    // Corpo do card (mesma cor do botão Unload)
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), rounding);

    float y = DrawAnimatedHeader(draw, textFont, mn, mx, "INFOS",
        gx, flip, amp, side) + 12.0f;

    // Infos (ícone + rótulo + valor)
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_USER, "Usuario", s_userName);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_DESKTOP, "Nome do PC", s_pcName);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_INFINITY, "Plan Type", "Life Time");

    // Data e hora atuais (atualiza todo frame)
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    char dtBuf[64];
    wsprintfA(dtBuf, "%02d/%02d/%04d %02d:%02d",
        st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
    DrawInfoRow(draw, textFont, mn.x + padX, y, ICON_FA_CLOCK, "Data e hora", dtBuf);

    // Label com ícone de "i": embaixo de "Data e hora", CENTRADO no eixo X do card
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
        const float ly = y + 16.0f; // posição aprovada do label
        draw->AddText(iconFont, iconSize,
            ImVec2(cx, ly + (ms.y - its.y) * 0.5f),
            IM_COL32(255, 255, 255, 200), ICON_FA_CIRCLE_INFO);
        draw->AddText(textFont, msgSize, ImVec2(cx + its.x + gap, ly),
            IM_COL32(255, 255, 255, 200), msg);
    }

    // Borda do card por cima de tudo (inclusive do header branco)
    draw->AddRect(mn, mx, IM_COL32(85, 85, 85, 80), rounding, 0, 1.0f);
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

    // Título + logo à esquerda: no TOPO, fora do card, alinhado ao card esquerdo
    {
        ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();
        const char* title = "Aura Bypass";
        const ImVec2 ts = textFont->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, title);

        const float x0 = p.x + kTitlePadX;
        const float logoH = kLogoSize;
        const float topY = p.y + (48.0f - logoH) * 0.5f; // centraliza na faixa do topo (Y)
        float textX = x0;
        if (g_logoSRV) {
            // proporção do recorte visível (sem as bordas transparentes)
            const float logoW = logoH * ((float)g_logoW / (float)g_logoH);
            draw->AddImage((ImTextureID)g_logoSRV,
                ImVec2(x0, topY), ImVec2(x0 + logoW, topY + logoH),
                g_logoUV0, g_logoUV1);
            textX = x0 + logoW + kTitleGap;
        }
        draw->AddText(textFont, kLabelSize,
            ImVec2(textX, topY + (logoH - ts.y) * 0.5f),
            IM_COL32(255, 255, 255, 255), title);
    }

    // Dois cards na mesma cor do botão Unload
    {
        const float cardW = 275.0f, cardH = 256.0f, gap = 18.0f;
        const float x0 = p.x + (kPanelW - (cardW * 2.0f + gap)) * 0.5f;
        const float y0 = p.y + 48.0f;

        // Varredura GLOBAL: a fronteira nasce na esquerda do header esquerdo,
        // atravessa o vão e morre na direita do header direito — os dois
        // headers dançam como uma coisa só.
        const float t = (float)ImGui::GetTime();
        const float ph = fmodf(t, 3.0f);
        const float sw = (ph < 1.5f) ? EaseSmooth(ph / 1.5f)
            : 1.0f - EaseSmooth((ph - 1.5f) / 1.5f);
        // Sincronia ESPELHADA: o preto nasce no vão entre os cards e flui
        // pra fora — pra ESQUERDA do header esquerdo e pra DIREITA do header
        // direito (e volta). Cada header cuida da sua onda; nunca troca de
        // cor entre um card e outro.
        const float M = 12.0f; // margem p/ curva S sair inteira nos extremos
        const float lx1 = x0 + cardW;                 // direita do card esquerdo
        const float rx0 = x0 + cardW + gap;           // esquerda do card direito
        const float rx1 = x0 + cardW * 2.0f + gap;    // direita do card direito
        const float gxL = (lx1 + M) - sw * ((lx1 + M) - (x0 - M));
        const float gxR = (rx0 - M) + sw * ((rx1 + M) - (rx0 - M));

        // Virada suave do S nos limites + amplitude "respirando"
        const float TR = 0.45f;
        float flip;
        if (ph < 1.5f - TR * 0.5f)
            flip = 1.0f;
        else if (ph < 1.5f + TR * 0.5f)
            flip = 1.0f - 2.0f * EaseSmooth((ph - (1.5f - TR * 0.5f)) / TR);
        else if (ph < 3.0f - TR * 0.5f)
            flip = -1.0f;
        else
            flip = -1.0f + 2.0f * EaseSmooth((ph - (3.0f - TR * 0.5f)) / TR);
        const float amp = 6.0f + 2.0f * sinf(t * 2.0f);

        // Card esquerdo: INFOS (preto flui da direita p/ a esquerda)
        DrawInfosCard(draw, ImVec2(x0, y0), ImVec2(x0 + cardW, y0 + cardH),
            gxL, -flip, amp, -1.0f);

        // Card direito: header animado "FEATURES" (preto flui da esquerda p/ a direita)
        {
            const ImVec2 rmn(x0 + cardW + gap, y0);
            const ImVec2 rmx(x0 + cardW * 2.0f + gap, y0 + cardH);
            DrawCardBase(draw, rmn, rmx);
            DrawAnimatedHeader(draw, g_fontText ? g_fontText : ImGui::GetFont(),
                rmn, rmx, "FEATURES", gxR, flip, amp, 1.0f);
            // borda por cima do header, igual ao card esquerdo
            draw->AddRect(rmn, rmx, IM_COL32(85, 85, 85, 80), kRounding, 0, 1.0f);
        }
    }

    // Botão power / "Unload" no canto superior direito
    ImVec2 exMin, exMax;
    DrawPowerButton(draw, p, q, exMin, exMax);

    HandleDrag(hWnd, p, q, exMin, exMax);

    ImGui::End();
}

void EnableTransparency(HWND hWnd) {
    MARGINS margins = { -1, -1, -1, -1 };
    ::DwmExtendFrameIntoClientArea(hWnd, &margins);
}

// ---------------------------------------------------------------------------
// Entry point do EXE — tudo roda na thread principal (sem DllMain/thread).
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                       hInstance, nullptr, nullptr, nullptr,
                       nullptr, L"bipasClass", nullptr };
    ::RegisterClassExW(&wc);

    // Centraliza na tela
    const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
    const int screenH = ::GetSystemMetrics(SM_CYSCREEN);
    const int x0 = (screenW - (int)kPanelW) / 2;
    const int y0 = (screenH - (int)kPanelH) / 2;

    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"bipas",
        WS_POPUP, x0, y0, (int)kPanelW, (int)kPanelH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    EnableTransparency(hwnd);

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

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
    LoadLogoTexture();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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

    if (g_logoSRV) { g_logoSRV->Release(); g_logoSRV = nullptr; }
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
