/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/app_manager.h"

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/apps/messenger_app.h"
#include "xenia/kernel/xam/apps/xam_app.h"
#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xam/apps/xlivebase_app.h"
#include "xenia/kernel/xam/apps/xmp_app.h"

namespace xe {
namespace kernel {
namespace xam {

App::App(KernelState* kernel_state, uint32_t app_id)
    : kernel_state_(kernel_state),
      memory_(kernel_state->memory()),
      app_id_(app_id) {}

void AppManager::RegisterApps(KernelState* kernel_state, AppManager* manager) {
  manager->RegisterApp(std::make_unique<apps::MessengerApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XmpApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XgiApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XLiveBaseApp>(kernel_state));
  manager->RegisterApp(std::make_unique<apps::XamApp>(kernel_state));
}

void AppManager::RegisterApp(std::unique_ptr<App> app) {
  assert_zero(app_lookup_.count(app->app_id()));
  app_lookup_.insert({app->app_id(), app.get()});
  apps_.push_back(std::move(app));
}

X_HRESULT AppManager::DispatchMessageSync(uint32_t app_id, uint32_t message,
                                          uint32_t buffer_ptr,
                                          uint32_t buffer_length) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }
  return it->second->DispatchMessageSync(message, buffer_ptr, buffer_length);
}

X_HRESULT AppManager::DispatchMessageAsync(uint32_t app_id, uint32_t message,
                                           uint32_t buffer_ptr,
                                           uint32_t buffer_length,
                                           uint32_t overlapped_ptr) {
  const auto& it = app_lookup_.find(app_id);
  if (it == app_lookup_.end()) {
    return X_E_NOTFOUND;
  }

  Memory* memory = it->second->kernel_state_->memory();

  auto buffer_in = memory->SystemHeapAlloc(buffer_length);

  auto hostOldBufferAddr = memory->TranslateVirtual(buffer_ptr);
  auto hostBufferAddr = memory->TranslateVirtual(buffer_in);

  memcpy(hostBufferAddr, hostOldBufferAddr, buffer_length);

  auto post = [memory, buffer_in]() { memory->SystemHeapFree(buffer_in); };

  auto run = [it, message, buffer_in, buffer_length](
                 uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    // XStorageDownloadToMemory:
    // 41560817, 545107D1 (TU0), 4D5307D6
    //
    // Expects X_ONLINE_E_STORAGE_FILE_NOT_FOUND as extended error and
    // X_ERROR_FUNCTION_FAILED as result

    extended_error =
        it->second->DispatchMessageSync(message, buffer_in, buffer_length);

    return extended_error == X_ERROR_SUCCESS ? X_ERROR_SUCCESS
                                             : X_ERROR_FUNCTION_FAILED;
  };

  if (overlapped_ptr) {
    it->second->kernel_state_->CompleteOverlappedDeferredEx(run, overlapped_ptr,
                                                            nullptr, post);
    return X_ERROR_IO_PENDING;
  };

  // Sync will return extended error codes.
  // Add extended error parameter to apps?
  const X_HRESULT result =
      DispatchMessageSync(app_id, message, buffer_in, buffer_length);
  post();
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
