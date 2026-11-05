#ifndef _PHOTON_FEEDER_H
#define _PHOTON_FEEDER_H

#include <functional>

#ifndef MOTOR_DEPS
    #define MOTOR_DEPS
    #include <RotaryEncoder.h>
#endif


#define VENDOR_SPECIFIC_OPTIONS_LENGTH 20  // Chosen by fair d20 roll


class PhotonFeeder {
    public:
        enum FeedResult {
            SUCCESS,
            INVALID_LENGTH,
            COULDNT_REACH,
            UNKNOWN_ERROR
        };

        PhotonFeeder(
            uint8_t drive1_pin,
            uint8_t drive2_pin,
            uint8_t peel1_pin,
            uint8_t peel2_pin,
            uint8_t led_red,
            uint8_t led_green,
            uint8_t led_blue,
            RotaryEncoder* encoder
        );

        FeedResult getMoveResult(); 
        
        // Async Functions
        void peel(bool forward);
        void peelValue(bool forward, uint8_t value);
        void drive(bool forward);
        void driveValue(bool forward, uint8_t value);
        void driveRamp(bool forward, uint8_t start, uint8_t end, uint8_t step_delay);
        void driveBrakeValue(bool forward, uint8_t value);
        void brakePeel();
        void brakeDrive();
        void halt();
        void set_rgb(bool red, bool green, bool blue);
        uint16_t calculateExpectedFeedTime(uint8_t distance, bool forward);
        void setMmPosition(uint16_t position); 
        void resetEncoderPosition(uint16_t position);
        void resetTapeSettings();
        bool getBeefyBoi();
        void setBeefyBoi(bool value);

        // Blocking Functions
        void feedDistance(uint16_t tenths_mm, bool forward);
        bool checkLoaded();

        // Vendor Specific Functions
        // Should probably be virtual with hardware overriding it as well as return status
        void vendorSpecific(uint8_t options[VENDOR_SPECIFIC_OPTIONS_LENGTH], uint8_t* response);
        void identify();
        void showVersion();

        
    private:
        uint8_t _drive1_pin;
        uint8_t _drive2_pin;

        uint8_t _peel1_pin;
        uint8_t _peel2_pin;  

        uint8_t _led_red;
        uint8_t _led_green;
        uint8_t _led_blue;

        uint8_t _retry_limit = 3;

        // firmware version text (VERSION_STRING, "debug" if built without one)
        const char *_version;
        
        bool _beefy_boi = false;
        bool _first_feed_since_load = true;
        // flag for if we should just drive full tilt
        // set when thick tape is detected
        // reset when tape is driven fast through buttons (likely swapping tape)

        FeedResult _lastFeedStatus;

        signed long _position;

        RotaryEncoder* _encoder;

        uint8_t _firmware_version = 2;

        bool moveForward(uint16_t tenths_mm);
        bool moveBackward(uint16_t tenths_mm);

        bool moveForwardSequenceLT2(uint16_t tenths_mm, bool first_attempt);
        bool moveForwardSequence2_4(uint16_t tenths_mm, bool first_attempt);
        bool moveForwardSequenceGT4(uint16_t tenths_mm, bool first_attempt);

        bool moveBackwardSequence(bool forward, uint16_t tenths_mm);

        // --- building blocks shared by the move sequences ---
        typedef bool (PhotonFeeder::*ForwardSequence)(uint16_t tenths_mm, bool first_attempt);
        bool moveForwardWithRetries(ForwardSequence sequence, uint16_t tenths_mm, int &retry_index);

        // goal position in tenths of mm and in encoder ticks (absolute, see feedGoal())
        struct FeedGoal {
            signed long mm;
            signed long tick_precise;   // thousandths of a tick
            int tick;
        };
        FeedGoal feedGoal(signed long goal_mm);
        void adoptGoalPosition(const FeedGoal &goal);

        // tick history used to detect a stalled (or settled) drive motor
        struct StallMonitor {
            int tick_history[20];
            int tick_history_index;
            int delta;                  // max - min over the history
            uint32_t last_sample_time;
            void reset();
            void record(int current_tick);
        };

        void peelForForwardFeed(uint16_t tenths_mm);
        void finishForwardMove(StallMonitor &stall, const FeedGoal &goal);
};

#endif //_PHOTON_FEEDER_H