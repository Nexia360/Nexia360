/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include "xenia/base/platform_win.h"

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | \
                            WINAPI_PARTITION_SYSTEM | WINAPI_PARTITION_GAMES)
#define XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
#endif
/*
        these two dont bypass much ms garbage compared to the threading ones,
   but Protect is used by PhysicalHeap::EnableAccessCallbacks which eats a lot
   of cpu time, so every bit counts
*/
XE_NTDLL_IMPORT(NtProtectVirtualMemory, cls_NtProtectVirtualMemory,
                NtProtectVirtualMemoryPointer);
XE_NTDLL_IMPORT(NtQueryVirtualMemory, cls_NtQueryVirtualMemory,
                NtQueryVirtualMemoryPointer);
namespace xe {
namespace memory {

size_t page_size() {
#if XE_ARCH_AMD64 == 1
  return 4096;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwPageSize;
  }
  return value;
#endif
}

size_t allocation_granularity() {
#if XE_ARCH_AMD64 == 1 && XE_PLATFORM_WIN32 == 1
  return 65536;
#else
  static size_t value = 0;
  if (!value) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    value = si.dwAllocationGranularity;
  }
  return value;
#endif
}

DWORD ToWin32ProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PAGE_NOACCESS;
    case PageAccess::kReadOnly:
      return PAGE_READONLY;
    case PageAccess::kReadWrite:
      return PAGE_READWRITE;
    case PageAccess::kExecuteReadOnly:
      return PAGE_EXECUTE_READ;
    case PageAccess::kExecuteReadWrite:
      return PAGE_EXECUTE_READWRITE;
    default:
      assert_unhandled_case(access);
      return PAGE_NOACCESS;
  }
}

PageAccess ToXeniaProtectFlags(DWORD access) {
  if (access & PAGE_GUARD) {
    // Strip the page guard flag for now...
    access &= ~PAGE_GUARD;
  }

  switch (access) {
    case PAGE_NOACCESS:
      return PageAccess::kNoAccess;
    case PAGE_READONLY:
      return PageAccess::kReadOnly;
    case PAGE_READWRITE:
      return PageAccess::kReadWrite;
    case PAGE_EXECUTE_READ:
      return PageAccess::kExecuteReadOnly;
    case PAGE_EXECUTE_READWRITE:
      return PageAccess::kExecuteReadWrite;
    default:
      return PageAccess::kNoAccess;
  }
}

bool IsWritableExecutableMemorySupported() {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return true;
#else
  // To test FromApp functions on desktop, undefine
  // XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS and link to WindowsApp.lib.
  return false;
#endif
}

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  DWORD alloc_type = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      alloc_type = MEM_RESERVE;
      break;
    case AllocationType::kCommit:
      alloc_type = MEM_COMMIT;
      break;
    case AllocationType::kReserveCommit:
      alloc_type = MEM_RESERVE | MEM_COMMIT;
      break;
    default:
      assert_unhandled_case(allocation_type);
      break;
  }
  DWORD protect = ToWin32ProtectFlags(access);
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return VirtualAlloc(base_address, length, alloc_type, protect);
#else
  return VirtualAllocFromApp(base_address, length, ULONG(alloc_type),
                             ULONG(protect));
#endif
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  DWORD free_type = 0;
  switch (deallocation_type) {
    case DeallocationType::kRelease:
      free_type = MEM_RELEASE;
      length = 0;
      break;
    case DeallocationType::kDecommit:
      free_type = MEM_DECOMMIT;
      break;
    default:
      assert_unhandled_case(deallocation_type);
      break;
  }
  return VirtualFree(base_address, length, free_type) ? true : false;
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  DWORD new_protect = ToWin32ProtectFlags(access);

#if XE_USE_NTDLL_FUNCTIONS == 1

  DWORD old_protect = 0;
  SIZE_T MemoryLength = length;
  PVOID MemoryCache = base_address;

  BOOL result = NtProtectVirtualMemoryPointer.invoke<NTSTATUS>(
                    (HANDLE)0xFFFFFFFFFFFFFFFFLL, &MemoryCache, &MemoryLength,
                    new_protect, &old_protect) >= 0;

#else
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD old_protect = 0;
  BOOL result = VirtualProtect(base_address, length, new_protect, &old_protect);
#else
  ULONG old_protect = 0;
  BOOL result = VirtualProtectFromApp(base_address, length, ULONG(new_protect),
                                      &old_protect);
#endif
#endif
  if (!result) {
    return false;
  }
  if (out_old_access) {
    *out_old_access = ToXeniaProtectFlags(DWORD(old_protect));
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;

  MEMORY_BASIC_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
#if XE_USE_NTDLL_FUNCTIONS == 1
  ULONG_PTR ResultLength;

  NTSTATUS query_result = NtQueryVirtualMemoryPointer.invoke<NTSTATUS>(
      (HANDLE)0xFFFFFFFFFFFFFFFFLL, (PVOID)base_address,
      0 /* MemoryBasicInformation*/, &info, length, &ResultLength);
  SIZE_T result = query_result >= 0 ? ResultLength : 0;
#else
  SIZE_T result = VirtualQuery(base_address, &info, sizeof(info));
#endif
  if (!result) {
    return false;
  }

  length = info.RegionSize;
  access_out = ToXeniaProtectFlags(info.Protect);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
  DWORD protect =
      ToWin32ProtectFlags(access) | (commit ? SEC_COMMIT : SEC_RESERVE);
  auto full_path = "Local" / path;
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, protect,
                            static_cast<DWORD>(length >> 32),
                            static_cast<DWORD>(length), full_path.c_str());
#else
  return CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, ULONG(protect),
                                  ULONG64(length), full_path.c_str());
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  CloseHandle(handle);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  DWORD target_address_low = static_cast<DWORD>(file_offset);
  DWORD target_address_high = static_cast<DWORD>(file_offset >> 32);
  DWORD file_access = 0;
  switch (access) {
    case PageAccess::kReadOnly:
      file_access = FILE_MAP_READ;
      break;
    case PageAccess::kReadWrite:
      file_access = FILE_MAP_ALL_ACCESS;
      break;
    case PageAccess::kExecuteReadOnly:
      file_access = FILE_MAP_READ | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kExecuteReadWrite:
      file_access = FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE;
      break;
    case PageAccess::kNoAccess:
    default:
      assert_unhandled_case(access);
      return nullptr;
  }
  return MapViewOfFileEx(handle, file_access, target_address_high,
                         target_address_low, length, base_address);
#else
  // VirtualAlloc2FromApp and MapViewOfFile3FromApp were added in 10.0.17134.0.
  // https://docs.microsoft.com/en-us/uwp/win32-and-com/win32-apis
  HANDLE process = GetCurrentProcess();
  void* placeholder = VirtualAlloc2FromApp(
      process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
  if (!placeholder) {
    return nullptr;
  }
  void* mapping = MapViewOfFile3FromApp(
      handle, process, placeholder, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
  if (!mapping) {
    VirtualFree(placeholder, length, MEM_RELEASE);
    return nullptr;
  }
  return mapping;
#endif
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  return UnmapViewOfFile(base_address) ? true : false;
}

// Placeholders, added in 10.0.17134.0, are what let a view start on any page.
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif

namespace {
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
// Looked up so the desktop build doesn't need onecore.lib.
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE process, PVOID base_address,
                                       SIZE_T size, ULONG allocation_type,
                                       ULONG page_protection,
                                       void* extended_parameters,
                                       ULONG parameter_count);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE file_mapping, HANDLE process,
                                        PVOID base_address, ULONG64 offset,
                                        SIZE_T view_size, ULONG allocation_type,
                                        ULONG page_protection,
                                        void* extended_parameters,
                                        ULONG parameter_count);
struct PlaceholderFunctions {
  VirtualAlloc2Fn virtual_alloc_2 = nullptr;
  MapViewOfFile3Fn map_view_of_file_3 = nullptr;
  PlaceholderFunctions() {
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
      virtual_alloc_2 = reinterpret_cast<VirtualAlloc2Fn>(
          GetProcAddress(kernelbase, "VirtualAlloc2"));
      map_view_of_file_3 = reinterpret_cast<MapViewOfFile3Fn>(
          GetProcAddress(kernelbase, "MapViewOfFile3"));
    }
  }
};
const PlaceholderFunctions& GetPlaceholderFunctions() {
  static const PlaceholderFunctions functions;
  return functions;
}
#endif
}  // namespace

bool ReserveFileViewPages(void* base_address, size_t length) {
  HANDLE process = GetCurrentProcess();
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  const auto& functions = GetPlaceholderFunctions();
  if (!functions.virtual_alloc_2 || !functions.map_view_of_file_3) {
    return false;
  }
  void* placeholder = functions.virtual_alloc_2(
      process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
#else
  void* placeholder = VirtualAlloc2FromApp(
      process, base_address, length, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
      PAGE_NOACCESS, nullptr, 0);
#endif
  if (!placeholder) {
    return false;
  }
  // A view replaces a whole placeholder and each page needs its own.
  const size_t page = page_size();
  for (size_t offset = 0; offset + page < length; offset += page) {
    if (!VirtualFree(static_cast<uint8_t*>(placeholder) + offset, page,
                     MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
      ReleaseFileViewPages(nullptr, placeholder, length);
      return false;
    }
  }
  return true;
}

void* MapFileViewPages(FileMappingHandle handle, void* base_address,
                       size_t length, PageAccess access, size_t file_offset) {
  // Pages are separate placeholders and one view replaces one of them.
  if (length != page_size()) {
    return nullptr;
  }
  HANDLE process = GetCurrentProcess();
#ifdef XE_BASE_MEMORY_WIN_USE_DESKTOP_FUNCTIONS
  const auto& functions = GetPlaceholderFunctions();
  if (!functions.map_view_of_file_3) {
    return nullptr;
  }
  return functions.map_view_of_file_3(
      handle, process, base_address, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ToWin32ProtectFlags(access), nullptr, 0);
#else
  return MapViewOfFile3FromApp(
      handle, process, base_address, ULONG64(file_offset), length,
      MEM_REPLACE_PLACEHOLDER, ULONG(ToWin32ProtectFlags(access)), nullptr, 0);
#endif
}

bool ReleaseFileViewPages(FileMappingHandle handle, void* base_address,
                          size_t length) {
  bool released = true;
  const size_t page = page_size();
  for (size_t offset = 0; offset < length; offset += page) {
    uint8_t* address = static_cast<uint8_t*>(base_address) + offset;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State == MEM_FREE) {
      continue;
    }
    if (info.Type == MEM_MAPPED) {
      released &= UnmapViewOfFile(address) != FALSE;
    } else {
      released &= VirtualFree(address, 0, MEM_RELEASE) != FALSE;
    }
  }
  return released;
}

}  // namespace memory
}  // namespace xe
