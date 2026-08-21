#pragma once

#include <functional>
#include <string_view>

namespace noisefactor::sync::companion {

// Applies a completed revocation to the UI only while the companion is still
// running. Completion can be delivered inline from CompanionProcess's
// destructor after its owner window and process pointer have been cleared.
void complete_revocation(
    bool quitting, bool revoked, std::string_view error,
    const std::function<void(std::string_view)>& report_error,
    const std::function<void()>& refresh_pairings);

}  // namespace noisefactor::sync::companion
