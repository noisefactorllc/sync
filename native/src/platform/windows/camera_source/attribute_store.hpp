#pragma once

#include <windows.h>

#include <mfapi.h>
#include <mfobjects.h>
#include <wrl/client.h>

namespace noisefactor::sync::camera {

// Implements IMFAttributes by forwarding to a store made with
// MFCreateAttributes.
//
// The frame server asks the activator, the source and the stream for an
// attribute store, and IMFActivate is itself an IMFAttributes, so all three
// would otherwise repeat the same thirty forwarding methods. Deriving classes
// still supply QueryInterface, AddRef and Release; only the attribute half
// lives here.
template <typename Interface>
class AttributeStore : public Interface {
 public:
  [[nodiscard]] auto InitializeAttributeStore(UINT32 initial_size) -> HRESULT {
    return ::MFCreateAttributes(&attributes_, initial_size);
  }

  [[nodiscard]] auto attribute_store() const noexcept -> IMFAttributes* {
    return attributes_.Get();
  }

  auto STDMETHODCALLTYPE GetItem(REFGUID key, PROPVARIANT* value) -> HRESULT override {
    return attributes_->GetItem(key, value);
  }
  auto STDMETHODCALLTYPE GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) -> HRESULT override {
    return attributes_->GetItemType(key, type);
  }
  auto STDMETHODCALLTYPE CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result)
      -> HRESULT override {
    return attributes_->CompareItem(key, value, result);
  }
  auto STDMETHODCALLTYPE Compare(IMFAttributes* other, MF_ATTRIBUTES_MATCH_TYPE type,
                                 BOOL* result) -> HRESULT override {
    return attributes_->Compare(other, type, result);
  }
  auto STDMETHODCALLTYPE GetUINT32(REFGUID key, UINT32* value) -> HRESULT override {
    return attributes_->GetUINT32(key, value);
  }
  auto STDMETHODCALLTYPE GetUINT64(REFGUID key, UINT64* value) -> HRESULT override {
    return attributes_->GetUINT64(key, value);
  }
  auto STDMETHODCALLTYPE GetDouble(REFGUID key, double* value) -> HRESULT override {
    return attributes_->GetDouble(key, value);
  }
  auto STDMETHODCALLTYPE GetGUID(REFGUID key, GUID* value) -> HRESULT override {
    return attributes_->GetGUID(key, value);
  }
  auto STDMETHODCALLTYPE GetStringLength(REFGUID key, UINT32* length) -> HRESULT override {
    return attributes_->GetStringLength(key, length);
  }
  auto STDMETHODCALLTYPE GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length)
      -> HRESULT override {
    return attributes_->GetString(key, value, size, length);
  }
  auto STDMETHODCALLTYPE GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length)
      -> HRESULT override {
    return attributes_->GetAllocatedString(key, value, length);
  }
  auto STDMETHODCALLTYPE GetBlobSize(REFGUID key, UINT32* size) -> HRESULT override {
    return attributes_->GetBlobSize(key, size);
  }
  auto STDMETHODCALLTYPE GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* written)
      -> HRESULT override {
    return attributes_->GetBlob(key, buffer, size, written);
  }
  auto STDMETHODCALLTYPE GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size)
      -> HRESULT override {
    return attributes_->GetAllocatedBlob(key, buffer, size);
  }
  auto STDMETHODCALLTYPE GetUnknown(REFGUID key, REFIID riid, LPVOID* object)
      -> HRESULT override {
    return attributes_->GetUnknown(key, riid, object);
  }
  auto STDMETHODCALLTYPE SetItem(REFGUID key, REFPROPVARIANT value) -> HRESULT override {
    return attributes_->SetItem(key, value);
  }
  auto STDMETHODCALLTYPE DeleteItem(REFGUID key) -> HRESULT override {
    return attributes_->DeleteItem(key);
  }
  auto STDMETHODCALLTYPE DeleteAllItems() -> HRESULT override {
    return attributes_->DeleteAllItems();
  }
  auto STDMETHODCALLTYPE SetUINT32(REFGUID key, UINT32 value) -> HRESULT override {
    return attributes_->SetUINT32(key, value);
  }
  auto STDMETHODCALLTYPE SetUINT64(REFGUID key, UINT64 value) -> HRESULT override {
    return attributes_->SetUINT64(key, value);
  }
  auto STDMETHODCALLTYPE SetDouble(REFGUID key, double value) -> HRESULT override {
    return attributes_->SetDouble(key, value);
  }
  auto STDMETHODCALLTYPE SetGUID(REFGUID key, REFGUID value) -> HRESULT override {
    return attributes_->SetGUID(key, value);
  }
  auto STDMETHODCALLTYPE SetString(REFGUID key, LPCWSTR value) -> HRESULT override {
    return attributes_->SetString(key, value);
  }
  auto STDMETHODCALLTYPE SetBlob(REFGUID key, const UINT8* buffer, UINT32 size)
      -> HRESULT override {
    return attributes_->SetBlob(key, buffer, size);
  }
  auto STDMETHODCALLTYPE SetUnknown(REFGUID key, IUnknown* value) -> HRESULT override {
    return attributes_->SetUnknown(key, value);
  }
  auto STDMETHODCALLTYPE LockStore() -> HRESULT override { return attributes_->LockStore(); }
  auto STDMETHODCALLTYPE UnlockStore() -> HRESULT override { return attributes_->UnlockStore(); }
  auto STDMETHODCALLTYPE GetCount(UINT32* count) -> HRESULT override {
    return attributes_->GetCount(count);
  }
  auto STDMETHODCALLTYPE GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value)
      -> HRESULT override {
    return attributes_->GetItemByIndex(index, key, value);
  }
  auto STDMETHODCALLTYPE CopyAllItems(IMFAttributes* destination) -> HRESULT override {
    return attributes_->CopyAllItems(destination);
  }

 protected:
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
};

}  // namespace noisefactor::sync::camera
