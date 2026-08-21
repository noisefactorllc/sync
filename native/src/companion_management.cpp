#include "companion_management.hpp"

namespace noisefactor::sync::companion {

void complete_revocation(
    bool quitting, bool revoked, std::string_view error,
    const std::function<void(std::string_view)>& report_error,
    const std::function<void()>& refresh_pairings) {
  if (quitting) return;
  if (!revoked) report_error(error);
  refresh_pairings();
}

}  // namespace noisefactor::sync::companion
