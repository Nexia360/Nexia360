/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/upnp.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

// Defined in XLiveAPI.cpp. Gates the DemonWare NAT-type override below: we only
// force Open when netplay is actually being relayed through the Nexia Hub
// transport (where every peer is reachable via the XUID relay regardless of
// real NAT). Off the transport, direct connections depend on the real NAT, so
// we must NOT fake it.
DECLARE_bool(nexiahub_transport);

namespace xe {
namespace kernel {
namespace xam {

dword_result_t XMsgInProcessCall_entry(dword_t app, dword_t message,
                                       dword_t arg1, dword_t arg2) {
  // DemonWare NAT-type read. IW/Treyarch titles' statically-linked
  // bdSocketManager reports the NAT type through XMsgInProcessCall on app 252
  // (0xFC), fn 0x00058006 (call site 0x82510E8C: lis r4,0x5; ori r4,0x8006),
  // writing the enum into *arg1:
  //   0 = Unknown, 1 = Open, 2 = Moderate, 3 = Strict.
  // The type is produced by a UDP:3074 DemonWare probe to stun.*.demonware.net,
  // which are dead -- with no responder it times out and the title reads
  // Strict(3), which gates matchmaking (e.g. MW3 sub_82350F88 blocks on ==3).
  // We have no socket manager to run that probe, so it can never succeed and
  // the title always reads a timeout. Answer it ourselves from what we DO know
  // about reachability:
  //   UPnP mapped our ports  -> Open(1). Inbound reaches us, which is exactly
  //                             what the probe was trying to establish.
  //   relayed via Nexia Hub  -> Open(1). Every peer is reachable through the
  //                             XUID relay, so real NAT is irrelevant.
  //   neither                -> Moderate(2). Honest: peers may still connect,
  //                             but nothing here has proven inbound works, and
  //                             claiming Open would produce joins that hang.
  if (app == 252 && message == 0x00058006 && arg1) {
    auto* out =
        kernel_state()->memory()->TranslateVirtual<xe::be<uint32_t>*>(arg1);
    if (out) {
      const auto upnp = kernel_state()->emulator()->GetUPnP();
      const bool reachable =
          (upnp && upnp->IsActive()) || cvars::nexiahub_transport;

      *out = reachable ? 1 : 2;
    }
    return X_ERROR_SUCCESS;
  }

  auto result = kernel_state()->app_manager()->DispatchMessageSync(app, message,
                                                                   arg1, arg2);
  if (result == X_ERROR_NOT_FOUND) {
    XELOGE("XMsgInProcessCall: app {:08X} undefined",
           static_cast<uint32_t>(app));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XMsgInProcessCall, kNone, kImplemented);

dword_result_t XMsgSystemProcessCall_entry(dword_t app, dword_t message,
                                           dword_t buffer,
                                           dword_t buffer_length) {
  auto result = kernel_state()->app_manager()->DispatchMessageSync(
      app, message, buffer, buffer_length);
  if (result == X_ERROR_NOT_FOUND) {
    XELOGE("XMsgSystemProcessCall: app {:08X} undefined",
           static_cast<uint32_t>(app));
  }
  return result;
}
DECLARE_XAM_EXPORT1(XMsgSystemProcessCall, kNone, kImplemented);

struct XMSGSTARTIOREQUEST_UNKNOWNARG {
  be<uint32_t> unk_0;
  be<uint32_t> unk_1;
};

X_HRESULT xeXMsgStartIORequestEx(uint32_t app, uint32_t message,
                                 uint32_t overlapped_ptr, uint32_t buffer_ptr,
                                 uint32_t buffer_length,
                                 XMSGSTARTIOREQUEST_UNKNOWNARG* unknown) {
  auto result = kernel_state()->app_manager()->DispatchMessageAsync(
      app, message, buffer_ptr, buffer_length, overlapped_ptr);

  if (result == X_E_NOTFOUND) {
    XELOGE("XMsgStartIORequestEx: app {:08X} undefined", app);
    result = X_E_INVALIDARG;
    XThread::SetLastError(X_ERROR_NOT_FOUND);
  }

  if (result == X_ERROR_SUCCESS || result == X_ERROR_IO_PENDING) {
    XThread::SetLastError(0);
  }

  return result;
}

dword_result_t XMsgStartIORequestEx_entry(
    dword_t app, dword_t message, pointer_t<XAM_OVERLAPPED> overlapped_ptr,
    dword_t buffer_ptr, dword_t buffer_length,
    pointer_t<XMSGSTARTIOREQUEST_UNKNOWNARG> unknown_ptr) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr, buffer_ptr,
                                buffer_length, unknown_ptr);
}
DECLARE_XAM_EXPORT1(XMsgStartIORequestEx, kNone, kImplemented);

dword_result_t XMsgStartIORequest_entry(
    dword_t app, dword_t message, pointer_t<XAM_OVERLAPPED> overlapped_ptr,
    dword_t buffer_ptr, dword_t buffer_length) {
  return xeXMsgStartIORequestEx(app, message, overlapped_ptr, buffer_ptr,
                                buffer_length, nullptr);
}
DECLARE_XAM_EXPORT1(XMsgStartIORequest, kNone, kImplemented);

dword_result_t XMsgCancelIORequest_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, dword_t wait) {
  X_HANDLE event_handle = XOverlappedGetEvent(overlapped_ptr);
  if (event_handle && wait) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Wait(0, 0, true, nullptr);
    }
  }

  return 0;
}
DECLARE_XAM_EXPORT1(XMsgCancelIORequest, kNone, kImplemented);

dword_result_t XMsgCompleteIORequest_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, dword_t result,
    dword_t extended_error, dword_t length) {
  kernel_state()->CompleteOverlappedImmediateEx(overlapped_ptr, result,
                                                extended_error, length);
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XMsgCompleteIORequest, kNone, kImplemented, kSketchy);

dword_result_t XamGetOverlappedResult_entry(
    pointer_t<XAM_OVERLAPPED> overlapped_ptr, lpdword_t length_ptr,
    dword_t wait) {
  uint32_t result = X_STATUS_SUCCESS;

  if (overlapped_ptr->result != X_ERROR_IO_PENDING) {
    result = overlapped_ptr->result;
  } else if (wait && overlapped_ptr->event) {
    auto ev = kernel_state()->object_table()->LookupObject<XEvent>(
        overlapped_ptr->event);
    result = ev->Wait(3, 1, 0, nullptr);
  } else {
    result = X_STATUS_TIMEOUT;
  }

  if (result == X_STATUS_TIMEOUT) {
    return X_ERROR_IO_INCOMPLETE;
  }

  if (XFAILED(result)) {
    return XThread::GetLastError();
  }

  if (length_ptr) {
    *length_ptr = overlapped_ptr->length;
  }

  return result;
}
DECLARE_XAM_EXPORT2(XamGetOverlappedResult, kNone, kImplemented, kSketchy);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Msg);
