// RSDKv3 Dreamcast system helpers: timing, boot breadcrumbs, VMU LCD, heap reporting.
#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <stdlib.h>

#include "DCCommon.hpp"

unsigned long long DC_GetTicksPerSecond() { return 1000000000ULL; }

unsigned long long DC_GetTicks() { return timer_ns_gettime64(); }

unsigned int DC_GetTicksMS() { return (unsigned int)(timer_ns_gettime64() / 1000000ULL); }

void DC_Probe(int phase) { DC_LOG("[DC] probe %d\n", phase); }

// ---------------------------------------------------------------------
// Hang watchdog state
// ---------------------------------------------------------------------
// Written by the main thread, read by the audio thread. See DCCommon.hpp for
// the reasoning; the report itself is DC_HangReport in DCGraphics.cpp, which is
// where the video hardware lives.
//
// volatile, not atomic: these are single naturally-aligned words on SH4, one
// writer each, and the reader only needs to notice that a value stopped
// changing. A torn read would cost one extra watchdog interval, nothing more.
volatile unsigned int dcHeartbeat       = 0;
volatile unsigned char dcPhase          = DC_PHASE_BOOT;
volatile unsigned char dcAudioLockWait  = 0;

// ---------------------------------------------------------------------
// VMU LCD icon
// ---------------------------------------------------------------------
// Sonic CD disc, drawn to fill the whole 48x32 VMU LCD.
// 48 wide x 32 tall, 1 bit/pixel, 6 bytes/row, MSB = leftmost, 1 = dark/ink.
static const unsigned char dcVmuArt[192] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFE, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xC0, 0x00, 0x00, 0x07, 0xFF, 0xFF, 0x20, 0x00,
    0x00, 0x0F, 0xFF, 0xFE, 0x10, 0x00, 0x00, 0x0F, 0xFF, 0xFE, 0x10, 0x00, 0x00, 0x1F, 0xFF, 0xFC, 0x08, 0x00,
    0x00, 0x3F, 0xFF, 0xF8, 0x0C, 0x00, 0x00, 0x3F, 0xFF, 0xF0, 0x3C, 0x00, 0x00, 0x3F, 0xFF, 0xF8, 0xFC, 0x00,
    0x00, 0x7F, 0xFC, 0x3D, 0xFE, 0x00, 0x00, 0x7F, 0xF8, 0x1F, 0xFE, 0x00, 0x00, 0x7F, 0xF0, 0x0F, 0xFE, 0x00,
    0x00, 0x7F, 0xF0, 0x0F, 0xFE, 0x00, 0x00, 0x7F, 0xF0, 0x0F, 0xFE, 0x00, 0x00, 0x7F, 0xF0, 0x0F, 0xFE, 0x00,
    0x00, 0x7F, 0xF8, 0x1F, 0xFE, 0x00, 0x00, 0x7F, 0xBC, 0x3F, 0xFE, 0x00, 0x00, 0x3F, 0x1F, 0xFF, 0xFC, 0x00,
    0x00, 0x3C, 0x0F, 0xFF, 0xFC, 0x00, 0x00, 0x30, 0x1F, 0xFF, 0xFC, 0x00, 0x00, 0x10, 0x3F, 0xFF, 0xF8, 0x00,
    0x00, 0x08, 0x7F, 0xFF, 0xF0, 0x00, 0x00, 0x08, 0x7F, 0xFF, 0xF0, 0x00, 0x00, 0x04, 0xFF, 0xFF, 0xE0, 0x00,
    0x00, 0x03, 0xFF, 0xFF, 0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x7F, 0xFE, 0x00, 0x00,
    0x00, 0x00, 0x0F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// The VMU LCD framebuffer is addressed from the bottom-right (origin flipped
// both ways), 6 bytes per row. Built once, pushed to every connected VMU.
static unsigned char dcVmuLcd[192];
static bool dcVmuLcdReady = false;

void DC_VmuShowIcon()
{
    if (!dcVmuLcdReady) {
        memset(dcVmuLcd, 0, sizeof(dcVmuLcd));
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 48; ++x) {
                if (dcVmuArt[y * 6 + (x >> 3)] & (0x80 >> (x & 7)))
                    dcVmuLcd[(31 - y) * 6 + (47 - x) / 8] |= (unsigned char)(0x80 >> ((47 - x) & 7));
            }
        }
        dcVmuLcdReady = true;
    }

    // Push to every connected LCD (both controller slots, both VMU slots).
    maple_device_t *dev;
    for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_LCD)) != NULL; ++i) vmu_draw_lcd(dev, dcVmuLcd);
}

// Called once per frame from DC_ProcessInput.
//
// Polling here rather than at stage load puts the icon up on the very first
// frame. Sonic CD's boot logos, title screen and menus run for a long time
// before any stage loads, and a VMU would otherwise sit blank through all of it.
//
// Enumerating twice a second and pushing only when the LCD count changes keeps
// the steady-state cost at one walk of the maple device table per 30 frames,
// and still covers hot-plugging a VMU mid-game.
void DC_VmuTick()
{
    static int cooldown  = 0;
    static int lastCount = -1;

    if (--cooldown > 0)
        return;
    cooldown = 30;

    int count = 0;
    while (maple_enum_type(count, MAPLE_FUNC_LCD) != NULL) ++count;

    if (count == lastCount)
        return;
    lastCount = count;

    if (count > 0)
        DC_VmuShowIcon();
}

// ---------------------------------------------------------------------
// Heap reporting
// ---------------------------------------------------------------------
// mallinfo() is not dependable under KOS, so free memory is measured the only
// way that cannot lie: keep asking malloc for the largest block it will still
// give, hold them all, then give everything back.
//
// This matters more in v3 than it did in v4. RSDKv3 puts roughly 8.6MB of the
// Dreamcast's 16MB into static globals before main() runs (graphicData alone is
// 4MB, scriptCode 1MB), so the margin left for the heap is thin and worth
// watching at every phase of boot.

size_t DC_LargestFreeBlock()
{
    size_t lo = 0, hi = 8 * 1024 * 1024;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo + 1) / 2;
        void *p          = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        }
        else {
            hi = mid - 1;
        }
    }
    return lo;
}

void DC_HeapReport(const char *tag)
{
    // Grab blocks largest-first until nothing usable is left, so `holes` counts
    // distinct free regions rather than an arbitrary chunking of one big one.
    void *blocks[128];
    size_t sizes[128];
    int held     = 0;
    size_t total = 0;

    while (held < 128) {
        const size_t got = DC_LargestFreeBlock();
        if (got < 1024)
            break;
        void *p = malloc(got);
        if (!p)
            break;
        blocks[held] = p;
        sizes[held]  = got;
        total += got;
        ++held;
    }

    const size_t largest = held ? sizes[0] : 0;
    for (int i = 0; i < held; ++i) free(blocks[i]);

    DC_LOG("[DC] heap %s: free=%uKB largest=%uKB holes=%d\n", tag, (unsigned)(total / 1024), (unsigned)(largest / 1024), held);
}

#endif // RETRO_USING_KOS
