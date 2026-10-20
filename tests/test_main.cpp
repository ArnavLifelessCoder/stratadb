#include <gtest/gtest.h>

#include <charconv>
#include <iostream>
#include <string_view>

#include "test_support.hpp"

namespace lsm::test {
std::uint64_t seed = 0x5eed;
bool quick = false;
}  // namespace lsm::test

int main(int argc, char** argv) {
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--quick") {
      lsm::test::quick = true;
    } else if (argument.starts_with("--seed=")) {
      const auto value = argument.substr(7);
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), lsm::test::seed);
      if (error != std::errc{} || end != value.data() + value.size()) {
        std::cerr << "Invalid --seed: expected an unsigned decimal 64-bit integer\n";
        return 2;
      }
    } else {
      argv[kept++] = argv[i];
    }
  }
  argc = kept;
  argv[argc] = nullptr;
  std::cout << "Randomized test seed: " << lsm::test::seed << '\n';
  std::cout << "Test mode: " << (lsm::test::quick ? "quick (not a milestone gate)" : "full")
            << '\n';
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
