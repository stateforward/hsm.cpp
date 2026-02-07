#include <iostream>

#include "hsm/hsm.hpp"

// Simple CRTP machine using the example model
constexpr auto model = hsm::define("example");
struct Machine : hsm::HSM<model, Machine> {};

int main() {
  Machine machine;

  const auto state = machine.state();
  std::cout << "hsm example current state: "
            << (state.empty() ? "<none>" : std::string(state)) << '\n';

  // machine.dispatch(hsm::EventBase{"noop"});
  return 0;
}
