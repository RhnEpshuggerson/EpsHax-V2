#include "lua_api.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <mutex>

float g_currentTime = 0;
bool g_debugMode = false;
std::mutex g_debugMutex;
std::vector<LogEntry> g_debugLogs;

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(900, 600, "Coems Executor", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    LuaExecutor executor;

    char scriptBuf[16384] = "";
    std::string currentFilePath;
    bool showConsole = true;
    bool showDebug = false;
    bool showSettings = false;
    bool autoScroll = true;

    // Settings
    bool debugPackets = false;
    bool debugCallbacks = false;
    bool debugTimer = false;
    bool debugPathfinding = false;
    bool debugInventory = false;
    bool debugPlayers = false;

    auto t0 = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        auto now = std::chrono::steady_clock::now();
        g_currentTime = std::chrono::duration<float>(now - t0).count();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Coems Executor", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── Menu Bar ──────────────────────────────────────────────
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Script...")) {}
                if (ImGui::MenuItem("Save Script") && !currentFilePath.empty()) {
                    std::ofstream f(currentFilePath);
                    f << scriptBuf;
                }
                if (ImGui::MenuItem("Save As...")) {}
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Tools")) {
                ImGui::MenuItem("Console", nullptr, &showConsole);
                ImGui::MenuItem("Debug Output", nullptr, &showDebug);
                ImGui::Separator();
                ImGui::MenuItem("Settings", nullptr, &showSettings);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // ── Toolbar ───────────────────────────────────────────────
        bool running = executor.isRunning();

        ImGui::PushStyleColor(ImGuiCol_Button, running
            ? ImVec4(0.6f, 0.2f, 0.2f, 1.0f)
            : ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button(running ? "Running..." : "Execute", ImVec2(100, 0)) && !running) {
            executor.clearLogs();
            g_debugLogs.clear();
            std::string script = scriptBuf;
            debugLog("[SYSTEM] Executing script...");
            std::thread([&executor, script]() {
                executor.execute(script);
            }).detach();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(70, 0))) {
            executor.stop();
            debugLog("[SYSTEM] Script stopped.");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();

        if (ImGui::Button("Clear")) {
            executor.clearLogs();
            g_debugLogs.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &autoScroll);
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();

        // Status indicator
        if (running) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "● Running");
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "○ Idle");
        }

        ImGui::Separator();

        // ── Main layout ──────────────────────────────────────────
        float bottomHeight = 0;
        if (showConsole) bottomHeight += io.DisplaySize.y * 0.25f;
        if (showDebug) bottomHeight += io.DisplaySize.y * 0.20f;
        if (showSettings) bottomHeight += io.DisplaySize.y * 0.18f;
        float editorHeight = io.DisplaySize.y - bottomHeight - 60;
        if (editorHeight < 100) editorHeight = 100;

        // Script editor
        ImGui::BeginChild("Editor", ImVec2(0, editorHeight), true);
        ImGui::InputTextMultiline("##script", scriptBuf, sizeof(scriptBuf),
            ImVec2(-1, -1), ImGuiInputTextFlags_AllowTabInput);
        ImGui::EndChild();

        // ── Settings Panel ────────────────────────────────────────
        if (showSettings) {
            float settingsH = io.DisplaySize.y * 0.18f;
            ImGui::BeginChild("Settings", ImVec2(0, settingsH), true);
            ImGui::Text("Settings");
            ImGui::Separator();

            ImGui::Columns(3, nullptr, false);

            // Column 1: Debug toggles
            ImGui::Text("Debug Filters");
            ImGui::Checkbox("Packets", &debugPackets);
            ImGui::Checkbox("Callbacks", &debugCallbacks);
            ImGui::Checkbox("Timers", &debugTimer);
            ImGui::NextColumn();

            // Column 2: More debug toggles
            ImGui::Text("Advanced");
            ImGui::Checkbox("Pathfinding", &debugPathfinding);
            ImGui::Checkbox("Inventory", &debugInventory);
            ImGui::Checkbox("Players", &debugPlayers);
            ImGui::NextColumn();

            // Column 3: Quick actions
            ImGui::Text("Quick Actions");
            if (ImGui::Button("Clear Debug", ImVec2(-1, 0))) {
                g_debugLogs.clear();
            }
            if (ImGui::Button("Copy Script", ImVec2(-1, 0))) {
                io.SetClipboardTextFn(nullptr, scriptBuf);
            }
            ImGui::Columns(1);

            ImGui::EndChild();
        }

        // ── Debug Output Panel ────────────────────────────────────
        if (showDebug) {
            float debugH = io.DisplaySize.y * 0.20f;
            ImGui::BeginChild("DebugOutput", ImVec2(0, debugH), true);
            ImGui::Text("Debug Output (%d entries)", (int)g_debugLogs.size());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
            if (ImGui::SmallButton("Clear##dbg")) {
                g_debugLogs.clear();
            }
            ImGui::Separator();

            {
                std::lock_guard<std::mutex> lock(g_debugMutex);
                for (const auto& entry : g_debugLogs) {
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

            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        // ── Console Panel ─────────────────────────────────────────
        if (showConsole) {
            float consoleH = io.DisplaySize.y * 0.25f;
            ImGui::BeginChild("Console", ImVec2(0, consoleH), true);
            ImGui::Text("Console");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
            if (ImGui::SmallButton("Clear##con")) {
                executor.clearLogs();
            }
            ImGui::Separator();

            const auto& logs = executor.getLogs();
            for (const auto& entry : logs) {
                if (entry.message.find("[ERROR]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "%s", entry.message.c_str());
                } else if (entry.message.find("[INFO]") != std::string::npos) {
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1, 1), "%s", entry.message.c_str());
                } else {
                    ImGui::TextUnformatted(entry.message.c_str());
                }
            }

            if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
