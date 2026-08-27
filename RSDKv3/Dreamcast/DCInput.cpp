// RSDKv3 Dreamcast input backend: maple controller -> engine input slots
//
// RSDKv3's input enum is narrower than v4's: UP/DOWN/LEFT/RIGHT, A, B, C,
// START, ANY — and that's all. There is no BUTTONY / BUTTONL / BUTTONR /
// SELECT to hang extra functions off, which is why the dev-menu chord is
// surfaced as a separate edge flag (DC_DevMenuEdge) rather than as an input
// slot the way the v4 port did it.
//
// Mapping:
//   D-pad / analog stick -> Up/Down/Left/Right
//   A -> Button A (jump)   B -> Button B   X -> Button C
//   Start -> Start
//   Y, or L+R+Start held together -> dev menu (when DevMenu=true in settings.ini)

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include "DCCommon.hpp"

#define DC_ANALOG_DEADZONE (64)
#define DC_TRIGGER_PRESS   (64)

static bool dcDevMenuHeld = false;
static bool dcDevMenuEdge = false;

static inline void DC_SetButton(int id, bool held)
{
    if (held)
        inputDevice[id].setHeld();
    else if (inputDevice[id].hold)
        inputDevice[id].setReleased();
}

// One-frame rising edge for the dev-menu chord. Consumed (and cleared) by
// ProcessEvents in RetroEngine.cpp, so it fires once per press.
bool DC_DevMenuEdge()
{
    const bool e = dcDevMenuEdge;
    dcDevMenuEdge = false;
    return e;
}

// Held-state for the atlas debug view: L+R together, WITHOUT Start (Start makes
// it the dev-menu chord instead). Level-triggered rather than edge, so the view
// shows while held and disappears on release.
static bool dcAtlasViewHeld = false;
bool DC_AtlasViewHeld() { return dcAtlasViewHeld; }

// Raw analogue trigger values, surfaced on the perf overlay. These exist because
// a resting trigger reading above DC_TRIGGER_PRESS silently broke the Start
// button, and nothing on screen could have told us that. If an input chord ever
// misbehaves again, look here first.
int dcRawTrigL = 0;
int dcRawTrigR = 0;

// Diagnostic for the "Start doesn't pause" fault. Counts the one-frame press
// EDGES the input layer actually produces for INPUT_START. If this climbs when
// you tap Start, input is fine and the fault is engine-side; if it stays at 0,
// the fault is here. Two hypotheses have already been wrong — measure instead.
int dcStartPresses = 0;

void DC_ProcessInput()
{
    // Keep the disc icon on any connected VMU LCD. This lives on the input path
    // because it is the one DC-side hook that runs every frame from the very
    // first one — the icon needs to be up during the boot logos, long before
    // any stage loads.
    DC_VmuTick();

    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

    if (!dev) {
        for (int i = 0; i < INPUT_BUTTONCOUNT; ++i) {
            if (inputDevice[i].hold)
                inputDevice[i].setReleased();
        }
        dcDevMenuHeld = false;
        return;
    }

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state)
        return;

    const bool up    = (state->buttons & CONT_DPAD_UP) || state->joyy < -DC_ANALOG_DEADZONE;
    const bool down  = (state->buttons & CONT_DPAD_DOWN) || state->joyy > DC_ANALOG_DEADZONE;
    const bool left  = (state->buttons & CONT_DPAD_LEFT) || state->joyx < -DC_ANALOG_DEADZONE;
    const bool right = (state->buttons & CONT_DPAD_RIGHT) || state->joyx > DC_ANALOG_DEADZONE;

    dcRawTrigL = (int)state->ltrig;
    dcRawTrigR = (int)state->rtrig;

    const bool trigL = state->ltrig > DC_TRIGGER_PRESS;
    const bool trigR = state->rtrig > DC_TRIGGER_PRESS;
    const bool start = (state->buttons & CONT_START) != 0;

    // Dev menu: Y on its own, or the L+R+Start chord for pads without a usable
    // Y. Latched here so ProcessEvents sees a clean one-frame edge.
    const bool devChord = ((state->buttons & CONT_Y) != 0) || (start && trigL && trigR);
    if (devChord && !dcDevMenuHeld)
        dcDevMenuEdge = true;
    dcDevMenuHeld = devChord;

    // L+R without Start shows the indexed atlas (phase 3 debug view).
    dcAtlasViewHeld = trigL && trigR && !start;

    DC_SetButton(INPUT_UP, up);
    DC_SetButton(INPUT_DOWN, down);
    DC_SetButton(INPUT_LEFT, left);
    DC_SetButton(INPUT_RIGHT, right);

    DC_SetButton(INPUT_BUTTONA, (state->buttons & CONT_A) != 0);
    DC_SetButton(INPUT_BUTTONB, (state->buttons & CONT_B) != 0);
    DC_SetButton(INPUT_BUTTONC, (state->buttons & CONT_X) != 0);

    // Start is mapped UNCONDITIONALLY.
    //
    // This used to be `start && !(trigL && trigR)`, to stop the L+R+Start dev
    // chord also pausing the game underneath the menu. That was a bad trade: the
    // triggers are analogue, and a pad that rests above DC_TRIGGER_PRESS — worn,
    // third-party, or just badly calibrated — suppresses Start FOREVER, with no
    // symptom other than "pause doesn't work". Y still opened the dev menu, so
    // the input system looked fine.
    //
    // A cosmetic nicety must never be able to disable a core button. If the
    // chord also pauses, that is harmless and visible; this was neither.
    DC_SetButton(INPUT_START, start);

    // Count the edge the engine will actually see (Scene.cpp reads
    // inputDevice[INPUT_START].press via CheckKeyPress).
    if (inputDevice[INPUT_START].press)
        ++dcStartPresses;

    bool anyHold = false;
    for (int i = 0; i < INPUT_ANY; ++i) {
        if (inputDevice[i].hold) {
            anyHold = true;
            break;
        }
    }
    DC_SetButton(INPUT_ANY, anyHold);

    if (anyHold)
        inputType = 1; // controller
}

#endif // RETRO_USING_KOS
