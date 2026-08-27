// ---------------------------------------------------------------------
// RSDKv3 Dreamcast (KallistiOS) — VMU save backend.
//
// gamePath is "/cd/" on the DC (the disc — read-only), so the engine's normal
// file saves silently fail. This redirects the four save entry points to the
// VMU (memory card).
//
// Two files, mirroring the engine's own SData/UData split so a userdata write
// can never clobber game progress and vice-versa:
//   SONICCD.VMS  : saveRAM[]  (game progress; trailing-zero-trimmed)
//   SONICCDR.VMS : achievement statuses + leaderboard times
// Each is wrapped in a proper VMU package (header + 32x32 icon) so it appears
// in the BIOS file manager. The first attached memory card is used.
//
// Writes are DEFERRED — the store entry points only raise a flag and the real
// VMU I/O happens in DC_FlushSaves(), at moments where a ~300ms pause is free.
// See the "deferred writing" block at the bottom of this file.
//
// Payload layout (little-endian ints; SH4 is little-endian):
//   SAV: [magic][ver=1][used]                   then `used` ints of saveRAM
//   USR: [magic][ver=1][achCount][lbCount]      then ach ints, then lb ints
// ---------------------------------------------------------------------
#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include "DCSave.hpp"
#include "DCSaveIcon.h"

#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// The card stores whatever 12-character name it is handed; ".VMS" is purely a
// PC-side convention, but it is the one every memory-card tool is built around,
// and most take the on-card name from the file being imported. Naming the
// primary files .VMS means a save pulled off a card is already a usable .VMS,
// and one built on a PC drops back on without being renamed first.
//
// Entry [0] is the name written. The rest are names a save may still be sitting
// under and are tried on read: the extensionless form left behind by tools that
// strip the suffix on import. Guessing at names is safe because every payload
// starts with its own magic — opening the wrong file fails the magic check and
// falls through to the next candidate rather than loading garbage.
static const char *const DC_SAV_NAMES[] = { "SONICCD.VMS", "SONICCD.SAV", "SONICCD" };
static const char *const DC_USR_NAMES[] = { "SONICCDR.VMS", "SONICCD.USR", "SONICCDR" };
#define DC_NAMECOUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Set by dcVmuRead when a payload was found under one of the non-primary names.
// The next successful write to the primary name removes exactly that file and
// nothing else. Deliberately not "unlink every alternative name": those names
// are guesses, and "SONICCD" in particular could plausibly belong to some other
// homebrew. A name only lands here after we opened it and found OUR magic in
// it, so the file being removed is provably ours.
static const char *dcSavSupersede = NULL;
static const char *dcUsrSupersede = NULL;

#define DC_SAV_MAGIC (0x33564153) // 'SAV3'
#define DC_USR_MAGIC (0x33525355) // 'USR3'

// A 4-aligned scratch buffer big enough for the largest file body (whole
// saveRAM + userdata + VMU package header/icon). int[] => 4-byte aligned, which
// vmu_pkg_parse's returned data pointer inherits (header + icon are 4-multiples).
static int dcScratch[(int)(sizeof(int) * (5 + SAVEDATA_SIZE + ACHIEVEMENT_COUNT + LEADERBOARD_COUNT) + 2048) / (int)sizeof(int)];

// FNV-1a, to skip re-writing an unchanged payload.
static uint32 dcCRC(const void *p, int n)
{
    const uint8 *b = (const uint8 *)p;
    uint32 h       = 2166136261u;
    for (int i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

// Path of the first attached memory card, e.g. "/vmu/a1". False if none.
static bool dcFindVMU(char *out)
{
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if (!dev)
        return false;
    sprintf(out, "/vmu/%c%d", 'a' + dev->port, dev->unit);
    return true;
}

// Build a VMU package around `payload` and write it to <vmu>/<fname>. Skips the
// write (returns true) when the payload's CRC matches *lastCRC from a prior write.
static bool dcVmuWrite(const char *fname, const char **supersede, const char *descLong, const int *payload, int payloadBytes, uint32 *lastCRC,
                       bool *haveLast)
{
    uint32 crc = dcCRC(payload, payloadBytes);
    if (*haveLast && crc == *lastCRC)
        return true; // unchanged since last successful write

    char vmu[32];
    if (!dcFindVMU(vmu)) {
        DC_LOG("[DCSave] no VMU attached; %s not written\n", fname);
        return false;
    }

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "SONIC CD", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long, descLong, sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id, "RSDKv3", sizeof(pkg.app_id) - 1);
    pkg.icon_cnt        = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type   = VMUPKG_EC_NONE;
    memcpy(pkg.icon_pal, DC_SAVE_ICON_PAL, sizeof(pkg.icon_pal));
    pkg.icon_data = (uint8 *)DC_SAVE_ICON_DATA;
    pkg.data_len  = payloadBytes;
    pkg.data      = (const uint8 *)payload;

    uint8 *blob  = NULL;
    int blobSize = 0;
    if (vmu_pkg_build(&pkg, &blob, &blobSize) < 0 || !blob) {
        DC_LOG("[DCSave] vmu_pkg_build failed for %s\n", fname);
        return false;
    }

    char path[48];
    sprintf(path, "%s/%s", vmu, fname);
    fs_unlink(path); // fs_vmu won't overwrite in place

    FILE *f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        ok = (fwrite(blob, 1, blobSize, f) == (size_t)blobSize);
        fclose(f);
    }
    free(blob);

    if (ok) {
        *lastCRC  = crc;
        *haveLast = true;
        DC_LOG("[DCSave] wrote %s (%d B, ~%d blocks)\n", path, blobSize, (blobSize + 511) / 512);

        // Migration. The payload is now safely under the primary name, so give
        // the card back the blocks the old copy is holding — a VMU only has 200
        // of them and a stale duplicate is 3 of those. Strictly after a verified
        // write, so there is never a moment where neither copy exists.
        if (*supersede) {
            char old[48];
            sprintf(old, "%s/%s", vmu, *supersede);
            if (fs_unlink(old) == 0)
                DC_LOG("[DCSave] removed superseded %s\n", old);
            *supersede = NULL; // migrated; do not try again
        }
    }
    else {
        DC_LOG("[DCSave] write %s FAILED (VMU full? needs ~%d blocks)\n", path, (blobSize + 511) / 512);
    }
    return ok;
}

// Locate our payload inside a raw VMU file body. Primary path: vmu_pkg_parse.
// Fallback (if parse rejects the package — some KOS/emulator builds are strict
// about block-padding or CRC): scan the raw bytes for the 4-byte magic that
// begins every payload we write. Either way *plOut points into dcScratch.
static bool dcVmuReadNamed(const char *vmu, const char *fname, uint32 magic, int **plOut, int *countOut)
{
    char path[48];
    sprintf(path, "%s/%s", vmu, fname);
    FILE *f = fopen(path, "rb");
    if (!f) {
        DC_LOG("[DCSave] read: %s not present\n", path);
        return false;
    }

    size_t got = fread(dcScratch, 1, sizeof(dcScratch), f);
    fclose(f);
    DC_LOG("[DCSave] read: %s -> %u bytes\n", path, (uint)got);
    if (got < 16)
        return false;

    // Primary: parse the VMU package properly.
    vmu_pkg_t pkg;
    int pr = vmu_pkg_parse((uint8 *)dcScratch, got, &pkg);
    if (pr >= 0 && pkg.data && pkg.data_len >= (int)sizeof(int)) {
        int *pl = (int *)pkg.data;
        if (pl[0] == (int)magic) {
            *plOut    = pl;
            *countOut = pkg.data_len / sizeof(int);
            DC_LOG("[DCSave] read: parsed via vmu_pkg (data_len=%d)\n", pkg.data_len);
            return true;
        }
        DC_LOG("[DCSave] read: vmu_pkg parsed but magic mismatch (0x%08x)\n", (uint)pl[0]);
    }
    else {
        DC_LOG("[DCSave] read: vmu_pkg_parse rejected (%d) - scanning for magic\n", pr);
    }

    // Fallback: the header (128) + one icon frame (512) puts our payload at a
    // fixed 640-byte offset, but rather than assume that, scan the whole body for
    // the magic (4-aligned) so a differently-sized package still works.
    int words = (int)(got / sizeof(int));
    for (int i = 0; i < words; ++i) {
        if (dcScratch[i] == (int)magic) {
            *plOut    = &dcScratch[i];
            *countOut = words - i;
            DC_LOG("[DCSave] read: found payload by magic scan at +%d B\n", i * (int)sizeof(int));
            return true;
        }
    }

    DC_LOG("[DCSave] read: magic not found in %s\n", path);
    return false;
}

// Try each candidate name in turn and stop at the first whose payload carries
// the expected magic.
static bool dcVmuRead(const char *const *names, int nameCount, uint32 magic, int **plOut, int *countOut, const char **supersede)
{
    char vmu[32];
    if (!dcFindVMU(vmu)) {
        DC_LOG("[DCSave] read: no VMU attached\n");
        return false;
    }

    for (int i = 0; i < nameCount; ++i) {
        if (dcVmuReadNamed(vmu, names[i], magic, plOut, countOut)) {
            *supersede = i ? names[i] : NULL; // non-primary => migrate on next write
            if (i)
                DC_LOG("[DCSave] read: loaded from %s; next write moves it to %s\n", names[i], names[0]);
            return true;
        }
    }
    return false;
}

// ---- game progress (saveRAM[]) ----

bool DC_LoadSaveRAM()
{
    // First thing the save layer does in a session, and the earliest point a
    // VMU is known to be attached — so it is where the LCD icon goes up. Doing
    // it here rather than adding an init entry point keeps the save backend's
    // surface the same as every other platform's.
    DC_VMUUpdateLCD();

    int *pl, cnt;
    if (!dcVmuRead(DC_SAV_NAMES, DC_NAMECOUNT(DC_SAV_NAMES), DC_SAV_MAGIC, &pl, &cnt, &dcSavSupersede)) {
        DC_LOG("[DCSave] no game save loaded\n");
        return false;
    }
    // pl[0]=magic, pl[1]=ver, pl[2]=used, pl[3..]=saveRAM
    if (cnt < 3) {
        DC_LOG("[DCSave] game save truncated\n");
        return false;
    }
    int used = pl[2];
    if (used < 0)
        used = 0;
    if (used > SAVEDATA_SIZE)
        used = SAVEDATA_SIZE;

    memset(saveRAM, 0, sizeof(saveRAM));
    for (int i = 0; i < used && (3 + i) < cnt; ++i) saveRAM[i] = pl[3 + i];

    DC_LOG("[DCSave] loaded game save (%d ints)\n", used);
    return true;
}

// Build the current saveRAM[] payload and write it. Only called from
// DC_FlushSaves(); DC_StoreSaveRAM() below just marks it pending.
static bool dcWriteSaveRAMNow()
{
    static uint32 lastCRC = 0;
    static bool haveLast  = false;

    // trim trailing zeros (saveRAM is zero-initialised, so this is lossless)
    int used = SAVEDATA_SIZE;
    while (used > 0 && saveRAM[used - 1] == 0) --used;

    int n          = 0;
    dcScratch[n++] = DC_SAV_MAGIC;
    dcScratch[n++] = 1; // version
    dcScratch[n++] = used;
    for (int i = 0; i < used; ++i) dcScratch[n++] = saveRAM[i];

    return dcVmuWrite(DC_SAV_NAMES[0], &dcSavSupersede, "Sonic CD Save Data", dcScratch, n * sizeof(int), &lastCRC, &haveLast);
}

// ---- achievements + leaderboards ----

void DC_LoadUserdata()
{
    int *pl, cnt;
    if (!dcVmuRead(DC_USR_NAMES, DC_NAMECOUNT(DC_USR_NAMES), DC_USR_MAGIC, &pl, &cnt, &dcUsrSupersede) || cnt < 4) {
        // no (valid) userdata: leaderboards default to "no record set"
        for (int l = 0; l < LEADERBOARD_COUNT; ++l) leaderboards[l].score = 0x7FFFFFF;
        DC_LOG("[DCSave] no userdata on VMU (defaults applied)\n");
        return;
    }

    int savedAch = pl[2];
    int savedLb  = pl[3];
    int base     = 4;
    for (int a = 0; a < ACHIEVEMENT_COUNT; ++a) achievements[a].status = (a < savedAch && (base + a) < cnt) ? pl[base + a] : 0;

    int lbase = base + (savedAch > 0 ? savedAch : 0);
    for (int l = 0; l < LEADERBOARD_COUNT; ++l) {
        int v                 = (l < savedLb && (lbase + l) < cnt) ? pl[lbase + l] : 0;
        leaderboards[l].score = v ? v : 0x7FFFFFF;
    }
    DC_LOG("[DCSave] loaded userdata (%d ach, %d lb)\n", savedAch, savedLb);
}

// As above, for achievements + leaderboards.
static bool dcWriteUserdataNow()
{
    static uint32 lastCRC = 0;
    static bool haveLast  = false;

    int n          = 0;
    dcScratch[n++] = DC_USR_MAGIC;
    dcScratch[n++] = 1; // version
    dcScratch[n++] = ACHIEVEMENT_COUNT;
    dcScratch[n++] = LEADERBOARD_COUNT;
    for (int a = 0; a < ACHIEVEMENT_COUNT; ++a) dcScratch[n++] = achievements[a].status;
    for (int l = 0; l < LEADERBOARD_COUNT; ++l) dcScratch[n++] = leaderboards[l].score;

    return dcVmuWrite(DC_USR_NAMES[0], &dcUsrSupersede, "Sonic CD Records", dcScratch, n * sizeof(int), &lastCRC, &haveLast);
}

// ---- deferred writing ----
//
// A VMU write costs roughly 200-400ms: fs_unlink rewrites the directory and
// FAT, then the data blocks go over the maple bus at VMU flash-write speed.
// Done inline on the main thread that is a visible freeze, and the engine asks
// for it at the worst possible moments — AwardAchievement() and
// SetLeaderboard() both call WriteUserdata() the instant the record lands.
// SetLeaderboard only writes when the new time BEATS the stored one, which is
// exactly why the freeze shows up on fast runs and not slow ones. In Sonic CD
// that means the frame you cross the goal plate with a new best time, or the
// frame a boss finally dies — the worst possible moments to drop 300ms.
//
// So the store entry points only raise a flag. DC_FlushSaves() does the real
// work, from the places where a pause that long is already free: the stage load
// and a settled pause menu.
//
// Nothing is snapshotted, deliberately. The payloads are rebuilt at flush time
// from the live saveRAM[] / achievements[] / leaderboards[] globals, so any
// number of updates between two flushes collapse into one write carrying the
// newest state, and there is no second copy of saveRAM to keep in sync.
//
// The pending flag is cleared only on success, so a flush with no VMU attached
// retries at the next flush point rather than dropping the data.
//
// Cost of deferring: an achievement or record earned and then powered off
// before the next stage load or pause is lost. In normal play the next stage
// load is seconds away.
static bool dcSavPending = false;
static bool dcUsrPending = false;

bool DC_StoreSaveRAM()
{
    dcSavPending = true;
    return true; // queued — the physical write happens in DC_FlushSaves()
}

bool DC_StoreUserdata()
{
    dcUsrPending = true;
    return true;
}

// ---------------------------------------------------------------------
// VMU LCD icon
// ---------------------------------------------------------------------
// The little screen on the VMU itself, which is blank by default while a game
// runs. Separate from the BIOS icon in DCSaveIcon.h: that one is 32x32 colour
// and lives inside the save file, this one is 48x32 monochrome and is pushed to
// the device over maple.
//
// vmu_draw_lcd_rotated() rather than vmu_draw_lcd(): the raw entry point wants
// the bitmap with its first byte at the BOTTOM-RIGHT, which is a 180-degree
// rotation of how anyone would author the art. The rotated one takes it the way
// round it looks, so DC_VMU_LCD_ICON can be read straight off the source image.
//
// Every attached VMU gets it, not just the one holding the save — a second card
// in port A2 is still a screen the player can see.
void DC_VMUUpdateLCD()
{
    maple_device_t *dev;
    for (int i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_LCD)) != NULL; ++i) vmu_draw_lcd_rotated(dev, DC_VMU_LCD_ICON);
}

void DC_FlushSaves()
{
    // dcVmuWrite() returns true both for a real write and for a payload whose
    // CRC is unchanged since the last one, so a flush with nothing to do costs
    // only the payload rebuild — no VMU traffic, no wear.
    if (dcSavPending && dcWriteSaveRAMNow())
        dcSavPending = false;
    if (dcUsrPending && dcWriteUserdataNow())
        dcUsrPending = false;

    // Redraw here as well as at boot, for two reasons: a block write to the card
    // can leave the LCD showing the BIOS's own save/load animation, and a VMU
    // plugged in mid-session has never been drawn to at all. A flush already
    // only happens where a pause is free, so the extra maple frame is invisible.
    DC_VMUUpdateLCD();
}

#endif // RETRO_USING_KOS
