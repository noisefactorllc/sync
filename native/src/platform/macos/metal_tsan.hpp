#pragma once

// ThreadSanitizer cannot see Metal's own synchronization. A completed handler
// is copied (Block_copy, with every value it captures) on the thread that adds
// it, then runs on a Metal GCD thread; the two are ordered only by code TSan
// does not instrument, so TSan reports the handler's reads of its own captures
// as races. The publisher states that ordering: release on the command buffer
// after adding the handler, acquire on the same command buffer as the
// handler's first action. Both compile to nothing without TSan.

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define SYNC_METAL_TSAN 1
#endif
#endif

#if defined(SYNC_METAL_TSAN)
extern "C" void __tsan_acquire(void* address);
extern "C" void __tsan_release(void* address);
#endif

namespace noisefactor::sync::detail {

inline void metal_handler_added(const void* command_buffer) noexcept {
#if defined(SYNC_METAL_TSAN)
  __tsan_release(const_cast<void*>(command_buffer));
#else
  (void)command_buffer;
#endif
}

inline void metal_handler_started(const void* command_buffer) noexcept {
#if defined(SYNC_METAL_TSAN)
  __tsan_acquire(const_cast<void*>(command_buffer));
#else
  (void)command_buffer;
#endif
}

}  // namespace noisefactor::sync::detail
