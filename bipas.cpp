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
// Botão de INJECT do card direito — é o ÚNICO controle do card:
//
//              [  Inject Hide  |  (o->)  ]
//                   245 x 40 px
//
// O botão tem DUAS zonas de clique:
//   - SÍMBOLO (faixa da direita): ALTERNA a seleção entre "Inject Hide" e
//     "Inject Normal" (o rótulo velho sai por cima, o novo entra por baixo, e
//     o símbolo dá meia volta como feedback).
//   - RESTO do botão: DISPARA o inject do modo selecionado, abre o MENU de
//     progresso (spinner + etapa) e trava o botão por 4s mostrando
//     "Inject Hide..." / "Inject Normal...".
//
// Hover: um CÍRCULO branco nasce no CENTRO do botão e cresce até cobrir tudo
// (nos dois eixos, voltando ao centro quando o mouse sai); o rótulo e o
// símbolo invertem de cor conforme o branco passa por baixo.
// ---------------------------------------------------------------------------
static int   s_injectMode = 0;      // 0 = "Inject Hide" | 1 = "Inject Normal"
static int   s_injectPrev = 0;      // rótulo que está saindo
static float s_injectAnim = 1.0f;   // 0..1 na troca (1 = parado, sem animação)
static const char* kInjectLabels[2] = { "Inject Hide", "Inject Normal" };
static float s_injectFill = 0.0f;   // cortina do hover: 0 = vazio | 1 = coberto
static float s_injectBusy = 0.0f;   // segundos restantes de trava (0 = livre)
static int   s_injectingMode = 0;   // modo que está sendo injetado agora

// Menu de progresso (abre no clique de disparo e dura o mesmo tempo da trava)
static const float kInjectWorkTime = 4.0f;   // duração (s): menu = trava do botão
static bool  s_menuOpen = false;
static float s_menuT = 0.0f;                 // tempo desde a abertura (s)

// Maior tamanho de fonte (<= maxSize) que faz o texto caber na largura dada.
static float FitTextSize(ImFont* font, const char* text, float maxSize, float availW) {
    const float w = font->CalcTextSizeA(maxSize, FLT_MAX, 0.0f, text).x;
    if (w <= availW || w <= 0.0f) return maxSize;
    return maxSize * (availW / w);
}

// ---------------------------------------------------------------------------
// Inversão de cor pela cortina CIRCULAR: o ImGui só tem clip retangular, então
// o miolo claro/círculo é aproximado por FAIXAS de 1px de altura (mesma técnica
// da máscara dos cantos do header). `paint(cor)` desenha o conteúdo.
//   1) pinta tudo na cor clara (fora do círculo)
//   2) repinta na cor escura, faixa por faixa, dentro do círculo
// ---------------------------------------------------------------------------
template <typename PaintFn>
static void RevealByCircle(ImDrawList* draw, const ImVec2& c, float radius,
    const ImVec2& bMin, const ImVec2& bMax, PaintFn paint) {
    draw->PushClipRect(bMin, bMax, true);
    paint(IM_COL32(255, 255, 255, 255));
    draw->PopClipRect();

    if (radius <= 0.5f) return;
    const int y0 = (int)floorf(fmaxf(bMin.y, c.y - radius));
    const int y1 = (int)ceilf(fminf(bMax.y, c.y + radius));
    for (int y = y0; y < y1; ++y) {
        const float dy = ((float)y + 0.5f) - c.y;
        const float r2 = radius * radius - dy * dy;
        if (r2 <= 0.0f) continue;
        const float dx = sqrtf(r2);
        const float x0 = fmaxf(c.x - dx, bMin.x);
        const float x1 = fminf(c.x + dx, bMax.x);
        if (x1 <= x0) continue;
        draw->PushClipRect(ImVec2(x0, (float)y), ImVec2(x1, (float)y + 1.0f), true);
        paint(IM_COL32(12, 12, 14, 255));
        draw->PopClipRect();
    }
}

// ---------------------------------------------------------------------------
// Spinner do menu: um arco de 270° girando.
// ---------------------------------------------------------------------------
static void DrawSpinner(ImDrawList* draw, const ImVec2& c, float r, float phase, ImU32 col) {
    const int   seg = 28;
    const float kSpan = 4.712389f;          // 270°
    ImVec2 pts[seg + 1];
    for (int i = 0; i <= seg; ++i) {
        const float a = phase + kSpan * ((float)i / (float)seg);
        pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
    }
    draw->AddPolyline(pts, seg + 1, col, 0, 2.0f);
}

// ---------------------------------------------------------------------------
// Símbolo de ALTERNAR: dois arcos opostos, cada um com uma ponta de seta no
// fim. Desenhado à mão (arcos amostrados + triângulos) em vez de usar um glifo
// de fonte: fica nítido nesse tamanho e gira no próprio centro.
// rot = rotação em radianos (o símbolo tem simetria de 180°, então terminar em
// PI deixa o desenho idêntico ao inicial — nada de "pulo" no fim).
// ---------------------------------------------------------------------------
static void DrawSwapIcon(ImDrawList* draw, const ImVec2& c, float r, ImU32 col, float rot) {
    const float kSpan = 112.0f * 0.0174533f;   // abertura de cada arco (rad)
    // 180-112 = 68° de vão entre as duas partes
    const float kHead = 2.6f;                  // tamanho da ponta de seta
    const float kThick = 1.5f;                 // espessura do arco
    const int   kSeg = 12;                     // segmentos por arco

    for (int k = 0; k < 2; ++k) {
        const float a0 = rot + (float)k * 3.14159265f;

        ImVec2 pts[kSeg + 1];
        for (int i = 0; i <= kSeg; ++i) {
            const float a = a0 + kSpan * ((float)i / (float)kSeg);
            pts[i] = ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r);
        }
        draw->AddPolyline(pts, kSeg + 1, col, 0, kThick);

        // Ponta de seta no fim do arco, seguindo a tangente do movimento
        const float a1 = a0 + kSpan;
        const ImVec2 p(c.x + cosf(a1) * r, c.y + sinf(a1) * r);
        const ImVec2 tg(-sinf(a1), cosf(a1));   // direção do movimento
        const ImVec2 nr(cosf(a1), sinf(a1));    // normal (radial)
        const ImVec2 apex(p.x + tg.x * kHead * 1.15f, p.y + tg.y * kHead * 1.15f);
        const ImVec2 b1(p.x + nr.x * kHead * 0.62f, p.y + nr.y * kHead * 0.62f);
        const ImVec2 b2(p.x - nr.x * kHead * 0.62f, p.y - nr.y * kHead * 0.62f);
        draw->AddTriangleFilled(apex, b1, b2, col);
    }
}

// ---------------------------------------------------------------------------
// Cortina do hover: círculo branco que nasce no CENTRO do controle e cresce
// (nos dois eixos) até cobrir o botão inteiro — inclusive os cantos, por isso o
// raio final é a MEIA-DIAGONAL do retângulo. Devolve o raio atual, que é o que
// o rótulo e o símbolo usam para inverter de cor.
// ---------------------------------------------------------------------------
static float CortinaCircle(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    float round, float& fill, float fillTime, bool active, bool pressed) {
    const float dt = ImGui::GetIO().DeltaTime;
    fill = Clamp01(fill + (active ? 1.0f : -1.0f) * dt / fillTime);
    const float t = EaseSmooth(fill);

    if (t > 0.01f) {                       // halo: 3 camadas por fora
        for (int i = 3; i >= 1; --i) {
            const float g = (float)i * 1.7f;
            draw->AddRectFilled(ImVec2(mn.x - g, mn.y - g), ImVec2(mx.x + g, mx.y + g),
                IM_COL32(255, 255, 255, (int)(t * 40.0f / (float)i)), round + g);
        }
    }

    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), round);   // base escura

    const float W = mx.x - mn.x, H = mx.y - mn.y;
    const float cxm = mn.x + W * 0.5f;     // centro do controle
    const float cym = mn.y + H * 0.5f;
    const float halfDiag = sqrtf((W * 0.5f) * (W * 0.5f) + (H * 0.5f) * (H * 0.5f)) + 1.0f;
    const float radius = halfDiag * t;     // NASCE no centro e abre nos 2 eixos

    if (radius > 0.5f) {
        // O círculo é recortado pelo retângulo do controle: os cantos redondos
        // continuam certos e o branco não vaza pra fora do botão.
        draw->PushClipRect(mn, mx, true);
        draw->AddCircleFilled(ImVec2(cxm, cym), radius,
            pressed ? IM_COL32(235, 235, 235, 255) : IM_COL32(255, 255, 255, 255), 64);
        if (t < 0.985f)                    // anel de luz na borda que avança
            draw->AddCircle(ImVec2(cxm, cym), radius, IM_COL32(255, 255, 255, 130), 64, 1.5f);
        draw->PopClipRect();
    }

    draw->AddRect(mn, mx, IM_COL32(255, 255, 255, (int)(70 + 175 * t)), round, 0, 1.0f);
    return radius;
}

void DrawInjectControl(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    ImVec2& outMin, ImVec2& outMax) {
    const float kRound = 6.0f;         // canto redondo
    const float kFillTime = 0.22f;     // tempo do círculo cobrir tudo (s)
    const float kSize = 17.0f;         // rótulo (o auto-fit reduz se não couber)
    const float kIconZone = 20.0f;     // faixa reservada p/ o símbolo (à direita)
    const float kSwapTime = 0.26f;     // duração da troca de rótulo (s)
    const float kBlockTime = kInjectWorkTime;   // trava depois de disparar (s)
    const float kSpinTime = 1.8f;      // volta completa do símbolo enquanto injeta (s)

    outMin = mn;                       // área do controle (p/ excluir do drag)
    outMax = mx;

    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();

    // Trava em andamento? (desconta o tempo; 0 = livre)
    if (s_injectBusy > 0.0f)
        s_injectBusy = fmaxf(0.0f, s_injectBusy - ImGui::GetIO().DeltaTime);
    const bool busy = s_injectBusy > 0.0f;

    ImGui::SetCursorScreenPos(mn);
    ImGui::InvisibleButton("##inject", ImVec2(mx.x - mn.x, mx.y - mn.y));
    const bool hovered = ImGui::IsItemHovered() && !s_dragging;
    const bool pressed = hovered && !busy && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    // Zonas: a faixa da direita (kIconZone) é o SÍMBOLO = alterna a seleção;
    // todo o resto do botão é o DISPARO do inject.
    const bool overIcon = ImGui::GetIO().MousePos.x >= (mx.x - kIconZone);

    // >>> CLIQUE — bloqueado enquanto s_injectBusy estiver rodando
    if (ImGui::IsItemClicked() && !busy) {
        if (overIcon) {
            // --- símbolo: ALTERNA a seleção (Hide <-> Normal) ---
            s_injectPrev = s_injectMode;
            s_injectMode = 1 - s_injectMode;
            s_injectAnim = 0.0f;
        }
        else {
            // --- resto do botão: DISPARA o inject do modo selecionado ---
            s_injectingMode = s_injectMode;   // congela o modo escolhido
            s_injectBusy = kBlockTime;        // trava o botão por 4s
            s_menuOpen = true;                // e abre o menu de progresso
            s_menuT = 0.0f;
            // TODO: dispara o inject de verdade aqui — use s_injectingMode
            //       (0 = "Inject Hide", 1 = "Inject Normal") p/ saber qual modo.
        }
    }

    // Enquanto injeta, o controle fica COBERTO: é o estado "trabalhando".
    const float radius = CortinaCircle(draw, mn, mx, kRound, s_injectFill,
        kFillTime, hovered || busy, pressed);
    const ImVec2 cc(mn.x + (mx.x - mn.x) * 0.5f, mn.y + (mx.y - mn.y) * 0.5f);

    // Progresso da troca: 1 = parado (sem animação pendente)
    if (s_injectAnim < 1.0f)
        s_injectAnim = Clamp01(s_injectAnim + ImGui::GetIO().DeltaTime / kSwapTime);
    const float ap = EaseSmooth(s_injectAnim);

    const float H = mx.y - mn.y;
    const ImVec2 tMin(mn.x + 10.0f, mn.y);
    const ImVec2 tMax(mx.x - kIconZone, mx.y);     // texto não invade o símbolo

    // Rótulo com auto-ajuste: usa kSize, mas encolhe o mínimo necessário para
    // caber na faixa de texto. Os DOIS rótulos usam o mesmo tamanho, o menor
    // dos dois, para a troca animada não ficar com fontes diferentes.
    const float availW = tMax.x - tMin.x;
    const float parkedSize = fminf(FitTextSize(font, kInjectLabels[0], kSize, availW),
        FitTextSize(font, kInjectLabels[1], kSize, availW));
    auto drawFitted = [&](const char* txt, float maxSize, float dy) {
        const float size = FitTextSize(font, txt, maxSize, availW);
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
        const ImVec2 tp(tMin.x, mn.y + (H - ts.y) * 0.5f + dy);
        const ImVec2 bMin(tp.x - 1.0f, tp.y - 1.0f);
        const ImVec2 bMax(tp.x + ts.x + 1.0f, tp.y + ts.y + 1.0f);
        RevealByCircle(draw, cc, radius, bMin, bMax, [&](ImU32 col) {
            draw->AddText(font, size, tp, col, txt);
            });
        };

    draw->PushClipRect(tMin, tMax, true);
    if (busy) {
        // Estado "injetando": mostra o modo escolhido pelo tempo da trava
        char buf[64];
        wsprintfA(buf, "%s...", kInjectLabels[s_injectingMode]);
        drawFitted(buf, kSize, 0.0f);
    }
    else {
        // Troca normal: o que está saindo sobe e some; o novo sobe vindo de baixo.
        if (s_injectAnim < 1.0f) drawFitted(kInjectLabels[s_injectPrev], parkedSize, -ap * H);
        drawFitted(kInjectLabels[s_injectMode], parkedSize, (1.0f - ap) * H);
    }
    draw->PopClipRect();

    // Símbolo de alternar, à direita.
    //   - na troca: meia volta (cai no mesmo desenho, por causa da simetria 180°)
    //   - injetando: gira sem parar, dando o feedback de "trabalhando"
    const ImVec2 ic(mx.x - 15.5f, mn.y + H * 0.5f);
    const float rot = busy
        ? ((kBlockTime - s_injectBusy) / kSpinTime) * 6.2831853f
        : ap * 3.14159265f;
    const ImVec2 iMin(ic.x - 7.0f, ic.y - 7.0f), iMax(ic.x + 7.0f, ic.y + 7.0f);
    RevealByCircle(draw, cc, radius, iMin, iMax, [&](ImU32 col) {
        DrawSwapIcon(draw, ic, 5.0f, col, rot);
        });
}

// ---------------------------------------------------------------------------
// MENU de progresso do inject (abre no clique de disparo). Mostra um spinner
// girando e, embaixo, a etapa atual:
//   0..2s  -> "Downloading Modules..."
//   2..4s  -> "Injecting..."
// A duração é a MESMA da trava do botão (kInjectWorkTime = 4s).
// ---------------------------------------------------------------------------
void DrawInjectMenu(ImDrawList* draw, const ImVec2& p, const ImVec2& q) {
    if (!s_menuOpen) return;

    s_menuT += ImGui::GetIO().DeltaTime;
    if (s_menuT >= kInjectWorkTime) {          // terminou: fecha o menu
        s_menuOpen = false;
        return;
    }
    const float t = s_menuT;
    const float fade = fminf(1.0f, fminf(t * 8.0f, (kInjectWorkTime - t) * 8.0f));

    // Escurece o painel inteiro para dar foco ao menu
    draw->AddRectFilled(p, q, IM_COL32(0, 0, 0, (int)(120.0f * fade)), kRounding);

    // Card centralizado
    const float w = 236.0f, h = 96.0f;
    const ImVec2 c((p.x + q.x) * 0.5f, (p.y + q.y) * 0.5f);
    const ImVec2 mn(c.x - w * 0.5f, c.y - h * 0.5f);
    const ImVec2 mx(c.x + w * 0.5f, c.y + h * 0.5f);
    const float round = 12.0f;
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, (int)(255.0f * fade)), round);
    draw->AddRect(mn, mx, IM_COL32(120, 120, 120, (int)(140.0f * fade)), round, 0, 1.0f);

    // Spinner girando
    DrawSpinner(draw, ImVec2(c.x, mn.y + 34.0f), 13.0f, t * 5.2f,
        IM_COL32(255, 255, 255, (int)(240.0f * fade)));

    // Etapa embaixo
    ImFont* font = g_fontText ? g_fontText : ImGui::GetFont();
    const char* txt = (t < kInjectWorkTime * 0.5f) ? "Downloading Modules..." : "Injecting...";
    const float size = 14.0f;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, txt);
    draw->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, mn.y + 60.0f),
        IM_COL32(255, 255, 255, (int)(245.0f * fade)), txt);
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
// HEADER COM FUMAÇA — o fundo do header é PRETO e a FUMAÇA BRANCA passeia por
// cima, envolvendo-o: névoa larga (volume) + fita ondulada (corpo) + volutas
// cruzando no sentido oposto (turbulência) + bolhas que sobem + fumaça
// lambendo as bordas + faíscas à deriva.
//
// O ImGui não tem gradiente radial, então cada "baforada" é um retângulo com
// 4 rampas de alfa nas bordas e o miolo recuado — NUNCA um quadrado opaco.
//
// >>> ATENÇÃO (era o bug do "card piscando"): a cor passada para
//     AddSmokeBlob tem que ter ALFA = 0 (ex.: IM_COL32(234,239,245,0)).
//     A função descarta o alfa recebido e monta o alfa POR BLOB
//     (rgb | alfa<<24). Se o RGB chegar com alfa 255, o OR mantém 255 e
//     cada blob vira um retângulo 100% OPACO — foi isso que deixou os cards
//     "piscando" cheios de quadrados brancos e criou um "fundo" atrás de
//     INFOS/FEATURES.
// ---------------------------------------------------------------------------
static inline float Hash01(int i) {
    const float s = sinf((float)i * 12.9898f) * 43758.5453f;
    return s - floorf(s);                       // 0..1 determinístico
}

static inline float WrapRange(float x, float lo, float hi) {
    const float span = hi - lo;
    float m = fmodf(x - lo, span);
    if (m < 0.0f) m += span;
    return lo + m;
}

// Baforada: miolo (alfa cheio) + 4 rampas (alfa 0 -> cheio).
// A rampa é proporcional ao raio: blob grande = borda bem macia; blob pequeno
// = as rampas se encontram no centro (perfil de "tenda", sem miolo duro).
static void AddSmokeBlob(ImDrawList* draw, ImVec2 c, float r, ImU32 rgb,
    float alpha) {
    if (alpha <= 0.004f || r <= 0.5f) return;

    rgb &= 0x00FFFFFFu;                       // RGB puro (ver aviso no topo)
    const ImU32 aFull = rgb | (((ImU32)(255.0f * alpha)) << 24);

    float fade = fmaxf(1.5f, r * 0.42f);
    float core = r - fade;
    if (core < 0.5f) core = 0.5f;   // garante miolo: sem buraco e sem virar "cruz"
    fade = r - core;                // a rampa se ajusta ao miolo garantido

    if (core > 0.0f)                          // miolo sólido (recuado)
        draw->AddRectFilled(ImVec2(c.x - core, c.y - core),
            ImVec2(c.x + core, c.y + core), aFull);

    const float x0 = c.x - r, x1 = c.x + r, y0 = c.y - r, y1 = c.y + r;
    const float fx0 = x0 + fade, fy0 = y0 + fade;
    const float fx1 = x1 - fade, fy1 = y1 - fade;
    draw->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(fx0, y1),
        rgb, aFull, aFull, rgb);              // rampa ESQUERDA
    draw->AddRectFilledMultiColor(ImVec2(fx1, y0), ImVec2(x1, y1),
        aFull, rgb, rgb, aFull);              // rampa DIREITA
    draw->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, fy0),
        rgb, rgb, aFull, aFull);              // rampa de CIMA
    draw->AddRectFilledMultiColor(ImVec2(x0, fy1), ImVec2(x1, y1),
        aFull, aFull, rgb, rgb);              // rampa de BAIXO
}

// O ImGui só tem clip RETANGULAR, então a fumaça (que é reta) passaria por
// cima das curvas de cima do header e deixaria as pontas "quadradas/pontudas".
// Aqui a gente devolve a curva: cobre o bico (quadrado menos arco) com a cor
// do fundo do painel, faixa por faixa. Como o próprio header é quase preto,
// a cobertura pode sobrar 1px pra dentro sem nenhum efeito visual.
static void MaskTopCorners(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx,
    float rounding, ImU32 bg) {
    const float R = rounding + 1.0f;                 // 1px de folga: nada escapa
    for (int i = 0; i <= (int)rounding; ++i) {
        const float dy = R - ((float)i + 0.5f);
        const float dx = sqrtf(fmaxf(0.0f, R * R - dy * dy));
        const float w = R - dx;                      // largura a cobrir nesta linha
        if (w <= 0.05f) continue;
        const float y0 = mn.y + (float)i;
        draw->AddRectFilled(ImVec2(mn.x, y0), ImVec2(mn.x + w, y0 + 1.0f), bg);
        draw->AddRectFilled(ImVec2(mx.x - w, y0), ImVec2(mx.x, y0 + 1.0f), bg);
    }
}

// Parâmetros da fumaça (mexa aqui para calibrar; nada de número solto no meio)
struct SmokeCfg {
    float kOmega = 2.4f;      // velocidade da ondulação
    float kTide = 7.0f;       // segundos entre uma inversão e outra

    int   nebN = 6;           // névoa de fundo
    float nebR0 = 17.0f, nebR1 = 38.0f, nebA0 = 0.020f, nebA1 = 0.038f;

    float ropeStep = 2.0f;    // corpo (fita ondulada)
    float ropeLam = 82.0f;    // comprimento de onda (px)
    float ropeR0 = 3.0f, ropeR1 = 6.5f, ropeA0 = 0.030f, ropeA1 = 0.065f;

    float haloStep = 5.0f, haloR = 11.0f, haloA = 0.013f;   // aura

    float volStep = 2.5f, volLam = 46.0f;                    // volutas
    float volR0 = 2.0f, volR1 = 4.2f, volA0 = 0.016f, volA1 = 0.048f;

    int   puffN = 6;          // bolhas que sobem
    float puffR0 = 6.0f, puffR1 = 13.0f, puffA0 = 0.030f, puffA1 = 0.060f;

    float edgeStep = 3.0f, edgeR = 7.0f;                     // bordas
    float edgeA0 = 0.014f, edgeA1 = 0.048f;

    int   sparkN = 10;        // faíscas (poeira de luz, discreta)
    float sparkR0 = 1.4f, sparkR1 = 2.6f, sparkA0 = 0.06f, sparkA1 = 0.14f;
};

// Desenha o header animado do card. O relógio vem só de ImGui::GetTime() e
// tudo é amostrado em coordenadas ABSOLUTAS de tela, então os dois headers
// mostram exatamente a MESMA fumaça (uma peça só, mesmo com o vão no meio).
// Retorna o Y da divisória (logo abaixo do header).
float DrawAnimatedHeader(ImDrawList* draw, ImFont* textFont,
    const ImVec2& mn, const ImVec2& mx, const char* label) {
    const float padX = 16.0f;
    const float rounding = 14.0f;
    const float headerH = 30.0f;
    const ImVec2 hmax(mx.x, mn.y + headerH);
    const ImU32 coreCol = IM_COL32(12, 12, 14, 255);   // base OPACA do header
    const ImU32 smoke = IM_COL32(234, 239, 245, 0);    // fumaça: RGB puro (alfa 0)
    static const SmokeCfg C;

    // Base PRETA — é ela que a fumaça envolve.
    draw->AddRectFilled(mn, hmax, coreCol, rounding);
    draw->AddRectFilled(ImVec2(mn.x, hmax.y - rounding), hmax, coreCol);

    const float t = (float)ImGui::GetTime();
    const float W = mx.x - mn.x;
    const float cy = mn.y + headerH * 0.5f;
    const float kTau = 6.2831853f;

    // Relógio GLOBAL: a "maré" integra uma frequência que vai e volta, então
    // phaseX = deslocamento da serpentina e flow = deriva dos pontos soltos.
    const float tideW = kTau / C.kTide;
    const float phaseX = (C.kOmega / tideW) * sinf(tideW * t);
    const float flow = phaseX / 0.080f;
    const float k0 = kTau / C.ropeLam;

    auto blob = [&](float x, float y, float r, float a) {
        AddSmokeBlob(draw, ImVec2(x, y), r, smoke, a);
        };
    // Partículas que dão "wrap" nas pontas somem suavemente no recorte
    // (sem pipocar na borda do card)
    const float fadeW = 20.0f;
    auto edgeFade = [&](float x) {
        const float d = fminf(x - mn.x, mx.x - x);
        return d >= fadeW ? 1.0f : fmaxf(0.0f, d / fadeW);
        };

    draw->PushClipRect(mn, hmax, true);

    // 1) NÉVOA — manchas largas e fracas: dão volume
    for (int i = 0; i < C.nebN; ++i) {
        const float s1 = Hash01(i * 13 + 1), s2 = Hash01(i * 29 + 5);
        const float x = WrapRange(mn.x + s1 * W + flow * 0.85f, mn.x, mx.x);
        const float y = cy + (s2 - 0.5f) * 10.0f;
        const float r = C.nebR0 + (C.nebR1 - C.nebR0) * Hash01(i * 41 + 9);
        const float a = C.nebA0 + (C.nebA1 - C.nebA0)
            * (0.5f + 0.5f * sinf(t * 0.5f + s1 * kTau));
        blob(x, y, r, a * edgeFade(x));
    }

    const float amp = 6.2f + 1.6f * sinf(t * 0.61f);   // respiração da fita

    // 2) HALO — aura macia em volta do corpo (tira o ar de "corda")
    for (float x = mn.x; x <= mx.x; x += C.haloStep) {
        const float v = x * k0 + phaseX;
        const float y = cy + amp * sinf(v) + 1.5f * sinf(v * 2.31f + t * 1.15f);
        const float puff = 0.5f + 0.5f * sinf(v * 0.47f + 1.9f);
        blob(x, y, C.haloR, C.haloA * (0.6f + 0.6f * puff) * edgeFade(x));
    }

    // 3) CORPO — passo fino + raio grande = fita contínua (sem "colar de contas")
    for (float x = mn.x; x <= mx.x; x += C.ropeStep) {
        const float v = x * k0 + phaseX;
        const float y = cy + amp * sinf(v) + 1.5f * sinf(v * 2.31f + t * 1.15f);
        float puff = 0.5f + 0.5f * sinf(v * 0.47f + 1.9f)
            + 0.25f * sinf(v * 1.13f + 4.2f)
            + 0.20f * sinf(v * 0.31f + t * 0.9f);
        puff = fmaxf(0.0f, fminf(1.3f, puff)) / 1.3f;
        const float r = C.ropeR0 + (C.ropeR1 - C.ropeR0) * puff;
        const float a = C.ropeA0 + (C.ropeA1 - C.ropeA0) * puff;
        // Jitter determinístico no eixo X: quebra a periodicidade do passo
        // (sem isso a fita vira um "pente" de listras verticais).
        const float jx = (Hash01((int)(x * 3.7f)) - 0.5f) * C.ropeStep * 0.8f;
        const float jy = (Hash01((int)(x * 1.3f)) - 0.5f) * 2.5f;
        blob(x + jx, y, r, a * edgeFade(x));
        blob(x + jx, y + jy, r * 0.8f, a * 0.6f * edgeFade(x));
    }

    // 4) VOLUTAS — fitas finas cruzando no sentido oposto: turbulência
    {
        const float k2 = kTau / C.volLam;
        for (float x = mn.x; x <= mx.x; x += C.volStep) {
            const float v = x * k2 - phaseX * 1.6f;
            const float y = cy + 4.6f * sinf(v + 0.9f)
                + 1.2f * sinf(v * 2.7f + t * 1.9f);
            const float dens = 0.5f + 0.5f * sinf(v * 0.71f + 2.4f);
            const float jv = (Hash01((int)(x * 5.1f)) - 0.5f) * C.volStep * 0.9f;
            blob(x + jv, y, C.volR0 + (C.volR1 - C.volR0) * dens,
                (C.volA0 + (C.volA1 - C.volA0) * dens) * edgeFade(x));
        }
    }

    // 5) BOLHAS QUE SOBEM — pistas clássicas de fumaça
    for (int i = 0; i < C.puffN; ++i) {
        const float s1 = Hash01(i * 31 + 7), s2 = Hash01(i * 19 + 3);
        const float cyc = fmodf(t * (0.20f + 0.14f * s2) + s1 * 7.0f, 1.0f);
        const float x = WrapRange(mn.x + s2 * W + flow * 0.7f, mn.x, mx.x);
        const float y = (mn.y + headerH - 3.0f) - cyc * (headerH + 6.0f);
        const float r = C.puffR0 + (C.puffR1 - C.puffR0)
            * (0.4f + 0.6f * sinf(3.1415927f * cyc));
        const float a = (C.puffA0 + (C.puffA1 - C.puffA0) * s1)
            * sinf(3.1415927f * cyc);
        blob(x, y, r, a * edgeFade(x));      // nasce e morre suave (sem pipoco)
    }

    // 6) BORDAS — fumaça lambendo em cima e embaixo: o "abraço" no preto
    for (float x = mn.x; x <= mx.x; x += C.edgeStep) {
        const float v = x * k0 * 1.35f + phaseX * 1.2f;
        const float aT = C.edgeA0 + (C.edgeA1 - C.edgeA0)
            * fmaxf(0.0f, sinf(v * 0.61f + 0.6f));
        const float aB = C.edgeA0 + (C.edgeA1 - C.edgeA0)
            * fmaxf(0.0f, sinf(v * 0.61f + 3.3f));
        const float je = (Hash01((int)(x * 7.9f)) - 0.5f) * C.edgeStep;
        blob(x + je, mn.y + 3.0f, C.edgeR, aT);
        blob(x + je, hmax.y - 3.0f, C.edgeR, aB);
    }

    // 7) FAÍSCAS — partículas claras que derivam
    for (int i = 0; i < C.sparkN; ++i) {
        const float s1 = Hash01(i * 17 + 2), s2 = Hash01(i * 23 + 11);
        const float spd = 6.0f + 12.0f * s2;
        const float x = WrapRange(mn.x + s1 * W + spd * t + flow * 0.9f,
            mn.x, mx.x);
        const float y = cy + sinf(t * (0.7f + 0.8f * s2) + s1 * kTau)
            * (headerH * 0.30f);
        const float a = C.sparkA0 + (C.sparkA1 - C.sparkA0)
            * (0.5f + 0.5f * sinf(t * (1.1f + 1.3f * s1) + s2 * kTau));
        blob(x, y, C.sparkR0 + (C.sparkR1 - C.sparkR0) * s2,
            a * edgeFade(x));
    }

    // RÓTULO — SEM fundo: só o texto, com uma sombra fina para continuar
    // legível quando a fumaça passa por baixo.
    {
        const ImVec2 ts = textFont->CalcTextSizeA(kLabelSize, FLT_MAX, 0.0f, label);
        const ImVec2 tpos(mn.x + padX, mn.y + (headerH - ts.y) * 0.5f);
        draw->AddText(textFont, kLabelSize, ImVec2(tpos.x + 1.0f, tpos.y + 1.0f),
            IM_COL32(0, 0, 0, 170), label);
        draw->AddText(textFont, kLabelSize, tpos,
            IM_COL32(255, 255, 255, 255), label);
    }

    draw->PopClipRect();

    // Cantos de cima: devolve a curva do header por cima da fumaça — sem isso
    // a fumaça (retangular) deixaria as pontas quadradas/pontudas por cima
    // do raio do header.
    MaskTopCorners(draw, mn, mx, rounding, IM_COL32(0, 0, 0, 255));

    // Divisória: traço cobrindo TODO o eixo X do card
    const float divY = mn.y + headerH;
    draw->AddLine(ImVec2(mn.x, divY), ImVec2(mx.x, divY), IM_COL32(150, 160, 175, 45), 1.0f);
    return divY;
}

// Card esquerdo: header "INFOS" + usuário, nome do PC, plano e data/hora.
void DrawInfosCard(ImDrawList* draw, const ImVec2& mn, const ImVec2& mx) {
    const float padX = 16.0f;
    const float rounding = 14.0f;
    ImFont* textFont = g_fontText ? g_fontText : ImGui::GetFont();

    // Corpo do card (mesma cor do botão Unload)
    draw->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255), rounding);

    float y = DrawAnimatedHeader(draw, textFont, mn, mx, "INFOS") + 12.0f;

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

    ImVec2 featMin(0.0f, 0.0f), featMax(0.0f, 0.0f); // área do controle Inject (p/ o drag)

    // Dois cards na mesma cor do botão Unload
    {
        const float cardW = 275.0f, cardH = 256.0f, gap = 18.0f;
        const float x0 = p.x + (kPanelW - (cardW * 2.0f + gap)) * 0.5f;
        const float y0 = p.y + 48.0f;

        // Card esquerdo: INFOS (a fumaça é uma só, compartilhada pelos dois)
        DrawInfosCard(draw, ImVec2(x0, y0), ImVec2(x0 + cardW, y0 + cardH));

        // Card direito: header animado "FEATURES" + botãozinho (20x15)
        {
            const ImVec2 rmn(x0 + cardW + gap, y0);
            const ImVec2 rmx(x0 + cardW * 2.0f + gap, y0 + cardH);
            DrawCardBase(draw, rmn, rmx);
            const float divY = DrawAnimatedHeader(draw,
                g_fontText ? g_fontText : ImGui::GetFont(), rmn, rmx, "FEATURES");

            // Botão de inject (único controle do card), colado na margem
            // direita (padX = 16) e 10px abaixo de onde o botãozinho estava.
            const float by = divY + 22.0f;
            const float bW = 245.0f, bH = 40.0f;   // 230+15 x 50-10
            // (a largura extra cresce p/ a ESQUERDA: a borda direita fica
            //  ancorada na margem de 16px do card)
            const ImVec2 iMn(rmx.x - 16.0f - bW, by);
            DrawInjectControl(draw, iMn, ImVec2(iMn.x + bW, by + bH),
                featMin, featMax);

            // borda por cima do header, igual ao card esquerdo
            draw->AddRect(rmn, rmx, IM_COL32(85, 85, 85, 80), kRounding, 0, 1.0f);
        }
    }

    // Botão power / "Unload" no canto superior direito
    ImVec2 exMin, exMax;
    DrawPowerButton(draw, p, q, exMin, exMax);

    HandleDrag(hWnd, p, q, exMin, exMax, featMin, featMax);

    // Menu de progresso do inject (só aparece quando s_menuOpen)
    DrawInjectMenu(draw, p, q);

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
