// =============================================================================
//  VitinOptimizer - Interface ImGui + FontAwesome 6  (DirectX 11 / Win32)
//  Painel preto com bordas arredondadas, SEM demo window.
//  Sidebar retratil (icone de 3 tracos) com: General / Services / About
//
//  ARQUIVO UNICO. Adicione ao projeto junto com:
//      imgui.cpp, imgui_draw.cpp, imgui_widgets.cpp, imgui_tables.cpp,
//      backends/imgui_impl_win32.cpp, backends/imgui_impl_dx11.cpp
//      IconsFontAwesome6.h   (github.com/juliettef/IconFontCppHeaders)
//      fa_solid_900_ttf.h    (fonte embutida - NAO precisa de .ttf em disco)
//  Linker: ja resolvido pelos #pragma comment(lib, ...) abaixo.
//
//  NOTA: usa imgui_internal.h apenas para o kill switch da demo window.
// =============================================================================

// IMPORTANTE: windows.h define macros min/max que quebram std::min/std::max.
// NOMINMAX precisa vir ANTES de qualquer include que puxe windows.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
 
#define IMGUI_DISABLE_DEMO_WINDOWS
#include "imgui.h"
 
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "IconsFontAwesome6.h"   // defines ICON_FA_*
#include "fa_solid_900_ttf.h"    // fonte embutida (array de bytes)

// ----------------------------------------------------------------------------
//  ADAPTADOR DO NOME DO ARRAY DA FONTE
//  Cada gerador de header usa um nome diferente. Ajuste as 2 linhas abaixo
//  para bater com o que existe dentro do seu fa_solid_900_ttf.h.
//
//    xxd -i / bin2h        ->  fa_solid_900_ttf[]      + fa_solid_900_ttf_len
//    (era esse o padrao usado aqui)
//
//  Outros formatos comuns:
//    #define FA_TTF_DATA  fa_solid_900        // unsigned char fa_solid_900[]
//    #define FA_TTF_SIZE  sizeof(fa_solid_900)
//
//    #define FA_TTF_DATA  s_faSolid900Data
//    #define FA_TTF_SIZE  s_faSolid900Size
// ----------------------------------------------------------------------------
#define FA_TTF_DATA   fa_solid_900_ttf
#define FA_TTF_SIZE   ((int)fa_solid_900_ttf_len)

#include <d3d11.h>
#include <tchar.h>

// ----------------------------------------------------------------------------
//  Bibliotecas do DirectX (resolve LNK2019: D3D11CreateDeviceAndSwapChain).
//  Assim nao precisa configurar Linker > Input > Additional Dependencies.
// ----------------------------------------------------------------------------
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Helpers proprios de min/max para floats.
// Evitam de vez o conflito com as macros min/max do windows.h e
// ImMin / ImMax sao internos; FMin/FMax evitam depender deles.
static inline float FMin(float a, float b) { return (a < b) ? a : b; }
static inline float FMax(float a, float b) { return (a > b) ? a : b; }

// ----------------------------------------------------------------------------
//  D3D11 globals
// ----------------------------------------------------------------------------
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ----------------------------------------------------------------------------
//  Estado da UI
// ----------------------------------------------------------------------------
enum Tab { TAB_GENERAL = 0, TAB_SERVICES, TAB_ABOUT };

static int    g_tab = TAB_GENERAL;
static bool   g_sidebarOpen = true;
static float  g_sidebarAnim = 1.0f;   // 0 = fechada, 1 = aberta
static bool   g_requestClose = false;
static ImFont* g_fontIconBig = nullptr;

static const float SIDEBAR_W_OPEN = 190.0f;
static const float SIDEBAR_W_CLOSED = 58.0f;
static const float WINDOW_ROUNDING = 14.0f;

static const ImVec4 COL_BG = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
static const ImVec4 COL_SIDEBAR = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
static const ImVec4 COL_ACCENT = ImVec4(0.29f, 0.56f, 1.00f, 1.00f);
static const ImVec4 COL_TEXT = ImVec4(0.86f, 0.87f, 0.90f, 1.00f);
static const ImVec4 COL_TEXT_DIM = ImVec4(0.48f, 0.50f, 0.56f, 1.00f);

// Helper: cor com alpha customizado, sem usar o overload inexistente
// GetColorU32(ImVec4, float)  -> corrige o erro E0304.
static inline ImU32 ColU32(ImVec4 c, float alpha_mul = 1.0f)
{
    c.w *= alpha_mul;
    return ImGui::GetColorU32(c);
}

// ----------------------------------------------------------------------------
//  Estilo
// ----------------------------------------------------------------------------
static void ApplyStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = WINDOW_ROUNDING;   // <- borda redonda do retangulo
    s.ChildRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.ScrollbarRounding = 10.0f;
    s.TabRounding = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = ImVec2(0, 0);
    s.ItemSpacing = ImVec2(10, 9);
    s.ScrollbarSize = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = COL_BG;          // <- fundo preto
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_Text] = COL_TEXT;
    c[ImGuiCol_TextDisabled] = COL_TEXT_DIM;
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.09f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.11f);
    c[ImGuiCol_ButtonActive] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_CheckMark] = COL_ACCENT;
    c[ImGuiCol_SliderGrab] = COL_ACCENT;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.42f, 0.66f, 1.00f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.11f);
    c[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.16f);
}

// ----------------------------------------------------------------------------
//  Fontes: Arial + merge dos glifos FontAwesome no mesmo atlas
// ----------------------------------------------------------------------------
//  IMPORTANTE (ImGui 1.92+): o novo sistema de fontes dinamicas diferencia
//  "reference size" IMPLICITO (AddFontDefault sem SizePixels) de EXPLICITO.
//  Nao se pode fazer MergeMode com tamanho explicito numa fonte base que tem
//  tamanho implicito -> assert ImFontFlags_ImplicitRefSize.
//  Solucao: a fonte base (Arial) e criada com SizePixels EXPLICITO, e o merge
//  dos icones usa EXATAMENTE o mesmo tamanho.
// ----------------------------------------------------------------------------
static const float UI_FONT_SIZE = 17.0f;

// PushFont mudou de assinatura no ImGui 1.92 (passou a exigir o tamanho).
// Estes helpers mantem o codigo compativel com as duas versoes.
static const float ICON_BIG_SIZE = 20.0f;

static inline void PushIconFont()
{
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
    ImGui::PushFont(g_fontIconBig, ICON_BIG_SIZE);   // ImGui 1.92+
#else
    ImGui::PushFont(g_fontIconBig);                  // ImGui <= 1.91
#endif
}

static inline void PopIconFont()
{
    ImGui::PopFont();
}

static void LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // --- 1) FONTE BASE: ARIAL ----------------------------------------------
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.SizePixels = UI_FONT_SIZE;   // <- tamanho EXPLICITO (evita o assert)

    ImFont* textFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\arial.ttf", UI_FONT_SIZE, &cfg);

    if (textFont == nullptr)          // Arial ausente -> fonte embutida
    {
        ImFontConfig dcfg;
        dcfg.SizePixels = UI_FONT_SIZE;   // explicito tambem aqui
        textFont = io.Fonts->AddFontDefault(&dcfg);
    }

    // --- 2) ICONES FONTAWESOME (do header, direto da memoria) ---------------
    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };

    // merge dentro da Arial, MESMO tamanho da fonte base
    ImFontConfig icfg;
    icfg.MergeMode = true;
    icfg.PixelSnapH = true;
    icfg.GlyphMinAdvanceX = UI_FONT_SIZE;
    icfg.GlyphOffset = ImVec2(0.0f, 2.0f);
    icfg.SizePixels = UI_FONT_SIZE;   // igual a base
    icfg.FontDataOwnedByAtlas = false;          // array estatico: nao dar free()
    io.Fonts->AddFontFromMemoryTTF((void*)FA_TTF_DATA, FA_TTF_SIZE,
        UI_FONT_SIZE, &icfg, icons_ranges);

    // --- 3) FONTE MAIOR SO DE ICONES (hamburguer / titulos) -----------------
    // Fonte NOVA (sem merge), entao pode ter tamanho proprio sem conflito.
    const float BIG = ICON_BIG_SIZE;
    ImFontConfig bcfg;
    bcfg.GlyphMinAdvanceX = BIG;
    bcfg.SizePixels = BIG;
    bcfg.FontDataOwnedByAtlas = false;
    g_fontIconBig = io.Fonts->AddFontFromMemoryTTF((void*)FA_TTF_DATA, FA_TTF_SIZE,
        BIG, &bcfg, icons_ranges);

    if (g_fontIconBig == nullptr)
        g_fontIconBig = textFont;

    io.Fonts->Build();
}

// ----------------------------------------------------------------------------
//  Item da sidebar (icone + label que some quando a sidebar fecha)
//  CORRIGIDO: nao usa ImGuiWindow nem ImGui::GetCurrentWindow (API interna).
// ----------------------------------------------------------------------------
static bool SidebarItem(const char* icon, const char* label, bool selected,
    float width, float alpha)
{
    const ImVec2 size(width, 40.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();   // <- substitui win->DrawList

    if (selected || hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
            ColU32(selected ? ImVec4(1, 1, 1, 0.10f) : ImVec4(1, 1, 1, 0.055f)),
            9.0f);

    if (selected) // barrinha de destaque a esquerda
        dl->AddRectFilled(ImVec2(pos.x, pos.y + 8.0f),
            ImVec2(pos.x + 3.0f, pos.y + size.y - 8.0f),
            ColU32(COL_ACCENT), 2.0f);

    const ImVec4 fg = selected ? COL_ACCENT : (hovered ? COL_TEXT : COL_TEXT_DIM);
    dl->AddText(ImVec2(pos.x + 18.0f, pos.y + 11.0f), ColU32(fg), icon);

    if (alpha > 0.02f)
    {
        const ImVec4 tc = (selected || hovered) ? COL_TEXT : COL_TEXT_DIM;
        dl->AddText(ImVec2(pos.x + 48.0f, pos.y + 11.0f), ColU32(tc, alpha), label);
    }

    if (hovered && alpha < 0.30f)
        ImGui::SetTooltip("%s", label);

    return clicked;
}

// ----------------------------------------------------------------------------
//  Paginas
// ----------------------------------------------------------------------------
static void PageGeneral()
{
    static bool  enabled = true, autoStart = false, notify = true;
    static float opacity = 0.85f;
    static int   mode = 0;
    static char  name[64] = "user";

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild("##gen", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    ImGui::TextColored(COL_ACCENT, ICON_FA_SLIDERS);
    ImGui::SameLine(); ImGui::Text("Preferences");
    ImGui::Separator(); ImGui::Spacing();

    ImGui::Checkbox(ICON_FA_POWER_OFF "  Enable module", &enabled);
    ImGui::Checkbox(ICON_FA_ROCKET    "  Start with system", &autoStart);
    ImGui::Checkbox(ICON_FA_BELL      "  Notifications", &notify);
    ImGui::Spacing();

    ImGui::TextDisabled(ICON_FA_DROPLET "  Opacity");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##op", &opacity, 0.0f, 1.0f, "%.2f");
    ImGui::Spacing();

    ImGui::TextDisabled(ICON_FA_GAUGE_HIGH "  Performance mode");
    ImGui::SetNextItemWidth(-1);
    const char* modes[] = { "Balanced", "Performance", "Quality" };
    ImGui::Combo("##mode", &mode, modes, IM_ARRAYSIZE(modes));
    ImGui::Spacing();

    ImGui::TextDisabled(ICON_FA_USER "  Profile name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", name, IM_ARRAYSIZE(name));
    ImGui::Spacing(); ImGui::Spacing();

    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save", ImVec2(120, 34))) {}
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_LEFT "  Reset", ImVec2(120, 34))) {}

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void PageServices()
{
    struct Svc { const char* icon; const char* name; bool running; const char* desc; };
    static Svc svcs[] = {
        { ICON_FA_SHIELD_HALVED, "Security",   true,  "Realtime protection"   },
        { ICON_FA_CLOUD,         "Sync",       true,  "Cloud synchronization" },
        { ICON_FA_DATABASE,      "Storage",    false, "Local cache service"   },
        { ICON_FA_NETWORK_WIRED, "Network",    true,  "Packet inspection"     },
        { ICON_FA_ROBOT,         "Automation", false, "Scheduled tasks"       },
    };

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 12));
    for (int i = 0; i < IM_ARRAYSIZE(svcs); i++)
    {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
        ImGui::BeginChild("##svc", ImVec2(0, 62), ImGuiChildFlags_AlwaysUseWindowPadding);

        ImGui::TextColored(COL_ACCENT, "%s", svcs[i].icon);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(svcs[i].name);
        ImGui::TextColored(COL_TEXT_DIM, "%s", svcs[i].desc);
        ImGui::EndGroup();

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 96.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        if (svcs[i].running)
            ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.45f, 1.0f), ICON_FA_CIRCLE_CHECK " on");
        else
            ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f), ICON_FA_CIRCLE_XMARK " off");

        ImGui::SameLine();
        if (ImGui::SmallButton(svcs[i].running ? ICON_FA_STOP : ICON_FA_PLAY))
            svcs[i].running = !svcs[i].running;

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::PopStyleVar(2);
}

static void PageAbout()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 0.035f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 16));
    ImGui::BeginChild("##about", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);

    PushIconFont();
    ImGui::TextColored(COL_ACCENT, ICON_FA_CUBES);
    PopIconFont();
    ImGui::SameLine();
    ImGui::Text("VitinOptimizer");
    ImGui::TextColored(COL_TEXT_DIM, "version 1.0.0  -  build %s", __DATE__);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextWrapped("Interface feita com Dear ImGui + FontAwesome 6, renderizada em "
        "DirectX 11. Painel preto com cantos arredondados, sem demo window, "
        "sidebar retratil.");
    ImGui::Spacing(); ImGui::Spacing();

    ImGui::TextColored(COL_TEXT_DIM, ICON_FA_CODE         "   Dear ImGui %s", IMGUI_VERSION);
    ImGui::TextColored(COL_TEXT_DIM, ICON_FA_FONT_AWESOME "   FontAwesome 6 Solid");
    ImGui::TextColored(COL_TEXT_DIM, ICON_FA_MICROCHIP    "   Backend: DirectX 11 / Win32");
    ImGui::TextColored(COL_TEXT_DIM, ICON_FA_CLOCK        "   %.1f FPS", ImGui::GetIO().Framerate);
    ImGui::Spacing(); ImGui::Spacing();

    // ICON_FA_GITHUB_ALT nao existe no IconsFontAwesome6.h (e brand, nao solid).
    if (ImGui::Button(ICON_FA_CODE_BRANCH "  Repository", ImVec2(150, 34))) {}
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_HEART "  Donate", ImVec2(150, 34))) {}

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ----------------------------------------------------------------------------
//  Janela principal = o retangulo preto arredondado
// ----------------------------------------------------------------------------
static void DrawUI()
{
    ImGuiIO& io = ImGui::GetIO();

    // animacao suave da sidebar (FMin/FMax no lugar de ImMin/ImMax)
    const float target = g_sidebarOpen ? 1.0f : 0.0f;
    g_sidebarAnim += (target - g_sidebarAnim) * FMin(1.0f, io.DeltaTime * 12.0f);

    const float sbW = SIDEBAR_W_CLOSED + (SIDEBAR_W_OPEN - SIDEBAR_W_CLOSED) * g_sidebarAnim;
    const float labelAlpha = FMax(0.0f, (g_sidebarAnim - 0.45f) / 0.55f);

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 470), ImGuiCond_Always);

    ImGui::Begin("##MainPanel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

    const ImVec2 wpos = ImGui::GetWindowPos();
    const ImVec2 wsize = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // fundo da sidebar, arredondado so nos cantos esquerdos
    dl->AddRectFilled(wpos, ImVec2(wpos.x + sbW, wpos.y + wsize.y),
        ColU32(COL_SIDEBAR), WINDOW_ROUNDING, ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(wpos.x + sbW, wpos.y + 10.0f),
        ImVec2(wpos.x + sbW, wpos.y + wsize.y - 10.0f),
        ColU32(ImVec4(1, 1, 1, 0.07f)));

    // ------------------------- SIDEBAR -------------------------
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("##sidebar", ImVec2(sbW, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(0, 14));

    // botao hamburguer (3 tracos) -> abre/fecha
    ImGui::SetCursorPosX(10.0f);
    PushIconFont();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(ICON_FA_BARS, ImVec2(38, 38)))
        g_sidebarOpen = !g_sidebarOpen;
    ImGui::PopStyleColor();
    PopIconFont();

    if (labelAlpha > 0.02f)
    {
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 9.0f);
        ImGui::TextColored(ImVec4(COL_TEXT.x, COL_TEXT.y, COL_TEXT.z, labelAlpha), "MENU");
    }

    ImGui::Dummy(ImVec2(0, 10));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    const float itemW = sbW - 16.0f;
    ImGui::SetCursorPosX(8.0f);
    if (SidebarItem(ICON_FA_GEAR, "General", g_tab == TAB_GENERAL, itemW, labelAlpha)) g_tab = TAB_GENERAL;
    ImGui::SetCursorPosX(8.0f);
    if (SidebarItem(ICON_FA_SERVER, "Services", g_tab == TAB_SERVICES, itemW, labelAlpha)) g_tab = TAB_SERVICES;
    ImGui::SetCursorPosX(8.0f);
    if (SidebarItem(ICON_FA_CIRCLE_INFO, "About", g_tab == TAB_ABOUT, itemW, labelAlpha)) g_tab = TAB_ABOUT;

    // rodape
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52.0f);
    ImGui::SetCursorPosX(8.0f);
    if (SidebarItem(ICON_FA_RIGHT_FROM_BRACKET, "Exit", false, itemW, labelAlpha))
        g_requestClose = true;

    ImGui::EndChild();

    // ------------------------- CONTEUDO -------------------------
    ImGui::SetCursorPos(ImVec2(sbW + 1.0f, 0));
    ImGui::BeginChild("##content", ImVec2(wsize.x - sbW - 1.0f, 0));

    ImGui::Dummy(ImVec2(0, 14));
    ImGui::Indent(20.0f);

    static const char* titles[] = { "General", "Services", "About" };
    static const char* subs[] = { "Configuracoes gerais da aplicacao",
                                    "Modulos e servicos em execucao",
                                    "Informacoes da build" };
    static const char* ticons[] = { ICON_FA_GEAR, ICON_FA_SERVER, ICON_FA_CIRCLE_INFO };

    PushIconFont();
    ImGui::TextColored(COL_ACCENT, "%s", ticons[g_tab]);
    PopIconFont();
    ImGui::SameLine();
    ImGui::Text("%s", titles[g_tab]);
    ImGui::TextColored(COL_TEXT_DIM, "%s", subs[g_tab]);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginChild("##page", ImVec2(wsize.x - sbW - 42.0f, -18.0f));
    switch (g_tab)
    {
    case TAB_GENERAL:  PageGeneral();  break;
    case TAB_SERVICES: PageServices(); break;
    case TAB_ABOUT:    PageAbout();    break;
    }
    ImGui::EndChild();

    ImGui::Unindent(20.0f);
    ImGui::EndChild();

    ImGui::End();
}

 
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       hInstance, nullptr, nullptr, nullptr, nullptr,
                       L"VitinOptimizerClass", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"VitinOptimizer",
        WS_OVERLAPPEDWINDOW, 100, 100, 900, 600,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ApplyStyle();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    LoadFonts();

    const float clear_color[4] = { 0.02f, 0.02f, 0.03f, 1.00f };

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawUI();            // unica UI nossa

       

        if (g_requestClose) done = true;

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);    // vsync
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ----------------------------------------------------------------------------
//  D3D11 helpers
// ----------------------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd)
{
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
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            flags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer)
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // desabilita menu do ALT
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
