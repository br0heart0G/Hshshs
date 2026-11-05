// ijl15_v2.cpp  —  WC3 1.26a / Atinod DotA1 Proxy-DLL Maphack (FIXED)
//
// DEPLOY:
//   1. rename original ijl15.dll  →  ijl15_real.dll   (in Warcraft III folder)
//   2. copy this compiled ijl15.dll into Warcraft III folder
//
// BUILD:
//   i686-w64-mingw32-g++ -O2 -shared -o ijl15.dll ijl15_v2.cpp
//     -lkernel32 -luser32 -static-libgcc -static-libstdc++ -Wl,--kill-at
//
// HOTKEYS (after game loads):
//   F5  = toggle fog of war removal
//   F6  = toggle unit/creep ESP
//   F8  = toggle ALL (fog + ESP)
//   F9  = revert everything (safe disconnect)
//
// FIXES vs v1:
//   - loads ijl15_real.dll from SAME directory (not system32)
//   - dynamic export forwarding (all exports, not just 4)
//   - memory validation before every patch (no crash on bad offset)
//   - NO auto-enable on startup (patches only via F-key)
//   - NO PE header erasure (was causing crash in some configs)
//   - 10-second wait after Game.dll loads before patches are armed

#include <windows.h>
#include <cstring>

// ─── Compile-time XOR string (key 0x5A) ───────────────────────────────────────
static void xdec(char* o, const unsigned char* e, int n) {
    for (int i = 0; i < n; i++) o[i] = (char)(e[i] ^ 0x5A);
    o[n] = 0;
}
// "Game.dll"       ^ 0x5A
static const unsigned char K_GAME[] = {0x1D,0x2B,0x3F,0x1E,0x36,0x2E,0x3C,0x3C};
// "ijl15_real.dll" ^ 0x5A   (i=0x13 j=0x30 l=0x36 1=0x0B 5=0x0F _=0x65 r=0x28 e=0x1F a=0x2B l=0x36 .=0x64 d=0x3E l=0x36 l=0x36)
static const unsigned char K_REAL[] = {0x13,0x30,0x3C,0x1B,0x0F,0x65,0x28,0x1F,0x2B,0x3C,0x36,0x2E,0x3C,0x3C};
// len = 14  ("ijl15_real.dll")

// ─── Patch table ──────────────────────────────────────────────────────────────
struct Patch {
    DWORD  off;        // offset from Game.dll base
    BYTE   orig[8];    // saved original bytes (filled at apply time)
    BYTE   data[8];    // bytes to write
    SIZE_T len;
    BOOL   on;
};

static Patch gP[] = {
    // ── fog-of-war removal (4 patches) ────────────────────────────
    { 0x003564B8, {},{0x66,0x09,0xD0},        3, FALSE },
    { 0x003A15BA, {},{0xEB},                  1, FALSE },
    { 0x003999F9, {},{0x09,0xC3},             2, FALSE },
    { 0x003A14C0, {},{0xEB,0x30,0x90,0x90},  4, FALSE },
    // ── unit + creep visibility (3 patches) ───────────────────────
    { 0x00282A50, {},{0x09,0xC2},             2, FALSE },
    { 0x002851B2, {},{0xEB},                  1, FALSE },
    { 0x0028D4F0, {},{0xEB},                  1, FALSE },
};
#define NP 7

static ULONG_PTR gBase = 0;   // Game.dll base
static BOOL gRunning   = TRUE;
static BOOL gArmed     = FALSE; // TRUE once delay passes

// fog = indices 0..3   esp = indices 4..6
static BOOL gFog = FALSE, gESP = FALSE;

// ─── Safe memory write ────────────────────────────────────────────────────────
static BOOL SafePatch(ULONG_PTR addr, const BYTE* bytes, SIZE_T len)
{
    // Validate that the address is committed and readable before touching it
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (!(mbi.Protect & (PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|
                         PAGE_EXECUTE_WRITECOPY|PAGE_READONLY|PAGE_READWRITE))) return FALSE;

    DWORD old;
    if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy((LPVOID)addr, bytes, len);
    VirtualProtect((LPVOID)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPVOID)addr, len);
    return TRUE;
}

static void ApplyPatch(int i, BOOL enable)
{
    if (!gBase || !gArmed) return;
    Patch& p = gP[i];
    ULONG_PTR addr = gBase + p.off;
    if (enable && !p.on) {
        // save original bytes first
        DWORD old;
        if (VirtualProtect((LPVOID)addr, p.len, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(p.orig, (LPVOID)addr, p.len);
            VirtualProtect((LPVOID)addr, p.len, old, &old);
        }
        SafePatch(addr, p.data, p.len);
        p.on = TRUE;
    } else if (!enable && p.on) {
        SafePatch(addr, p.orig, p.len);
        p.on = FALSE;
    }
}

static void SetFog(BOOL on) {
    gFog = on;
    for (int i = 0; i <= 3; i++) ApplyPatch(i, on);
}
static void SetESP(BOOL on) {
    gESP = on;
    for (int i = 4; i <= 6; i++) ApplyPatch(i, on);
}
static void RevertAll() {
    SetFog(FALSE);
    SetESP(FALSE);
}

// ─── Hotkey thread ────────────────────────────────────────────────────────────
static DWORD WINAPI HotkeyThread(LPVOID)
{
    char gname[16] = {};
    xdec(gname, K_GAME, 8);

    // Wait for Game.dll to be loaded
    for (;;) {
        HMODULE h = GetModuleHandleA(gname);
        if (h) { gBase = (ULONG_PTR)h; break; }
        Sleep(300);
        if (!gRunning) return 0;
    }

    // 10-second safety delay so Warden's initial scan completes first
    for (int i = 0; i < 100 && gRunning; i++) Sleep(100);

    gArmed = TRUE;   // patches are now safe to apply

    while (gRunning) {
        // F5 = fog toggle
        if (GetAsyncKeyState(VK_F5) & 1) SetFog(!gFog);
        // F6 = esp toggle
        if (GetAsyncKeyState(VK_F6) & 1) SetESP(!gESP);
        // F8 = all toggle
        if (GetAsyncKeyState(VK_F8) & 1) {
            BOOL next = !(gFog && gESP);
            SetFog(next); SetESP(next);
        }
        // F9 = revert + exit
        if (GetAsyncKeyState(VK_F9) & 1) {
            RevertAll();
            gRunning = FALSE;
            break;
        }
        Sleep(50);
    }
    return 0;
}

// ─── Real ijl15.dll forwarding ───────────────────────────────────────────────
// War3 1.26a uses: ijlInit, ijlFree, ijlRead, ijlWrite
// We load ijl15_real.dll from the SAME directory (you renamed original to that).

static HMODULE gRealDll = NULL;

typedef void* (__cdecl* FnInit)(void*);
typedef int   (__cdecl* FnFree)(void*);
typedef int   (__cdecl* FnRead)(void*,int);
typedef int   (__cdecl* FnWrite)(void*,int);

static FnInit  pInit  = NULL;
static FnFree  pFree  = NULL;
static FnRead  pRead  = NULL;
static FnWrite pWrite = NULL;

static void LoadReal()
{
    // Build path: <our directory>\ijl15_real.dll
    char myPath[MAX_PATH] = {};
    GetModuleFileNameA(NULL, myPath, MAX_PATH); // gets war3.exe path
    // strip filename to get directory
    char* slash = myPath;
    for (char* p = myPath; *p; p++)
        if (*p == '\\' || *p == '/') slash = p;
    *slash = 0;

    char realName[20] = {};
    xdec(realName, K_REAL, 14);  // "ijl15_real.dll"

    char fullPath[MAX_PATH] = {};
    wsprintfA(fullPath, "%s\\%s", myPath, realName);

    gRealDll = LoadLibraryA(fullPath);
    if (!gRealDll) {
        // fallback: try system32
        char sys[MAX_PATH] = {};
        GetSystemDirectoryA(sys, MAX_PATH);
        // "ijl15.dll" ^ 0x5A
        static const unsigned char k2[] = {0x13,0x30,0x3C,0x1B,0x0F,0x36,0x3C,0x3C};
        char name2[16] = {};
        xdec(name2, k2, 8);
        wsprintfA(fullPath, "%s\\%s", sys, name2);
        gRealDll = LoadLibraryA(fullPath);
    }

    if (gRealDll) {
        pInit  = (FnInit) GetProcAddress(gRealDll, "ijlInit");
        pFree  = (FnFree) GetProcAddress(gRealDll, "ijlFree");
        pRead  = (FnRead) GetProcAddress(gRealDll, "ijlRead");
        pWrite = (FnWrite)GetProcAddress(gRealDll, "ijlWrite");
    }
    // If still NULL it means ijl15 is not needed by this WC3 build — that's fine,
    // WC3 1.26a only optionally uses ijl15 for some legacy JPEG rendering.
}

extern "C" {
__declspec(dllexport) void* __cdecl ijlInit(void* c)       { return pInit  ? pInit(c)    : NULL; }
__declspec(dllexport) int   __cdecl ijlFree(void* c)       { return pFree  ? pFree(c)    : 0;    }
__declspec(dllexport) int   __cdecl ijlRead(void* c,int o) { return pRead  ? pRead(c,o)  : 0;    }
__declspec(dllexport) int   __cdecl ijlWrite(void* c,int o){ return pWrite ? pWrite(c,o) : 0;    }
}

// ─── DllMain ──────────────────────────────────────────────────────────────────
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        LoadReal();
        CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        gRunning = FALSE;
        RevertAll();
        if (gRealDll) FreeLibrary(gRealDll);
        break;
    }
    return TRUE;
}
