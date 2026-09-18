#include "hook.h"
#include "lua_api.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_opengl3.h>
#include <gl/GL.h>

#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Globals ──────────────────────────────────────────────────────────
wglSwapBuffers_t o_wglSwapBuffers = nullptr;
bool g_Initialized = false;
HWND g_GameHWND = nullptr;
bool g_MenuOpen = false;
WNDPROC oWndProc = nullptr;

// ── Keyboard hook ────────────────────────────────────────────────────
static HHOOK g_KeyHook = nullptr;

static ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_1: return ImGuiKey_Semicolon;
        case VK_OEM_PLUS: return ImGuiKey_Equal;
        case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_5: return ImGuiKey_Backslash;
        case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_3: return ImGuiKey_GraveAccent;
        case VK_LSHIFT: return ImGuiKey_LeftShift;
        case VK_LCONTROL: return ImGuiKey_LeftCtrl;
        case VK_LMENU: return ImGuiKey_LeftAlt;
        case VK_RSHIFT: return ImGuiKey_RightShift;
        case VK_RCONTROL: return ImGuiKey_RightCtrl;
        case VK_RMENU: return ImGuiKey_RightAlt;
        case '0': return ImGuiKey_0; case '1': return ImGuiKey_1;
        case '2': return ImGuiKey_2; case '3': return ImGuiKey_3;
        case '4': return ImGuiKey_4; case '5': return ImGuiKey_5;
        case '6': return ImGuiKey_6; case '7': return ImGuiKey_7;
        case '8': return ImGuiKey_8; case '9': return ImGuiKey_9;
        case 'A': return ImGuiKey_A; case 'B': return ImGuiKey_B;
        case 'C': return ImGuiKey_C; case 'D': return ImGuiKey_D;
        case 'E': return ImGuiKey_E; case 'F': return ImGuiKey_F;
        case 'G': return ImGuiKey_G; case 'H': return ImGuiKey_H;
        case 'I': return ImGuiKey_I; case 'J': return ImGuiKey_J;
        case 'K': return ImGuiKey_K; case 'L': return ImGuiKey_L;
        case 'M': return ImGuiKey_M; case 'N': return ImGuiKey_N;
        case 'O': return ImGuiKey_O; case 'P': return ImGuiKey_P;
        case 'Q': return ImGuiKey_Q; case 'R': return ImGuiKey_R;
        case 'S': return ImGuiKey_S; case 'T': return ImGuiKey_T;
        case 'U': return ImGuiKey_U; case 'V': return ImGuiKey_V;
        case 'W': return ImGuiKey_W; case 'X': return ImGuiKey_X;
        case 'Y': return ImGuiKey_Y; case 'Z': return ImGuiKey_Z;
        case VK_F1: return ImGuiKey_F1; case VK_F2: return ImGuiKey_F2;
        case VK_F3: return ImGuiKey_F3; case VK_F4: return ImGuiKey_F4;
        case VK_F5: return ImGuiKey_F5; case VK_F6: return ImGuiKey_F6;
        case VK_F7: return ImGuiKey_F7; case VK_F8: return ImGuiKey_F8;
        case VK_F9: return ImGuiKey_F9; case VK_F10: return ImGuiKey_F10;
        case VK_F11: return ImGuiKey_F11; case VK_F12: return ImGuiKey_F12;
        default: return ImGuiKey_None;
    }
}

static LRESULT CALLBACK KeyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        int vk = kb->vkCode;
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        static bool f1Was = false;
        if (vk == VK_F1 && isDown && !f1Was) {
            g_MenuOpen = !g_MenuOpen;
        }
        f1Was = isDown;
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ── Clipboard ────────────────────────────────────────────────────────
static const char* ClipGetText(void*) {
    if (!OpenClipboard(nullptr)) return "";
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return ""; }
    char* text = (char*)GlobalLock(hData);
    if (!text) { CloseClipboard(); return ""; }
    static std::string s; s = text;
    GlobalUnlock(hData); CloseClipboard();
    return s.c_str();
}

static void ClipSetText(void*, const char* text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    memcpy(GlobalLock(hMem), text, len);
    GlobalUnlock(hMem);
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
}

// ── WndProc hook ─────────────────────────────────────────────────────
LRESULT CALLBACK hkWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_MenuOpen) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return 1;

        switch (msg) {
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetCapture(hWnd); return 1;
            case WM_LBUTTONUP: ReleaseCapture(); return 1;
            case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP: return 1;
            case WM_MOUSEMOVE: case WM_MOUSEWHEEL: return 1;
            case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            case WM_CHAR: case WM_UNICHAR:
                return 1;
        }
    }

    return CallWindowProcW(oWndProc, hWnd, msg, wParam, lParam);
}

// ── UI State ─────────────────────────────────────────────────────────
static bool showConsole = true;
static bool showDebug = true;
static bool showSettings = false;
static bool autoScroll = true;
static bool debugPackets = true;
static bool debugCallbacks = true;
static bool debugTimer = true;
static bool debugPathfinding = true;
static bool debugInventory = true;
static bool debugPlayers = true;
static char scriptBuf[16384] = "";
static LuaExecutor* g_executor = nullptr;

// ── wglSwapBuffers hook ──────────────────────────────────────────────
BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
    if (!g_Initialized) {
        g_GameHWND = WindowFromDC(hdc);

        if (g_GameHWND) {
            oWndProc = (WNDPROC)SetWindowLongPtrW(g_GameHWND, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;
        io.SetClipboardTextFn = ClipSetText;
        io.GetClipboardTextFn = ClipGetText;

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.Alpha = 0.95f;
        style.WindowBorderSize = 1.0f;

        ImGui_ImplWin32_Init(g_GameHWND);
        ImGui_ImplOpenGL3_Init("#version 330");

        g_KeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, GetModuleHandleW(nullptr), 0);

        g_executor = new LuaExecutor();
        g_Initialized = true;
        consoleLog("[INFO] Coems Executor initialized inside Growtopia");
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_MenuOpen) {
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Coems Executor  |  F1 to toggle##executor", &g_MenuOpen,
            ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Tools")) {
                ImGui::MenuItem("Console", nullptr, &showConsole);
                ImGui::MenuItem("Debug Output", nullptr, &showDebug);
                ImGui::MenuItem("Settings", nullptr, &showSettings);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        bool running = g_executor->isRunning();

        ImGui::PushStyleColor(ImGuiCol_Button, running
            ? ImVec4(0.6f, 0.2f, 0.2f, 1.0f)
            : ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button(running ? "Running..." : "Execute", ImVec2(100, 25)) && !running) {
            g_consoleLogs.clear();
            g_debugLogs.clear();
            std::string script(scriptBuf);
            debugLog("[SYSTEM] Executing script...");
            LuaExecutor* exec = g_executor;
            std::thread t([exec, script]() { exec->execute(script); });
            t.detach();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(70, 25))) {
            g_executor->stop();
            debugLog("[SYSTEM] Script stopped.");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        if (ImGui::Button("Clear")) { g_consoleLogs.clear(); g_debugLogs.clear(); }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoScroll);
        ImGui::SameLine(0, 20);
        ImGui::TextColored(running ? ImVec4(0,1,0,1) : ImVec4(0.5f,0.5f,0.5f,1),
            running ? "RUNNING" : "IDLE");

        ImGui::Separator();

        float bottomH = 0;
        if (showConsole) bottomH += 120;
        if (showDebug) bottomH += 100;
        if (showSettings) bottomH += 80;
        float editorH = ImGui::GetContentRegionAvail().y - bottomH;
        if (editorH < 80) editorH = 80;

        ImGui::BeginChild("Editor", ImVec2(0, editorH), true);
        ImGui::InputTextMultiline("##script", scriptBuf, sizeof(scriptBuf),
            ImVec2(-1, -1), ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoHorizontalScroll);
        ImGui::EndChild();

        if (showSettings) {
            ImGui::BeginChild("Settings", ImVec2(0, 75), true);
            ImGui::Text("Debug Filters");
            ImGui::Columns(3, nullptr, false);
            ImGui::Checkbox("Packets", &debugPackets);
            ImGui::Checkbox("Callbacks", &debugCallbacks);
            ImGui::Checkbox("Timers", &debugTimer);
            ImGui::NextColumn();
            ImGui::Checkbox("Pathfinding", &debugPathfinding);
            ImGui::Checkbox("Inventory", &debugInventory);
            ImGui::Checkbox("Players", &debugPlayers);
            ImGui::NextColumn();
            if (ImGui::Button("Clear Debug", ImVec2(-1, 0))) g_debugLogs.clear();
            ImGui::Columns(1);
            ImGui::EndChild();
        }

        if (showDebug) {
            ImGui::BeginChild("DebugOutput", ImVec2(0, 95), true);
            {
                std::lock_guard<std::mutex> lock(g_debugMutex);
                int start = (int)g_debugLogs.size() - 15;
                if (start < 0) start = 0;
                for (int i = start; i < (int)g_debugLogs.size(); i++) {
                    const auto& e = g_debugLogs[i];
                    bool show = true;
                    if (!debugPackets && e.message.find("[PACKET]") != std::string::npos) show = false;
                    if (!debugCallbacks && e.message.find("[CALLBACK]") != std::string::npos) show = false;
                    if (!debugTimer && e.message.find("[TIMER]") != std::string::npos) show = false;
                    if (!debugPathfinding && e.message.find("[PATH]") != std::string::npos) show = false;
                    if (!debugInventory && e.message.find("[INV]") != std::string::npos) show = false;
                    if (!debugPlayers && e.message.find("[PLAYER]") != std::string::npos) show = false;
                    if (show) ImGui::TextUnformatted(e.message.c_str());
                }
            }
            ImGui::EndChild();
        }

        if (showConsole) {
            ImGui::BeginChild("Console", ImVec2(0, 115), true);
            {
                std::lock_guard<std::mutex> lock(g_consoleMutex);
                int start = (int)g_consoleLogs.size() - 15;
                if (start < 0) start = 0;
                for (int i = start; i < (int)g_consoleLogs.size(); i++) {
                    const auto& e = g_consoleLogs[i];
                    if (e.message.find("[ERROR]") != std::string::npos)
                        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", e.message.c_str());
                    else if (e.message.find("[INFO]") != std::string::npos)
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1, 1), "%s", e.message.c_str());
                    else
                        ImGui::TextUnformatted(e.message.c_str());
                }
            }
            if (autoScroll) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }

        ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    return o_wglSwapBuffers(hdc);
}
