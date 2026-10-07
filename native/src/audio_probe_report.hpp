#pragma once

#include <cstdint>
#include <string_view>

namespace noisefactor::sync::audio::probe {

// The qualification harness classifies every failed capture attempt so the
// driver warmup retry cannot launder a substantive failure into a pass.
enum class FailureClass {
  kQualified,
  kStartup,
  kIntegrity,
};

inline constexpr const char* failure_class_name(FailureClass klass) {
  switch (klass) {
    case FailureClass::kQualified: return "qualified";
    case FailureClass::kStartup: return "startup";
    case FailureClass::kIntegrity: return "integrity";
  }
  return "startup";
}

// A capture that never produced a format or never produced any frame never
// delivered samples, so the endpoint was unavailable and the attempt is
// retryable startup behavior. Any capture that did produce frames and still
// failed checked real samples and is a substantive integrity failure: it must
// not be retried, because a later clean attempt cannot qualify the source
// whose samples were bad.
inline constexpr FailureClass classify_failure(bool qualified, bool have_format,
                                               std::uint64_t received_frames) {
  if (qualified) return FailureClass::kQualified;
  return (have_format && received_frames != 0) ? FailureClass::kIntegrity
                                                : FailureClass::kStartup;
}

// A capture whose read threw partway through the window is not qualified,
// whatever the statistics collected before the throw say: the window is
// partial. The thrown text is carried in the report's failure field.
inline constexpr bool capture_qualified(bool statistics_qualified,
                                        std::string_view thrown_failure) {
  return statistics_qualified && thrown_failure.empty();
}

}  // namespace noisefactor::sync::audio::probe
