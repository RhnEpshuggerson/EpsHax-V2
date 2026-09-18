# Coems Executor

Lua executor with ImGui GUI for Growtopia scripts.

## Build Instructions (Windows)

### Prerequisites
- CMake 3.20+
- Visual Studio 2019/2022 (or any C++ compiler with C++17 support)
- Git

### Steps

1. Clone ImGui into libs/:
```bash
cd coems_executor
git clone https://github.com/ocornut/imgui.git libs/imgui
```

2. Build:
```bash
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

3. Run:
```
build\Release\coems_executor.exe
```

## Features

- Script editor with syntax highlighting
- Execute / Stop buttons
- Console output panel
- All Growtopia API functions stubbed (SendPacket, GetLocal, GetTile, etc.)

## API Functions Registered

| Function | Status |
|----------|--------|
| SendPacket | Stub |
| SendPacketRaw | Stub |
| SendVarlist | Stub |
| log | Working (console output) |
| GetLocal | Returns mock player data |
| GetInventory | Returns empty table |
| GetPlayers | Returns empty table |
| GetObjects | Returns empty table |
| GetTile | Returns empty tile |
| GetTiles | Returns empty table |
| FindPath | Stub |
| PathFind | Returns empty table |
| CheckPath | Returns true |
| IsSolid | Returns false |
| RunThread | Runs function |
| Sleep | Working |
| GetPing | Returns 42 |
| GetItemCount | Returns 0 |
| GetItemInfo | Returns mock item info |
| MessageBox | Logs to console |
| AddCallback | Stub |
| RemoveCallback | Stub |
| RemoveCallbacks | Stub |
| EditToggle | Logs to console |
| SendWebhook | Logs to console |
| timer.Create | Stub |
| timer.Destroy | Stub |
| timer.Update | Stub |
