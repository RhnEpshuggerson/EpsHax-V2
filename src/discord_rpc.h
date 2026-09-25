#pragma once

namespace discordrpc {

// Starts a background thread that connects to the local Discord client over
// IPC and publishes the EpsHax rich presence. Reconnects automatically if
// Discord restarts. |log| may be null; it is called from the RPC thread.
using LogFn = void (*)(const char* message);
void Start(LogFn log);

} // namespace discordrpc
