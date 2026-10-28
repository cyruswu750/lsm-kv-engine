#include <gtest/gtest.h>

#include "core/kv.h"

// Verifies only that the build, include paths, and test runner are wired up.
// It exercises no storage behavior.
TEST(Harness, BuildsAndLinks) {
  SUCCEED();
}
