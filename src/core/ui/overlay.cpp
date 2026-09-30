#include "overlay.h"

#include "core/framework/framework.h"
#include "core/gfx/frame_inspector.h"
#include "core/input/xinput_bridge.h"
#include "core/util/crash.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"
#include "game/igame_adapter.h"

#include <windows.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <cstdio>
#include <cwchar>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace bvr::overlay {
namespace {

bool g_initialized = false;
bool g_visible = false;
std::atomic<int> g_visibleRequest{-1}; // session 22: seam toggle (-1 = none)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_rtvBackbuffer = nullptr; // identity only, never deref'd
HWND g_window = nullptr;
WNDPROC g_originalWndProc = nullptr;

// ---------------------------------------------------------------------------
// Readable overlay at any backbuffer size.
//
// ImGui's Win32 backend lays out in WINDOW CLIENT pixels (io.DisplaySize from
// GetClientRect, and the mouse arrives in the same units), but the overlay is
// drawn into the game's BACKBUFFER. The README tells users to run a near-square
// backbuffer (e.g. 2750x2850); on a normal monitor Windows shrinks that window,
// so the backbuffer and the client area differ - by a DIFFERENT factor on each
// axis. Drawn 1:1 into the backbuffer, the menu then landed in a corner and was
// scaled down non-uniformly on its way to the window: squashed, unreadable, and
// the mouse no longer lined up with it.
//
// io.DisplayFramebufferScale is ImGui's own lever for exactly this split
// (logical vs framebuffer pixels; the DX11 backend honours it for the viewport
// and scissor): layout and mouse stay in client pixels, the render stretches to
// the full backbuffer, and the window's own downscale undoes the stretch. At
// backbuffer == client size the scale is (1,1) and nothing changes.
//
// On top of that, a user text-size factor (style.FontScaleMain + spacing),
// persisted in overlay.ini beside the log.
// ---------------------------------------------------------------------------
constexpr float kUiScaleMin = 0.75f;
constexpr float kUiScaleMax = 3.0f;
float g_uiScale = 1.25f;     // user factor; 1.25 reads comfortably on a desktop mirror
float g_appliedScale = 0.0f; // what the style currently carries (0 = base style)
ImGuiStyle g_baseStyle;      // unscaled style, so re-scaling never compounds

void ui_scale_path(wchar_t* out, size_t cap) {
    const wchar_t* dir = bvr::log::data_dir();
    swprintf(out, cap, L"%s\\overlay.ini", dir ? dir : L".");
}

void load_ui_scale() {
    wchar_t path[MAX_PATH];
    ui_scale_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        float v = 0.0f;
        if (sscanf_s(line, "uiScale=%f", &v) == 1 && v >= kUiScaleMin && v <= kUiScaleMax)
            g_uiScale = v;
    }
    fclose(f);
}

void save_ui_scale() {
    wchar_t path[MAX_PATH];
    ui_scale_path(path, MAX_PATH);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "uiScale=%.2f\n", g_uiScale);
    fclose(f);
    BVR_LOG("overlay: text size %.2f saved", g_uiScale);
}

void apply_ui_scale() {
    if (g_appliedScale == g_uiScale) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_baseStyle;
    st.ScaleAllSizes(g_uiScale);
    st.FontScaleMain = g_uiScale;
    g_appliedScale = g_uiScale;
}

// Called between the Win32 NewFrame (which sets DisplaySize = client size) and
// ImGui::NewFrame.
void apply_framebuffer_scale(IDXGISwapChain* swapchain) {
    ImGuiIO& io = ImGui::GetIO();
    DXGI_SWAP_CHAIN_DESC d{};
    float sx = 1.0f, sy = 1.0f;
    if (SUCCEEDED(swapchain->GetDesc(&d)) && io.DisplaySize.x > 0.0f &&
        io.DisplaySize.y > 0.0f && d.BufferDesc.Width && d.BufferDesc.Height) {
        sx = static_cast<float>(d.BufferDesc.Width) / io.DisplaySize.x;
        sy = static_cast<float>(d.BufferDesc.Height) / io.DisplaySize.y;
    }
    static float s_loggedX = 0.0f, s_loggedY = 0.0f;
    if (sx != s_loggedX || sy != s_loggedY) {
        s_loggedX = sx;
        s_loggedY = sy;
        BVR_LOG("overlay: window %.0fx%.0f, backbuffer %ux%u -> framebuffer scale %.3f x %.3f",
                io.DisplaySize.x, io.DisplaySize.y, d.BufferDesc.Width, d.BufferDesc.Height,
                sx, sy);
    }
    io.DisplayFramebufferScale = ImVec2(sx, sy);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    // Session 38: the subclass is on the GAME's main window, so it is the
    // earliest game-agnostic sight of a close. BS2's engine faults on its own
    // exit path (hook-free-proven); noting teardown here turns that into a
    // quiet fast exit instead of a dump per close. WM_ENDSESSION covers
    // logoff/shutdown. Always forwarded - observation only.
    if (msg == WM_CLOSE || msg == WM_DESTROY || (msg == WM_ENDSESSION && wparam))
        crash::note_teardown(msg == WM_CLOSE     ? "WM_CLOSE"
                             : msg == WM_DESTROY ? "WM_DESTROY"
                                                 : "WM_ENDSESSION");
    if (g_visible) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);
        // Session 22 (user report: overlay unusable while scrolling): while
        // ImGui owns the mouse/keyboard, CONSUME those messages instead of
        // letting the game fight the overlay for them (the wheel doubled as
        // weapon-cycle, clicks re-captured the cursor mid-drag).
        ImGuiIO& io = ImGui::GetIO();
        bool mouseMsg = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool keyMsg = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        if ((mouseMsg && io.WantCaptureMouse) || (keyMsg && io.WantCaptureKeyboard))
            return TRUE;
    }
    return CallWindowProcW(g_originalWndProc, hwnd, msg, wparam, lparam);
}

bool CreateRenderTarget(IDXGISwapChain* swapchain) {
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(backbuffer, nullptr, &g_rtv);
    g_rtvBackbuffer = SUCCEEDED(hr) ? backbuffer : nullptr;
    backbuffer->Release();
    return SUCCEEDED(hr);
}

bool Init(IDXGISwapChain* swapchain) {
    if (FAILED(swapchain->GetDevice(IID_PPV_ARGS(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swapchain->GetDesc(&desc);
    g_window = desc.OutputWindow;

    if (!CreateRenderTarget(swapchain)) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't scatter imgui.ini into the game folder
    ImGui::StyleColorsDark();
    g_baseStyle = ImGui::GetStyle();
    load_ui_scale();
    apply_ui_scale();
    ImGui_ImplWin32_Init(g_window);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_originalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));

    BVR_LOG("overlay initialized (hwnd=%p) - F10 toggles it", g_window);
    return true;
}

void DrawUi() {
    ImGui::SetNextWindowSize(ImVec2(420 * g_uiScale, 420 * g_uiScale), ImGuiCond_FirstUseEver);
    // Build id in the title so an in-headset screenshot identifies the build.
    ImGui::Begin("BioShock VR " BVR_VERSION " [" BVR_BUILD_ID "]");
    ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    // Readability first, so it is reachable however unreadable the rest is.
    ImGui::SetNextItemWidth(160.0f * g_uiScale);
    ImGui::SliderFloat("Menu text size", &g_uiScale, kUiScaleMin, kUiScaleMax, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) save_ui_scale();
    ImGui::Separator();
    vr::draw_debug_ui();
    ImGui::Separator();
    input::draw_debug_ui();
    if (auto* adapter = game::adapter()) {
        ImGui::Separator();
        adapter->drawDebugUi();
    }
    ImGui::Separator();
    frame_inspector::draw_debug_ui();
    ImGui::Separator();
    ImGui::TextWrapped("Log: %%LOCALAPPDATA%%\\BioshockVR\\bioshockvr.log");
    ImGui::End();
}

} // namespace

void on_present(IDXGISwapChain* swapchain) {
    if (!g_initialized) {
        g_initialized = Init(swapchain);
        if (!g_initialized) return;
    }
    // Session 22 (user report: overlay gone until an alt-tab): if the game
    // swaps its backbuffer object WITHOUT a ResizeBuffers (fullscreen-state
    // churn), a held RTV keeps drawing into the dead buffer - F10 toggles an
    // overlay nobody can see. Track the buffer identity and re-create.
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            if (g_rtv && bb != g_rtvBackbuffer) {
                g_rtv->Release();
                g_rtv = nullptr;
                BVR_LOG("overlay: backbuffer identity changed - RTV re-created");
            }
            bb->Release();
        }
    }
    if (!g_rtv && !CreateRenderTarget(swapchain)) return;

    // F10, edge-triggered (Insert was the original choice, but not every
    // keyboard has it); the seam request lane covers harness toggling.
    static bool wasDown = false;
    bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (isDown && !wasDown) {
        g_visible = !g_visible;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }
    wasDown = isDown;
    int req = g_visibleRequest.exchange(-1, std::memory_order_relaxed);
    if (req >= 0) {
        g_visible = req != 0;
        ImGui::GetIO().MouseDrawCursor = g_visible;
    }

    if (!g_visible) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    apply_framebuffer_scale(swapchain);
    apply_ui_scale();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

void on_resize() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    g_rtvBackbuffer = nullptr;
}

void set_visible(bool on) {
    g_visibleRequest.store(on ? 1 : 0, std::memory_order_relaxed);
}

} // namespace bvr::overlay
