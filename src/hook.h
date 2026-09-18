#pragma once
#include <windows.h>

typedef BOOL(WINAPI* wglSwapBuffers_t)(HDC hdc);
extern wglSwapBuffers_t o_wglSwapBuffers;
BOOL WINAPI hk_wglSwapBuffers(HDC hdc);

extern bool g_Initialized;
extern HWND g_GameHWND;
extern bool g_MenuOpen;
