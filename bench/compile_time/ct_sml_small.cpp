// Compile-time benchmark: SML small (2 states, 2 transitions)
#include <boost/sml.hpp>

namespace sml = boost::sml;

struct E1 {};
struct E2 {};

struct ping_pong_sm {
    auto operator()() const {
        using namespace sml;
        return make_transition_table(
            *"A"_s + event<E1> = "B"_s,
             "B"_s + event<E2> = "A"_s);
    }
};

int main() {
    sml::sm<ping_pong_sm> sm;
    sm.process_event(E1{});
    sm.process_event(E2{});
    return 0;
}
