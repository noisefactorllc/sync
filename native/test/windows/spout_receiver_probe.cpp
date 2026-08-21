// Independent Spout receiver for certifying Sync's Spout publisher.
//
// Deliberately built with MinGW against a MinGW build of SpoutLibrary, while
// the sender under test is the MSVC-built DLL inside the shipped syncd. Two
// different builds of the library agreeing about the same shared texture is a
// stronger statement than one build agreeing with itself.
//
// It reads the DXGI shared texture directly rather than going through
// OpenGL: no GL context, and it sees exactly the bytes the sender published.

#include <windows.h>

#include <GL/gl.h>

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "SpoutLibrary.h"

namespace {

SPOUTLIBRARY* load_spout(const char* dll_path) {
  const HMODULE module = ::LoadLibraryA(dll_path);
  if (module == nullptr) {
    std::fprintf(stderr, "could not load %s (error %lu)\n", dll_path, ::GetLastError());
    return nullptr;
  }
  using get_spout_fn = SPOUTLIBRARY* (*)();
  const auto get_spout =
      reinterpret_cast<get_spout_fn>(::GetProcAddress(module, "GetSpout"));
  if (get_spout == nullptr) {
    std::fprintf(stderr, "GetSpout missing from %s\n", dll_path);
    return nullptr;
  }
  return get_spout();
}

const char* format_name(DWORD format) {
  switch (format) {
    case 87: return "DXGI_FORMAT_B8G8R8A8_UNORM";
    case 28: return "DXGI_FORMAT_R8G8B8A8_UNORM";
    default: return "other";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::fprintf(stderr,
                 "usage: spout_receiver_probe <SpoutLibrary.dll> <name-substring> "
                 "<width> <height> [timeout-ms]\n");
    return 2;
  }
  const char* dll_path = argv[1];
  const std::string wanted = argv[2];
  const unsigned expected_width = static_cast<unsigned>(std::atoi(argv[3]));
  const unsigned expected_height = static_cast<unsigned>(std::atoi(argv[4]));
  const int timeout_ms = argc > 5 ? std::atoi(argv[5]) : 15000;

  SPOUTLIBRARY* spout = load_spout(dll_path);
  if (spout == nullptr) return 3;

  std::string found;
  unsigned width = 0;
  unsigned height = 0;
  HANDLE share_handle = nullptr;
  DWORD format = 0;
  const DWORD deadline = ::GetTickCount() + static_cast<DWORD>(timeout_ms);
  while (::GetTickCount() < deadline && found.empty()) {
    const int count = spout->GetSenderCount();
    for (int index = 0; index < count; ++index) {
      char name[256] = {};
      if (!spout->GetSender(index, name, sizeof(name))) continue;
      if (std::string(name).find(wanted) == std::string::npos) continue;
      if (!spout->GetSenderInfo(name, width, height, share_handle, format)) continue;
      found = name;
      break;
    }
    if (found.empty()) ::Sleep(200);
  }
  if (found.empty()) {
    std::fprintf(stderr, "no Spout sender matching \"%s\" appeared within %dms\n",
                 wanted.c_str(), timeout_ms);
    return 4;
  }
  std::printf("discovered sender: %s\n", found.c_str());
  std::printf("  %ux%u format=%lu (%s) share=%p\n", width, height, format,
              format_name(format), share_handle);

  if (expected_width != 0 && width != expected_width) {
    std::fprintf(stderr, "width %u != expected %u\n", width, expected_width);
    return 6;
  }
  if (expected_height != 0 && height != expected_height) {
    std::fprintf(stderr, "height %u != expected %u\n", height, expected_height);
    return 6;
  }
  if (share_handle == nullptr) {
    std::fprintf(stderr, "sender published no shared texture handle\n");
    return 6;
  }

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level{};
  HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                   &context);
  if (FAILED(hr)) {
    std::fprintf(stderr, "no D3D11 hardware device (hr=0x%08lX)\n", hr);
    return 3;
  }

  ID3D11Texture2D* shared = nullptr;
  hr = device->OpenSharedResource(share_handle, __uuidof(ID3D11Texture2D),
                                  reinterpret_cast<void**>(&shared));
  if (FAILED(hr) || shared == nullptr) {
    std::fprintf(stderr, "could not open the shared texture (hr=0x%08lX)\n", hr);
    return 5;
  }

  D3D11_TEXTURE2D_DESC desc{};
  shared->GetDesc(&desc);
  std::printf("  shared texture: %ux%u dxgi_format=%u\n", desc.Width, desc.Height,
              static_cast<unsigned>(desc.Format));

  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(hr)) {
    std::fprintf(stderr, "could not create a staging texture (hr=0x%08lX)\n", hr);
    return 5;
  }

  context->CopyResource(staging, shared);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    std::fprintf(stderr, "could not map the staging texture (hr=0x%08lX)\n", hr);
    return 5;
  }

  const auto* rows = static_cast<const uint8_t*>(mapped.pData);
  const auto sample = [&](unsigned y, unsigned x) {
    return rows + static_cast<size_t>(y) * mapped.RowPitch + static_cast<size_t>(x) * 4;
  };
  const uint8_t* upper = sample(desc.Height / 4, desc.Width / 2);
  const uint8_t* lower = sample(desc.Height * 3 / 4, desc.Width / 2);

  // Spout's shared texture is usually BGRA, so the byte order on this side is
  // not the RGBA order the frame was published in. Read the channels by
  // meaning rather than by index.
  const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
  const int r = bgra ? 2 : 0;
  const int b = bgra ? 0 : 2;

  std::printf("  upper quarter r=%u g=%u b=%u a=%u\n", upper[r], upper[1], upper[b], upper[3]);
  std::printf("  lower quarter r=%u g=%u b=%u a=%u\n", lower[r], lower[1], lower[b], lower[3]);

  int status = 0;
  // A shared texture is uncompressed, so unlike NDI these are the exact bytes
  // that were published and can be compared exactly.
  const bool upper_exact = upper[r] == 0xe0 && upper[b] == 0x10 && upper[3] == 0xff;
  const bool lower_exact = lower[r] == 0x10 && lower[b] == 0xe0 && lower[3] == 0xff;
  const bool flipped = upper[b] == 0xe0 && lower[r] == 0xe0;

  if (upper_exact && lower_exact) {
    std::printf("SPOUT RECEIVE OK: exact pixels, correct size, and upright\n");
  } else if (flipped) {
    std::fprintf(stderr, "IMAGE IS VERTICALLY FLIPPED: blue half arrived on top\n");
    status = 7;
  } else {
    std::fprintf(stderr, "pixels do not match the published pattern exactly\n");
    status = 6;
  }

  context->Unmap(staging, 0);
  staging->Release();
  shared->Release();
  context->Release();
  device->Release();
  return status;
}
