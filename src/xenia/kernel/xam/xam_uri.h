/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_XAM_URI_H_
#define XENIA_KERNEL_XAM_XAM_URI_H_

#include <cstdint>
#include <string>

#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// What a resolved URI turned out to name.
//
// The dashboard navigates by URI - every hub tile is one. "My Games" is
// literally `gameslibrary:_RootTitle=My Games;` and "My Apps" is
// `applibrary:_LibraryType=apps;`. Neither names a title on its own: xam
// rewrites the scheme, over and over, until what is left starts with one of
// the four terminal schemes below.
struct UriResolution {
  // The whole URI after every rewrite, e.g.
  //   title:FFFE07D1:pam:BuiltIn.Hub.xzp:root:BuiltIn.ContentApp.xzp:
  //   contentlibrary:PackageType=1;_RootTitle=My Games;
  // This exact string - not a piece of it - is what the launch data carries.
  std::string uri;
  // The terminal scheme: "title", "app", "game" or "sap".
  std::string scheme;
  // Everything after "<scheme>:".
  std::string rest;
  // Parsed out of `rest` when it begins with eight hex digits and a colon.
  bool has_title_id = false;
  uint32_t title_id = 0;
  // Whatever followed "%08X:".
  std::string args;
};

// Expands a URI the way xam 17559 does (sub_819A0000), at most ten times.
//
// `trusted` is the caller's privileged bit - bit 31 of the flags word in the
// 0x00022003 request. It is not constant: matching any rewrite record sets it,
// so an untrusted `gameslibrary:` reaches the privileged `hub:` record two
// rewrites later exactly as it does on hardware. Do not seed it true.
//
// `prefer_content_explorer` picks between the two expansions a record may
// carry - see the note on the table in the .cc. Passing false selects the
// built-in package, which is what a console without the ContentExplorer
// module resolves to.
X_HRESULT ResolveUri(const std::string& uri, bool trusted,
                     bool prefer_content_explorer, UriResolution* out);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_XAM_URI_H_
