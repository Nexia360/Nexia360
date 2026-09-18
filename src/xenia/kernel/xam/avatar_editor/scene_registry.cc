/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// The command to scene map sub_920EDEA8 builds, one row per registration:
//
//     p = operator new(size);
//     if (p) construct(p, constants...);
//     table[command] = p;
//
// Eleven commands build a CreatorGridScene, and their last constructor constant
// runs 0x0C..0x16 with no gaps and no repeats: one grid per component category.
//
// The table is GENERATED - `python tools/registry.py --emit` in the port
// project. Regenerate it; never edit a row by hand.

#include "xenia/kernel/xam/avatar_editor/scene.h"

#include "xenia/kernel/xam/avatar_editor/component_collection.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

namespace {

#define AVATAR_SCENE(command, construct, class_id, size, a, b, c) \
  {                                                               \
    command, 0x##construct, class_id, size, { a, b, c }           \
  }

const SceneRegistration kRegistrations[] = {
    AVATAR_SCENE(1, 920DB320, kClassUnidentified, 0x94, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(2, 920E38A0, kClassStaticScene, 0x94, 0xBB, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(4, 920DA0E0, kClassStaticScene, 0x94, 0x8F, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(5, 920D92B8, kClassUnidentified, 0x94, 0x75, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(6, 920E6510, kClassCreatorGridScene, 0xA8, 0x38, 0xA5, 0xC),
    AVATAR_SCENE(7, 920DACA0, kClassStaticScene, 0x94, 0x99, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(8, 920D9558, kClassUnidentified, 0xA8, 0x98, 0x3, kNoConstant),
    AVATAR_SCENE(9, 920DAC18, kClassStaticScene, 0x94, 0x9C, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(10, 920D9558, kClassUnidentified, 0xA8, 0x90, 0x5,
                 kNoConstant),
    AVATAR_SCENE(11, 920D9558, kClassUnidentified, 0xA8, 0xAC, 0x4,
                 kNoConstant),
    AVATAR_SCENE(12, 920EF3B0, kClassUnidentified, 0xA8, 0x39, 0xA9, 0x6),
    AVATAR_SCENE(13, 920DAD28, kClassStaticScene, 0x94, 0x95, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(14, 920EF3B0, kClassUnidentified, 0xA8, 0x3B, 0x94, 0x7),
    AVATAR_SCENE(15, 920EF3B0, kClassUnidentified, 0xA8, 0x3C, 0x92, 0x8),
    AVATAR_SCENE(16, 920D8FE0, kClassUnidentified, 0xA0, 0x73, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(17, 920E6E40, kClassUnidentified, 0x11C, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(18, 920D8F18, kClassUnidentified, 0x11C, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(19, 920EF3B0, kClassUnidentified, 0xA8, 0x3A, 0x9D, 0x9),
    AVATAR_SCENE(20, 920EF3B0, kClassUnidentified, 0xA8, 0x3F, 0xA0, 0xB),
    AVATAR_SCENE(21, 920DADB0, kClassUnidentified, 0x94, 0x7C, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(22, 920D95B0, kClassStaticScene, 0x94, 0x7B, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(23, 920E64B8, kClassCreatorGridScene, 0xA8, 0xBC, 0xD,
                 kNoConstant),
    AVATAR_SCENE(24, 920E64B8, kClassCreatorGridScene, 0xA8, 0x77, 0xE,
                 kNoConstant),
    AVATAR_SCENE(25, 920E64B8, kClassCreatorGridScene, 0xA8, 0xB9, 0xF,
                 kNoConstant),
    AVATAR_SCENE(26, 920E64B8, kClassCreatorGridScene, 0xA8, 0xA7, 0x10,
                 kNoConstant),
    AVATAR_SCENE(28, 920E64B8, kClassCreatorGridScene, 0xA8, 0x79, 0x16,
                 kNoConstant),
    AVATAR_SCENE(29, 920D9230, kClassStaticScene, 0x94, 0x69, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(30, 920E6378, kClassUnidentified, 0xA8, 0x8D, 0x17,
                 kNoConstant),
    AVATAR_SCENE(31, 920E6EA8, kClassUnidentified, 0xE0, 0xB0, 0xFFF, 0x30000),
    AVATAR_SCENE(32, 920E64B8, kClassCreatorGridScene, 0xA8, 0x6D, 0x12,
                 kNoConstant),
    AVATAR_SCENE(33, 920E64B8, kClassCreatorGridScene, 0xA8, 0x6B, 0x14,
                 kNoConstant),
    AVATAR_SCENE(34, 920E64B8, kClassCreatorGridScene, 0xA8, 0x72, 0x13,
                 kNoConstant),
    AVATAR_SCENE(35, 920E64B8, kClassCreatorGridScene, 0xA8, 0x71, 0x15,
                 kNoConstant),
    AVATAR_SCENE(36, 920E64B8, kClassCreatorGridScene, 0xA8, 0x6F, 0x11,
                 kNoConstant),
    AVATAR_SCENE(37, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(38, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(39, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(40, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(41, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(42, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(43, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(44, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(45, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(46, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(47, 920E3B20, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(50, 920EB580, kClassUnidentified, 0x94, 0xB4, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(51, 920EB8C0, kClassUnidentified, 0xA4, 0xB4, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(52, 920EBB98, kClassUnidentified, 0xA4, 0xB4, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(53, 920EBC88, kClassUnidentified, 0xA4, 0xB4, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(54, 920D87F8, kClassUnidentified, 0x94, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(55, 920D9890, kClassUnidentified, 0x98, 0x8C, 0x0,
                 kNoConstant),
    // The one row the two views of sub_920EDEA8 could not be reconciled on.
    AVATAR_SCENE(56, 920BD690, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(57, 920D9890, kClassUnidentified, 0x98, 0x8A, 0x2,
                 kNoConstant),
    AVATAR_SCENE(58, 920D9890, kClassUnidentified, 0x98, 0x7D, 0x6,
                 kNoConstant),
    AVATAR_SCENE(59, 920D9890, kClassUnidentified, 0x98, 0x86, 0x3,
                 kNoConstant),
    AVATAR_SCENE(60, 920D9890, kClassUnidentified, 0x98, 0x82, 0x4,
                 kNoConstant),
    AVATAR_SCENE(61, 920D9F70, kClassUnidentified, 0x98, 0x84, 0x5,
                 kNoConstant),
    AVATAR_SCENE(63, 920D9890, kClassUnidentified, 0x98, 0x80, 0x7,
                 kNoConstant),
    AVATAR_SCENE(65, 920E6D68, kClassUnidentified, 0xAC, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(66, 920E6D68, kClassUnidentified, 0xAC, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(67, 920E4270, kClassUnidentified, 0x9C, 0xC1, 0xF,
                 kNoConstant),
    AVATAR_SCENE(68, 920E48D0, kClassUnidentified, 0xA8, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(69, 920E5130, kClassUnidentified, 0xA4, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(70, 920E5CF8, kClassUnidentified, 0xA4, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(71, 920E58B8, kClassUnidentified, 0x98, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(72, 920E5C68, kClassUnidentified, 0x98, 0xC1, kNoConstant,
                 kNoConstant),
    AVATAR_SCENE(73, 920E3C08, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(74, 920E4E58, kClassUnidentified, 0xA4, kNoConstant,
                 kNoConstant, kNoConstant),
    AVATAR_SCENE(75, 920DAAE8, kClassUnidentified, 0x8C, 0xC1, 0x13,
                 kNoConstant),
    AVATAR_SCENE(76, 920DA8E8, kClassUnidentified, 0x8C, 0xC1, 0xB8,
                 kNoConstant),
    AVATAR_SCENE(77, 920DA8E8, kClassUnidentified, 0x8C, 0xC1, 0xB8,
                 kNoConstant),
    AVATAR_SCENE(78, 920DA8E8, kClassUnidentified, 0x8C, 0xC1, 0xC1,
                 kNoConstant),
    AVATAR_SCENE(79, 920DA740, kClassUnidentified, 0x8C, 0xC1, 0xC1,
                 kNoConstant),
    AVATAR_SCENE(80, 920DA838, kClassUnidentified, 0x8C, 0xC1, 0x12,
                 kNoConstant),
    AVATAR_SCENE(81, 920DA838, kClassUnidentified, 0x8C, 0xC1, 0xE,
                 kNoConstant),
    AVATAR_SCENE(82, 920DA740, kClassUnidentified, 0x8C, 0xC1, 0x24,
                 kNoConstant),
    AVATAR_SCENE(83, 920DA838, kClassUnidentified, 0x8C, 0xC1, 0xD,
                 kNoConstant),
    AVATAR_SCENE(84, 920DA838, kClassUnidentified, 0x8C, 0xC1, 0x10,
                 kNoConstant),
    AVATAR_SCENE(85, 920DA8E8, kClassUnidentified, 0x8C, 0xC1, 0xF,
                 kNoConstant),
    AVATAR_SCENE(86, 920DA740, kClassUnidentified, 0x8C, 0xC1, 0xC1,
                 kNoConstant),
    AVATAR_SCENE(87, 920DA740, kClassUnidentified, 0x8C, 0xC1, 0xB,
                 kNoConstant),
    AVATAR_SCENE(88, 920DA990, kClassUnidentified, 0x8C, 0xC1, 0xC1,
                 kNoConstant),
    AVATAR_SCENE(92, 920DABC8, kClassUnidentified, 0x8C, 0xC6, 0xC5,
                 kNoConstant),
    AVATAR_SCENE(93, 920DABC8, kClassUnidentified, 0x8C, 0x8, 0x7, kNoConstant),
    AVATAR_SCENE(94, 920DABC8, kClassUnidentified, 0x8C, 0xCB, 0xCA,
                 kNoConstant),
    AVATAR_SCENE(96, 920EF460, kClassUnidentified, 0x94, 0xC1, 0xF,
                 kNoConstant),
};

#undef AVATAR_SCENE

}  // namespace

const SceneRegistration* SceneRegistrations(size_t* count) {
  *count = sizeof(kRegistrations) / sizeof(kRegistrations[0]);
  return kRegistrations;
}

// sub_920EDDD0
const SceneRegistration* SceneRegistrationFor(uint32_t command) {
  for (const SceneRegistration& entry : kRegistrations) {
    if (entry.command == command) {
      return &entry;
    }
  }
  return nullptr;
}

int32_t SceneCategoryForCommand(uint32_t command) {
  const SceneRegistration* entry = SceneRegistrationFor(command);
  if (!entry || entry->class_id != kClassCreatorGridScene) {
    return kNoConstant;
  }
  for (int i = 2; i >= 0; --i) {
    if (entry->constants[i] != kNoConstant) {
      return entry->constants[i];
    }
  }
  return kNoConstant;
}

bool CommandNeedsBuiltCollection(uint32_t command) {
  const SceneRegistration* entry = SceneRegistrationFor(command);
  return entry && entry->class_id == kClassCreatorGridScene;
}

uint32_t SceneTitleStringForCommand(uint32_t command) {
  const SceneRegistration* entry = SceneRegistrationFor(command);
  if (!entry) {
    return kEmptyStringOrdinal;
  }
  // A grid's constructor takes its category last and its title immediately
  // before it; every other screen is handed its title first.
  if (entry->class_id == kClassCreatorGridScene) {
    for (int i = 2; i >= 1; --i) {
      if (entry->constants[i] != kNoConstant) {
        return static_cast<uint32_t>(entry->constants[i - 1]);
      }
    }
  }
  return entry->constants[0] == kNoConstant
             ? kEmptyStringOrdinal
             : static_cast<uint32_t>(entry->constants[0]);
}

namespace {

// sub_920EA1A8, the child sub_920E38A0 builds. Its nine slots run 0x0C..0x30
// in the order the buttons appear, and the two body alternates sit past the
// gap at 0x28 that the constructor clears.
const MenuEntry kMainMenuEntries[] = {
    {"goto_creator", 0xA4, 4, "goto_creator_male", 4, kBodyTypeFirst},
    {"goto_closet", 0xA3, 22, "goto_closet_male", 22, kBodyTypeFirst},
    {"goto_marketplace", 0xB7, 67, nullptr, 0, 0},
    // sub_920DDC90 calls the application's own exit rather than posting, and
    // the exit command is where that lands.
    {"goto_saveandexit", 0x17, 3, nullptr, 0, 0},
    {"awardables", 0x74, 96, nullptr, 0, 0},
    {"photobooth", 0xBA, 50, nullptr, 0, 0},
    {"goto_startover", 0x0C, 81, nullptr, 0, 0},
};

// sub_920E8D98
const MenuEntry kFeaturesMenuEntries[] = {
    {"hair", 0xA6, 6, nullptr, 0, 0},
    {"eye_and_eyebrow", 0x96, 13, nullptr, 0, 0},
    {"facial", 0x9F, 9, "facial_female", 20, kBodyTypeSecond},
    {"body", 0x76, 5, "body_female", 5, kBodyTypeSecond},
    {"ears", 0x91, 10, nullptr, 0, 0},
    {"nose", 0xAD, 11, nullptr, 0, 0},
    {"mouth_and_chin", 0x9A, 7, nullptr, 0, 0},
    {"colour_male", 0x7F, 21, "colour_female", 21, kBodyTypeSecond},
};

// sub_920E8748
const MenuEntry kStyleMenuEntries[] = {
    {"tops", 0xBC, 23, "tops_female", 23, kBodyTypeSecond},
    {"hat", 0xA8, 26, "hat_female", 26, kBodyTypeSecond},
    {"access", 0x6A, 29, nullptr, 0, 0},
    {"carry", 0x7A, 28, nullptr, 0, 0},
    {"bottoms", 0x78, 24, "bottoms_female", 24, kBodyTypeSecond},
    {"shoes", 0xB9, 25, nullptr, 0, 0},
    {"malecostume", 0x8E, 30, "femalecostume", 30, kBodyTypeSecond},
    {"loadoutfit", 0xB1, 31, "loadoutfit_female", 31, kBodyTypeSecond},
};

// sub_920E99D8
const MenuEntry kMouthAndChinMenuEntries[] = {
    {"faceshape", 0x9B, 8, nullptr, 0, 0},
    {"mouth", 0xAA, 12, "mouth_male", 12, kBodyTypeFirst},
};

// sub_920E98C0
const MenuEntry kFaceMenuEntries[] = {
    {"facial_hair_male", 0x9E, 19, nullptr, 0, 0},
    {"facial_skin_male", 0xA1, 20, nullptr, 0, 0},
};

// sub_920E9B90
const MenuEntry kEyesMenuEntries[] = {
    {"eyes", 0x97, 14, nullptr, 0, 0},
    {"eyebrows", 0x93, 15, nullptr, 0, 0},
};

// sub_920E7FC8
const MenuEntry kAccessoriesMenuEntries[] = {
    {"glasses", 0x6E, 32, nullptr, 0, 0},
    {"watch", 0x72, 34, nullptr, 0, 0},
    {"gloves", 0x70, 36, "gloves_female", 36, kBodyTypeSecond},
    {"ring", 0x71, 35, nullptr, 0, 0},
    {"earring", 0x6C, 33, nullptr, 0, 0},
};

#define AVATAR_MENU(command, title, entries) \
  {command, title, entries, uint32_t(sizeof(entries) / sizeof(entries[0]))}

const MenuScreen kMenuScreens[] = {
    AVATAR_MENU(2, 0xBB, kMainMenuEntries),
    AVATAR_MENU(4, 0x8F, kFeaturesMenuEntries),
    AVATAR_MENU(7, 0x99, kMouthAndChinMenuEntries),
    AVATAR_MENU(9, 0x9C, kFaceMenuEntries),
    AVATAR_MENU(13, 0x95, kEyesMenuEntries),
    AVATAR_MENU(22, 0x7B, kStyleMenuEntries),
    AVATAR_MENU(29, 0x69, kAccessoriesMenuEntries),
};

#undef AVATAR_MENU

}  // namespace

const MenuScreen* MenuScreenForCommand(uint32_t command) {
  for (const MenuScreen& menu : kMenuScreens) {
    if (menu.command == command) {
      return &menu;
    }
  }
  return nullptr;
}

const MenuScreen* MenuScreenForTitleString(uint32_t title_string) {
  for (const MenuScreen& menu : kMenuScreens) {
    if (menu.title_string == title_string) {
      return &menu;
    }
  }
  return nullptr;
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
