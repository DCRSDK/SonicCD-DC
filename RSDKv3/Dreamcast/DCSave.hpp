// ---------------------------------------------------------------------
// RSDKv3 Dreamcast (KallistiOS) — VMU save backend.
//
// On every other platform RSDKv3 saves to files under gamePath. On the DC
// gamePath is "/cd/" (the disc — read-only), so those writes silently fail and
// nothing persists. This backend redirects the engine's four save entry points
// to the VMU (memory card) instead. See DCSave.cpp for the format/design.
//
// Ported from the RSDKv4 Dreamcast layer with no structural change: v3 and v4
// have identical SAVEDATA_SIZE (0x2000), ACHIEVEMENT_COUNT (0x40) and
// LEADERBOARD_COUNT (0x80), and the same Achievement/LeaderboardEntry shapes,
// so only the file names and descriptions differ.
// ---------------------------------------------------------------------
#ifndef DC_SAVE_HPP
#define DC_SAVE_HPP

#if RETRO_USING_KOS

// Two VMU files, mirroring the engine's own SData/UData split so neither writer
// can clobber the other's data: "SONICCD.VMS" (game progress) and "SONICCDR.VMS"
// (achievements + leaderboard times). Both carry a proper VMU package header +
// icon so they show up in the BIOS. Each writer skips the physical write when
// its payload is unchanged since the last one (no redundant VMU wear/hitches).

// Load the game-progress blob (saveRAM[]) from the VMU. Returns true if a save
// was found and loaded (mirrors ReadSaveRAMData's return contract).
bool DC_LoadSaveRAM();

// Mark the game-progress blob as needing a write (mirrors WriteSaveRAMData).
// Does NO I/O — a VMU write is ~200-400ms and the engine asks for one mid-frame.
// Always returns true (queued); DC_FlushSaves() performs the write.
bool DC_StoreSaveRAM();

// Load achievements + leaderboard times from the VMU (mirrors ReadUserdata).
void DC_LoadUserdata();

// Mark achievements + leaderboards as needing a write (mirrors WriteUserdata).
// Deferred the same way as DC_StoreSaveRAM.
bool DC_StoreUserdata();

// Push the game's icon (DC_VMU_LCD_ICON in DCSaveIcon.h) to the LCD of every
// attached VMU. Cheap — one maple frame per device — and safe with no VMU
// present. Called at load and again after every flush, since a block write can
// leave the BIOS's own save animation on the screen and a card plugged in
// mid-session has never been drawn to.
void DC_VMUUpdateLCD();

// Perform any pending VMU writes. Call ONLY where a ~300ms stall is invisible:
// a stage load, or a settled pause. Payloads are rebuilt from the live globals
// at this point, so repeated stores between flushes collapse into one write.
// Cheap and safe to call when nothing is pending.
void DC_FlushSaves();

#endif // RETRO_USING_KOS
#endif // DC_SAVE_HPP
