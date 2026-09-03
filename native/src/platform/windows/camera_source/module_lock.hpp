#pragma once

#include <atomic>

namespace noisefactor::sync::camera {

// How many live objects this DLL has handed out.
//
// DllCanUnloadNow has to answer for outstanding objects as well as explicit
// LockServer calls. Counting only locks says "yes, unload me" while the frame
// server still holds a media source, and COM hosts really do act on that --
// CoFreeUnusedLibraries runs on idle timers. The DLL then unloads under a live
// object and every vtable pointer in it dangles, which is an access violation
// inside the frame server and takes the camera down for every application.
[[nodiscard]] inline auto module_references() noexcept -> std::atomic<long>& {
  static std::atomic<long> references{0};
  return references;
}

// Held as a member by every COM object this DLL creates, so the count follows
// object lifetime with no way to forget one half of it.
class ModuleReference {
 public:
  ModuleReference() noexcept { module_references().fetch_add(1, std::memory_order_relaxed); }
  ~ModuleReference() { module_references().fetch_sub(1, std::memory_order_acq_rel); }

  ModuleReference(const ModuleReference&) = delete;
  auto operator=(const ModuleReference&) -> ModuleReference& = delete;
  ModuleReference(ModuleReference&&) = delete;
  auto operator=(ModuleReference&&) -> ModuleReference& = delete;
};

}  // namespace noisefactor::sync::camera
