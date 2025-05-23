#include <windows.h>
#include <string>
#include <imgui.h>
#include <psapi.h>

#include <stdio.h>
#include <vector>
#include <fstream>
#include <thread>
#include <chrono>
#include <detours.h>

#define UPSCALER_FOLDER L"mods\\"
#define SCRIPTEXTENDER_DLL_NAME L"ScriptExtender.dll"
#define OUR_LOG_FILE L"UpscalerProxy.log"
const wchar_t* UPSCALER_LOG_FILES[] = {
    L"BG3Upscaler.log"
};
const wchar_t* DLSS_LOG_FILE = L"dlssg_to_fsr3.log";
const char* UPSCALER_SUCCESS_STRINGS[] = {
    "hk_vkCreateDevice",
    "D3D11CreateDevice",
    "DX11WrapperForDX12 Resource Created"
};
const char* DLSS_SUCCESS_STRING = "NVSDK_NGX_D3D12_CreateFeature: Succeeded.";
     // Global to store addresses of ImGui functions in version.dll (or other)
static void* g_versionAddrs[4] = { nullptr, nullptr, nullptr, nullptr };
HMODULE g_scriptExtenderDll = NULL;

std::ofstream g_logFile;

// Game thread handling
HANDLE g_mainThreadHandle = NULL;
bool g_gameNeedsSuspend = false;

// Event for thread synchronization instead of hard suspending
HANDLE g_syncEvent = NULL;

// Exported function pointers from the script extender
typedef HRESULT(WINAPI* DWriteCreateFactoryFunc)(int factoryType, REFIID iid, IUnknown** factory);
DWriteCreateFactoryFunc g_originalDWriteCreateFactory = NULL;

void LogMessage(const char* format, ...);

// ──────────────────────────────────────────────────────────────────────────────
//  ImGui-related globals
// ──────────────────────────────────────────────────────────────────────────────
static ImGuiContext* g_ImCtx        = nullptr;   // captured from SE
static uint64_t g_TelGetIO   = 0;
static uint64_t g_TelNewFrame= 0;
static uint64_t g_TelRender  = 0;

// Pointers to SE's original functions (set after Detours attaches)
using SE_GetIO_Fn   = ImGuiIO* (__cdecl*)();
using SE_NewFrame_Fn= void (__cdecl*)();
using SE_Render_Fn  = void (__cdecl*)();
static SE_GetIO_Fn    g_SEGetIO    = nullptr;
static SE_NewFrame_Fn g_SENewFrame = nullptr;
static SE_Render_Fn   g_SERender   = nullptr;

using CreateCtxFn = ImGuiContext* (__cdecl*)(ImFontAtlas*);
static CreateCtxFn   RealCreate = nullptr;     

static ImGuiContext* WINAPI Hook_CreateContext(ImFontAtlas* atlas)
{
    ImGuiContext* ctx = RealCreate(atlas);
    if (!g_ImCtx) {
        g_ImCtx = ctx;
        LogMessage("Captured SE ImGuiContext at %p\n", g_ImCtx);
    }
    // Forcefully override GImGui globally
    extern ImGuiContext* GImGui;
    GImGui = g_ImCtx;
    LogMessage("Forced GImGui override to %p\n", GImGui);
    return ctx;
}

void HookCreateContextOnce()
{
    if (!RealCreate) {
        RealCreate = (CreateCtxFn)ImGui::CreateContext;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)RealCreate, Hook_CreateContext);
        DetourTransactionCommit();
        LogMessage("Detours: Hooked ImGui::CreateContext\n");
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  2)  Wrappers exported by DWrite.dll – always restore g_ImCtx
// ──────────────────────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) ImGuiIO* ImGui_GetIO()
{
    ++g_TelGetIO;
    if (g_ImCtx)
        ImGui::SetCurrentContext(g_ImCtx);

    // Call SE's original function if captured
    if (g_SEGetIO)
        return g_SEGetIO();

    LogMessage("ImGui_GetIO called (%d): original not yet hooked\n", g_TelGetIO);
    return nullptr;
}
extern "C" __declspec(dllexport) void ImGui_NewFrame()
{
    ++g_TelNewFrame;
    if (g_ImCtx)
        ImGui::SetCurrentContext(g_ImCtx);

    if (g_SENewFrame) {
        g_SENewFrame();
    } else {
        LogMessage("ImGui_NewFrame called (%d): original not yet hooked\n", g_TelNewFrame);
    }
}
extern "C" __declspec(dllexport) void ImGui_Render()
{
    ++g_TelRender;
    if (g_ImCtx)
        ImGui::SetCurrentContext(g_ImCtx);

    if (g_SERender) {
        g_SERender();
    } else {
        LogMessage("ImGui_Render called (%d): original not yet hooked\n", g_TelRender);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
//  3)  Combined patcher – sig scan first, inline-JMP fall-back
// ──────────────────────────────────────────────────────────────────────────────

// Forward declarations
void PatchImGuiInModule(HMODULE module, bool captureOriginal);
static bool BakeOutImGuiFile(const char* path, BYTE* moduleBase, void* addrs[4]);
static bool BakeOutImGuiCopy(HMODULE module, const char* destPath, void* addrsOverride[4]);

// Helper: naive pattern scan with mask (x = match, ? = wildcard)
static BYTE* FindPattern(BYTE* begin, BYTE* end, const BYTE* pattern, const char* mask, size_t len)
{
    for (BYTE* cur = begin; cur <= end - (ptrdiff_t)len; ++cur)
    {
        bool match = true;
        for (size_t i = 0; i < len; ++i)
        {
            if (mask[i] == 'x' && cur[i] != pattern[i])
            {
                match = false;
                break;
            }
        }
        if (match)
            return cur;
    }
    return nullptr;
}

void LogBytes(const char* label, void* func, size_t len) {
    BYTE* p = (BYTE*)func;
    char buf[256] = {};
    char* out = buf;
    for (size_t i = 0; i < len; ++i)
        out += sprintf_s(out, 256 - (out - buf), "%02X ", p[i]);
    LogMessage("%s: %s\n", label, buf);
}

void PatchSEImGuiCalls()
{
    if (!g_scriptExtenderDll) {
        LogMessage("Script Extender DLL not loaded, cannot patch ImGui calls\n");
        return;
    }
    LogBytes("ImGui::GetIO", (void*)ImGui::GetIO, 16);
    LogBytes("ImGui::NewFrame", (void*)ImGui::NewFrame, 16);
    LogBytes("ImGui::Render", (void*)ImGui::Render, 16);

    // Function signatures to search for (first 8 bytes of each)
    struct Target {
        const char* name;
        BYTE sig[8];
        void* detour;
        void** orig;
    } targets[] = {
        { "GetIO",     {}, (void*)ImGui_GetIO,    nullptr },
        { "NewFrame",  {}, (void*)ImGui_NewFrame, nullptr },
        { "Render",    {}, (void*)ImGui_Render,   nullptr },
    };

    // Fill in signatures from local ImGui functions
    memcpy(targets[0].sig, ImGui::GetIO,    8);
    memcpy(targets[1].sig, ImGui::NewFrame, 8);
    memcpy(targets[2].sig, ImGui::Render,   8);

    // Scan SE .text for those eight-byte prologues
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), g_scriptExtenderDll, &mi, sizeof(mi));
    BYTE* beg = (BYTE*)g_scriptExtenderDll;
    BYTE* end = beg + mi.SizeOfImage;

    void* foundAddrs[3] = { nullptr, nullptr, nullptr };

    for (int i = 0; i < 3; ++i) {
        for (BYTE* cur = beg; cur < end - 8; ++cur) {
            if (memcmp(cur, targets[i].sig, 8) == 0) {
                foundAddrs[i] = (void*)cur;
                LogMessage("Found SE ImGui::%s at %p\n", targets[i].name, cur);
                break;
            }
        }
        if (!foundAddrs[i]) {
            if (i==0 || i==1) {
                LogMessage("Attempting fall-back pattern scan for missing ImGui functions...\n");
        
                // Pattern for GetIO: 48 8B 0D ?? ?? ?? ?? 48 8B 41 38
                const BYTE patGetIO[]  = { 0x48, 0x8B, 0x0D, 0,0,0,0, 0x48, 0x8B, 0x41, 0x38 };
                const char maskGetIO[] = "xxx????xxxx";
        
                // Pattern for NewFrame: 48 89 5C 24 08 57 48 83 EC 20 48 8B 35 ?? ?? ?? ?? 48 8B F9
                const BYTE patNewFrame[]  = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x35,0,0,0,0,0x48,0x8B,0xF9 };
                const char maskNewFrame[] = "xxxxxxxxxxxxx????xxx";
        
                // Additional alternative patterns
                const BYTE patGetIO2[]  = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x83, 0xC0, 0x08, 0xC3 };
                const char maskGetIO2[] = "xxx????xxxxx";
                const BYTE patNewFrame2[] = { 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x57, 0x48, 0x81, 0xEC, 0,0,0,0, 0x48, 0x8B, 0x2D, 0,0,0,0 };
                const char maskNewFrame2[] = "xxxxxxxxx????xxx????";
        
                // Additional fallback pattern 3 – covers other common prologue variants
                const BYTE patGetIO3[]  = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x8B, 0x40, 0x38 };           // mov rax,[rip+disp]; mov rax, [rax+38]
                const char maskGetIO3[] = "xxx????xxxx";
                const BYTE patNewFrame3[] = { 0x48,0x89,0x5C,0x24,0,0x57,0x48,0x83,0xEC,0,0x48,0x8B,0x05 }; // mov [rsp+?],rbx; push rdi; sub rsp,?; mov rax, [rip+disp]
                const char maskNewFrame3[] = "xxxx?xxxx?xxx";
        
                if (i==0) {
                    foundAddrs[0] = FindPattern(beg, end, patGetIO, maskGetIO, sizeof(patGetIO));
                    if (!foundAddrs[0]) {
                        // secondary heuristic for GetIO prologue variant: 48 8B 05 ?? ?? ?? ?? 48 83 C0 08 C3
                        for (BYTE* cur = beg; cur < end - 12; ++cur) {
                            if (cur[0]==0x48 && cur[1]==0x8B && cur[2]==0x05 &&
                                cur[7]==0x48 && cur[8]==0x83 && cur[9]==0xC0 && cur[10]==0x08 && cur[11]==0xC3) {
                                foundAddrs[0] = cur;
                                LogMessage("Secondary: Found SE ImGui::GetIO at %p\n", cur);
                                break;
                            }
                        }
                        if (!foundAddrs[0]) {
                            foundAddrs[0] = FindPattern(beg, end, patGetIO2, maskGetIO2, sizeof(patGetIO2));
                            if (foundAddrs[0]) LogMessage("Alt2: Found SE ImGui::GetIO at %p\n", foundAddrs[0]);
                            if (!foundAddrs[0]) {
                                foundAddrs[0] = FindPattern(beg, end, patGetIO3, maskGetIO3, sizeof(patGetIO3));
                                if (foundAddrs[0]) LogMessage("Alt3: Found SE ImGui::GetIO at %p\n", foundAddrs[0]);
                                if (!foundAddrs[0]) {
                                    // Additional fallback pattern 4 – same as patGetIO2 but tolerant ret (C3/C2 ??)
                                    const BYTE patGetIO4[]  = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x83, 0xC0, 0x08, 0 }; // last byte wildcard
                                    const char maskGetIO4[] = "xxx????xxxxx?";
                                    foundAddrs[0] = FindPattern(beg, end, patGetIO4, maskGetIO4, sizeof(patGetIO4));
                                    if (foundAddrs[0]) LogMessage("Alt4: Found SE ImGui::GetIO at %p\n", foundAddrs[0]);
                                    if (!foundAddrs[0]) {
                                        // Pattern 5 – minimal stub: mov rax,[rip+disp]; ret
                                        const BYTE patGetIO5[]  = { 0x48, 0x8B, 0x05, 0,0,0,0, 0xC3 };
                                        const char maskGetIO5[] = "xxx????x""x"; // eight bytes pattern -> xx???xx?? actually need 8 char; simplifying
                                        foundAddrs[0] = FindPattern(beg, end, patGetIO5, maskGetIO5, sizeof(patGetIO5));
                                        if (foundAddrs[0]) LogMessage("Alt5: Found SE ImGui::GetIO at %p\n", foundAddrs[0]);
                                    }
                                }
                            }
                        }
                    }
                }
                if (i==1) {
                    foundAddrs[1] = FindPattern(beg, end, patNewFrame, maskNewFrame, sizeof(patNewFrame));
                    if (!foundAddrs[1]) {
                        // secondary heuristic for NewFrame prologue variant
                        for (BYTE* cur = beg; cur < end - 20; ++cur) {
                            if (cur[0]==0x48 && cur[1]==0x8B && cur[2]==0xC4 && cur[3]==0x53 && cur[4]==0x55 && cur[5]==0x57 &&
                                cur[6]==0x48 && cur[7]==0x81 && cur[8]==0xEC &&
                                cur[13]==0x48 && cur[14]==0x8B && cur[15]==0x2D) {
                                foundAddrs[1] = cur;
                                LogMessage("Secondary: Found SE ImGui::NewFrame at %p\n", cur);
                                break;
                            }
                        }
                        if (!foundAddrs[1]) {
                            foundAddrs[1] = FindPattern(beg, end, patNewFrame2, maskNewFrame2, sizeof(patNewFrame2));
                            if (foundAddrs[1]) LogMessage("Alt2: Found SE ImGui::NewFrame at %p\n", foundAddrs[1]);
                            if (!foundAddrs[1]) {
                                foundAddrs[1] = FindPattern(beg, end, patNewFrame3, maskNewFrame3, sizeof(patNewFrame3));
                                if (foundAddrs[1]) LogMessage("Alt3: Found SE ImGui::NewFrame at %p\n", foundAddrs[1]);
                            }
                        }
                    }
                    if (foundAddrs[1]) {
                        LogMessage("Fallback: Found SE ImGui::NewFrame at %p\n", foundAddrs[1]);
                    }
                }
            }
        }
    }
    // Also scan ImGui copies that reside in other DLLs (e.g. version.dll) without patching them
    HMODULE verDll = GetModuleHandleA("version.dll");
    if (verDll && verDll != g_scriptExtenderDll) {
        char verPath[MAX_PATH] = {0};
        GetModuleFileNameA(verDll, verPath, MAX_PATH);
        LogMessage("Scanning ImGui functions in %s ...\n", verPath);
        PatchImGuiInModule(verDll, /*captureOriginal=*/true); // true => scan only, no BakeOut

        // Create patched copy
        char verTempPath[MAX_PATH];
        strcpy_s(verTempPath, verPath);
        char* dot = strrchr(verTempPath, '.');
        if (dot)
            strcpy_s(dot, MAX_PATH - (dot - verTempPath), "_temp.dll");
        else
            strcat_s(verTempPath, "_temp.dll");

        if (BakeOutImGuiCopy(verDll, verTempPath, g_versionAddrs))
            LogMessage("BakeOutImGuiCopy: Patched copy saved as %s\n", verTempPath);
        else
            LogMessage("BakeOutImGuiCopy: No patches written to %s\n", verTempPath);
    }
    // Permanently disable duplicate ImGui implementations inside Script Extender DL
    //char sePath[MAX_PATH] = {};
    //GetModuleFileNameA(g_scriptExtenderDll, sePath, MAX_PATH);
    //if (BakeOutImGuiFile(sePath, (BYTE*)g_scriptExtenderDll, foundAddrs))
        //LogMessage("BakeOutImGuiFile: Patched Script Extender ImGui functions on disk (%s)\n", sePath);
    //else
        //LogMessage("BakeOutImGuiFile: No ImGui functions patched for %s\n", sePath);
}

// ──────────────────────────────────────────────────────────────────────────────
//  Helper to scan & detour ImGui functions in an arbitrary module
//  captureOriginal=true  → store originals in g_SE* globals (SE module)
//                     false → just detour (upscaler module)
// ──────────────────────────────────────────────────────────────────────────────
void PatchImGuiInModule(HMODULE module, bool captureOriginal)
{
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &mi, sizeof(mi))) {
        LogMessage("PatchImGuiInModule: GetModuleInformation failed\n");
        return;
    }
    BYTE* beg = (BYTE*)module;
    BYTE* end = beg + mi.SizeOfImage;

    void* foundAddrs[4] = { nullptr, nullptr, nullptr, nullptr };

    // Patterns (re-use definitions from PatchSEImGuiCalls)
    const BYTE patGetIO[] = { 0x48, 0x8B, 0x0D, 0,0,0,0, 0x48, 0x8B, 0x41, 0x38 };
    const char maskGetIO[] = "xxx????xxxx";
    const BYTE patNewFrame[] = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0x35,0,0,0,0,0x48,0x8B,0xF9 };
    const char maskNewFrame[] = "xxxxxxxxxxxxx????xxx";
    const BYTE patGetIO2[] = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x83, 0xC0, 0x08, 0xC3 };
    const char maskGetIO2[] = "xxx????xxxxx";
    const BYTE patNewFrame2[] = { 0x48, 0x8B, 0xC4, 0x53, 0x55, 0x57, 0x48, 0x81, 0xEC, 0,0,0,0, 0x48, 0x8B, 0x2D, 0,0,0,0 };
    const char maskNewFrame2[] = "xxxxxxxxx????xxx????";
    const BYTE patGetIO3[] = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x8B, 0x40, 0x38 };
    const char maskGetIO3[] = "xxx????xxxx";
    const BYTE patNewFrame3[] = { 0x48,0x89,0x5C,0x24,0,0x57,0x48,0x83,0xEC,0,0x48,0x8B,0x05 };
    const char maskNewFrame3[] = "xxxx?xxxx?xxx";
    const BYTE patGetIO4[] = { 0x48, 0x8B, 0x05, 0,0,0,0, 0x48, 0x83, 0xC0, 0x08, 0 };
    const char maskGetIO4[] = "xxx????xxxxx?";
    const BYTE patGetIO5[] = { 0x48, 0x8B, 0x05, 0,0,0,0, 0xC3 };
    const char maskGetIO5[] = "xxx????x""x"; // eight bytes pattern -> xx???xx?? actually need 8 char; simplifying

    // reuse original eight-byte prologues from local ImGui funcs (optional)
    BYTE sigGetIO[8]; BYTE sigNewFrame[8]; BYTE sigRender[8]; BYTE sigCreate[8];
    memcpy(sigGetIO, ImGui::GetIO, 8);
    memcpy(sigNewFrame, ImGui::NewFrame, 8);
    memcpy(sigRender, ImGui::Render, 8);
    memcpy(sigCreate, ImGui::CreateContext, 8);

    struct { const char* name; BYTE* sig; void* detour; void** orig; } targets[4] = {
        { "GetIO",         sigGetIO,   (void*)ImGui_GetIO,      nullptr },
        { "NewFrame",      sigNewFrame,(void*)ImGui_NewFrame,   nullptr },
        { "Render",        sigRender,  (void*)ImGui_Render,     nullptr },
        { "CreateContext", sigCreate,  (void*)Hook_CreateContext, nullptr },
    };

    // First: try exact sig match inside this module
    for (int i=0;i<4;++i){
        for (BYTE* cur=beg; cur<end-8; ++cur){
            if (memcmp(cur, targets[i].sig, 8)==0){
                foundAddrs[i]=cur;
                break;
            }
        }
    }

    // Fallback patterns for GetIO/NewFrame if missing
    if(!foundAddrs[0]||!foundAddrs[1]){
        for(int i=0;i<2;++i){
            if((i==0 && foundAddrs[0])||(i==1 && foundAddrs[1])) continue;
            const BYTE* p1 = (i==0)?patGetIO:patNewFrame;
            const char* m1 = (i==0)?maskGetIO:maskNewFrame;
            size_t   l1 = (i==0)?sizeof(patGetIO):sizeof(patNewFrame);
            void* found = FindPattern(beg,end,p1,m1,l1);
            if(!found){
                // alt patterns chain ... simplified: reuse same order as earlier
                if(i==0){ // GetIO alts
                    found = FindPattern(beg,end,patGetIO2,maskGetIO2,sizeof(patGetIO2));
                    if(!found) found=FindPattern(beg,end,patGetIO3,maskGetIO3,sizeof(patGetIO3));
                    if(!found) found=FindPattern(beg,end,patGetIO4,maskGetIO4,sizeof(patGetIO4));
                    if(!found) found=FindPattern(beg,end,patGetIO5,maskGetIO5,sizeof(patGetIO5));
                } else {
                    found = FindPattern(beg,end,patNewFrame2,maskNewFrame2,sizeof(patNewFrame2));
                    if(!found) found=FindPattern(beg,end,patNewFrame3,maskNewFrame3,sizeof(patNewFrame3));
                }
            }
            if(found){ foundAddrs[i]=found; }
        }
    }

    if(!foundAddrs[2]){
        // Render prologue simple 8-byte sig only; already attempted above.
    }

    // Fallback patterns for CreateContext
    if(!foundAddrs[3]){
        // Patterns for ImGui::CreateContext (common prologues across versions)
        const BYTE patCreate1[] = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20 };                // push regs / sub rsp,20h
        const char maskCreate1[] = "xxxxxxxxxx";
        const BYTE patCreate2[] = { 0x48,0x8B,0xC4,0x53,0x48,0x81,0xEC,0,0,0,0,0x48,0x8B,0xD9 };       // preamble with stack frame
        const char maskCreate2[] = "xxxxxxx????xxx";
        const BYTE patCreate3[] = { 0x48,0x89,0x4C,0x24,0x18,0x55,0x56,0x57,0x41,0x54 };               // some older builds
        const char maskCreate3[] = "xxxxxxxxxx";

        void* found = FindPattern(beg,end,patCreate1,maskCreate1,sizeof(patCreate1));
        if(!found) found = FindPattern(beg,end,patCreate2,maskCreate2,sizeof(patCreate2));
        if(!found) found = FindPattern(beg,end,patCreate3,maskCreate3,sizeof(patCreate3));
        if(found) {
            foundAddrs[3]=found;
            LogMessage("Fallback: Found ImGui::CreateContext at %p\n", found);
        }
    }

    // Obtain DLL path for logging
    char modulePath[MAX_PATH] = {0};
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    LogMessage("PatchImGuiInModule: Target DLL = %s (%p) captureOriginal=%d\n", modulePath, module, captureOriginal);

    // Log resolved addresses before detouring
    for(int i=0;i<4;++i){
        if(foundAddrs[i]){
            LogMessage("PatchImGuiInModule: %s – ImGui::%s at %p\n", modulePath, targets[i].name, foundAddrs[i]);
        }
    }

    // Permanently patch this DLL on disk (skip for Script Extender, handled above)
    if (!captureOriginal) {
        if (BakeOutImGuiFile(modulePath, (BYTE*)module, foundAddrs))
            LogMessage("BakeOutImGuiFile: Patched ImGui functions in %s on disk\n", modulePath);
        else
            LogMessage("BakeOutImGuiFile: No ImGui functions patched for %s\n", modulePath);
    }

    // Store addresses globally if caller requested captureOriginal (used for later baking)
    if (captureOriginal) {
        memcpy(g_versionAddrs, foundAddrs, sizeof(foundAddrs));
    }

    // No runtime hooks attached anymore.
    LogMessage("PatchImGuiInModule: %s (%p) – addresses processed, captureOriginal=%d\n", modulePath, module, captureOriginal);
}

// ──────────────────────────────────────────────────────────────────────────────
//  Permanently patch a DLL so specified functions start with "ret"+nops.
//  This disables duplicate ImGui implementations without needing runtime detours.
// ──────────────────────────────────────────────────────────────────────────────
static bool BakeOutImGuiFile(const char* path, BYTE* moduleBase, void* addrs[4])
{
    // Create backup once
    char backup[MAX_PATH];
    sprintf_s(backup, "%s.orig", path);
    CopyFileA(path, backup, FALSE);

    HANDLE hFile = CreateFileA(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (!hMap) { CloseHandle(hFile); return false; }

    BYTE* fileMem = (BYTE*)MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, 0);
    if (!fileMem) { CloseHandle(hMap); CloseHandle(hFile); return false; }

    auto dos = (IMAGE_DOS_HEADER*)fileMem;
    auto nt  = (IMAGE_NT_HEADERS*)(fileMem + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    WORD nSec = nt->FileHeader.NumberOfSections;

    auto RvaToOff = [&](DWORD rva)->DWORD {
        for (WORD i = 0; i < nSec; ++i) {
            DWORD va = sec[i].VirtualAddress;
            DWORD raw = sec[i].PointerToRawData;
            DWORD vsz = sec[i].Misc.VirtualSize;
            DWORD rsz = sec[i].SizeOfRawData;
            DWORD maxSize = (vsz > rsz && vsz != 0) ? vsz : rsz; // use larger of sizes
            if (rva >= va && rva < va + maxSize)
                return raw + (rva - va);
        }
        return 0u;
    };

    BYTE stub[8] = { 0x33, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90 }; // xor eax,eax; ret; nop *5

    bool patched = false;
    for (int i = 0; i < 4; ++i) {
        if (!addrs[i]) continue;
        DWORD rva = (DWORD)((BYTE*)addrs[i] - moduleBase);
        DWORD off = RvaToOff(rva);
        if (!off) {
            LogMessage("BakeOutImGuiFile: RVA %08X for idx %d did not map to file offset – section too small?\n", rva, i);
        }
        if (off) { memcpy(fileMem + off, stub, sizeof(stub)); patched = true; }
    }

    FlushViewOfFile(fileMem, 0);
    UnmapViewOfFile(fileMem);
    CloseHandle(hMap);
    CloseHandle(hFile);
    return patched;
}

// ──────────────────────────────────────────────────────────────────────────────
//  Bake out ImGui functions by copying a loaded module to a new file, then
//  patching that copy so functions do nothing (return 0). Returns true if any
//  byte patched in the copy.
// ──────────────────────────────────────────────────────────────────────────────
static bool BakeOutImGuiCopy(HMODULE module, const char* destPath, void* addrsOverride[4])
{
    char srcPath[MAX_PATH] = {};
    GetModuleFileNameA(module, srcPath, MAX_PATH);

    // Copy file (overwrite if already exists)
    if (!CopyFileA(srcPath, destPath, FALSE)) {
        DWORD err = GetLastError();
        LogMessage("BakeOutImGuiCopy: CopyFile failed (%lu) from %s to %s\n", err, srcPath, destPath);
    }

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &mi, sizeof(mi)))
        return false;

    BYTE* base = (BYTE*)mi.lpBaseOfDll;
    BYTE* beg  = base;
    BYTE* end  = base + mi.SizeOfImage;

    void* addrs[4] = {nullptr};

    if (addrsOverride) {
        memcpy(addrs, addrsOverride, sizeof(addrs));
    }

    // Simple reuse: scan using the same patterns as PatchImGuiInModule
    if (!addrs[0] || !addrs[1] || !addrs[2] || !addrs[3]) {
        const BYTE patGetIO[]   = { 0x48,0x8B,0x05,0,0,0,0,0x48,0x8B,0x40,0x38 };
        const char maskGetIO[]  = "xxx????xxxx";
        const BYTE patNewFrame[] = { 0x48,0x8B,0xC4,0x53,0x55,0x57,0x48,0x81,0xEC,0,0,0,0,0x41,0x8B };
        const char maskNewFrame[] = "xxxxxxxxx????xx";
        const BYTE patRender[]  = { 0x41,0x57,0x48,0x83,0xEC,0x60,0x48,0x8B,0x05 };
        const char maskRender[] = "xxxxxxxxx";

        if(!addrs[0]) addrs[0] = FindPattern(beg,end,patGetIO,maskGetIO,sizeof(patGetIO));
        if(!addrs[1]) addrs[1] = FindPattern(beg,end,patNewFrame,maskNewFrame,sizeof(patNewFrame));
        if(!addrs[2]) addrs[2] = FindPattern(beg,end,patRender,maskRender,sizeof(patRender));

        // Fallback patterns for ImGui::CreateContext
        if (!addrs[3]) {
            const BYTE patCreate1[] = { 0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20 };
            const char maskCreate1[] = "xxxxxxxxxx";
            const BYTE patCreate2[] = { 0x48,0x8B,0xC4,0x53,0x48,0x81,0xEC,0,0,0,0,0x48,0x8B,0xD9 };
            const char maskCreate2[] = "xxxxxxx????xxx";
            const BYTE patCreate3[] = { 0x48,0x89,0x4C,0x24,0x18,0x55,0x56,0x57,0x41,0x54 };
            const char maskCreate3[] = "xxxxxxxxxx";

            addrs[3] = FindPattern(beg,end,patCreate1,maskCreate1,sizeof(patCreate1));
            if (!addrs[3]) addrs[3] = FindPattern(beg,end,patCreate2,maskCreate2,sizeof(patCreate2));
            if (!addrs[3]) addrs[3] = FindPattern(beg,end,patCreate3,maskCreate3,sizeof(patCreate3));
        }
    }

    LogMessage("BakeOutImGuiCopy: Scanned %s -> GetIO=%p, NewFrame=%p, Render=%p, CreateCtx=%p\n",
               srcPath, addrs[0], addrs[1], addrs[2], addrs[3]);

    return BakeOutImGuiFile(destPath, base, addrs);
}

std::wstring GetDllDirectory()
{
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleA("DWrite.dll"), dllPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
    if (lastSlash)
    {
        *(lastSlash + 1) = L'\0';
    }
    
    return dllPath;
}

void LogMessage(const char* format, ...)
{
    char buffer[2048];
    
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    SYSTEMTIME st;
    GetLocalTime(&st);
    
    char timestampedBuffer[2200];
    sprintf_s(timestampedBuffer, "[%02d:%02d:%02d.%03d] %s", 
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
    
    OutputDebugStringA(timestampedBuffer);
    
    if (g_logFile.is_open())
    {
        g_logFile << timestampedBuffer;
        g_logFile.flush();
    }
}

void CheckForConfigFiles(const std::wstring& modsFolder)
{
    const wchar_t* configFiles[] = {
        L"BG3Upscaler.ini",
    };
    
    for (const wchar_t* configFile : configFiles)
    {
        std::wstring fullPath = modsFolder + configFile;
        DWORD fileAttrs = GetFileAttributesW(fullPath.c_str());
        
        if (fileAttrs != INVALID_FILE_ATTRIBUTES && !(fileAttrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            LogMessage("Found configuration file: %ls\n", configFile);
        }
    }
}

// Set main game thread handle
void SetMainThread(HANDLE threadHandle)
{
    g_mainThreadHandle = threadHandle;
    LogMessage("Main thread handle set: %p (Thread ID: %d)\n", g_mainThreadHandle, GetCurrentThreadId());
}

bool CheckUpscalerLogs(const std::wstring& modsFolder, int maxWaitTimeMs = 60000)
{
    using namespace std::chrono;
    auto startTime = high_resolution_clock::now();
    bool logFound = false;
    bool hookConfirmed = false;
    bool dlssLogFound = false;
    bool dlssHookConfirmed = false;
    
    // Extract the bin folder path (parent of modsFolder)
    std::wstring binFolder = GetDllDirectory();
    
    LogMessage("Checking for upscaler log files...\n");
    
    while (duration_cast<milliseconds>(high_resolution_clock::now() - startTime).count() < maxWaitTimeMs)
    {
        for (const wchar_t* logFile : UPSCALER_LOG_FILES)
        {
            std::wstring logPath = modsFolder + logFile;
            std::ifstream file(logPath);
            
            if (file.is_open())
            {
                logFound = true;
                LogMessage("Found log file: %ls\n", logFile);
                
                // Once we found the BG3Upscaler.log, check if DLSS log file exists
                std::wstring dlssLogPath = binFolder + DLSS_LOG_FILE;
                std::ifstream dlssFile(dlssLogPath);
                
                if (dlssFile.is_open())
                {
                    // DLSS log file exists, prioritize checking this instead
                    dlssLogFound = true;
                    LogMessage("Found DLSS log file: %ls\n", DLSS_LOG_FILE);
                    
                    // Check for success string in the DLSS log file
                    std::string dlssLine;
                    int dlssLogLines = 0;
                    
                    while (std::getline(dlssFile, dlssLine))
                    {
                        dlssLogLines++;
                        LogMessage("Read DLSS log line %d: %s\n", dlssLogLines, dlssLine.c_str());
                        
                        if (dlssLine.find(DLSS_SUCCESS_STRING) != std::string::npos)
                        {
                            LogMessage("Found DLSS success indicator in log: %s\n", dlssLine.c_str());
                            dlssHookConfirmed = true;
                            break;
                        }
                    }
                    
                    dlssFile.close();
                    
                    // If DLSS hook is confirmed, we can proceed
                    if (dlssHookConfirmed)
                    {
                        LogMessage("DLSS hook confirmation found!\n");
                        file.close();
                        return true;
                    }
                    
                    // If DLSS log exists but hook not confirmed, still check the original log
                    LogMessage("DLSS log file found but no hook confirmation. Will also check original log.\n");
                }
                
                // Only check the original log if DLSS log doesn't exist or if DLSS hook was not confirmed
                if (!dlssLogFound || !dlssHookConfirmed)
                {
                    // Check for success strings in the BG3Upscaler log file
                    std::string line;
                    int logLines = 0;
                    
                    // Rewind the file to the beginning since we might have read some already
                    file.clear();
                    file.seekg(0, std::ios::beg);
                    
                    while (std::getline(file, line))
                    {
                        logLines++;
                        LogMessage("Read log line %d: %s\n", logLines, line.c_str());
                        
                        for (const char* successString : UPSCALER_SUCCESS_STRINGS)
                        {
                            if (line.find(successString) != std::string::npos)
                            {
                                LogMessage("Found success indicator in log: %s\n", line.c_str());
                                hookConfirmed = true;
                                break;
                            }
                        }
                        
                        if (hookConfirmed) break;
                    }
                }
                
                file.close();
                
                // If we found confirmation in the original log, proceed
                if (hookConfirmed)
                {
                    LogMessage("Upscaler hook confirmation found in original log!\n");
                    return true;
                }
            }
        }
        
        // At this point, we've checked both logs but haven't found confirmation
        if (logFound)
        {
            if (dlssLogFound)
            {
                // If both logs found but no confirmation yet
                LogMessage("Both log files found but no hook confirmation yet, waiting... (Thread ID: %d)\n", GetCurrentThreadId());
            }
            else
            {
                // If only original log found but no confirmation yet
                LogMessage("Original log file found but no hook confirmation yet, waiting... (Thread ID: %d)\n", GetCurrentThreadId());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Wait before checking again
        }
        else
        {
            // If no logs found yet
            LogMessage("No log files found yet, waiting... (Thread ID: %d)\n", GetCurrentThreadId());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Wait longer between checks
        }
    }
    
    // If we get here, we've waited the maximum time but didn't find confirmation
    if (logFound)
    {
        if (dlssLogFound)
        {
            LogMessage("WARNING: Found both log files but could not confirm hooks. Proceeding anyway.\n");
        }
        else
        {
            LogMessage("WARNING: Found original log file but could not confirm upscaler hooks. Proceeding anyway.\n");
        }
        return true; // Return true anyway since we at least found a log file
    }
    else
    {
        LogMessage("WARNING: No upscaler log files found. Proceeding without confirmation.\n");
        return false;
    }
}

// The main updater thread that runs in background
DWORD WINAPI UpdaterThread(LPVOID param)
{
    LogMessage("Updater thread started. Thread ID: %d\n", GetCurrentThreadId());
    
    std::wstring dllDir = GetDllDirectory();
    std::wstring modsFolder = dllDir + UPSCALER_FOLDER;
    std::wstring scriptExtenderPath = dllDir + SCRIPTEXTENDER_DLL_NAME;
    
    // First, let the game run normally while we wait for the upscaler to initialize
    LogMessage("Waiting for upscaler initialization...\n");
    
    // We don't proceed unless we definitively confirm upscaler is hooked
    bool upscalerInitialized = CheckUpscalerLogs(modsFolder, 30000);
    
    if (upscalerInitialized)
    {
        LogMessage("Upscaler initialization confirmed! Safe to load Script Extender.\n");
        
        // Use event-based synchronization instead of thread suspension
        LogMessage("Requesting sync window to load Script Extender...\n");
        if (g_syncEvent != NULL) {
            // Reset the event to non-signaled state
            ResetEvent(g_syncEvent);
            LogMessage("Synchronization event reset, main thread should yield\n");
            
            // Wait a short time to ensure the main thread has a chance to yield
            Sleep(50);
        }
        
        // Load the script extender during synchronization window
        LogMessage("Loading script extender...\n");
        g_scriptExtenderDll = LoadLibraryW(scriptExtenderPath.c_str());
        if (!g_scriptExtenderDll)
        {
            LogMessage("Failed to load script extender DLL. Error code: %d\n", GetLastError());
            // Signal event to let the game continue despite error
            if (g_syncEvent != NULL) {
                SetEvent(g_syncEvent);
                LogMessage("Synchronization event signaled after error\n");
            }
            return 1;
        }
        LogMessage("Script extender loaded successfully\n");
        
        // Get the original DWriteCreateFactory function from the script extender
        g_originalDWriteCreateFactory = (DWriteCreateFactoryFunc)GetProcAddress(g_scriptExtenderDll, "DWriteCreateFactory");
        if (!g_originalDWriteCreateFactory)
        {
            LogMessage("Failed to get DWriteCreateFactory function from script extender\n");
            // Signal event to let the game continue despite error
            if (g_syncEvent != NULL) {
                SetEvent(g_syncEvent);
                LogMessage("Synchronization event signaled after error\n");
            }
            return 1;
        }
        
        LogMessage("Dependencies loaded successfully\n");
        LogMessage("======= BG3 Upscaler Proxy DLL Setup Complete =======\n");
        
        // Give Script Extender time to fully initialize
        LogMessage("Waiting for Script Extender to initialize before patching...\n");
        Sleep(40000); // Wait 10 seconds
        
        // Now patch the ImGui calls
        LogMessage("Applying ImGui patches...\n");
        PatchSEImGuiCalls();
        
        // Signal the event to let the game continue
        LogMessage("Signaling synchronization event...\n");
        if (g_syncEvent != NULL) {
            SetEvent(g_syncEvent);
            LogMessage("Synchronization event signaled, main thread can continue\n");
        }
    }
    else
    {
        LogMessage("ERROR: Could not confirm upscaler initialization after timeout.\n");
        LogMessage("Script Extender will not be loaded to avoid conflicts.\n");
        return 1;
    }
    
    return 0;
}

// Wait for upscaler to be hooked, then load script extender
bool LoadDependencies()
{
    std::wstring dllDir = GetDllDirectory();
    std::wstring modsFolder = dllDir + UPSCALER_FOLDER;
    
    // Create synchronization event
    g_syncEvent = CreateEvent(NULL, TRUE, TRUE, NULL);  // Manual reset, initially signaled
    if (g_syncEvent == NULL) {
        OutputDebugStringA("Failed to create synchronization event!\n");
    }
    
    LogMessage("======= BG3 Upscaler Proxy DLL Starting =======\n");
    
    // Reset our log file but NOT the upscaler logs
    // We need to let BG3Upscaler create its own log
    std::wstring ourLogPath = modsFolder + OUR_LOG_FILE;
    DeleteFileW(ourLogPath.c_str());
    OutputDebugStringA("Reset our log file\n");
    
    // Create or clear our own log file
    g_logFile.open(ourLogPath, std::ios::out | std::ios::trunc);
    if (g_logFile.is_open())
    {
        // Get current time for log header
        SYSTEMTIME st;
        GetLocalTime(&st);
        LogMessage("UpscalerProxy log started at %04d-%02d-%02d %02d:%02d:%02d\n", 
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        LogMessage("Cleared all log files for fresh start\n");
    }
    else
    {
        OutputDebugStringA("Failed to create our log file!\n");
    }
    
    LogMessage("DLL directory: %ls\n", dllDir.c_str());
    LogMessage("Mods folder: %ls\n", modsFolder.c_str());
    
    // Check for configuration files
    CheckForConfigFiles(modsFolder);
    
    LogMessage("Not loading any upscaler DLLs - waiting for upscaler to be hooked via external mechanism\n");
    
    auto hProcess = GetCurrentProcess();
    HANDLE hThread{ NULL };
    DuplicateHandle(hProcess, GetCurrentThread(), hProcess, &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SetMainThread(hThread);
    
    LogMessage("Main thread continuing. Thread ID: %d. Waiting for background thread to run...\n", GetCurrentThreadId());
    
    // Create a background thread to handle everything else
    HANDLE updaterThread = CreateThread(NULL, 0, &UpdaterThread, NULL, 0, NULL);
    if (updaterThread) {
        LogMessage("Created background thread to check for upscaler hooks\n");
        CloseHandle(updaterThread); // We don't need to keep the handle open
    } else {
        LogMessage("ERROR: Failed to create background thread! Error code: %d\n", GetLastError());
        LogMessage("Game will continue normally without Script Extender\n");
    }
    
    // Return success to continue loading
    return true;
}

// Exported function that forwards to the original DWriteCreateFactory
extern "C" __declspec(dllexport) HRESULT WINAPI DWriteCreateFactory(int factoryType, REFIID iid, IUnknown** factory)
{
    // Check if we need to yield for synchronization
    if (g_syncEvent != NULL) {
        // If event is not signaled, we should yield briefly
        if (WaitForSingleObject(g_syncEvent, 0) == WAIT_TIMEOUT) {
            // Yield CPU time to allow updater thread to complete its work
            // This is a soft yield, not a hard suspension
            Sleep(10);
            WaitForSingleObject(g_syncEvent, 100);
        }
    }
    
    if (!g_originalDWriteCreateFactory)
    {
        return E_FAIL;
    }
    
    return g_originalDWriteCreateFactory(factoryType, iid, factory);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DetourRestoreAfterWith();
        HookCreateContextOnce();          // capture SE context later
        if (!LoadDependencies())
        {
            return FALSE;
        }
        break;
    case DLL_PROCESS_DETACH:
        if (RealCreate) {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourDetach(&(PVOID&)RealCreate, Hook_CreateContext);
            DetourTransactionCommit();
            LogMessage("Detours: Unhooked ImGui::CreateContext\n");
        }
        if (g_scriptExtenderDll)
        {
            FreeLibrary(g_scriptExtenderDll);
            g_scriptExtenderDll = NULL;
        }
        if (g_logFile.is_open())
        {
            g_logFile.close();
        }
        if (g_syncEvent != NULL)
        {
            CloseHandle(g_syncEvent);
            g_syncEvent = NULL;
        }
        break;
    }
    return TRUE;
}