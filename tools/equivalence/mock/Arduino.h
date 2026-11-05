// Host mock of the Arduino API used by PhotonFeeder.cpp (equivalence harness).
// Every actuator call is traced with the virtual time; see sim.cpp.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>

typedef uint8_t byte;
typedef bool boolean;

enum { INPUT = 0, OUTPUT = 1, INPUT_PULLUP = 2 };
#define HIGH 1
#define LOW 0

// pin names used by define.h
enum PinNames {
    PA0 = 0, PA1, PA2, PA3, PA4, PA5, PA6, PA7, PA8, PA9, PA10, PA11, PA12, PA13, PA14, PA15,
    PB0 = 16, PB1, PB2, PB3, PB4, PB5, PB6, PB7, PB8, PB9
};

void pinMode(uint32_t pin, uint32_t mode);
void digitalWrite(uint32_t pin, uint32_t value);
int digitalRead(uint32_t pin);
void analogWrite(uint32_t pin, int value);
void delay(uint32_t ms);
uint32_t millis();

using std::min;
using std::max;

// Minimal Arduino String (only what the firmware uses)
class String {
public:
    String(const char *s = "") : s_(s) {}
    String &operator=(const char *s) { s_ = s; return *this; }
    bool operator==(const char *o) const { return s_ == o; }
    size_t length() const { return s_.size(); }   // size_t == unsigned int on the target
    const char *c_str() const { return s_.c_str(); }
    int indexOf(const char *o) const { size_t p = s_.find(o); return p == std::string::npos ? -1 : (int)p; }
private:
    std::string s_;
};
