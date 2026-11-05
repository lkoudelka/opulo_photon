// Host mock of mathertel/RotaryEncoder: position comes from the motor model in sim.cpp.
#pragma once
#include <cstdint>

long sim_encoder_get();
void sim_encoder_set(long p);

class RotaryEncoder {
public:
    enum class LatchMode { FOUR3 = 1, FOUR0 = 2, TWO03 = 3 };
    RotaryEncoder(int, int, LatchMode = LatchMode::FOUR0) {}
    long getPosition() { return sim_encoder_get(); }
    void setPosition(long p) { sim_encoder_set(p); }
    void tick() {}
};
