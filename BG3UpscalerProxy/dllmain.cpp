#include <windows.h>
#include <string>
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
const char* UPSCALER_SUCCESS_STRINGS[] = {
    "[info] hk_vkCreateDevice"
};
     
HMODULE g_scriptExtenderDll = NULL;

std::ofstream g_logFile;

// Game thread handling
HANDLE g_mainThreadHandle = NULL;
bool g_gameNeedsSuspend = false;

// Exported function pointers from the script extender
typedef HRESULT(WINAPI* DWriteCreateFactoryFunc)(int factoryType, REFIID iid, IUnknown** factory);
DWriteCreateFactoryFunc g_originalDWriteCreateFactory = NULL;

std::wstring GetDllDirectory()
{
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleA("DWrite.dll"), dllPath, MAX_PATH);
    
    // Find the last backslash
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
                
                                // Check for success strings in the log file
                std::string line;
                int logLines = 0;
                while (std::getline(file, line))
                {
                    // Log each line we read for debugging
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
                
                file.close();
                
                if (hookConfirmed)
                {
                    LogMessage("Upscaler hook confirmation found in logs!\n");
                    return true;
                }
            }
        }
        
        if (logFound && !hookConfirmed)
        {
            // If we found a log but no confirmation yet, wait a bit and try again
            LogMessage("Log file found but no hook confirmation yet, waiting... (Thread ID: %d)\n", GetCurrentThreadId());
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Wait longer between checks
        }
        else if (!logFound)
        {
            // If no log file found yet, wait a bit longer
            LogMessage("No log file found yet, waiting... (Thread ID: %d)\n", GetCurrentThreadId());
            std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Wait longer between checks
        }
    }
    
    // If we get here, we've waited the maximum time but didn't find confirmation
    if (logFound)
    {
        LogMessage("WARNING: Found log file(s) but could not confirm upscaler hooks. Proceeding anyway.\n");
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
    // Critical - log that thread is starting to verify it's running
    LogMessage("Updater thread started. Thread ID: %d\n", GetCurrentThreadId());
    
    std::wstring dllDir = GetDllDirectory();
    std::wstring modsFolder = dllDir + UPSCALER_FOLDER;
    std::wstring scriptExtenderPath = dllDir + SCRIPTEXTENDER_DLL_NAME;
    
    // First, let the game run normally while we wait for the upscaler to initialize
    LogMessage("Waiting for upscaler initialization...\n");
    
    // We don't proceed unless we definitively confirm upscaler is hooked
    bool upscalerInitialized = CheckUpscalerLogs(modsFolder, 30000);  // Wait up to 30 seconds
    
    if (upscalerInitialized)
    {
        LogMessage("Upscaler initialization confirmed! Safe to load Script Extender.\n");
        
        // Now that upscaler is confirmed initialized, suspend the game thread briefly
        LogMessage("Suspending game thread to load Script Extender...\n");
        if (g_mainThreadHandle != NULL) {
            DWORD suspendCount = SuspendThread(g_mainThreadHandle);
            LogMessage("Game thread suspended. Suspend count: %d\n", suspendCount);
        }
        
        // Load the script extender while game is suspended
        LogMessage("Loading script extender...\n");
        g_scriptExtenderDll = LoadLibraryW(scriptExtenderPath.c_str());
        if (!g_scriptExtenderDll)
        {
            LogMessage("Failed to load script extender DLL. Error code: %d\n", GetLastError());
            // Resume the game despite error
            if (g_mainThreadHandle != NULL) {
                ResumeThread(g_mainThreadHandle);
                LogMessage("Game thread resumed after error\n");
            }
            return 1;
        }
        LogMessage("Script extender loaded successfully\n");
        
        // Get the original DWriteCreateFactory function from the script extender
        g_originalDWriteCreateFactory = (DWriteCreateFactoryFunc)GetProcAddress(g_scriptExtenderDll, "DWriteCreateFactory");
        if (!g_originalDWriteCreateFactory)
        {
            LogMessage("Failed to get DWriteCreateFactory function from script extender\n");
            // Resume the game despite error
            if (g_mainThreadHandle != NULL) {
                ResumeThread(g_mainThreadHandle);
                LogMessage("Game thread resumed after error\n");
            }
            return 1;
        }
        
        LogMessage("Dependencies loaded successfully\n");
        LogMessage("======= BG3 Upscaler Proxy DLL Setup Complete =======\n");
        
        // Now resume the game since everything is ready
        LogMessage("Resuming game thread...\n");
        if (g_mainThreadHandle != NULL) {
            ResumeThread(g_mainThreadHandle);
            LogMessage("Game thread resumed\n");
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
    
    LogMessage("======= BG3 Upscaler Proxy DLL Starting =======\n");
    
    // Reset our log file but NOT the upscaler logs
    // We need to let BG3Upscaler create its own log
    std::wstring ourLogPath = modsFolder + OUR_LOG_FILE;
    DeleteFileW(ourLogPath.c_str()); // It's okay if the file doesn't exist
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
    
    // Get the main thread handle - this is crucial for suspension to work
    auto hProcess = GetCurrentProcess();
    HANDLE hThread{ NULL };
    DuplicateHandle(hProcess, GetCurrentThread(), hProcess, &hThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    SetMainThread(hThread);
    
    // We no longer suspend here - the background thread will do it
    LogMessage("Main thread continuing. Thread ID: %d. Waiting for background thread to run...\n", GetCurrentThreadId());
    
    // Create a background thread to handle everything else
    HANDLE updaterThread = CreateThread(NULL, 0, &UpdaterThread, NULL, 0, NULL);
    if (updaterThread) {
        LogMessage("Created background thread to check for upscaler hooks\n");
        CloseHandle(updaterThread); // We don't need to keep the handle open
    } else {
        LogMessage("ERROR: Failed to create background thread! Error code: %d\n", GetLastError());
        // We don't need to do anything special here since the game thread isn't suspended
        LogMessage("Game will continue normally without Script Extender\n");
    }
    
    // Return success to continue loading
    return true;
}

// Exported function that forwards to the original DWriteCreateFactory
extern "C" __declspec(dllexport) HRESULT WINAPI DWriteCreateFactory(int factoryType, REFIID iid, IUnknown** factory)
{
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
        // Load dependencies when the DLL is attached
        if (!LoadDependencies())
        {
            // If dependencies failed to load, return FALSE to prevent the DLL from loading
            return FALSE;
        }
        break;
        
    case DLL_PROCESS_DETACH:
        // Clean up when the DLL is detached
        if (g_scriptExtenderDll)
        {
            FreeLibrary(g_scriptExtenderDll);
            g_scriptExtenderDll = NULL;
        }
        
        // Close our log file
        if (g_logFile.is_open())
        {
            g_logFile.close();
        }
        break;
    }
    
    return TRUE;
}
