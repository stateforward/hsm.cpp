// Compile-time benchmark: SML large (20 states, 22 transitions)
// SML is flat so we encode the same state count and transition count.
#include <boost/sml.hpp>

namespace sml = boost::sml;

struct E1 {};
struct E2 {};
struct E3 {};
struct E4 {};
struct E5 {};

struct large_sm {
    auto operator()() const {
        using namespace sml;
        return make_transition_table(
            // Region A: 5-state ring on E1
            *"A1"_s + event<E1> = "A2"_s,
             "A2"_s + event<E1> = "A3"_s,
             "A3"_s + event<E1> = "A4"_s,
             "A4"_s + event<E1> = "A5"_s,
             "A5"_s + event<E1> = "A1"_s,
            // Region B: 5-state ring on E2
             "B1"_s + event<E2> = "B2"_s,
             "B2"_s + event<E2> = "B3"_s,
             "B3"_s + event<E2> = "B4"_s,
             "B4"_s + event<E2> = "B5"_s,
             "B5"_s + event<E2> = "B1"_s,
            // Region C outer: 3-state ring on E3
             "C1"_s + event<E3> = "C2"_s,
             "C2"_s + event<E3> = "C3"_s,
             "C3"_s + event<E3> = "C1"_s,
            // Region C inner: 2-state toggle on E4
             "D1"_s + event<E4> = "D2"_s,
             "D2"_s + event<E4> = "D1"_s,
            // Cross-region transitions on E5
             "C1"_s + event<E5> = "D1"_s,
             "D1"_s + event<E5> = "C1"_s,
             "A1"_s + event<E5> = "B1"_s,
             "B1"_s + event<E5> = "A1"_s,
            // Extra transitions to reach 22 total
             "A3"_s + event<E5> = "C1"_s,
             "B3"_s + event<E5> = "D1"_s);
    }
};

int main() {
    sml::sm<large_sm> sm;
    sm.process_event(E1{});
    sm.process_event(E2{});
    sm.process_event(E3{});
    sm.process_event(E4{});
    sm.process_event(E5{});
    return 0;
}
