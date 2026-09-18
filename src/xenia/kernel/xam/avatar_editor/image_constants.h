/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_AVATAR_EDITOR_IMAGE_CONSTANTS_H_
#define XENIA_KERNEL_XAM_AVATAR_EDITOR_IMAGE_CONSTANTS_H_

#include <stdint.h>

#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"

// AvatarEditor.xex's .rdata, as host data.
//
// The session commits the editor's range itself and zeroes it, so the image's
// own constants are not there at runtime: every literal the translated code
// reads out of .rdata would come back as zero. The values below were read out
// of the image and are the same bytes, at the same addresses.
//
// Host code uses the typed constants directly. Translated code that still
// reaches a literal through Guest finds it because SeedImageConstants writes
// the whole set back where the image kept it.

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

// The components a new avatar is built from: 33 asset ids, sixteen bytes each,
// every one of them signed with Microsoft's publisher suffix.
constexpr uint32_t kDefaultComponentAssetIdsAddress = 0x92000BB0;
constexpr uint32_t kDefaultComponentAssetIdCount = 33;
extern const uint8_t kDefaultComponentAssetIds[kDefaultComponentAssetIdCount]
                                              [kAssetIdBytes];

// The id that means "nothing chosen", which every category compares against.
constexpr uint32_t kEmptyAssetIdAddress = 0x92017EF8;
extern const uint8_t kEmptyAssetId[kAssetIdBytes];

// One row per component category: the asset type bits that select it, the
// extra bits its grid filters on, and the number the editor gives it.
struct ComponentCategoryRow {
  uint32_t type_mask;
  uint32_t filter_mask;
  uint32_t category;
};
constexpr uint32_t kComponentCategoryTableAddress = 0x92053098;
constexpr uint32_t kComponentCategoryCount = 22;
extern const ComponentCategoryRow
    kComponentCategoryTable[kComponentCategoryCount];

// The camera the editor opens on: 45 degrees across a 16:9 frame.
struct EditorCamera {
  float field_of_view;
  float aspect_ratio;
  float near_plane;
  float far_plane;
};
constexpr uint32_t kDefaultCameraAddress = 0x92006BE0;
extern const EditorCamera kDefaultCamera;

// Seconds the application holds the loading screen after the collection
// reports ready.
constexpr uint32_t kCollectionReadyDelaySecondsAddress = 0x92005AE0;
constexpr float kCollectionReadyDelaySeconds = 2.0f;

// A path starting with this is fetched over the network rather than opened.
constexpr uint32_t kRemoteImagePrefixAddress = 0x9200C068;
inline constexpr char16_t kRemoteImagePrefix[] = u"http://";
inline constexpr uint32_t kRemoteImagePrefixChars = 7;

// The two background widgets are named from this, one per slot.
constexpr uint32_t kBackgroundWidgetNameFormatAddress = 0x9200C078;
inline constexpr char16_t kBackgroundWidgetNameFormat[] = u"BGImage%i";

// The picture behind the root screen, and the scene that sells assets.
constexpr uint32_t kRootBackgroundImageFileAddress = 0x9200C08C;
inline constexpr char kRootBackgroundImageFile[] = "background.jpg";
constexpr uint32_t kMarketplaceSceneNameAddress = 0x9200C09C;
inline constexpr char kMarketplaceSceneName[] = "marketplace";
constexpr uint32_t kMarketplaceYesButtonNameAddress = 0x9200C0A8;
inline constexpr char16_t kMarketplaceYesButtonName[] =
    u"MarketplaceDialogYesButton";

// The widget behind each body part on the preview, and the property that says
// which strip of eight a tile came from.
constexpr uint32_t kAvatarButtonNameFormatAddress = 0x92001CFC;
inline constexpr char16_t kAvatarButtonNameFormat[] =
    u"AvatarPositions/AvatarButton%i";
constexpr uint32_t kGroupIndexPropertyNameAddress = 0x92001D48;
inline constexpr char16_t kGroupIndexPropertyName[] = u"groupIndex";

// The timeline flag the editor sets while a screen transition runs, and the
// first of the handle scenes it loads with it.
constexpr uint32_t kDisableTimelineRecursionNameAddress = 0x92005AE4;
inline constexpr char16_t kDisableTimelineRecursionName[] =
    u"DisableTimelineRecursion";
constexpr uint32_t kEditorHandleLeftAssetAddress = 0x92005B18;
inline constexpr char16_t kEditorHandleLeftAsset[] = u"EditorHandleLeft.xur";

// The element a screen gives the focus to when it opens.
constexpr uint32_t kDefaultFocusNameAddress = 0x92006C3C;
inline constexpr char16_t kDefaultFocusName[] = u"DefaultFocus";

// The two conversions the editor formats text through: an ANSI string into a
// wide buffer, and a wide string back out.
inline constexpr char kAnsiToWideFormat[] = "%S";
inline constexpr char16_t kWideFromAnsiFormat[] = u"%hs";

// The caption a category falls back to when an asset has no name.
constexpr uint32_t kEmptyCaptionAddress = 0x92053090;
inline constexpr char16_t kEmptyCaption[] = u"";

// The block those two formats sit in, with the layout floats around them whose
// readers are not translated yet.
constexpr uint32_t kTextFormatBlockAddress = 0x92006C18;
constexpr uint32_t kTextFormatBlockBytes = 0x24;
extern const uint8_t kTextFormatBlock[kTextFormatBlockBytes];

// Writes every constant above back into guest memory at the address the image
// kept it at, in the console's byte order.
void SeedImageConstants(const Guest& guest);

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_AVATAR_EDITOR_IMAGE_CONSTANTS_H_
