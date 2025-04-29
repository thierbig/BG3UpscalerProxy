#include <windows.h>
#include <string>
#include <imgui.h>
#include <psapi.h>

#include <stdio.h>
#include <vector>
#include <fstream>
#include <thread>
#include <chrono>

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
static FARPROC       g_TrampCreate  = nullptr;   // unused (could store trampoline)


static ImGuiContext* WINAPI Hook_CreateContext(ImFontAtlas* atlas)
{
    auto RealCreate =
        (ImGuiContext* (WINAPI*)(ImFontAtlas*))g_TrampCreate;
    ImGuiContext* ctx = RealCreate(atlas);

    if (!g_ImCtx) {
        g_ImCtx = ctx;
        LogMessage("Captured SE ImGuiContext at %p\n", g_ImCtx);
    }
    return ctx;
}

static BYTE          g_Gateway[32];               // raw bytes
using CreateCtxFn = ImGuiContext* (__cdecl*)(ImFontAtlas*);
static CreateCtxFn   RealCreate = nullptr;     

void HookCreateContextOnce()
{
    BYTE* tgt = (BYTE*)ImGui::CreateContext;
    DWORD  old;
    VirtualProtect(tgt, 14, PAGE_EXECUTE_READWRITE, &old);

    // 1. keep a copy of the first 5 bytes (size of the JMP we’ll overwrite with)
    memcpy(g_Gateway, tgt, 5);

    // 2. append absolute JMP back to the remainder of the original function
    BYTE* p = g_Gateway + 5;
    p[0] = 0xFF; p[1] = 0x25; *(DWORD*)(p+2) = 0;
    *(uint64_t*)(p+6) = (uint64_t)(tgt + 5);

    // 3. write our hook (5-byte RVA-relative JMP is enough here)
    intptr_t rel = (BYTE*)Hook_CreateContext - (tgt + 5);
    tgt[0] = 0xE9; *(int32_t*)(tgt + 1) = (int32_t)rel;

    VirtualProtect(tgt, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), tgt, 14);
}

// ──────────────────────────────────────────────────────────────────────────────
//  2)  Wrappers exported by DWrite.dll – always restore g_ImCtx
// ──────────────────────────────────────────────────────────────────────────────
extern "C" __declspec(dllexport) ImGuiIO* ImGui_GetIO()
{
    if (!g_ImCtx) return nullptr;
    ImGui::SetCurrentContext(g_ImCtx);
    return &ImGui::GetIO();
}
extern "C" __declspec(dllexport) void ImGui_NewFrame()
{
    if (!g_ImCtx) return;
    ImGui::SetCurrentContext(g_ImCtx);
    ImGui::NewFrame();
}
extern "C" __declspec(dllexport) void ImGui_Render()
{
    if (!g_ImCtx) return;
    ImGui::SetCurrentContext(g_ImCtx);
    ImGui::Render();
}

// ──────────────────────────────────────────────────────────────────────────────
//  3)  Combined patcher – sig scan first, inline-JMP fall-back
// ──────────────────────────────────────────────────────────────────────────────
void PatchSEImGuiCalls()
{
    // --- helper table with sig + wrapper targets ----------------------------
    HMODULE hSelf = GetModuleHandleA("DWrite.dll");
    struct Sig { BYTE pat[8]; FARPROC wrap; const char* name; } S[3] = {
        {{}, GetProcAddress(hSelf,"ImGui_GetIO")   ,"GetIO"   },
        {{}, GetProcAddress(hSelf,"ImGui_NewFrame"),"NewFrame"},
        {{}, GetProcAddress(hSelf,"ImGui_Render")  ,"Render"  }
    };
    memcpy(S[0].pat, ImGui::GetIO   , 8);
    memcpy(S[1].pat, ImGui::NewFrame, 8);
    memcpy(S[2].pat, ImGui::Render  , 8);

    // scan SE .text for those eight-byte prologues
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), g_scriptExtenderDll, &mi,sizeof(mi));
    BYTE* beg = (BYTE*)g_scriptExtenderDll;
    BYTE* end = beg + mi.SizeOfImage;
    int   patched = 0;

    for (auto& s : S)
    {
        for (BYTE* cur = beg; cur < end-8; ++cur)
        {
            if (memcmp(cur, s.pat, 8)) continue;

            DWORD old; VirtualProtect(cur,8,PAGE_EXECUTE_READWRITE,&old);
            *(DWORD*)(cur+1) = (DWORD)((BYTE*)s.wrap - (cur+5)); // rewrite rel32
            VirtualProtect(cur,8,old,&old);

            LogMessage("Patched SE call ImGui::%s at %p\n", s.name, cur);
            ++patched; break;
        }
    }

    // ------------------------------------------------------------------------
    // fall-back for any function not found by sig scan – inline absolute JMP
    // ------------------------------------------------------------------------
    if (patched < 3)
    {
        struct Hook { void* seProc; FARPROC wrap; const char* name; } H[] = {
            { (void*)ImGui::GetIO   , S[0].wrap , "GetIO"    },
            { (void*)ImGui::NewFrame, S[1].wrap , "NewFrame" },
            { (void*)ImGui::Render  , S[2].wrap , "Render"   }
        };

        for (auto& h : H)
        {
            BYTE* p = (BYTE*)h.seProc;
            if (*(uint16_t*)p == 0x25FF) continue;        // already JMP

            DWORD old; VirtualProtect(p,12,PAGE_EXECUTE_READWRITE,&old);
            p[0]=0xFF; p[1]=0x25; *(DWORD*)(p+2)=0;
            *(uint64_t*)(p+6) = (uint64_t)h.wrap;
            VirtualProtect(p,12,old,&old);
            FlushInstructionCache(GetCurrentProcess(),p,12);

            LogMessage("Inline-JMP-hooked ImGui::%s at %p → %p\n",
                       h.name,p,h.wrap);
            ++patched;
        }
    }
    LogMessage("Total ImGui patches applied: %d\n", patched);
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
    HookCreateContextOnce();          // capture SE context later
        if (!LoadDependencies())
        {
            return FALSE;
        }

        break;
        
    case DLL_PROCESS_DETACH:
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