#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

#include <cstdint>
#include <string_view>

using namespace hsm;

TEST_CASE("detail::fixed_string basic behavior") {
  using hsm::detail::fixed_string;
  using hsm::detail::make_fixed_string;

  constexpr auto hello = make_fixed_string("hello");
  constexpr fixed_string<6> hello_with_padding{"hello"};

  static_assert(hello.size() == 5);
  static_assert(hello_with_padding.size() == 5);
  static_assert(hello == hello_with_padding);

  CHECK(hello.size() == 5);
  CHECK(hello[0] == 'h');
  CHECK(hello[4] == 'o');
  CHECK(hello.view() == std::string_view("hello"));

  constexpr auto shorter = make_fixed_string("hi");
  static_assert(shorter.size() == 2);
  static_assert(hello != shorter);

  CHECK(shorter.size() == 2);
  CHECK(shorter.view() == std::string_view("hi"));
  CHECK(shorter != hello);
}

TEST_CASE("detail::fnv1a_64 produces stable hashes") {
  using hsm::detail::fnv1a_64;

  constexpr std::uint64_t h_noop = fnv1a_64("NoOp");
  constexpr std::uint64_t h_noop_again = fnv1a_64("NoOp");
  constexpr std::uint64_t h_other = fnv1a_64("Other");

  static_assert(h_noop == h_noop_again, "hash must be stable for same input");
  static_assert(h_noop != h_other, "different strings should hash differently");

  CHECK(h_noop == h_noop_again);
  CHECK(h_noop != h_other);
}

TEST_CASE("detail::normalized_model_data get_state_name/get_event_name") {
  using hsm::detail::normalized_model_data;

  using Data = normalized_model_data<2, 0, 2, 0, 0, 16, 1>;
  Data data{};

  const char raw[] = "rootchilde1e2";
  for (std::size_t i = 0; i < sizeof(raw) - 1; ++i) {
    data.string_buffer[i] = raw[i];
  }

  // State names: "root" and "child"
  data.states[0].name_offset = 0;
  data.states[0].name_length = 4;
  data.states[1].name_offset = 4;
  data.states[1].name_length = 5;

  // Event names: "e1" and "e2"
  data.events[0].name_offset = 9;
  data.events[0].name_length = 2;
  data.events[1].name_offset = 11;
  data.events[1].name_length = 2;

  CHECK(data.get_state_name(0) == std::string_view("root"));
  CHECK(data.get_state_name(1) == std::string_view("child"));
  CHECK(data.get_state_name(2).empty());

  CHECK(data.get_event_name(0) == std::string_view("e1"));
  CHECK(data.get_event_name(1) == std::string_view("e2"));
  CHECK(data.get_event_name(2).empty());
}
