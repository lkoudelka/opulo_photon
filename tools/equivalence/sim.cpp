// Equivalence harness for photon/src/PhotonFeeder.cpp.
//
// Runs a fixed set of feed scenarios against a deterministic motor/encoder
// model and prints a trace of every actuator call (pin writes, delays) with
// the virtual time and encoder position, plus the results.  Build it once
// with the original and once with the modified PhotonFeeder.cpp: identical
// traces = identical observable behaviour, including timing (call sequence,
// delays, PWM values, timeouts, retries, position bookkeeping).
//
// Virtual time advances with delay() and by a fixed cost per millis() and
// getPosition() call, so a refactor that changes the number or order of
// those calls also shows up as a difference (deliberately strict).
#include <cstdio>
#include <cmath>
#include <cstring>
#include "Arduino.h"
#include "RotaryEncoder.h"
#include "PhotonFeeder.h"
#include "define.h"

static uint64_t t_us = 0;
static double pos = 0, vel = 0;           // ticks, ticks/ms
static int pwm[32];
static double friction = 25;              // PWM units needed to start moving
static FILE *out;

static void step(uint64_t dt_us)
{
    // first-order motor: target speed from net PWM, brake = fast decay
    const uint64_t h = 100;
    while (dt_us) {
        uint64_t d = dt_us < h ? dt_us : h;
        dt_us -= d;
        int a = pwm[DRIVE1], b = pwm[DRIVE2];
        double eff = b - a;                       // forward > 0
        bool brake = (a == 255 && b == 255);
        double target = 0;
        if (std::fabs(eff) > friction)
            target = (eff > 0 ? 1 : -1) * (std::fabs(eff) - friction) * 0.05;
        double tau = brake ? 2.0 : (eff == 0 ? 20.0 : 5.0);   // ms
        double dtm = d / 1000.0;
        vel += (target - vel) * (dtm / (tau + dtm));
        pos += vel * dtm;
        t_us += d;
    }
}

long sim_encoder_get() { step(5); return (long)std::floor(pos); }
void sim_encoder_set(long p) { pos = p + (pos - std::floor(pos)); fprintf(out, "%10llu setPosition %ld\n", (unsigned long long)t_us, p); }

void pinMode(uint32_t pin, uint32_t mode) { fprintf(out, "%10llu pinMode %u %u\n", (unsigned long long)t_us, pin, mode); }
void digitalWrite(uint32_t pin, uint32_t v) { fprintf(out, "%10llu digitalWrite %u %u\n", (unsigned long long)t_us, pin, v); }
int digitalRead(uint32_t) { return 1; }
void analogWrite(uint32_t pin, int v)
{
    if (pwm[pin] != v)
        fprintf(out, "%10llu analogWrite %u %d enc=%ld\n", (unsigned long long)t_us, pin, v, (long)std::floor(pos));
    pwm[pin] = v;
}
void delay(uint32_t ms) { fprintf(out, "%10llu delay %u\n", (unsigned long long)t_us, ms); step((uint64_t)ms * 1000); }
uint32_t millis() { step(20); return (uint32_t)(t_us / 1000); }

static RotaryEncoder enc(DRIVE_ENC_A, DRIVE_ENC_B, RotaryEncoder::LatchMode::TWO03);

static void run(const char *name, double fr, bool beefy, const int *moves, int n)
{
    fprintf(out, "=== scenario %s friction=%.0f beefy=%d\n", name, fr, beefy);
    friction = fr;
    pos = vel = 0;
    memset(pwm, 0, sizeof pwm);
    PhotonFeeder f(DRIVE1, DRIVE2, PEEL1, PEEL2, LED_R, LED_G, LED_B, &enc);
    f.resetEncoderPosition(0);
    f.setMmPosition(0);
    f.setBeefyBoi(beefy);
    for (int i = 0; i < n; i++) {
        int d = moves[i];
        bool fwd = d > 0;
        uint16_t td = (uint16_t)(fwd ? d : -d);
        fprintf(out, "--- feed %d expected=%u\n", d, f.calculateExpectedFeedTime((uint8_t)td, fwd));
        f.feedDistance(td, fwd);
        fprintf(out, "--- result %d beefy=%d enc=%ld t=%llu\n", (int)f.getMoveResult(), f.getBeefyBoi(),
                (long)std::floor(pos), (unsigned long long)t_us);
    }
    // non-motion API
    uint8_t opt[VENDOR_SPECIFIC_OPTIONS_LENGTH], resp[VENDOR_SPECIFIC_OPTIONS_LENGTH];
    for (int c : {0x00, 0x01, 0x02, 0x0F, 0x10, 0x11, 0x20, 0xFF}) {
        for (int mask : {0x00, 0x08, 0x0F, 0x0A}) {
            memset(opt, 0, sizeof opt);
            memset(resp, 0xEE, sizeof resp);
            opt[0] = (uint8_t)c;
            opt[1] = (uint8_t)mask;
            f.vendorSpecific(opt, resp);
            fprintf(out, "vendor %02x %02x ->", c, mask);
            for (uint8_t r : resp)
                fprintf(out, " %02x", r);
            fprintf(out, "\n");
        }
    }
    f.showVersion();
    f.identify();
    f.resetTapeSettings();
    fprintf(out, "after reset beefy=%d\n", f.getBeefyBoi());
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    static const int fwd[] = {1, 5, 10, 19, 20, 21, 25, 39, 40, 41, 45, 60, 79, 80, 81, 100, 160, 255};
    static const int mix[] = {20, -10, 40, -20, 40, -40, 20, 20, -5, 80};
    static const int few[] = {20, 40, 80, -20};
    for (double fr : {10.0, 25.0, 60.0, 120.0, 170.0}) {
        for (bool b : {false, true}) {
            char n[32];
            snprintf(n, sizeof n, "fwd-%g-%d", fr, b);
            run(n, fr, b, fwd, sizeof fwd / sizeof *fwd);
            snprintf(n, sizeof n, "mix-%g-%d", fr, b);
            run(n, fr, b, mix, sizeof mix / sizeof *mix);
        }
    }
    run("stuck", 400, false, few, sizeof few / sizeof *few);   // never moves: retries + COULDNT_REACH
    for (int d = 0; d <= 255; d += 17) {
        PhotonFeeder f(DRIVE1, DRIVE2, PEEL1, PEEL2, LED_R, LED_G, LED_B, &enc);
        fprintf(out, "expected %d: %u %u\n", d, f.calculateExpectedFeedTime((uint8_t)d, true),
                f.calculateExpectedFeedTime((uint8_t)d, false));
    }
    fprintf(out, "END t=%llu\n", (unsigned long long)t_us);
    return 0;
}
