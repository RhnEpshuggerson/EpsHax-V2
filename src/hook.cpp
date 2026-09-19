#include "hook.h"
#include "lua_api.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_opengl3.h>
#include <gl/GL.h>

#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <chrono>
#include <sstream>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Globals ──────────────────────────────────────────────────────────
wglSwapBuffers_t o_wglSwapBuffers = nullptr;
bool g_Initialized = false;
HWND g_GameHWND = nullptr;
bool g_MenuOpen = false;
WNDPROC oWndProc = nullptr;

// ── Keyboard hook (F1 only) ──────────────────────────────────────────
static HHOOK g_KeyHook = nullptr;

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
static bool debugPackets = false;
static bool debugCallbacks = true;
static bool debugTimer = true;
static bool debugPathfinding = false;
static bool debugInventory = false;
static bool debugPlayers = false;
static float g_TickInterval = 1.0f;
static char scriptBuf[16384] = "";
static LuaExecutor* g_executor = nullptr;

// ── GameState ────────────────────────────────────────────────────────
GameState& GameState::instance() {
    static GameState inst;
    return inst;
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

static std::map<std::string, std::string> parseTextLines(const std::string& text) {
    std::map<std::string, std::string> kv;
    std::istringstream stream(text);
    std::string line;
    int idx = 0;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;
        size_t pipe = line.find('|');
        if (pipe != std::string::npos) {
            std::string key = trim(line.substr(0, pipe));
            std::string val = trim(line.substr(pipe + 1));
            kv[key] = val;
        } else {
            kv["_" + std::to_string(idx)] = line;
        }
        idx++;
    }
    return kv;
}

static float safeFloat(const std::string& s, float def = 0) {
    try { return std::stof(s); } catch (...) { return def; }
}

static int safeInt(const std::string& s, int def = 0) {
    try { return std::stoi(s); } catch (...) { return def; }
}

void GameState::parseTextPacket(const std::string& text, bool incoming) {
    if (text.size() < 4) return;

    auto kv = parseTextLines(text);
    std::string action = kv.count("action") ? kv["action"] : "";

    // Always log incoming text packet action for debugging
    if (incoming) {
        std::string preview = text.substr(0, 150);
        for (auto& c : preview) { if (c == '\n') c = '|'; }
        consoleLog("[IN] action=[" + action + "] " + preview);
    }

    if (action == "spawn" || action == "on_spawn") {
        std::lock_guard<std::mutex> lock(mtx);
        if (incoming) {
            PlayerData p;
            p.name = kv.count("name") ? kv["name"] : "Unknown";
            p.country = kv.count("country") ? kv["country"] : "us";
            p.netid = safeInt(kv.count("NetID") ? kv["NetID"] : (kv.count("netID") ? kv["netID"] : "0"));
            p.userid = safeInt(kv.count("UserID") ? kv["UserID"] : "0");
            p.pos_x = safeFloat(kv.count("posX") ? kv["posX"] : "0");
            p.pos_y = safeFloat(kv.count("posY") ? kv["posY"] : "0");
            p.size_x = safeFloat(kv.count("sizeX") ? kv["sizeX"] : "0");
            p.size_y = safeFloat(kv.count("sizeY") ? kv["sizeY"] : "0");
            p.world = kv.count("world") ? kv["world"] : "";

            debugLog("[SPAWN] name=" + p.name + " world=" + p.world +
                " netid=" + std::to_string(p.netid));

            bool found = false;
            for (auto& existing : players) {
                if (existing.netid == p.netid) {
                    existing = p;
                    found = true;
                    break;
                }
            }
            if (!found) players.push_back(p);
        }
        return;
    }

    if (action == "on_varlist") {
        std::lock_guard<std::mutex> lock(mtx);
        std::string msg = kv.count("msg") ? kv["msg"] : "";

        debugLog("[VARLIST] msg=" + msg);

        if (msg == "OnSetBux" || msg.find("SetBux") != std::string::npos) {
            if (kv.count("1")) localPlayer.gems = safeInt(kv["1"]);
        }

        PacketEvent ev;
        ev.type = "OnVarlist";
        ev.text = text;
        pushEvent(ev);
        return;
    }

    if (action == "set_field_init" || action == "set_field_update") {
        debugLog("[FIELD] type=" + (kv.count("type") ? kv["type"] : "?") +
            " value=" + (kv.count("value") ? kv["value"] : "?"));
        std::lock_guard<std::mutex> lock(mtx);
        if (kv.count("type")) {
            std::string fieldType = kv["type"];
            if (fieldType == "gems" && kv.count("value")) {
                localPlayer.gems = safeInt(kv["value"]);
            }
            if (fieldType == "world" && kv.count("value")) {
                localPlayer.world = kv["value"];
            }
        }
        if (kv.count("world_name")) localPlayer.world = kv["world_name"];
        if (kv.count("width")) world_size_x = safeInt(kv["width"]);
        if (kv.count("height")) world_size_y = safeInt(kv["height"]);
        return;
    }

    if (action == "on_requestWorldSelectMenu" || action == "on_killed" || action == "on_disconnect") {
        std::lock_guard<std::mutex> lock(mtx);
        players.clear();
        localPlayer = PlayerData();
        return;
    }

    if (action == "on_chat_message" || action == "on_console_message") {
        PacketEvent ev;
        ev.type = "OnVarlist";
        ev.text = text;
        pushEvent(ev);
        return;
    }

    if (action.find("on_") == 0 || action.find("action|") == 0) {
        PacketEvent ev;
        ev.type = incoming ? "OnVarlist" : "OnPacket";
        ev.text = text;
        pushEvent(ev);
        return;
    }

    if (incoming) {
        PacketEvent ev;
        ev.type = "OnVarlist";
        ev.text = text;
        pushEvent(ev);
    }
}

void GameState::parseIncoming(const char* data, int len) {
    if (len < 4) return;

    uint32_t header = *(uint32_t*)data;
    int pktType = header & 0xFF;

    if (pktType == 4) {
        std::string text(data + 4, len - 4);
        if (text.size() > 2) {
            parseTextPacket(text, true);
        }
        return;
    }

    if (pktType == 1 || pktType == 2 || pktType == 3) {
        PacketEvent ev;
        ev.type = "OnRawPacket";
        ev.packet_type = pktType;
        if (len >= 16) ev.netid = *(int*)(data + 8);
        if (len >= 20) ev.item = *(int*)(data + 12);
        if (len >= 28) { ev.pos_x = *(float*)(data + 16); ev.pos_y = *(float*)(data + 20); }
        if (len >= 36) { ev.pos2_x = *(float*)(data + 24); ev.pos2_y = *(float*)(data + 28); }
        if (len >= 24) ev.flags = *(int*)(data + 20);

        debugLog("[PKT RAW] type=" + std::to_string(pktType) + " netid=" + std::to_string(ev.netid));

        if (pktType == 1) {
            std::lock_guard<std::mutex> lock(mtx);
            for (auto& p : players) {
                if (p.netid == ev.netid) {
                    p.pos_x = ev.pos_x;
                    p.pos_y = ev.pos_y;
                    p.flags = ev.flags;
                    p.tile_x = (int)(ev.pos_x / 32);
                    p.tile_y = (int)(ev.pos_y / 32);
                    break;
                }
            }
            if (ev.netid == localPlayer.netid || ev.netid == -1) {
                localPlayer.pos_x = ev.pos_x;
                localPlayer.pos_y = ev.pos_y;
                localPlayer.flags = ev.flags;
                localPlayer.tile_x = (int)(ev.pos_x / 32);
                localPlayer.tile_y = (int)(ev.pos_y / 32);
            }
        }

        pushEvent(ev);
        return;
    }
}

void GameState::parseOutgoing(const char* data, int len) {
    if (len < 4) return;

    uint32_t header = *(uint32_t*)data;
    int pktType = header & 0xFF;

    if (pktType == 4) {
        std::string text(data + 4, len - 4);
        if (text.size() > 2) {
            parseTextPacket(text, false);
        }
        return;
    }

    if (pktType == 1 || pktType == 2 || pktType == 3) {
        PacketEvent ev;
        ev.type = "OnPacket";
        ev.packet_type = pktType;
        if (len >= 16) ev.netid = *(int*)(data + 8);
        if (len >= 20) ev.item = *(int*)(data + 12);
        if (len >= 28) { ev.pos_x = *(float*)(data + 16); ev.pos_y = *(float*)(data + 20); }
        debugLog("[PKT OUT RAW] type=" + std::to_string(pktType));
        pushEvent(ev);
    }
}

void GameState::pushEvent(const PacketEvent& ev) {
    std::lock_guard<std::mutex> lock(mtx);
    events.push(ev);
    if (events.size() > 500) {
        std::queue<PacketEvent> empty;
        std::swap(events, empty);
    }
}

bool GameState::popEvent(PacketEvent& ev) {
    std::lock_guard<std::mutex> lock(mtx);
    if (events.empty()) return false;
    ev = events.front();
    events.pop();
    return true;
}

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
            ImGui::BeginChild("Settings", ImVec2(0, 95), true);
            ImGui::Text("Tick Interval: %.1fs", g_TickInterval);
            ImGui::SameLine();
            if (ImGui::SliderFloat("##tick", &g_TickInterval, 0.1f, 5.0f, "%.1fs")) {
                if (g_executor) g_executor->setTickInterval(g_TickInterval);
            }
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

    // Tick executor from render thread
    if (g_executor && g_executor->isRunning()) {
        static float lastTick = 0;
        float dt = g_currentTime - lastTick;
        if (dt >= g_TickInterval) {
            lastTick = g_currentTime;
            g_executor->tick(dt);
        }
    }

    // Retry socket hooks if not installed yet
    if (!g_SocketHooksInstalled) {
        static int retryCount = 0;
        if (++retryCount % 60 == 0) TryInstallSocketHooks();
    }

    return o_wglSwapBuffers(hdc);
}
