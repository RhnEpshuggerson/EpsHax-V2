#include "lua_api.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <thread>
#include <mutex>
#include <windows.h>

float g_currentTime = 0;
bool g_debugMode = false;
std::mutex g_debugMutex;
std::vector<LogEntry> g_debugLogs;
bool g_menuOpen = false;
bool g_hookActive = false;

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

// ── Keyboard hook ────────────────────────────────────────────────────
static int VkToImGuiKey(int vk) {
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

static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        int vk = kb->vkCode;

        // Always let F1 through
        if (vk == VK_F1) return CallNextHookEx(nullptr, nCode, wParam, lParam);

        // Let system keys through (PrintScreen, Alt+Tab, Win key, Ctrl+Esc, etc.)
        if (vk == VK_SNAPSHOT || vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_APPS || vk == VK_SLEEP || vk == VK_VOLUME_MUTE ||
            vk == VK_VOLUME_DOWN || vk == VK_VOLUME_UP ||
            vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK ||
            vk == VK_MEDIA_STOP || vk == VK_MEDIA_PLAY_PAUSE ||
            vk == VK_LAUNCH_MAIL || vk == VK_LAUNCH_MEDIA_SELECT) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // Alt+Tab, Alt+F4, Ctrl+Esc, Ctrl+Shift+Esc
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (alt && (vk == VK_TAB || vk == VK_F4)) return CallNextHookEx(nullptr, nCode, wParam, lParam);
        if (ctrl && vk == VK_ESCAPE) return CallNextHookEx(nullptr, nCode, wParam, lParam);
        if (ctrl && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && vk == VK_ESCAPE) return CallNextHookEx(nullptr, nCode, wParam, lParam);

        // Only process when menu is open
        if (g_menuOpen && g_hookActive) {
            bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

            if (isDown || isUp) {
                ImGuiIO& io = ImGui::GetIO();
                ImGuiKey imguiKey = (ImGuiKey)VkToImGuiKey(vk);
                if (imguiKey != ImGuiKey_None) {
                    io.AddKeyEvent(imguiKey, isDown);
                }

                // Ctrl+V paste
                if (io.KeyCtrl && isDown && vk == 'V') {
                    if (OpenClipboard(nullptr)) {
                        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                        if (hData) {
                            wchar_t* text = (wchar_t*)GlobalLock(hData);
                            if (text) {
                                for (int i = 0; text[i] != 0; i++) {
                                    if (text[i] <= 127) {
                                        io.AddInputCharacter((unsigned char)text[i]);
                                    }
                                }
                                GlobalUnlock(hData);
                            }
                        }
                        CloseClipboard();
                    }
                }

                // Character input (letters, numbers, symbols)
                if (isDown && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
                    if (vk >= 0x20 && vk <= 0x7E) {
                        char c = (char)vk;
                        if (vk >= 'A' && vk <= 'Z' && !GetAsyncKeyState(VK_SHIFT)) {
                            c = c + 32;
                        }
                        if (io.KeyShift) {
                            const char* shifted = "~!@#$%^&*()_+{}|:\"<>?ASDFGHJKLQWERTYUIOPZXCVBNM";
                            const char* normal = "`1234567890-=[]\\;',./asdfghjklqwertyuiopzxcvbnm";
                            for (int i = 0; normal[i]; i++) {
                                if (c == normal[i]) { c = shifted[i]; break; }
                            }
                        }
                        io.AddInputCharacter((unsigned char)c);
                    }
                }

                // Block keyboard from reaching Growtopia
                if (isDown && vk != VK_F1) {
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static HHOOK g_keyboardHook = nullptr;

// ── Clipboard callbacks ──────────────────────────────────────────────
static const char* ClipboardGetText(void*) {
    if (!OpenClipboard(nullptr)) return "";
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return ""; }
    char* text = (char*)GlobalLock(hData);
    if (!text) { CloseClipboard(); return ""; }
    static std::string clipStr;
    clipStr = text;
    GlobalUnlock(hData);
    CloseClipboard();
    return clipStr.c_str();
}

static void ClipboardSetText(void*, const char* text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    size_t len = strlen(text) + 1;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
    memcpy(GlobalLock(hMem), text, len);
    GlobalUnlock(hMem);
    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
}

// ── Helpers ──────────────────────────────────────────────────────────
void GetGrowtopiaClientPos(HWND hwnd, int& x, int& y, int& w, int& h) {
    RECT client;
    GetClientRect(hwnd, &client);
    POINT topLeft = { client.left, client.top };
    ClientToScreen(hwnd, &topLeft);
    x = topLeft.x;
    y = topLeft.y;
    w = client.right - client.left;
    h = client.bottom - client.top;
}

HWND FindGrowtopia() {
    HWND hwnd = nullptr;
    hwnd = FindWindowA(nullptr, "Growtopia");
    if (!hwnd) hwnd = FindWindowA(nullptr, "Growtopia by Robinson Technologies");
    return hwnd;
}

// ── Overlay Thread ───────────────────────────────────────────────────
void OverlayThread(HMODULE hModule) {
    HWND growtopia = nullptr;
    while (!growtopia) {
        growtopia = FindGrowtopia();
        Sleep(500);
    }
    Sleep(1000);

    if (!glfwInit()) {
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_MOUSE_PASSTHROUGH, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);

    int ox, oy, ow, oh;
    GetGrowtopiaClientPos(growtopia, ox, oy, ow, oh);

    GLFWwindow* window = glfwCreateWindow(ow, oh, "Coems Executor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    glfwSetWindowPos(window, ox, oy);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    HWND overlay_hwnd = glfwGetWin32Window(window);

    // Start hidden (passthrough) — user presses F1 to show
    g_menuOpen = false;
    SetWindowLongA(overlay_hwnd, GWL_EXSTYLE,
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    io.SetClipboardTextFn = ClipboardSetText;
    io.GetClipboardTextFn = ClipboardGetText;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Alpha = 0.95f;
    style.WindowBorderSize = 1.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Install keyboard hook
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, GetModuleHandleW(nullptr), 0);
    g_hookActive = true;

    LuaExecutor executor;
    char scriptBuf[16384] = "";
    bool autoScroll = true;
    bool showConsole = true;
    bool showDebug = true;
    bool showSettings = false;
    bool debugPackets = true;
    bool debugCallbacks = true;
    bool debugTimer = true;
    bool debugPathfinding = true;
    bool debugInventory = true;
    bool debugPlayers = true;

    auto t0 = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        // Check if Growtopia is still alive
        if (!IsWindow(growtopia)) {
            break;
        }

        // Sync position and size with Growtopia client area
        int nx, ny, nw, nh;
        GetGrowtopiaClientPos(growtopia, nx, ny, nw, nh);
        glfwSetWindowPos(window, nx, ny);
        glfwSetWindowSize(window, nw, nh);

        glfwPollEvents();

        auto now = std::chrono::steady_clock::now();
        g_currentTime = std::chrono::duration<float>(now - t0).count();

        // F1 toggle
        static bool f1WasDown = false;
        bool f1Down = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1Down && !f1WasDown) {
            g_menuOpen = !g_menuOpen;
            if (g_menuOpen) {
                SetWindowLongA(overlay_hwnd, GWL_EXSTYLE,
                    WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
                SetWindowPos(overlay_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(overlay_hwnd);
                BringWindowToTop(overlay_hwnd);
            } else {
                SetWindowLongA(overlay_hwnd, GWL_EXSTYLE,
                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
                SetForegroundWindow(growtopia);
            }
        }
        f1WasDown = f1Down;

        // ImGui new frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (g_menuOpen) {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
            ImGui::Begin("Coems Executor  |  F1 to toggle##executor", &g_menuOpen,
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

            // Toolbar
            bool running = executor.isRunning();

            ImGui::PushStyleColor(ImGuiCol_Button, running
                ? ImVec4(0.6f, 0.2f, 0.2f, 1.0f)
                : ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
            if (ImGui::Button(running ? "Running..." : "Execute", ImVec2(100, 25)) && !running) {
                g_consoleLogs.clear();
                g_debugLogs.clear();
                std::string script(scriptBuf);
                debugLog("[SYSTEM] Executing script...");
                std::thread t([&executor, script]() {
                    executor.execute(script);
                });
                t.detach();
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(70, 25))) {
                executor.stop();
                debugLog("[SYSTEM] Script stopped.");
            }
            ImGui::PopStyleColor();
            ImGui::SameLine();

            if (ImGui::Button("Clear")) {
                g_consoleLogs.clear();
                g_debugLogs.clear();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &autoScroll);
            ImGui::SameLine(0, 20);

            if (running) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "RUNNING");
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "IDLE");
            }

            ImGui::Separator();

            // Layout
            float bottomHeight = 0;
            if (showConsole) bottomHeight += 120;
            if (showDebug) bottomHeight += 100;
            if (showSettings) bottomHeight += 80;
            float editorHeight = ImGui::GetContentRegionAvail().y - bottomHeight;
            if (editorHeight < 80) editorHeight = 80;

            ImGui::BeginChild("Editor", ImVec2(0, editorHeight), true);
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
                        const auto& entry = g_debugLogs[i];
                        bool show = true;
                        if (!debugPackets && entry.message.find("[PACKET]") != std::string::npos) show = false;
                        if (!debugCallbacks && entry.message.find("[CALLBACK]") != std::string::npos) show = false;
                        if (!debugTimer && entry.message.find("[TIMER]") != std::string::npos) show = false;
                        if (!debugPathfinding && entry.message.find("[PATH]") != std::string::npos) show = false;
                        if (!debugInventory && entry.message.find("[INV]") != std::string::npos) show = false;
                        if (!debugPlayers && entry.message.find("[PLAYER]") != std::string::npos) show = false;
                        if (show) ImGui::TextUnformatted(entry.message.c_str());
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
                        const auto& entry = g_consoleLogs[i];
                        if (entry.message.find("[ERROR]") != std::string::npos) {
                            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", entry.message.c_str());
                        } else if (entry.message.find("[INFO]") != std::string::npos) {
                            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1, 1), "%s", entry.message.c_str());
                        } else {
                            ImGui::TextUnformatted(entry.message.c_str());
                        }
                    }
                }
                if (autoScroll) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
            }

            ImGui::End();
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    g_hookActive = false;
    g_menuOpen = false;
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    executor.stop();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    FreeLibraryAndExitThread(hModule, 0);
}

// ── DLL Entry Point ──────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)OverlayThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
