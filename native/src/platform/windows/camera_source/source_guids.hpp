#pragma once

#include <guiddef.h>

// The media source's CLSID. syncd passes this to MFCreateVirtualCamera, the
// elevated registration step writes it under HKLM, and the frame server
// resolves it to this DLL. All three have to agree, so it is defined here and
// nowhere else.
// {2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}
inline constexpr GUID kSyncCameraSourceClsid = {
    0x2f8e7b14, 0x9c3d, 0x4a62, {0xb5, 0xe1, 0x7d, 0x4a, 0x9f, 0x2c, 0x6b, 0x08}};

inline constexpr wchar_t kSyncCameraSourceClsidString[] =
    L"{2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}";
inline constexpr wchar_t kSyncCameraSourceFriendlyName[] = L"Sync Camera Source";

// Media Foundation appends "Windows Virtual Camera" to whatever friendly name
// the camera is created with, so this is deliberately just the product name:
// passing "Sync Camera" would put "Sync Camera Windows Virtual Camera" in
// every picker.
inline constexpr wchar_t kSyncCameraDisplayName[] = L"Sync";
