// Compile-time benchmark: SML medium (traffic light - 6 states, 6 transitions)
#include <boost/sml.hpp>

namespace sml = boost::sml;

struct T {};

struct traffic_sm {
    auto operator()() const {
        using namespace sml;
        return make_transition_table(
            *"NS_Green"_s  + event<T> = "NS_Yellow"_s,
             "NS_Yellow"_s + event<T> = "AllRed1"_s,
             "AllRed1"_s   + event<T> = "EW_Green"_s,
             "EW_Green"_s  + event<T> = "EW_Yellow"_s,
             "EW_Yellow"_s + event<T> = "AllRed2"_s,
             "AllRed2"_s   + event<T> = "NS_Green"_s);
    }
};

int main() {
    sml::sm<traffic_sm> sm;
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    return 0;
}
