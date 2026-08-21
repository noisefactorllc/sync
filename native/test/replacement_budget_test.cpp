#include "test_harness.hpp"

#include "../src/replacement_budget.hpp"

#include <cstddef>
#include <limits>

namespace {

using noisefactor::sync::allocation::replacement_total_if_peak_fits;

SYNC_TEST(replacement_budget_rejects_a_transient_peak_above_the_limit) {
  SYNC_REQUIRE(!replacement_total_if_peak_fits(4, 4, 8, 11).has_value());
}

SYNC_TEST(replacement_budget_commits_only_the_steady_state_total) {
  const auto total = replacement_total_if_peak_fits(4, 4, 8, 12);
  SYNC_REQUIRE(total.has_value());
  SYNC_REQUIRE(*total == 8);
}

SYNC_TEST(replacement_budget_rejects_corrupt_and_overflowing_accounting) {
  SYNC_REQUIRE(!replacement_total_if_peak_fits(3, 4, 1, 8).has_value());
  SYNC_REQUIRE(!replacement_total_if_peak_fits(
                    std::numeric_limits<std::size_t>::max(), 0, 1,
                    std::numeric_limits<std::size_t>::max())
                    .has_value());
}

}  // namespace
