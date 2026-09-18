/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/xam_ui_new.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"

namespace xe {
namespace kernel {
namespace xam {
namespace xui {

static const Property kControlPackNuiBack_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kControlPackNuiBack_Table = {
    kControlPackNuiBack_Properties, 1};

static const Property kControlPackNuiHoverBack_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
    {"Magnetism", PropertyType::kBool, 0x100, nullptr},
    {"NuiFastAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowDetachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiApproachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"MagnetOffset", PropertyType::kVector, 0x100, nullptr},
};
static const PropertyTable kControlPackNuiHoverBack_Table = {
    kControlPackNuiHoverBack_Properties, 7};

static const Property kControlPackNuiHoverButton_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
    {"Magnetism", PropertyType::kBool, 0x100, nullptr},
    {"NuiFastAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowDetachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiApproachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"MagnetOffset", PropertyType::kVector, 0x100, nullptr},
};
static const PropertyTable kControlPackNuiHoverButton_Table = {
    kControlPackNuiHoverButton_Properties, 7};

static const Property kControlPackNuiHoverCheckbox_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
    {"Magnetism", PropertyType::kBool, 0x100, nullptr},
    {"NuiFastAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowDetachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiApproachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"MagnetOffset", PropertyType::kVector, 0x100, nullptr},
};
static const PropertyTable kControlPackNuiHoverCheckbox_Table = {
    kControlPackNuiHoverCheckbox_Properties, 7};

static const Property kControlPackNuiHoverChrome_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
    {"Magnetism", PropertyType::kBool, 0x100, nullptr},
    {"NuiFastAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowAttachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiSlowDetachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"NuiApproachRadius", PropertyType::kFloat, 0x100, nullptr},
    {"MagnetOffset", PropertyType::kVector, 0x100, nullptr},
};
static const PropertyTable kControlPackNuiHoverChrome_Table = {
    kControlPackNuiHoverChrome_Properties, 7};

static const Property kControlPackNuiSideNav_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
    {"Magnetism", PropertyType::kBool, 0x100, nullptr},
    {"NuiFastAttachRadius", PropertyType::kFloat, 0x100, nullptr},
};
static const PropertyTable kControlPackNuiSideNav_Table = {
    kControlPackNuiSideNav_Properties, 3};

static const Property kControlPackNuiSwipeNav_Properties[] = {
    {"Handedness", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kControlPackNuiSwipeNav_Table = {
    kControlPackNuiSwipeNav_Properties, 1};

static const Property kHUDScene_Properties[] = {
    {"OpenType", PropertyType::kUnsigned, 0x0, nullptr},
    {"LegendA", PropertyType::kString, 0x0, nullptr},
    {"LegendB", PropertyType::kString, 0x0, nullptr},
    {"LegendX", PropertyType::kString, 0x0, nullptr},
    {"LegendY", PropertyType::kString, 0x0, nullptr},
    {"ShowGamerInfo", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kHUDScene_Table = {kHUDScene_Properties, 6};

static const Property kXuiButton_Properties[] = {
    {"PressKey", PropertyType::kUnsigned, 0x0, nullptr},
    {"PressAnimObject", PropertyType::kString, 0x0, nullptr},
    {"PressAnimStartFrame", PropertyType::kString, 0x0, nullptr},
    {"PressAnimEndFrame", PropertyType::kString, 0x0, nullptr},
    {"FocusAnimObject", PropertyType::kString, 0x0, nullptr},
    {"FocusAnimStartFrame", PropertyType::kString, 0x0, nullptr},
    {"FocusAnimEndFrame", PropertyType::kString, 0x0, nullptr},
};
static const PropertyTable kXuiButton_Table = {kXuiButton_Properties, 7};

static const Property kXuiCheckbox_Properties[] = {
    {"PressKey", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiCheckbox_Table = {kXuiCheckbox_Properties, 1};

static const Property kXuiCommonList_Properties[] = {
    {"ItemsText", PropertyType::kString, 0x0, nullptr},
    {"ItemsImage", PropertyType::kString, 0x0, nullptr},
    {"ItemsNavPath", PropertyType::kString, 0x0, nullptr},
};
static const PropertyTable kXuiCommonList_Table = {kXuiCommonList_Properties,
                                                   3};

static const Property kXuiControl_Properties[] = {
    {"ClassOverride", PropertyType::kString, 0x0, nullptr},
    {"Visual", PropertyType::kString, 0x0, nullptr},
    {"Enabled", PropertyType::kBool, 0x0, nullptr},
    {"UnfocusedInput", PropertyType::kBool, 0x0, nullptr},
    {"NavLeft", PropertyType::kString, 0x0, nullptr},
    {"NavRight", PropertyType::kString, 0x0, nullptr},
    {"NavUp", PropertyType::kString, 0x0, nullptr},
    {"NavDown", PropertyType::kString, 0x0, nullptr},
    {"NavTabForward", PropertyType::kString, 0x0, nullptr},
    {"NavTabBackward", PropertyType::kString, 0x0, nullptr},
    {"Text", PropertyType::kString, 0x4, nullptr},
    {"PointSize", PropertyType::kFloat, 0x6, nullptr},
    {"ImagePath", PropertyType::kString, 0x10, nullptr},
    {"HasContextMenu", PropertyType::kBool, 0x0, nullptr},
    {"SizeToText", PropertyType::kBool, 0x0, nullptr},
    {"UseNuiAsMouse", PropertyType::kBool, 0x0, nullptr},
    {"AutoId", PropertyType::kString, 0x0, nullptr},
    {"HoverSelectTimer", PropertyType::kFloat, 0x8, nullptr},
    {"QuickInput", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiControl_Table = {kXuiControl_Properties, 19};

static const Property kXuiEdit_Properties[] = {
    {"TextLimit", PropertyType::kUnsigned, 0x0, nullptr},
    {"AllowedChars", PropertyType::kString, 0x4, nullptr},
    {"PasswordChar", PropertyType::kString, 0x0, nullptr},
    {"ReadOnly", PropertyType::kBool, 0x0, nullptr},
    {"Multiline", PropertyType::kBool, 0x0, nullptr},
    {"SmoothScroll", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiEdit_Table = {kXuiEdit_Properties, 6};

static const Property kXuiElement_Properties[] = {
    {"Id", PropertyType::kString, 0x0, nullptr},
    {"Width", PropertyType::kFloat, 0x0, nullptr},
    {"Height", PropertyType::kFloat, 0x0, nullptr},
    {"Position", PropertyType::kVector, 0x0, nullptr},
    {"Scale", PropertyType::kVector, 0x0, nullptr},
    {"Rotation", PropertyType::kQuaternion, 0x0, nullptr},
    {"Opacity", PropertyType::kFloat, 0x0, nullptr},
    {"Anchor", PropertyType::kUnsigned, 0x0, nullptr},
    {"Pivot", PropertyType::kVector, 0x0, nullptr},
    {"Show", PropertyType::kBool, 0x0, nullptr},
    {"BlendMode", PropertyType::kUnsigned, 0x0, nullptr},
    {"DisableTimelineRecursion", PropertyType::kBool, 0x0, nullptr},
    {"DesignTime", PropertyType::kBool, 0x8, nullptr},
    {"ColorWriteFlags", PropertyType::kUnsigned, 0x0, nullptr},
    {"ClipChildren", PropertyType::kBool, 0x0, nullptr},
    {"EnableEffects", PropertyType::kBool, 0x2, nullptr},
    {"DisableFocusRecursion", PropertyType::kBool, 0x0, nullptr},
    {"GripTarget", PropertyType::kBool, 0x8, nullptr},
    {"Hittable", PropertyType::kBool, 0x0, nullptr},
    {"LayoutLineBreak", PropertyType::kBool, 0x8, nullptr},
    {"LayoutFloat", PropertyType::kBool, 0x8, nullptr},
    {"Column", PropertyType::kUnsigned, 0x0, nullptr},
    {"Row", PropertyType::kUnsigned, 0x0, nullptr},
    {"ColumnSpan", PropertyType::kUnsigned, 0x8, nullptr},
    {"RowSpan", PropertyType::kUnsigned, 0x8, nullptr},
    {"ColorFactor", PropertyType::kColor, 0x0, nullptr},
    {"CenterPivot", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiElement_Table = {kXuiElement_Properties, 27};

static const Property kXuiFigure_Stroke_Properties[] = {
    {"StrokeWidth", PropertyType::kFloat, 0x0, nullptr},
    {"StrokeColor", PropertyType::kColor, 0x0, nullptr},
};
static const PropertyTable kXuiFigure_Stroke_Table = {
    kXuiFigure_Stroke_Properties, 2};

static const Property kXuiFigure_Fill_Gradient_Properties[] = {
    {"Radial", PropertyType::kBool, 0x0, nullptr},
    {"NumStops", PropertyType::kInteger, 0xA, nullptr},
    {"StopColor", PropertyType::kColor, 0x1, nullptr},
    {"StopPos", PropertyType::kFloat, 0x1, nullptr},
};
static const PropertyTable kXuiFigure_Fill_Gradient_Table = {
    kXuiFigure_Fill_Gradient_Properties, 4};

static const Property kXuiFigure_Fill_Properties[] = {
    {"FillType", PropertyType::kUnsigned, 0x0, nullptr},
    {"FillColor", PropertyType::kColor, 0x2, nullptr},
    {"TextureFileName", PropertyType::kString, 0x2, nullptr},
    {"Gradient", PropertyType::kObject, 0x2, &kXuiFigure_Fill_Gradient_Table},
    {"Translation", PropertyType::kVector, 0x0, nullptr},
    {"Scale", PropertyType::kVector, 0x0, nullptr},
    {"Rotation", PropertyType::kFloat, 0x0, nullptr},
    {"WrapX", PropertyType::kUnsigned, 0x0, nullptr},
    {"WrapY", PropertyType::kUnsigned, 0x0, nullptr},
    {"BrushFlags", PropertyType::kUnsigned, 0x0, nullptr},
    {"TransformVersion", PropertyType::kUnsigned, 0xA, nullptr},
};
static const PropertyTable kXuiFigure_Fill_Table = {kXuiFigure_Fill_Properties,
                                                    11};

static const Property kXuiFigure_Properties[] = {
    {"Stroke", PropertyType::kObject, 0x0, &kXuiFigure_Stroke_Table},
    {"Fill", PropertyType::kObject, 0x0, &kXuiFigure_Fill_Table},
    {"Closed", PropertyType::kBool, 0x2, nullptr},
    {"Points", PropertyType::kCustom, 0x2, nullptr},
};
static const PropertyTable kXuiFigure_Table = {kXuiFigure_Properties, 4};

static const Property kXuiGamerCard_Properties[] = {
    {"Format", PropertyType::kString, 0x0, nullptr},
    {"ShowExtendedPanel", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiGamerCard_Table = {kXuiGamerCard_Properties, 2};

static const Property kXuiGridPanel_Properties[] = {
    {"Columns", PropertyType::kString, 0x0, nullptr},
    {"Rows", PropertyType::kString, 0x8, nullptr},
    {"CellSpacing", PropertyType::kFloat, 0x8, nullptr},
    {"Param0", PropertyType::kFloat, 0x0, nullptr},
    {"Param1", PropertyType::kFloat, 0x0, nullptr},
    {"Param2", PropertyType::kFloat, 0x0, nullptr},
    {"Param3", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiGridPanel_Table = {kXuiGridPanel_Properties, 7};

static const Property kXuiImage_Properties[] = {
    {"SizeMode", PropertyType::kUnsigned, 0x0, nullptr},
    {"ImagePath", PropertyType::kString, 0x10, nullptr},
    {"BrushFlags", PropertyType::kUnsigned, 0x0, nullptr},
    {"TextureSurfaceElement", PropertyType::kString, 0x0, nullptr},
    {"LoadType", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiImage_Table = {kXuiImage_Properties, 5};

static const Property kXuiImagePresenter_Properties[] = {
    {"SizeMode", PropertyType::kUnsigned, 0x0, nullptr},
    {"DataAssociation", PropertyType::kUnsigned, 0x0, nullptr},
    {"BrushFlags", PropertyType::kUnsigned, 0x0, nullptr},
    {"LoadType", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiImagePresenter_Table = {
    kXuiImagePresenter_Properties, 4};

static const Property kXuiLabel_Properties[] = {
    {"MaxFlowLines", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiLabel_Table = {kXuiLabel_Properties, 1};

static const Property kXuiList_Properties[] = {
    {"Wrap", PropertyType::kBool, 0x0, nullptr},
    {"WrapBump", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiList_Table = {kXuiList_Properties, 2};

static const Property kXuiListItem_Properties[] = {
    {"Layout", PropertyType::kUnsigned, 0x0, nullptr},
    {"Checkable", PropertyType::kBool, 0x0, nullptr},
    {"SelectedSize", PropertyType::kVector, 0x0, nullptr},
    {"KeepSizeUnfocused", PropertyType::kBool, 0x0, nullptr},
    {"InterItemSpacing", PropertyType::kVector, 0x0, nullptr},
    {"SmoothScroll", PropertyType::kBool, 0x0, nullptr},
    {"SmoothScrollBaseSpeed", PropertyType::kFloat, 0x0, nullptr},
    {"SmoothScrollMaxSpeed", PropertyType::kFloat, 0x0, nullptr},
    {"SmoothScrollAcceleration", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiListItem_Table = {kXuiListItem_Properties, 9};

static const Property kXuiNavButton_Properties[] = {
    {"PressPath", PropertyType::kString, 0x0, nullptr},
    {"StayVisible", PropertyType::kBool, 0x0, nullptr},
    {"SrcTransIndex", PropertyType::kUnsigned, 0x0, nullptr},
    {"DestTransIndex", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiNavButton_Table = {kXuiNavButton_Properties, 4};

static const Property kXuiNineGrid_Properties[] = {
    {"TextureFileName", PropertyType::kString, 0x0, nullptr},
    {"LeftOffset", PropertyType::kUnsigned, 0x8, nullptr},
    {"TopOffset", PropertyType::kUnsigned, 0x8, nullptr},
    {"RightOffset", PropertyType::kUnsigned, 0x8, nullptr},
    {"BottomOffset", PropertyType::kUnsigned, 0x8, nullptr},
    {"NoCenter", PropertyType::kBool, 0x8, nullptr},
};
static const PropertyTable kXuiNineGrid_Table = {kXuiNineGrid_Properties, 6};

static const Property kXuiPerspectiveScene_Properties[] = {
    {"ProjectionScale", PropertyType::kFloat, 0x0, nullptr},
    {"ProjectionCenterU", PropertyType::kFloat, 0x0, nullptr},
    {"ProjectionCenterV", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiPerspectiveScene_Table = {
    kXuiPerspectiveScene_Properties, 3};

static const Property kXuiProgressBar_Properties[] = {
    {"RangeMin", PropertyType::kInteger, 0x0, nullptr},
    {"RangeMax", PropertyType::kInteger, 0x8, nullptr},
    {"Value", PropertyType::kInteger, 0x0, nullptr},
};
static const PropertyTable kXuiProgressBar_Table = {kXuiProgressBar_Properties,
                                                    3};

static const Property kXuiRadioButton_Properties[] = {
    {"PressKey", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiRadioButton_Table = {kXuiRadioButton_Properties,
                                                    1};

static const Property kXuiScene_Properties[] = {
    {"DefaultFocus", PropertyType::kString, 0x0, nullptr},
    {"TransFrom", PropertyType::kString, 0x0, nullptr},
    {"TransTo", PropertyType::kString, 0x0, nullptr},
    {"TransBackFrom", PropertyType::kString, 0x0, nullptr},
    {"TransBackTo", PropertyType::kString, 0x0, nullptr},
    {"InterruptTransitions", PropertyType::kUnsigned, 0x8, nullptr},
    {"IgnorePresses", PropertyType::kBool, 0x0, nullptr},
    {"RecurseTransitions", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiScene_Table = {kXuiScene_Properties, 8};

static const Property kXuiScrollBar_Properties[] = {
    {"Direction", PropertyType::kUnsigned, 0x0, nullptr},
    {"MinThumbSize", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiScrollBar_Table = {kXuiScrollBar_Properties, 2};

static const Property kXuiScrollEnd_Properties[] = {
    {"Direction", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiScrollEnd_Table = {kXuiScrollEnd_Properties, 1};

static const Property kXuiShader_Properties[] = {
    {"Id", PropertyType::kString, 0x0, nullptr},
    {"ShaderFile", PropertyType::kString, 0x0, nullptr},
    {"TextureFileName", PropertyType::kString, 0x0, nullptr},
    {"TextureSurfaceElement", PropertyType::kString, 0x2, nullptr},
    {"WrapX", PropertyType::kUnsigned, 0x0, nullptr},
    {"WrapY", PropertyType::kUnsigned, 0x0, nullptr},
    {"BrushFlags", PropertyType::kUnsigned, 0x0, nullptr},
    {"CompositeEdge", PropertyType::kFloat, 0x0, nullptr},
    {"ForceComposite", PropertyType::kBool, 0x0, nullptr},
    {"EffectParams1", PropertyType::kVector, 0x0, nullptr},
    {"EffectParams2", PropertyType::kVector, 0x0, nullptr},
    {"EffectParams3", PropertyType::kVector, 0x0, nullptr},
    {"EffectParams4", PropertyType::kVector, 0x0, nullptr},
    {"EffectParams5", PropertyType::kVector, 0x0, nullptr},
};
static const PropertyTable kXuiShader_Table = {kXuiShader_Properties, 14};

static const Property kXuiSlider_Properties[] = {
    {"RangeMin", PropertyType::kInteger, 0x0, nullptr},
    {"RangeMax", PropertyType::kInteger, 0x8, nullptr},
    {"Value", PropertyType::kInteger, 0x0, nullptr},
    {"Step", PropertyType::kInteger, 0x0, nullptr},
    {"Vertical", PropertyType::kBool, 0x0, nullptr},
    {"AccelInc", PropertyType::kInteger, 0x0, nullptr},
    {"AccelTime", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiSlider_Table = {kXuiSlider_Properties, 7};

static const Property kXuiSound_Properties[] = {
    {"State", PropertyType::kUnsigned, 0x0, nullptr},
    {"Loop", PropertyType::kBool, 0x8, nullptr},
    {"Finish", PropertyType::kBool, 0x8, nullptr},
    {"Volume", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiSound_Table = {kXuiSound_Properties, 4};

static const Property kXuiSoundXAudio_Properties[] = {
    {"File", PropertyType::kString, 0x0, nullptr},
};
static const PropertyTable kXuiSoundXAudio_Table = {kXuiSoundXAudio_Properties,
                                                    1};

static const Property kXuiTabScene_Properties[] = {
    {"TabCount", PropertyType::kUnsigned, 0x0, nullptr},
    {"Wrap", PropertyType::kBool, 0x0, nullptr},
    {"UserInterrupt", PropertyType::kBool, 0x0, nullptr},
    {"VerticalTabs", PropertyType::kBool, 0x0, nullptr},
    {"NoAutoHide", PropertyType::kBool, 0x0, nullptr},
    {"DefaultTab", PropertyType::kUnsigned, 0x0, nullptr},
};
static const PropertyTable kXuiTabScene_Table = {kXuiTabScene_Properties, 6};

static const Property kXuiText_Properties[] = {
    {"Text", PropertyType::kString, 0x0, nullptr},
    {"TextColor", PropertyType::kColor, 0x0, nullptr},
    {"DropShadowColor", PropertyType::kColor, 0x0, nullptr},
    {"PointSize", PropertyType::kFloat, 0x84, nullptr},
    {"Font", PropertyType::kString, 0x0, nullptr},
    {"TextStyle", PropertyType::kUnsigned, 0x8, nullptr},
    {"LineSpacingAdjust", PropertyType::kInteger, 0x0, nullptr},
    {"TextScale", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiText_Table = {kXuiText_Properties, 8};

static const Property kXuiTextPresenter_Properties[] = {
    {"TextColor", PropertyType::kColor, 0x0, nullptr},
    {"DropShadowColor", PropertyType::kColor, 0x0, nullptr},
    {"PointSize", PropertyType::kFloat, 0xC, nullptr},
    {"Font", PropertyType::kString, 0x0, nullptr},
    {"TextStyle", PropertyType::kUnsigned, 0x8, nullptr},
    {"LineSpacingAdjust", PropertyType::kInteger, 0x0, nullptr},
    {"DataAssociation", PropertyType::kUnsigned, 0x0, nullptr},
    {"TextScale", PropertyType::kFloat, 0x0, nullptr},
};
static const PropertyTable kXuiTextPresenter_Table = {
    kXuiTextPresenter_Properties, 8};

static const Property kXuiTextureSurface_Properties[] = {
    {"Offscreen", PropertyType::kBool, 0x0, nullptr},
    {"DepthStencil", PropertyType::kBool, 0x8, nullptr},
    {"PreRender", PropertyType::kBool, 0x8, nullptr},
    {"Buffered", PropertyType::kBool, 0x0, nullptr},
};
static const PropertyTable kXuiTextureSurface_Table = {
    kXuiTextureSurface_Properties, 4};

static const Property kXuiVariable_Properties[] = {
    {"Id", PropertyType::kString, 0x0, nullptr},
    {"VectorVariable", PropertyType::kVector, 0x0, nullptr},
    {"FloatVariable", PropertyType::kFloat, 0x0, nullptr},
    {"IntegerVariable", PropertyType::kInteger, 0x0, nullptr},
};
static const PropertyTable kXuiVariable_Table = {kXuiVariable_Properties, 4};

static const XuiClass kClasses[] = {
    {"AchievementDetailsScene", "HUDScene", {nullptr, 0}},
    {"AchievementsGameListScene", "HUDScene", {nullptr, 0}},
    {"AddXBoxLiveScene", "HUDScene", {nullptr, 0}},
    {"AsyncTaskManager0", "XuiElement", {nullptr, 0}},
    {"AvatarAwardGameList", "XuiList", {nullptr, 0}},
    {"AvatarAwardGamesMeScene", "HUDScene", {nullptr, 0}},
    {"BaseScene", "XuiScene", {nullptr, 0}},
    {"BrushImageControl", "XuiScene", {nullptr, 0}},
    {"CAchievementsGridList", "XuiList", {nullptr, 0}},
    {"CAchievementsGridScene", "HUDScene", {nullptr, 0}},
    {"CAvatarAwardsGridList", "XuiList", {nullptr, 0}},
    {"CAvatarAwardsGridScene", "HUDScene", {nullptr, 0}},
    {"CSignInPreferencesScene", "HUDScene", {nullptr, 0}},
    {"CXuiEditAutoScroll", "XuiEdit", {nullptr, 0}},
    {"ChangeGamerTileScene", "HUDScene", {nullptr, 0}},
    {"ChangePersonalTileScene", "HUDScene", {nullptr, 0}},
    {"ClauseList", "XuiList", {nullptr, 0}},
    {"CongratulationsScene", "HUDScene", {nullptr, 0}},
    {"ControlPackNuiBack", "XuiControl", {kControlPackNuiBack_Properties, 1}},
    {"ControlPackNuiButton", "XuiButton", {nullptr, 0}},
    {"ControlPackNuiCheckbox", "XuiCheckbox", {nullptr, 0}},
    {"ControlPackNuiHoverBack",
     "XuiButton",
     {kControlPackNuiHoverBack_Properties, 7}},
    {"ControlPackNuiHoverButton",
     "XuiButton",
     {kControlPackNuiHoverButton_Properties, 7}},
    {"ControlPackNuiHoverCheckbox",
     "XuiCheckbox",
     {kControlPackNuiHoverCheckbox_Properties, 7}},
    {"ControlPackNuiHoverChrome",
     "XuiButton",
     {kControlPackNuiHoverChrome_Properties, 7}},
    {"ControlPackNuiRadioButton", "XuiRadioButton", {nullptr, 0}},
    {"ControlPackNuiSideNav",
     "XuiControl",
     {kControlPackNuiSideNav_Properties, 3}},
    {"ControlPackNuiSwipeNav",
     "XuiControl",
     {kControlPackNuiSwipeNav_Properties, 1}},
    {"ControlPackNuiTipsScene", "XuiScene", {nullptr, 0}},
    {"ControlPackNuiVScroll", "XuiElement", {nullptr, 0}},
    {"ControlPackSimpleCursorScene", "XuiScene", {nullptr, 0}},
    {"ControlPackSystemGesture", "XuiElement", {nullptr, 0}},
    {"ControlPackVariablesScene", "XuiScene", {nullptr, 0}},
    {"EditProfileScene", "HUDScene", {nullptr, 0}},
    {"ExtendedScene", "XuiScene", {nullptr, 0}},
    {"GameScene", "HUDScene", {nullptr, 0}},
    {"GameShowcaseMeScene", "HUDScene", {nullptr, 0}},
    {"GameShowcaseYouScene", "HUDScene", {nullptr, 0}},
    {"GamerCardScene", "XuiScene", {nullptr, 0}},
    {"GamerCardUIScene", "HUDScene", {nullptr, 0}},
    {"GamerPicButton", "XuiButton", {nullptr, 0}},
    {"GamerPreferencesList", "XuiList", {nullptr, 0}},
    {"GamerPreferencesScene", "HUDScene", {nullptr, 0}},
    {"GamerTagScene", "HUDScene", {nullptr, 0}},
    {"GamerTileList", "XuiList", {nullptr, 0}},
    {"HUDScene", "XuiScene", {kHUDScene_Properties, 6}},
    {"InsPadScene", "XuiScene", {nullptr, 0}},
    {"JoinScene", "XuiScene", {nullptr, 0}},
    {"KeyboardControl", "XuiScene", {nullptr, 0}},
    {"LoadAchievementsScene", "HUDScene", {nullptr, 0}},
    {"LoadAvatarAwardsScene", "HUDScene", {nullptr, 0}},
    {"LoadNuiAchievementsScene", "HUDScene", {nullptr, 0}},
    {"MeGameList", "XuiList", {nullptr, 0}},
    {"MeGamercardWorker", "XuiControl", {nullptr, 0}},
    {"MetaPanelScene", "XuiScene", {nullptr, 0}},
    {"NotifyPopupScene", "XuiScene", {nullptr, 0}},
    {"NuiAchievementDetailsScene", "HUDScene", {nullptr, 0}},
    {"NuiAchievementsGridScene", "HUDScene", {nullptr, 0}},
    {"NuiBack", "XuiScene", {nullptr, 0}},
    {"NuiNotch", "XuiScene", {nullptr, 0}},
    {"NuiSideNav", "XuiScene", {nullptr, 0}},
    {"NuiSignIn", "HUDScene", {nullptr, 0}},
    {"NuiUserListScene", "XuiScene", {nullptr, 0}},
    {"NuiVScroll", "XuiScene", {nullptr, 0}},
    {"PasscodeGridScene", "XuiScene", {nullptr, 0}},
    {"PasscodeScene", "XuiScene", {nullptr, 0}},
    {"PreferenceCategoryScene", "HUDScene", {nullptr, 0}},
    {"PreferenceSettingScene", "HUDScene", {nullptr, 0}},
    {"PreferenceValueList", "XuiList", {nullptr, 0}},
    {"RecoveryIntroScene", "HUDScene", {nullptr, 0}},
    {"ReputationScene", "XuiScene", {nullptr, 0}},
    {"SelectGamerZoneScene", "HUDScene", {nullptr, 0}},
    {"SelectedProfile", "XuiControl", {nullptr, 0}},
    {"SignIn", "HUDScene", {nullptr, 0}},
    {"SingleHandleVisual", "XuiScene", {nullptr, 0}},
    {"StatusScene", "XuiScene", {nullptr, 0}},
    {"UserList", "XuiList", {nullptr, 0}},
    {"UserListScene", "XuiScene", {nullptr, 0}},
    {"VKXuiButton", "XuiButton", {nullptr, 0}},
    {"XuiBackButton", "XuiButton", {nullptr, 0}},
    {"XuiButton", "XuiControl", {kXuiButton_Properties, 7}},
    {"XuiCanvas", "XuiElement", {nullptr, 0}},
    {"XuiCaret", "XuiControl", {nullptr, 0}},
    {"XuiCheckbox", "XuiControl", {kXuiCheckbox_Properties, 1}},
    {"XuiCommonList", "XuiList", {kXuiCommonList_Properties, 3}},
    {"XuiControl", "XuiElement", {kXuiControl_Properties, 19}},
    {"XuiEdit", "XuiControl", {kXuiEdit_Properties, 6}},
    {"XuiElement", nullptr, {kXuiElement_Properties, 27}},
    {"XuiFigure", "XuiElement", {kXuiFigure_Properties, 4}},
    {"XuiGamerCard", "XuiControl", {kXuiGamerCard_Properties, 2}},
    {"XuiGridPanel", "XuiElement", {kXuiGridPanel_Properties, 7}},
    {"XuiGroup", "XuiElement", {nullptr, 0}},
    {"XuiImage", "XuiElement", {kXuiImage_Properties, 5}},
    {"XuiImagePresenter", "XuiElement", {kXuiImagePresenter_Properties, 4}},
    {"XuiLabel", "XuiControl", {kXuiLabel_Properties, 1}},
    {"XuiList", "XuiControl", {kXuiList_Properties, 2}},
    {"XuiListItem", "XuiCheckbox", {kXuiListItem_Properties, 9}},
    {"XuiMessageBox", "XuiScene", {nullptr, 0}},
    {"XuiNavButton", "XuiButton", {kXuiNavButton_Properties, 4}},
    {"XuiNineGrid", "XuiElement", {kXuiNineGrid_Properties, 6}},
    {"XuiPerspectiveScene", "XuiScene", {kXuiPerspectiveScene_Properties, 3}},
    {"XuiProgressBar", "XuiControl", {kXuiProgressBar_Properties, 3}},
    {"XuiRadioButton", "XuiControl", {kXuiRadioButton_Properties, 1}},
    {"XuiRadioGroup", "XuiControl", {nullptr, 0}},
    {"XuiScene", "XuiControl", {kXuiScene_Properties, 8}},
    {"XuiScrollBar", "XuiControl", {kXuiScrollBar_Properties, 2}},
    {"XuiScrollEnd", "XuiControl", {kXuiScrollEnd_Properties, 1}},
    {"XuiShader", "XuiElement", {kXuiShader_Properties, 14}},
    {"XuiSlider", "XuiControl", {kXuiSlider_Properties, 7}},
    {"XuiSound", "XuiElement", {kXuiSound_Properties, 4}},
    {"XuiSoundXAudio", "XuiSound", {kXuiSoundXAudio_Properties, 1}},
    {"XuiTabScene", "XuiScene", {kXuiTabScene_Properties, 6}},
    {"XuiText", "XuiElement", {kXuiText_Properties, 8}},
    {"XuiTextPresenter", "XuiElement", {kXuiTextPresenter_Properties, 8}},
    {"XuiTextureSurface", "XuiGroup", {kXuiTextureSurface_Properties, 4}},
    {"XuiTransition", "XuiElement", {nullptr, 0}},
    {"XuiVariable", "XuiElement", {kXuiVariable_Properties, 4}},
    {"XuiVisual", "XuiElement", {nullptr, 0}},
    {"YouAchievementsList", "XuiList", {nullptr, 0}},
    {"YouAchievementsScene", "HUDScene", {nullptr, 0}},
    {"YouAsyncWorker", "XuiControl", {nullptr, 0}},
    {"YouGameList", "XuiList", {nullptr, 0}},
    {"YouGamercardWorker", "XuiControl", {nullptr, 0}},
};

const XuiClass* FindClass(const std::string_view name) {
  for (const auto& entry : kClasses) {
    if (name == entry.name) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<const XuiClass*> ClassChain(const std::string_view name) {
  std::vector<const XuiClass*> chain;
  const XuiClass* current = FindClass(name);
  while (current) {
    if (std::find(chain.begin(), chain.end(), current) != chain.end()) {
      break;
    }
    chain.push_back(current);
    current = current->base ? FindClass(current->base) : nullptr;
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

uint8_t PackedReader::ReadByte() {
  if (at_ >= size_) {
    failed_ = true;
    return 0;
  }
  return data_[at_++];
}

uint16_t PackedReader::ReadUint16() {
  if (at_ + 2 > size_) {
    failed_ = true;
    at_ = size_;
    return 0;
  }
  const uint16_t value = xe::load_and_swap<uint16_t>(data_ + at_);
  at_ += 2;
  return value;
}

uint32_t PackedReader::ReadUint32() {
  if (at_ + 4 > size_) {
    failed_ = true;
    at_ = size_;
    return 0;
  }
  const uint32_t value = xe::load_and_swap<uint32_t>(data_ + at_);
  at_ += 4;
  return value;
}

float PackedReader::ReadFloat() {
  const uint32_t bits = ReadUint32();
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t PackedReader::ReadPacked() {
  const uint8_t lead = ReadByte();
  if (lead == 0xFF) {
    return ReadUint32();
  }
  if (lead >= 0xF0) {
    return ((lead & 0x0F) << 8) | ReadByte();
  }
  return lead;
}

bool OpenPackage(const uint8_t* data, size_t size,
                 std::vector<PackageEntry>* out_entries) {
  out_entries->clear();
  if (!data || size < kPackageHeaderSize) {
    return false;
  }
  size_t base = 0;
  bool found = false;
  for (size_t i = 0; i + 4 <= size; ++i) {
    if (std::memcmp(data + i, "XUIZ", 4) == 0) {
      base = i;
      found = true;
      break;
    }
  }
  if (!found || base + kPackageHeaderSize > size) {
    return false;
  }
  const uint32_t version = xe::load_and_swap<uint32_t>(data + base + 4);
  if (version != 3) {
    XELOGW("xui: unsupported package version {}", version);
    return false;
  }
  const uint32_t directory_size = xe::load_and_swap<uint32_t>(data + base + 16);
  const uint16_t count = xe::load_and_swap<uint16_t>(data + base + 20);
  const size_t content = base + kPackageHeaderSize + directory_size;
  if (content > size) {
    return false;
  }
  size_t at = base + kPackageHeaderSize;
  for (uint16_t i = 0; i < count; ++i) {
    if (at + 9 > size) {
      return false;
    }
    const uint32_t entry_size = xe::load_and_swap<uint32_t>(data + at);
    const uint32_t entry_offset = xe::load_and_swap<uint32_t>(data + at + 4);
    const uint8_t name_length = data[at + 8];
    at += 9;
    if (at + name_length > size) {
      return false;
    }
    PackageEntry entry;
    entry.name.assign(reinterpret_cast<const char*>(data + at), name_length);
    at += name_length;
    if (content + entry_offset + entry_size > size) {
      return false;
    }
    entry.data = data + content + entry_offset;
    entry.size = entry_size;
    out_entries->push_back(std::move(entry));
  }
  return true;
}

const PackageEntry* FindPackageEntry(const std::vector<PackageEntry>& entries,
                                     const std::string_view name) {
  for (const auto& entry : entries) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool Scene::Load(const uint8_t* data, size_t size) {
  strings_.clear();
  floats_.clear();
  vectors_.clear();
  quaternions_.clear();
  colors_.clear();
  sections_.clear();
  pool_.clear();
  root_ = Node();
  if (!data || size < kSceneHeaderSize) {
    return false;
  }
  size_t base = 0;
  bool found = false;
  for (size_t i = 0; i + 4 <= size; ++i) {
    if (std::memcmp(data + i, "XUIB", 4) == 0) {
      base = i;
      found = true;
      break;
    }
  }
  if (!found || base + kSceneHeaderSize > size) {
    return false;
  }
  const uint32_t version = xe::load_and_swap<uint32_t>(data + base + 4);
  const uint16_t kind = xe::load_and_swap<uint16_t>(data + base + 0x0C);
  const uint32_t total = xe::load_and_swap<uint32_t>(data + base + 0x0E);
  const uint16_t section_count =
      xe::load_and_swap<uint16_t>(data + base + 0x12);
  if (version != 8 || kind != 0x0E) {
    XELOGW("xui: unsupported scene {}/{}", version, kind);
    return false;
  }
  if (base + total != size) {
    XELOGW("xui: scene size {} does not match {}", total, size - base);
    return false;
  }
  PackedReader header(data + base + kSceneHeaderSize,
                      size - base - kSceneHeaderSize);
  for (size_t i = 0; i < kPoolCount; ++i) {
    pool_hint_[i] = header.ReadPacked();
  }
  size_t at = base + kSceneHeaderSize + header.offset();
  for (uint16_t i = 0; i < section_count; ++i) {
    if (at + 12 > size) {
      return false;
    }
    Section section;
    section.tag.assign(reinterpret_cast<const char*>(data + at), 4);
    const uint32_t offset = xe::load_and_swap<uint32_t>(data + at + 4);
    const uint32_t length = xe::load_and_swap<uint32_t>(data + at + 8);
    at += 12;
    if (base + offset + length > size) {
      return false;
    }
    section.data = data + base + offset;
    section.size = length;
    sections_.push_back(std::move(section));
  }
  LoadStrings();
  LoadNumbers();
  // The object stream indexes the key and name pools as it goes, so they have
  // to be standing before it is read.
  LoadKeyPools();
  const Section* data_section = FindSection("DATA");
  if (!data_section) {
    return false;
  }
  PackedReader reader(data_section->data, data_section->size);
  root_ = ReadObject(reader);
  if (reader.failed()) {
    XELOGW("xui: scene object stream overran");
    return false;
  }
  ResolveTimelines(root_);
  return true;
}

const Scene::Section* Scene::FindSection(const std::string_view tag) const {
  for (const auto& section : sections_) {
    if (section.tag == tag) {
      return &section;
    }
  }
  return nullptr;
}

void Scene::LoadStrings() {
  const Section* section = FindSection("STRN");
  if (!section || section->size < 6) {
    return;
  }
  const uint16_t count = xe::load_and_swap<uint16_t>(section->data + 4);
  size_t at = 6;
  while (at < section->size && strings_.size() < count) {
    const uint8_t* start = section->data + at;
    const size_t remaining = section->size - at;
    const void* end = std::memchr(start, 0, remaining);
    const size_t length =
        end ? static_cast<size_t>(static_cast<const uint8_t*>(end) - start)
            : remaining;
    strings_.emplace_back(reinterpret_cast<const char*>(start), length);
    at += length + 1;
  }
}

void Scene::LoadNumbers() {
  if (const Section* section = FindSection("FLOT")) {
    for (size_t at = 0; at + 4 <= section->size; at += 4) {
      const uint32_t bits = xe::load_and_swap<uint32_t>(section->data + at);
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      floats_.push_back(value);
    }
  }
  if (const Section* section = FindSection("VECT")) {
    for (size_t at = 0; at + 12 <= section->size; at += 12) {
      Vector vector;
      for (size_t i = 0; i < 3; ++i) {
        const uint32_t bits =
            xe::load_and_swap<uint32_t>(section->data + at + i * 4);
        std::memcpy(&vector.value[i], &bits, sizeof(float));
      }
      vectors_.push_back(vector);
    }
  }
  if (const Section* section = FindSection("QUAT")) {
    for (size_t at = 0; at + 16 <= section->size; at += 16) {
      Quaternion value;
      for (size_t i = 0; i < 4; ++i) {
        const uint32_t bits =
            xe::load_and_swap<uint32_t>(section->data + at + i * 4);
        std::memcpy(&value.value[i], &bits, sizeof(float));
      }
      quaternions_.push_back(value);
    }
  }
  if (const Section* section = FindSection("COLR")) {
    for (size_t at = 0; at + 4 <= section->size; at += 4) {
      colors_.push_back(xe::load_and_swap<uint32_t>(section->data + at));
    }
  }
}

namespace {

// Interpolation modes that carry a tangent vector.
bool InterpolationHasTangent(uint8_t interpolation) {
  return interpolation == 7 || interpolation == 10 || interpolation == 11 ||
         interpolation == 12;
}

// Named states below this kind describe the owning object; from 2 up they
// name another object to drive.
constexpr uint8_t kStateHasTarget = 2;

}  // namespace

void Scene::LoadKeyPools() {
  key_values_.clear();
  key_records_.clear();
  state_records_.clear();

  // Every animatable value in DATA is written as a pool index, so the key
  // value block is a flat array of packed indices; what each one means comes
  // from the track that reads it.
  if (const Section* section = FindSection("KEYP")) {
    PackedReader reader(section->data, section->size);
    while (!reader.eof() && !reader.failed()) {
      key_values_.push_back(reader.ReadPacked());
    }
    if (reader.failed() && !key_values_.empty()) {
      key_values_.pop_back();
    }
  }

  // Both blocks are one continuous run of packed records, not an array of
  // fixed slots, so a record can only be reached by decoding everything ahead
  // of it. An object's timelines index this decoded array.
  if (const Section* section = FindSection("KEYD")) {
    PackedReader reader(section->data, section->size);
    while (!reader.eof() && !reader.failed()) {
      Key key;
      key.frame = reader.ReadPacked();
      const uint8_t mode = reader.ReadByte();
      key.interpolation = mode & 0x3F;
      key.flags = mode >> 6;
      if (key.interpolation == kInterpolateEase) {
        for (size_t j = 0; j < 3; ++j) {
          key.ease[j] = reader.ReadByte();
        }
      } else if (InterpolationHasTangent(key.interpolation)) {
        key.has_tangent = true;
        key.tangent = reader.ReadPacked();
      }
      key.value_base = reader.ReadPacked();
      if (reader.failed()) {
        break;
      }
      key_records_.push_back(key);
    }
  }

  if (const Section* section = FindSection("NAME")) {
    PackedReader reader(section->data, section->size);
    while (!reader.eof() && !reader.failed()) {
      TimelineState state;
      state.name = StringAt(reader.ReadPacked());
      state.frame = reader.ReadPacked();
      state.kind = reader.ReadByte();
      if (state.kind >= kStateHasTarget) {
        state.target = StringAt(reader.ReadPacked());
      }
      if (reader.failed()) {
        break;
      }
      state_records_.push_back(std::move(state));
    }
  }
}

float Scene::FloatAt(uint32_t index, float fallback) const {
  return index < floats_.size() ? floats_[index] : fallback;
}

uint32_t Scene::ColorAt(uint32_t index, uint32_t fallback) const {
  return index < colors_.size() ? colors_[index] : fallback;
}

Vector Scene::VectorAt(uint32_t index) const {
  return index < vectors_.size() ? vectors_[index] : Vector();
}

Quaternion Scene::QuaternionAt(uint32_t index) const {
  return index < quaternions_.size() ? quaternions_[index] : Quaternion();
}

float Scene::RotationDegrees(const Quaternion& value) {
  // Only the z term survives a rotation in the plane, so the angle comes
  // straight out of the z and w components.
  const float angle = 2.0f * std::atan2(value.value[2], value.value[3]);
  return angle * 180.0f / 3.14159265f;
}

// The track names its property as a position in the target's class chain
// (base first, exactly as the property blocks are written) plus a descent
// through any nested object properties.
bool Scene::ResolveTrack(const std::string_view class_name,
                         Track& track) const {
  const std::vector<const XuiClass*> chain = ClassChain(class_name);
  if (track.levels >= chain.size()) {
    return false;
  }
  const PropertyTable* table = &chain[track.levels]->properties;
  if (track.property >= table->count) {
    return false;
  }
  const Property* property = &table->properties[track.property];
  for (const uint8_t step : track.path) {
    if (!property->sub || step >= property->sub->count) {
      return false;
    }
    table = property->sub;
    property = &table->properties[step];
  }
  track.property_name = property->name;
  track.property_type = property->type;
  return true;
}

void Scene::ResolveTimeline(Node& owner, Timeline& timeline) {
  // A timeline drives one object: a named child, or the owner itself when the
  // target is its own id or absent.
  const Node* target = &owner;
  if (!timeline.target.empty()) {
    for (const Node& child : owner.children) {
      const Value* id = FindProperty(child, "Id");
      if (id && id->string == timeline.target) {
        target = &child;
        break;
      }
    }
  }

  for (Track& track : timeline.tracks) {
    ResolveTrack(target->class_name, track);
  }

  timeline.keys.clear();
  timeline.keys.reserve(timeline.key_count);
  for (uint32_t i = 0; i < timeline.key_count; ++i) {
    const size_t at = timeline.key_block + i;
    if (at >= key_records_.size()) {
      break;
    }
    timeline.keys.push_back(key_records_[at]);
  }

  // Key j of track t sits one element past key j of track t-1, so a whole
  // key's worth of tracks is contiguous from its own base.
  for (size_t t = 0; t < timeline.tracks.size(); ++t) {
    Track& track = timeline.tracks[t];
    track.values.clear();
    track.values.reserve(timeline.keys.size());
    for (const Key& key : timeline.keys) {
      const size_t at = key.value_base + t;
      track.values.push_back(at < key_values_.size() ? key_values_[at] : 0);
    }
  }
}

void Scene::ResolveTimelines(Node& node) {
  for (Timeline& timeline : node.timelines) {
    ResolveTimeline(node, timeline);
  }
  for (Node& child : node.children) {
    ResolveTimelines(child);
  }
}

std::string Scene::StringAt(uint32_t index) const {
  if (!index || index > strings_.size()) {
    return std::string();
  }
  return strings_[index - 1];
}

Value Scene::ReadValue(PackedReader& reader, const Property& property) {
  Value value;
  value.type = property.type;
  switch (property.type) {
    case PropertyType::kBool:
      value.boolean = reader.ReadByte() != 0;
      break;
    case PropertyType::kInteger:
    case PropertyType::kUnsigned:
      value.integer = reader.ReadPacked();
      break;
    case PropertyType::kFloat: {
      const uint32_t index = reader.ReadPacked();
      value.number = index < floats_.size() ? floats_[index] : 0.0f;
      break;
    }
    case PropertyType::kString:
      value.string = StringAt(reader.ReadPacked());
      break;
    case PropertyType::kColor: {
      const uint32_t index = reader.ReadPacked();
      value.color = index < colors_.size() ? colors_[index] : 0;
      break;
    }
    case PropertyType::kVector: {
      const uint32_t index = reader.ReadPacked();
      if (index < vectors_.size()) {
        value.vector = vectors_[index];
      }
      break;
    }
    case PropertyType::kQuaternion:
    case PropertyType::kCustom:
      value.index = reader.ReadPacked();
      break;
    case PropertyType::kObject:
      value.object = std::make_shared<Node>(ReadNested(reader, property));
      break;
    default:
      reader.set_failed();
      break;
  }
  return value;
}

Node Scene::ReadNested(PackedReader& reader, const Property& property) {
  Node node;
  const uint32_t index = reader.ReadPacked();
  node.shared_index = index;
  const auto existing = pool_.find(index);
  if (existing != pool_.end()) {
    node.shared = true;
    node.properties = existing->second;
    return node;
  }
  reader.ReadPacked();
  if (property.sub) {
    node.properties = ReadBlock(reader, *property.sub);
  }
  pool_.emplace(index, node.properties);
  return node;
}

PropertyMap Scene::ReadBlock(PackedReader& reader, const PropertyTable& table) {
  PropertyMap values;
  if (reader.eof()) {
    return values;
  }
  const uint32_t mask = reader.ReadPacked();
  for (uint32_t i = 0; i < table.count; ++i) {
    if (!(mask & (1u << i))) {
      continue;
    }
    const Property& property = table.properties[i];
    const uint32_t count = (property.flags & 1) ? reader.ReadPacked() : 1;
    std::vector<Value> items;
    items.reserve(count);
    for (uint32_t item = 0; item < count; ++item) {
      items.push_back(ReadValue(reader, property));
    }
    values.emplace(property.name, std::move(items));
  }
  return values;
}

PropertyMap Scene::ReadProperties(PackedReader& reader,
                                  const std::string_view class_name) {
  PropertyMap values;
  for (const XuiClass* level : ClassChain(class_name)) {
    if (reader.eof()) {
      break;
    }
    const uint32_t mask = reader.ReadPacked();
    if (!mask) {
      continue;
    }
    for (uint32_t i = 0; i < level->properties.count; ++i) {
      if (!(mask & (1u << i))) {
        continue;
      }
      const Property& property = level->properties.properties[i];
      const uint32_t count = (property.flags & 1) ? reader.ReadPacked() : 1;
      std::vector<Value> items;
      items.reserve(count);
      for (uint32_t item = 0; item < count; ++item) {
        items.push_back(ReadValue(reader, property));
      }
      values.emplace(property.name, std::move(items));
    }
  }
  return values;
}

Node Scene::ReadObject(PackedReader& reader) {
  Node node;
  const uint32_t class_index = reader.ReadPacked();
  node.flags = reader.ReadByte();
  node.class_name = StringAt(class_index);
  if (node.flags & kObjectHasProperties) {
    node.property_count = reader.ReadPacked();
    node.properties = ReadProperties(reader, node.class_name);
  } else if (node.flags & kObjectIsShared) {
    node.shared = true;
    node.shared_index = reader.ReadPacked();
  }
  if (node.flags & kObjectHasChildren) {
    const uint32_t count = reader.ReadPacked();
    node.children.reserve(count);
    for (uint32_t i = 0; i < count && !reader.eof(); ++i) {
      node.children.push_back(ReadObject(reader));
    }
  }
  if (node.flags & kObjectHasTimelines) {
    node.timelines =
        ReadTimelines(reader, !node.children.empty(), &node.states);
  }
  return node;
}

Track Scene::ReadTrack(PackedReader& reader) {
  Track track;
  const uint8_t head = reader.ReadByte();
  const uint8_t depth = head & 0x7F;
  if (depth) {
    track.levels = reader.ReadByte();
    track.property = reader.ReadByte();
    for (uint8_t i = 1; i < depth; ++i) {
      track.path.push_back(reader.ReadByte());
    }
  }
  if (head & 0x80) {
    track.has_data = true;
    track.data = reader.ReadPacked();
  }
  return track;
}

std::vector<Timeline> Scene::ReadTimelines(PackedReader& reader,
                                           bool has_children,
                                           std::vector<TimelineState>* states) {
  std::vector<Timeline> timelines;
  // The named states are a run inside the shared NAME block, not a block of
  // this object's own.
  const uint32_t named_count = reader.ReadPacked();
  if (named_count) {
    const uint32_t named_base = reader.ReadPacked();
    for (uint32_t i = 0; i < named_count; ++i) {
      const size_t at = named_base + i;
      if (at >= state_records_.size()) {
        break;
      }
      states->push_back(state_records_[at]);
    }
  }
  if (!has_children) {
    return timelines;
  }
  const uint32_t count = reader.ReadPacked();
  timelines.reserve(count);
  for (uint32_t i = 0; i < count && !reader.eof(); ++i) {
    Timeline timeline;
    timeline.target = StringAt(reader.ReadPacked());
    const uint32_t tracks = reader.ReadPacked();
    timeline.tracks.reserve(tracks);
    for (uint32_t track = 0; track < tracks && !reader.eof(); ++track) {
      timeline.tracks.push_back(ReadTrack(reader));
    }
    timeline.key_count = reader.ReadPacked();
    timeline.key_block = reader.ReadPacked();
    timelines.push_back(std::move(timeline));
  }
  return timelines;
}

const Value* FindProperty(const Node& node, const std::string_view name) {
  const auto it = node.properties.find(std::string(name));
  if (it == node.properties.end() || it->second.empty()) {
    return nullptr;
  }
  return &it->second.front();
}

}  // namespace xui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
