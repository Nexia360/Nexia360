/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/hid_flags.h"

DEFINE_bool(guide_button, true, "Forward guide button presses to guest.",
            "HID");

DEFINE_bool(guide_button_chord, true,
            "Hold Back and Start together to stand in for a Guide button the "
            "pad does not have. A Steam Deck's own Steam button never reaches "
            "the emulator, and under Steam Input its trackpads are not "
            "exposed either.",
            "HID");

DEFINE_int32(guide_button_chord_hold_ms, 300,
             "How long Back and Start must be held together before they count "
             "as the Guide button, so a title that uses them separately is "
             "unaffected.",
             "HID");
