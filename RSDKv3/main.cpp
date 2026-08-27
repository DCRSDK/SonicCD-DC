#include "RetroEngine.hpp"

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
#include "Windows.h"
#endif

void parseArguments(int argc, char *argv[])
{
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

#if RETRO_USING_KOS
#include <kos.h>
// INIT_FS_PTY is required for stdio (printf) to reach the debug console.
// INIT_CDROM gives us /cd; INIT_VMU is needed before any save can be written.
KOS_INIT_FLAGS(INIT_IRQ | INIT_CONTROLLER | INIT_VMU | INIT_CDROM | INIT_FS_PTY);

// Pre-main probe: static constructors run after KOS subsystem init but before
// main(). RSDKv3 has a lot of them (RetroEngine Engine = RetroEngine(), plus
// every large global). If this prints and main()'s line doesn't, the hang is in
// a later ctor; if neither prints, KOS init itself (maple/cdrom/fs) hung.
extern "C" __attribute__((constructor(101))) void DC_CtorProbe()
{
    printf("[DC] static ctors running\n");
    fflush(stdout);
    DC_Probe(0);
}
#endif

int main(int argc, char *argv[])
{
#if RETRO_USING_KOS
    printf("[DC] main() entered\n");
    fflush(stdout);
    DC_Probe(1);
#endif

#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

#if RETRO_USING_KOS
    printf("[DC] RSDKv3 booting\n");
    fflush(stdout);
    // Engine PrintLog output is the only diagnostic channel here: always on.
    Engine.consoleEnabled = true;
#if defined(RSDK_DEBUG) && RSDK_DEBUG
    engineDebugMode = true;
    Engine.devMenu  = true;
#endif
#endif

    Engine.Init();
#if RETRO_USING_KOS
    printf("[DC] engine init done: running=%d gameMode=%d\n", Engine.running, Engine.gameMode);
    fflush(stdout);
#endif
    Engine.Run();

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN && _MSC_VER
        FreeConsole();
#endif
    }
#endif

    return 0;
}
