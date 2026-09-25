#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include "discord_rpc.h"

int main() {
    puts("[test] EpsHax Discord presence test - open your Discord profile now");
    discordrpc::Start([](const char* m) {
        printf("[rpc] %s\n", m);
        fflush(stdout);
    });
    puts("[test] waiting 30s (presence clears when this process exits)...");
    Sleep(30000);
    puts("[test] done");
    return 0;
}
