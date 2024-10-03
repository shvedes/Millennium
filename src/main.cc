﻿#define _WINSOCKAPI_
#define WIN32_LEAN_AND_MEAN 
#define UNICODE
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <filesystem>
#include <fstream>
#include <fmt/core.h>
// #include <boxer/boxer.h>
#include <sys/log.h>
#include <core/loader.h>
#include <core/py_controller/co_spawn.h>
#include <core/ftp/serv.h>
#include <signal.h>
#include <pipes/terminal_pipe.h>
#include <git/vcs.h>
#include <api/executor.h>

class Preload 
{
public:
    Preload() 
    { 
        this->VerifyEnvironment(); 
    }

    ~Preload() { }

    const void VerifyEnvironment() 
    {
        const auto filePath = SystemIO::GetSteamPath() / ".cef-enable-remote-debugging";

        // Steam's CEF Remote Debugger isn't exposed to port 8080
        if (!std::filesystem::exists(filePath)) 
        {
            std::ofstream(filePath).close();

            Logger.Log("Successfully enabled CEF remote debugging, you can now restart Steam...");
            std::exit(1);
        }
    }

    const void Start() 
    {
        CheckForUpdates();
    }
};

/* Wrapped cross platform entrypoint */
const static void EntryMain() 
{
    /** Handle signal interrupts (^C) */
    signal(SIGINT, [](int signalCode) { std::exit(128 + SIGINT); });
    std::unique_ptr<StartupParameters> startupParams = std::make_unique<StartupParameters>();

    if (startupParams->HasArgument("-verbose"))
    {
        CreateTerminalPipe();
    }

    uint16_t ftpPort = Crow::CreateAsyncServer();

    const auto startTime = std::chrono::system_clock::now();
    {
        std::unique_ptr<Preload> preload = std::make_unique<Preload>();
        preload->Start();
    }

    std::shared_ptr<PluginLoader> loader = std::make_shared<PluginLoader>(startTime, ftpPort);
    SetPluginLoader(loader);

    PythonManager& manager = PythonManager::GetInstance();

    auto backendThread   = std::thread([&loader, &manager] { loader->StartBackEnds(manager); });
    auto frontendThreads = std::thread([&loader] { loader->StartFrontEnds(); });

    backendThread.join();
    frontendThreads.join();

    // std::this_thread::sleep_for(std::chrono::milliseconds(10000));
}

#ifdef _WIN32
std::unique_ptr<std::thread> g_millenniumThread;

int __stdcall DllMain(void*, unsigned long fdwReason, void*) 
{
    switch (fdwReason) 
    {
        case DLL_PROCESS_ATTACH: 
        {
            g_millenniumThread = std::make_unique<std::thread>(EntryMain);
            break;
        }
        case DLL_PROCESS_DETACH: 
        {
            Logger.PrintMessage(" MAIN ", "Shutting down Millennium...", COL_MAGENTA);
            std::exit(EXIT_SUCCESS);
            g_threadTerminateFlag->flag.store(true);
            Sockets::Shutdown();
            g_millenniumThread->join();
            Logger.PrintMessage(" MAIN ", "Millennium has been shut down.", COL_MAGENTA);
            break;
        }
    }

    return true;
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hInstPrev, PSTR cmdline, int cmdShow)
{
    //boxer::show("Millennium successfully loaded!", "Yay!");
    //EntryMain();
    return 1;
}
#elif __linux__
#include <stdio.h>
#include <stdlib.h>
#include <posix/helpers.h>
#include <dlfcn.h>
#include <fstream>
#include <chrono>
#include <ctime>
#include <stdexcept>

extern "C"
{
    /* Trampoline for the real main() */
    static int (*fnMainOriginal)(int, char **, char **);
    std::unique_ptr<std::thread> g_millenniumThread;

    /* Our fake main() that gets called by __libc_start_main() */
    int MainHooked(int argc, char **argv, char **envp)
    {
        Logger.Log("Hooked main() with PID: {}", getpid());

        /** 
         * Since this isn't an executable, and is "preloaded", the kernel doesn't implicitly load dependencies, so we need to manually. 
         * libpython3.11.so.1.0 should already be in $PATH, so we can just load it from there.
        */
        std::string pythonPath = fmt::format("{}/.millennium/libpython-3.11.8.so", std::getenv("HOME"));

        if (!dlopen(pythonPath.c_str(), RTLD_LAZY | RTLD_GLOBAL)) 
        {
            LOG_ERROR("Failed to load python libraries: {},\n\nThis is likely because it was not found on disk, try reinstalling Millennium.", dlerror());
        }

        #ifdef MILLENNIUM_SHARED
        {
            /** Start Millennium on a new thread to prevent I/O blocking */
            g_millenniumThread = std::make_unique<std::thread>(EntryMain);
            int steam_main = fnMainOriginal(argc, argv, envp);
            Logger.Log("Hooked Steam entry returned {}", steam_main);

            g_threadTerminateFlag->flag.store(true);
            Sockets::Shutdown();
            g_millenniumThread->join();

            Logger.Log("Shutting down Millennium...");
            return steam_main;
        }
        #else
        {
            g_threadTerminateFlag->flag.store(true);
            g_millenniumThread = std::make_unique<std::thread>(EntryMain);
            g_millenniumThread->join();
            return 0;
        }
        #endif
    }

    #ifdef MILLENNIUM_SHARED
    /*
    * Trampoline for __libc_start_main() that replaces the real main
    * function with our hooked version.
    */
    int __libc_start_main(
        int (*main)(int, char **, char **), int argc, char **argv,
        int (*init)(int, char **, char **), void (*fini)(void), void (*rtld_fini)(void), void *stack_end)
    {
        /* Save the real main function address */
        fnMainOriginal = main;

        /* Get the address of the real __libc_start_main() */
        decltype(&__libc_start_main) orig = (decltype(&__libc_start_main))dlsym(RTLD_NEXT, "__libc_start_main");

        /* Get the path to the Steam executable */
        std::filesystem::path steamPath = std::filesystem::path(std::getenv("HOME")) / ".steam/steam/ubuntu12_32/steam";

        /** not loaded in a invalid child process */
        if (argv[0] != steamPath.string()) 
        {
            return orig(main, argc, argv, init, fini, rtld_fini, stack_end);
        }

        Logger.Log("Loaded Millennium on {}, system architecture {}", GetLinuxDistro(), GetSystemArchitecture());
        /* ... and call it with our custom main function */
        return orig(MainHooked, argc, argv, init, fini, rtld_fini, stack_end);
    }
    #endif
}

int main(int argc, char **argv, char **envp)
{
    return MainHooked(argc, argv, envp);
}
#endif
