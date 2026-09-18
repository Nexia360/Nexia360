/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/avatar_editor/avatar_editor.h"

#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace xam {
namespace avatar_editor {

void NotTranslated(const char* action, const char* origin) {
  XELOGW("avatar_editor: {} is not translated yet [{}]", action, origin);
}

}  // namespace avatar_editor
}  // namespace xam
}  // namespace kernel
}  // namespace xe
