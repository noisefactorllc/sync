#pragma once

#include <cstdint>

namespace noisefactor::sync {

// Labels supplied by a browser application are rendered in surfaces the user
// reads to make a decision: the native pairing prompt, and the source pickers
// of unrelated applications that list a Syphon server by name. Invisible and
// bidirectional formatting characters carry no meaning in a label and exist
// only to make rendered text disagree with its bytes, so they are rejected
// rather than stripped.
[[nodiscard]] constexpr bool formatting_code_point(
    std::uint32_t code_point) noexcept {
  return (code_point >= 0x200bU && code_point <= 0x200fU) ||  // ZWSP..RLM
         (code_point >= 0x202aU && code_point <= 0x202eU) ||  // LRE..RLO
         (code_point >= 0x2060U && code_point <= 0x2064U) ||  // WJ..invisible ops
         (code_point >= 0x2066U && code_point <= 0x2069U) ||  // LRI..PDI
         code_point == 0xfeffU;                               // ZWNBSP
}

// C0 controls, DEL, and the C1 range. Separate from the formatting set so the
// two rules can be reported and reasoned about independently.
[[nodiscard]] constexpr bool control_code_point(
    std::uint32_t code_point) noexcept {
  return code_point <= 0x1fU || (code_point >= 0x7fU && code_point <= 0x9fU);
}

}  // namespace noisefactor::sync
