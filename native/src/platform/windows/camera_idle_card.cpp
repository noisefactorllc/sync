#include <sync/platform/camera_idle_card.hpp>

#include <windows.h>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <iterator>

namespace noisefactor::sync::camera {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kMessage[] = L"Sync: waiting for Noisedeck";
// Deliberately not pure black: a viewer has to be able to tell a drawn card
// from a dead signal, and 0x14 reads as "off" without reading as "broken".
constexpr float kBackgroundLevel = 0x14 / 255.0F;

void paint_opaque_black(std::span<std::byte> bgra, std::size_t canvas_stride,
                        CameraCanvas canvas) noexcept {
  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* out = bgra.data() + static_cast<std::size_t>(row) * canvas_stride;
    for (std::uint32_t column = 0; column < canvas.width; ++column) {
      out[0] = std::byte{0};
      out[1] = std::byte{0};
      out[2] = std::byte{0};
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

}  // namespace

auto draw_camera_idle_card(std::span<std::byte> bgra, std::size_t canvas_stride,
                           CameraCanvas canvas) noexcept -> bool {
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (bgra.size() < canvas_stride * canvas.height) return false;

  // Past this point the buffer is ours, so any failure leaves an opaque black
  // canvas rather than whatever the caller's memory happened to hold.
  const auto fail_black = [&]() noexcept -> bool {
    paint_opaque_black(bgra, canvas_stride, canvas);
    return false;
  };

  ComPtr<IWICImagingFactory> wic;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
    return fail_black();
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(canvas.width, canvas.height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnLoad, &bitmap))) {
    return fail_black();
  }

  ComPtr<ID2D1Factory> d2d;
  if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
    return fail_black();
  }
  // A software target on a WIC bitmap: no window, no device, so this runs the
  // same inside the frame server as it does under ctest.
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
    return fail_black();
  }

  ComPtr<IDWriteFactory> dwrite;
  if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
    return fail_black();
  }
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      canvas.height / 18.0F, L"en-us", &format))) {
    return fail_black();
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  target->BeginDraw();
  target->Clear(D2D1::ColorF(kBackgroundLevel, kBackgroundLevel, kBackgroundLevel, 1.0F));
  ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.78F, 0.80F, 0.84F, 1.0F), &brush))) {
    const D2D1_RECT_F box = D2D1::RectF(0.0F, 0.0F, static_cast<float>(canvas.width),
                                        static_cast<float>(canvas.height));
    target->DrawTextW(kMessage, static_cast<UINT32>(std::size(kMessage) - 1), format.Get(), box,
                      brush.Get());
  }
  if (FAILED(target->EndDraw())) return fail_black();

  ComPtr<IWICBitmapLock> lock;
  const WICRect whole{0, 0, static_cast<INT>(canvas.width), static_cast<INT>(canvas.height)};
  if (FAILED(bitmap->Lock(&whole, WICBitmapLockRead, &lock))) return fail_black();
  UINT source_stride = 0;
  UINT source_bytes = 0;
  WICInProcPointer source = nullptr;
  if (FAILED(lock->GetStride(&source_stride)) ||
      FAILED(lock->GetDataPointer(&source_bytes, &source)) || source == nullptr) {
    return fail_black();
  }
  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* out = bgra.data() + static_cast<std::size_t>(row) * canvas_stride;
    std::memcpy(out, source + static_cast<std::size_t>(row) * source_stride,
                static_cast<std::size_t>(canvas.width) * kBytesPerPixel);
    // The clear is opaque so alpha is already 255, but a camera has no alpha
    // to offer and the contract promises opaque, so make it so regardless.
    for (std::uint32_t column = 0; column < canvas.width; ++column) {
      out[static_cast<std::size_t>(column) * kBytesPerPixel + 3] = std::byte{255};
    }
  }
  return true;
}

}  // namespace noisefactor::sync::camera
