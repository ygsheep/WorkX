/**
 * @file main.cpp
 * @brief 消息灵动岛原型：Dear ImGui + Win32 透明置顶窗口 + D3D11
 * @details 无边框/置顶/分层透明窗口（UpdateLayeredWindow），右上角显示
 *          macOS 风格灵动岛通知；EventBridge 订阅 workx EventBus 事件。
 *          操作：Esc 退出；F2 注入演示通知；右键 菜单（清空/退出）；
 *          点击岛 = 展开/收起 + 重置停留计时。
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "core/events/agent_events.h"
#include "core/events/event_bus.h"

#include "dynamic_island.h"
#include "event_bridge.h"

// ---- 全局状态 ------------------------------------------------------------

struct Context {
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    IDXGISurface1* surface = nullptr;
    int width = 400;
    int height = 200;
    bool running = true;

    di::DynamicIsland island;

    // 演示：周期性发布模拟事件
    std::chrono::steady_clock::time_point last_demo = {};
    int demo_step = 0;
    std::mt19937 rng{0x5EED};
} g;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

// ---- 工具函数 ------------------------------------------------------------

static void LogF(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

static const char* kDemoTitles[] = {
    "工具调用: Bash", "工具调用: ReadFile", "工具调用: Glob",
    "缓存命中下降", "上下文压缩已恢复",
};

static void InjectDemoNotification(di::DynamicIsland& island) {
    static std::uniform_int_distribution<int> kind_dist(0, 3);
    const auto kind = static_cast<di::NotifyKind>(kind_dist(g.rng));
    const char* title = kDemoTitles[g.demo_step % 5];
    const char* body = "这是一个来自演示注入的灵动岛通知，用于验证滑入、展开与自动消失动画。";
    island.Push(kind, title, body, 4.0f);
    ++g.demo_step;
}

/// 发布一帧模拟 agent 事件（走真实 EventBus → EventBridge 路径）
static void PublishDemoEvents(agent::IEventBus& bus) {
    static int call_no = 0;
    switch (g.demo_step % 4) {
    case 0:
        bus.publish(agent::ToolCallEvent{
            .tool_name = "Bash",
            .arguments = "cmake --build build --config Debug",
            .call_id = "call_" + std::to_string(call_no++),
            .tool_type = agent::tool::ToolType::Execute,
        });
        break;
    case 1:
        bus.publish(agent::ToolResultEvent{
            .call_id = "call_" + std::to_string(call_no - 1),
            .result = "exit code 0, 构建成功（42 个目标）",
            .is_error = false,
        });
        break;
    case 2:
        bus.publish(agent::AgentDoneEvent{
            .final_response = "已完成库化改造的构建验证，全部目标编译通过。",
            .total_steps = 3,
            .total_tool_calls = 2,
            .total_duration_ms = 1234.5,
        });
        break;
    default:
        bus.publish(agent::CompactionPausedEvent{
            .session_id = "demo",
            .paused = true,
            .consecutive_compacts = 3,
            .tokens_before = 42000,
            .ratio = 0.93f,
            .notice = "连续 3 次 compact 未降低占用比，缓存命中守卫已暂停压缩",
        });
        break;
    }
    ++g.demo_step;
}

// ---- Win32 窗口 ----------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE || (msg == WM_KEYDOWN && wParam == VK_ESCAPE)) {
        g.running = false;
        return 0;
    }
    if (msg == WM_DESTROY) {
        g.running = false;
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_F2) {
        InjectDemoNotification(g.island);
        return 0;
    }
    if (msg == WM_RBUTTONUP) {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "清空通知");
        AppendMenuA(menu, MF_STRING, 2, "注入演示通知");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 3, "退出");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) g.island.Clear();
        else if (cmd == 2) InjectDemoNotification(g.island);
        else if (cmd == 3) g.running = false;
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ResizeSwapChain(int width, int height) {
    if (!g.swap_chain) return;
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; }
    g.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    g.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    g.device->CreateRenderTargetView(back, nullptr, &g.rtv);
    if (g.surface) { g.surface->Release(); g.surface = nullptr; }
    back->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&g.surface));
    back->Release();
    g.width = width;
    g.height = height;
}

/// 初始化 D3D11 + 透明分层交换链（DXGI_ALPHA_MODE_PREMULTIPLIED + DISCARD）
static bool InitD3D11(HWND hwnd, int width, int height) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
            D3D11_SDK_VERSION, &g.device, nullptr, &g.ctx))) {
        LogF("D3D11CreateDevice failed");
        return false;
    }

    IDXGIDevice* dxgi_device = nullptr;
    g.device->QueryInterface(__uuidof(IDXGIDevice),
                             reinterpret_cast<void**>(&dxgi_device));
    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    dxgi_device->Release();
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory2),
                       reinterpret_cast<void**>(&factory));
    adapter->Release();

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    // 注意：DXGI_ALPHA_MODE_PREMULTIPLIED 仅支持 CreateSwapChainForCoreWindow
    // （UWP），HWND 交换链必须 UNSPECIFIED。imgui_impl_dx11 的 blend state
    // （SrcBlendAlpha=ONE）输出即为 premultiplied 像素，配合
    // UpdateLayeredWindow(AC_SRC_ALPHA) 实现真透明。

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd{};
    fsd.Windowed = TRUE;   // 窗口模式（分层窗口不能全屏）
    const HRESULT hr = factory->CreateSwapChainForHwnd(
        g.device, hwnd, &sd, &fsd, nullptr,
        reinterpret_cast<IDXGISwapChain1**>(&g.swap_chain));
    factory->Release();
    if (FAILED(hr)) {
        LogF("CreateSwapChainForHwnd failed: %08X", (unsigned)hr);
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    g.swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&back));
    g.device->CreateRenderTargetView(back, nullptr, &g.rtv);
    back->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&g.surface));
    back->Release();
    return true;
}

/// 呈现到分层窗口（Present → GetDC → UpdateLayeredWindow）
static void PresentLayered() {
    g.swap_chain->Present(0, 0);

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return;

    POINT pt_src{0, 0};
    POINT pt_dst{0, 0};
    SIZE size{g.width, g.height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    RECT rc{};
    GetWindowRect(g.hwnd, &rc);
    pt_dst.x = rc.left;
    pt_dst.y = rc.top;

    HDC surface_dc = nullptr;
    if (SUCCEEDED(g.surface->GetDC(FALSE, &surface_dc))) {
        UpdateLayeredWindow(g.hwnd, screen_dc, &pt_dst, &size,
                            surface_dc, &pt_src, 0, &blend, ULW_ALPHA);
        g.surface->ReleaseDC(nullptr);
    }
    ReleaseDC(nullptr, screen_dc);
}

/// 窗口位置：工作区右上角，下移 24px（避开系统托盘区域）
static void PlaceWindow(HWND hwnd, int width, int height) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.right - width - 16;
    const int y = work.top + 24;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static bool CreateOverlayWindow(int width, int height) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WorkxDynamicIsland";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, L"workx-dynamic-island",
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) {
        LogF("CreateWindowExW failed");
        return false;
    }
    return true;
}

// ---- 主循环 --------------------------------------------------------------

int main() {
    constexpr int kWidth = 400;
    constexpr int kHeight = 160;

    if (!CreateOverlayWindow(kWidth, kHeight)) return 1;
    if (!InitD3D11(g.hwnd, kWidth, kHeight)) return 1;
    PlaceWindow(g.hwnd, kWidth, kHeight);
    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 字体：黑体（中文），14 标题 / 12 正文
    static const char* kFontPath = "C:/Windows/Fonts/simhei.ttf";
    if (GetFileAttributesA(kFontPath) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(kFontPath, 14.0f, nullptr,
                                     io.Fonts->GetGlyphRangesChineseFull());
        io.Fonts->AddFontFromFileTTF(kFontPath, 12.0f, nullptr,
                                     io.Fonts->GetGlyphRangesChineseFull());
        io.Fonts->AddFontFromFileTTF(kFontPath, 13.0f, nullptr,
                                     io.Fonts->GetGlyphRangesChineseFull());
    } else {
        io.Fonts->AddFontDefault();
    }
    io.Fonts->Build();

    ImGui_ImplWin32_Init(g.hwnd);
    if (!ImGui_ImplDX11_Init(g.device, g.ctx)) {
        LogF("ImGui_ImplDX11_Init failed");
        return 1;
    }

    agent::EventBus& bus = agent::EventBus::instance();
    di::EventBridge bridge(bus);

    g.last_demo = std::chrono::steady_clock::now();
    g.demo_step = 0;

    LogF("dynamic island running. Esc=exit, F2=inject demo, right-click=menu");
    ShowWindow(g.hwnd, SW_SHOW);

    auto prev = std::chrono::steady_clock::now();
    while (g.running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g.running) break;

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;

        // 演示：每 3 秒发布一轮模拟事件（走真实 EventBus → 桥 → 岛）
        if (std::chrono::duration<float>(now - g.last_demo).count() > 3.0f) {
            PublishDemoEvents(bus);
            g.last_demo = now;
        }

        // 事件桥 → 岛
        std::vector<di::IslandMessage> messages;
        bridge.Drain(messages);
        for (const auto& m : messages) {
            g.island.Push(m.kind, m.title, m.body, 4.0f);
        }

        // 动画 + 尺寸自适应（无通知时隐藏窗口）
        g.island.Update(dt);
        const float island_h = g.island.Height() + 12.0f;
        const int new_h = (int)island_h + 1;
        const bool visible = g.island.Empty() ? false : true;
        if (!visible) {
            if (IsWindowVisible(g.hwnd)) ShowWindow(g.hwnd, SW_HIDE);
        } else {
            if (!IsWindowVisible(g.hwnd)) ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
            if (new_h != g.height) {
                ResizeSwapChain(g.width, new_h);
                RECT rc{};
                GetWindowRect(g.hwnd, &rc);
                SetWindowPos(g.hwnd, HWND_TOPMOST, rc.left, rc.top,
                             g.width, new_h, SWP_NOACTIVATE);
            }
        }

        // 渲染
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
        g.ctx->ClearRenderTargetView(g.rtv, clear_color);

        g.island.Draw(ImGui::GetBackgroundDrawList(), 0.0f, 6.0f,
                      (float)g.width - 12.0f);

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        PresentLayered();
        Sleep(1);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g.rtv) g.rtv->Release();
    if (g.surface) g.surface->Release();
    if (g.swap_chain) g.swap_chain->Release();
    if (g.ctx) g.ctx->Release();
    if (g.device) g.device->Release();
    DestroyWindow(g.hwnd);
    return 0;
}
