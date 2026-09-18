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
#include <WinUser.h>

float g_currentTime = 0;
bool g_debugMode = false;
std::mutex g_debugMutex;
std::vector<LogEntry> g_debugLogs;
bool g_menuOpen = true;

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

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

// ── Find Growtopia window ────────────────────────────────────────────
HWND FindGrowtopia() {
    HWND hwnd = nullptr;
    hwnd = FindWindowA(nullptr, "Growtopia");
    if (!hwnd) hwnd = FindWindowA(nullptr, "Growtopia by Robinson Technologies");
    if (!hwnd) hwnd = FindWindowA(nullptr, "Growtopia");
    return hwnd;
}

// ── ImGui Overlay Thread ─────────────────────────────────────────────
void OverlayThread(HMODULE hModule) {
    // Wait for Growtopia
    HWND growtopia = nullptr;
    while (!growtopia) {
        growtopia = FindGrowtopia();
        Sleep(500);
    }
    Sleep(1000); // Let growtopia fully load

    // Init GLFW for overlay
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

    // Get growtopia position and size
    RECT rect;
    GetWindowRect(growtopia, &rect);
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    GLFWwindow* window = glfwCreateWindow(w, h, "Coems Executor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    glfwSetWindowPos(window, rect.left, rect.top);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    HWND overlay_hwnd = glfwGetWin32Window(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Set clipboard callbacks
    io.SetClipboardTextFn = ClipboardSetText;
    io.GetClipboardTextFn = ClipboardGetText;

    // Dark style
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Alpha = 0.95f;
    style.WindowBorderSize = 1.0f;

    ImGui_ImplGlfw_InitForOpenGL(window, false);
    ImGui_ImplOpenGL3_Init("#version 330");

    LuaExecutor executor;
    char scriptBuf[16384] = "";
    bool autoScroll = true;
    bool showConsole = true;
    bool showDebug = true;
    bool showSettings = false;

    // Settings
    bool debugPackets = true;
    bool debugCallbacks = true;
    bool debugTimer = true;
    bool debugPathfinding = true;
    bool debugInventory = true;
    bool debugPlayers = true;

    // Set overlay to interactive initially
    SetWindowLongA(overlay_hwnd, GWL_EXSTYLE,
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    BringWindowToTop(overlay_hwnd);

    auto t0 = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        // Follow growtopia
        if (IsWindow(growtopia)) {
            RECT r;
            GetWindowRect(growtopia, &r);
            int nw = r.right - r.left;
            int nh = r.bottom - r.top;
            glfwSetWindowPos(window, r.left, r.top);
            glfwSetWindowSize(window, nw, nh);
        } else {
            break;
        }

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
                BringWindowToTop(overlay_hwnd);
                SetForegroundWindow(overlay_hwnd);
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

            // ── Menu Bar ──────────────────────────────────────────
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("Tools")) {
                    ImGui::MenuItem("Console", nullptr, &showConsole);
                    ImGui::MenuItem("Debug Output", nullptr, &showDebug);
                    ImGui::MenuItem("Settings", nullptr, &showSettings);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }

            // ── Toolbar ───────────────────────────────────────────
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

            // ── Layout ────────────────────────────────────────────
            float bottomHeight = 0;
            if (showConsole) bottomHeight += 120;
            if (showDebug) bottomHeight += 100;
            if (showSettings) bottomHeight += 80;
            float editorHeight = ImGui::GetContentRegionAvail().y - bottomHeight;
            if (editorHeight < 80) editorHeight = 80;

            // Script editor
            ImGui::BeginChild("Editor", ImVec2(0, editorHeight), true);
            ImGui::InputTextMultiline("##script", scriptBuf, sizeof(scriptBuf),
                ImVec2(-1, -1), ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_NoHorizontalScroll);
            ImGui::EndChild();

            // Settings
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
                if (ImGui::Button("Clear Debug", ImVec2(-1, 0))) {
                    g_debugLogs.clear();
                }
                ImGui::Columns(1);
                ImGui::EndChild();
            }

            // Debug output
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
                        if (show) {
                            ImGui::TextUnformatted(entry.message.c_str());
                        }
                    }
                }
                ImGui::EndChild();
            }

            // Console
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
