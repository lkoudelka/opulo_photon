#include <Arduino.h>
#include "PhotonFeeder.h"
#include "define.h"
#include "versions.h"
#include <vector>
#include <string.h>

// --------------------

// Calculating Ticks per Tenth MM
//
// The "1:1030" gearbox is actually 1:1030.4 or 5:5152
// Found by counting the actual teeth of the gears:
// Ratio 1:  1:20
// Ratio 2: 10:33
// Ratio 3: 11:32
// Ratio 4:  9:23
// Ratio 5: 10:21
//
// Encoder has 14 ticks per revolution (7 per channel)
// One full rotation of the output shaft is therefore 14*1030.4 = 14,425.6 ticks
// divided by 32 teeth is 450.8 ticks per tooth
// divided by 40 tenths of a mm per tooth (4mm) is 11.27 ticks per tenth mm
// ~8.873 microns per tick

#define TICKS_PER_TENTH_MM 11.27
#define THOUSANDTHS_TICKS_PER_TENTH_MM ((uint32_t)(TICKS_PER_TENTH_MM * 1000))
#define TENTH_MM_PER_PIP 40

// -----------
// Thresholds
// -----------

// number of ticks within requested tick position we should begin halting
#define DRIVE_APPROACH_FINAL_TICKS 150
// when moving backwards, how far further backwards past requested position to approach from the back
#define BACKLASH_COMP_TENTH_MM 10

// --------
// Timing
// --------

// how long the peel motor will peel film per tenth mm of tape requested to be driven
#define PEEL_TIME_PER_TENTH_MM 22
// how long the peel motor will peel film during backwards movements per tenth mm of tape requested to be driven
#define BACKWARDS_PEEL_TIME_PER_TENTH_MM 30
// short amount of time peel motor moves backwards to reduce tension on film after peeling
#define PEEL_BACKOFF_TIME 100
// amount of time we allow for each tenth mm before timeout (in ms)
#define TIMEOUT_TIME_PER_TENTH_MM 50
// after driving backwards, how long do we peel to take up any potential slack in the film
#define BACKWARDS_FEED_FILM_SLACK_REMOVAL_TIME 350

// Unit Tests Fail Because This Isn't Defined In ArduinoFake for some reason
#ifndef INPUT_ANALOG
#define INPUT_ANALOG 0x04
#endif

PhotonFeeder::PhotonFeeder(
            uint8_t drive1_pin,
            uint8_t drive2_pin,
            uint8_t peel1_pin,
            uint8_t peel2_pin,
            uint8_t led_red,
            uint8_t led_green,
            uint8_t led_blue,
            RotaryEncoder* encoder
        ) :
    _drive1_pin(drive1_pin),
    _drive2_pin(drive2_pin),
    _peel1_pin(peel1_pin),
    _peel2_pin(peel2_pin),
    _led_red(led_red),
    _led_green(led_green),
    _led_blue(led_blue),
    _version(VERSION_STRING[0] != '\0' ? VERSION_STRING : "debug"),
    _position(0),
    _encoder(encoder) {

    pinMode(_drive1_pin, OUTPUT);
    pinMode(_drive2_pin, OUTPUT);
    pinMode(_peel1_pin, OUTPUT);
    pinMode(_peel2_pin, OUTPUT);

    pinMode(_led_red, OUTPUT);
    pinMode(_led_green, OUTPUT);
    pinMode(_led_blue, OUTPUT);

}

//-----------
//
//    TOOLS
//
//-----------

PhotonFeeder::FeedResult PhotonFeeder::getMoveResult(){
    return _lastFeedStatus;
}

uint16_t PhotonFeeder::calculateExpectedFeedTime(uint8_t distance, bool forward){

    // This command is ONLY for generating a time that we send back to the host
    // calculating timeouts actually determining if we've failed a feed is separate.
    if(forward){
        // we're calculating expected feed time of an _optimal_ forward feed command. this includes:
        // - peel forward time
        // - peel backoff time
        // - expected time to drive forward assuming one attempt
        
        return (distance * PEEL_TIME_PER_TENTH_MM) + (2 * PEEL_BACKOFF_TIME) + (distance * TIMEOUT_TIME_PER_TENTH_MM) + 1000;

    }
    else {
        // we're calculating expected feed time of an _optimal_ backward feed command. this includes:
        // - unpeeling film time to prep for backwards movement
        // - backwards movement including backlash distance
        // - remaining film slack takeup
        return (distance * BACKWARDS_PEEL_TIME_PER_TENTH_MM) + ((distance + (BACKLASH_COMP_TENTH_MM*2)) * TIMEOUT_TIME_PER_TENTH_MM) + BACKWARDS_FEED_FILM_SLACK_REMOVAL_TIME + 50;
    }
}

void PhotonFeeder::identify() {
    for(int i = 0;i<3;i++){
        set_rgb(true, true, true);
        delay(300);
        set_rgb(false, false, false);
        delay(300);
    }
    
}

void PhotonFeeder::showVersion() {
    delay(500);

    if(strstr(_version, "debug") != nullptr){ // flashing red, beta
        set_rgb(true, false, false);
        delay(250);
        set_rgb(false, false, false);
        delay(250);
    }

    else { // flashing green, v1.X.X

        set_rgb(false, true, false);
        delay(250);
        set_rgb(false, false, false);
        delay(250);

    }
}

void PhotonFeeder::vendorSpecific(uint8_t options[VENDOR_SPECIFIC_OPTIONS_LENGTH], uint8_t* response) {
    uint8_t command = options[0];

    // Commands 0x00 - 0x0F get the firmware version string in chunks. The firmware version string may be smaller than
    // the allotted 7 chunks, but it is null-terminated as expected.
    if(command <= 0x0F) {
        const auto start_index = min(size_t(command) * VENDOR_SPECIFIC_OPTIONS_LENGTH, strlen(_version));
        const auto chunk = _version + start_index;

        // NOTE: Using strncpy here specifically because we want its behavior where it handles encountering a null byte
        // in the src string.
        strncpy((char*)(response), chunk, VENDOR_SPECIFIC_OPTIONS_LENGTH);
    }    

    switch(command){
        case 0x10:
            uint8_t ledMask = options[1];

            int blue = ledMask & 1;
            int green = (ledMask >> 1) & 1;
            int red = (ledMask >> 2) & 1;
            int set = (ledMask >> 3) & 1;

            if(set){
                set_rgb(red, green, blue);
            }
            break;

    }

    return;
    
    
}

bool PhotonFeeder::getBeefyBoi(){
    return _beefy_boi;
}

void PhotonFeeder::setBeefyBoi(bool value){
    _beefy_boi = value;
}

void PhotonFeeder::resetTapeSettings() {
    _beefy_boi = false;
    _first_feed_since_load = true;
}

bool PhotonFeeder::checkLoaded() {
/*  checkLoaded()
    The checkLoaded() function checks to see what's loaded in the feeder, and sets drive methodology appropriately.
    This gets run on first movement command after boot, and after tape is driven through buttons
*/

    //takes up any backlash slack, ensures any forward movement is tape movement
    analogWrite(_drive1_pin, 0);
    analogWrite(_drive2_pin, 250);
    delay(2);

    analogWrite(_drive1_pin, 0);
    analogWrite(_drive2_pin, 20);
    delay(100);

    halt();

    // find starting threshold of movement
    int errorThreshold = 3;
    int movedAt = 256;
    signed long startingTick, currentTick;

    startingTick = _encoder->getPosition();

    for(int movementIndex = 20; movementIndex<255; movementIndex=movementIndex + 5){

        analogWrite(_drive1_pin, 0);
        analogWrite(_drive2_pin, movementIndex);

        delay(75);

        currentTick = _encoder->getPosition();

        if(abs(startingTick - currentTick) > errorThreshold){
            movedAt = movementIndex;
            break;
        }
    }

    halt();

    if(movedAt > 140){
        _beefy_boi = true;
        //set_rgb(true, false, false);
        //delay(200);
    }
    else{
        _beefy_boi = false;
        //set_rgb(false, false, true);
        //delay(200);
    }

    return true;

}

void PhotonFeeder::resetEncoderPosition(uint16_t position){
    _encoder->setPosition(position);
}

void PhotonFeeder::setMmPosition(uint16_t position){
    _position = position;
}

void PhotonFeeder::set_rgb(bool red, bool green, bool blue) {
  digitalWrite(LED_R, ! red);
  digitalWrite(LED_G, ! green);
  digitalWrite(LED_B, ! blue);
}

//-----------
//
//    MANUAL MOTOR CONTROL
//
//-----------

void PhotonFeeder::peel(bool forward) {
    if(forward){
        analogWrite(_peel1_pin, 255);
        analogWrite(_peel2_pin, 0);
    }
    else{
        analogWrite(_peel1_pin, 0);
        analogWrite(_peel2_pin, 255);
    }
}

void PhotonFeeder::peelValue(bool forward, uint8_t value){
    if(forward){
        analogWrite(_peel1_pin, value);
        analogWrite(_peel2_pin, 0);
    }
    else{
        analogWrite(_peel1_pin, 0);
        analogWrite(_peel2_pin, value);
    }
}

void PhotonFeeder::drive(bool forward){

    if(forward){
        analogWrite(_drive1_pin, 0);
        analogWrite(_drive2_pin, 255);
    }
    else{
        analogWrite(_drive1_pin, 255);
        analogWrite(_drive2_pin, 0);
    }
}

void PhotonFeeder::driveValue(bool forward, uint8_t value){
    if(forward){
        analogWrite(_drive1_pin, 0);
        analogWrite(_drive2_pin, value);
    }
    else{
        analogWrite(_drive1_pin, value);
        analogWrite(_drive2_pin, 0);
    }
}

void PhotonFeeder::driveRamp(bool forward, uint8_t start, uint8_t end, uint8_t step_delay){

    if(start < end){
        for(uint8_t i = start; i < end; i++){
            driveValue(forward, i);
            delay(step_delay);
        }
    }
    else{
        for(uint8_t i = start; i > end; i--){
            driveValue(forward, i);
            delay(step_delay);
        }
    }
}

void PhotonFeeder::driveBrakeValue(bool forward, uint8_t value){
    if(forward){
        analogWrite(_drive1_pin, 255-value);
        analogWrite(_drive2_pin, 255);
    }
    else{
        analogWrite(_drive1_pin, 255);
        analogWrite(_drive2_pin, 255-value);
    }
}

void PhotonFeeder::brakeDrive(){
    //brings both drive pins high
    analogWrite(_drive1_pin, 255);
    analogWrite(_drive2_pin, 255);
}

void PhotonFeeder::brakePeel(){
    //brings both drive pins high
    analogWrite(_peel1_pin, 255);
    analogWrite(_peel2_pin, 255);
}

void PhotonFeeder::halt(){
    brakeDrive();
    brakePeel();
}

//-----------
//
//    FEEDING
//
//-----------

// if (forward){
//     if (beefyboi){
//         drive full tilt
//     }
//     else{
//         if 1mm{
//             drive precise but with a shorter approach
            
//         }

//         else if 2-4mm {
//             drive as precise as possible
//         }
//         else if >4mm {
//             drive a mix of precise and fast
//         }
//         
//         if (failed_feed){
//             beefyboi = true
//             retry
//         }
//     }
// }
// else if (backward){
//     do the same backwards feed process
// }


void PhotonFeeder::feedDistance(uint16_t tenths_mm, bool forward) {

    if (_first_feed_since_load){
        // checkLoaded();
        _first_feed_since_load = false;
    }

    set_rgb(true, true, true);

    bool success = (forward) ? moveForward(tenths_mm) : moveBackward(tenths_mm);

    if(success){
        set_rgb(false, false, false);
    }
    else{
        set_rgb(true, false, false);
    }

}

bool PhotonFeeder::moveForward(uint16_t tenths_mm) {
    int retry_index = 0;
    // First, ensure everything is stopped
    halt();


    // determine how to drive based on the distance

    // less than 2mm movements
    if (tenths_mm < 20){
        //move forward very small distance
        if(!moveForwardWithRetries(&PhotonFeeder::moveForwardSequenceLT2, tenths_mm, retry_index)){
            return false;
        }
    }

    // 2-4mm movements
    // if we're feeding a distance from 2mm to 4mm, we should do it very precisely
    else if (tenths_mm >= 20 && tenths_mm <= 40){
        // move forward somewhere between 20-40 tenths
        if(!moveForwardWithRetries(&PhotonFeeder::moveForwardSequence2_4, tenths_mm, retry_index)){
            return false;
        }
    }
    
    // >4mm movements
    // if we're feeding a distance greater than 4mm, we should do it fast
    else{
        // segment our total distance into pip chunks
        div_t result = div(tenths_mm, TENTH_MM_PER_PIP);
        // quot is the number of 1 pip loops we run
        for(int i = 0;i<result.quot;i++){
            // move forward 40 tenths
            if(!moveForwardWithRetries(&PhotonFeeder::moveForwardSequenceGT4, 40, retry_index)){
                return false;
            }
        }

        // rem is the value we drive in a last loop to move any remaining distance less than a pip
        if(result.rem > 0){
            //move forward result.rem
            if(!moveForwardWithRetries(&PhotonFeeder::moveForwardSequenceGT4, result.rem, retry_index)){
                return false;
            }
        }

    }


    _lastFeedStatus = PhotonFeeder::FeedResult::SUCCESS;
    return true;
    
}

bool PhotonFeeder::moveForwardWithRetries(ForwardSequence sequence, uint16_t tenths_mm, int &retry_index) {
    // Run one forward sequence. If it fails, try again with a fresh pulse of power after moving the motor back a
    // bit. retry_index counts across all calls of one moveForward() (shared by all pip chunks).
    // Returns false (and sets _lastFeedStatus) once the retry limit is exceeded.
    if((this->*sequence)(tenths_mm, true)){
        return true;
    }
    while(true){
        retry_index++;

        // if we've failed at all, switch to beefy boi
        _beefy_boi = true;

        if(retry_index > _retry_limit){
            _lastFeedStatus = PhotonFeeder::FeedResult::COULDNT_REACH;
            return false;
        }
        drive(false);
        delay(50);
        halt();
        if((this->*sequence)(tenths_mm, false)){
            return true;
        }
    }
}

bool PhotonFeeder::moveBackward(uint16_t tenths_mm) {
    // First, ensure everything is stopped
    halt();

    // Next, unspool some film to give the tape slack
    signed long peel_time = (tenths_mm * BACKWARDS_PEEL_TIME_PER_TENTH_MM);
    peel(false);
    delay(peel_time);
    brakePeel();

    // move tape backward
    // first we overshoot by the backlash distance, then approach from the forward direction
    if (moveBackwardSequence(false, tenths_mm + BACKLASH_COMP_TENTH_MM)){

        // if this is successful, we peel film to take up any slack
        peel(true);
        delay(BACKWARDS_FEED_FILM_SLACK_REMOVAL_TIME);
        brakePeel();

        //then drive the last little bit
        if(moveBackwardSequence(true, BACKLASH_COMP_TENTH_MM)){
            //peel again to take up any slack
            
            _lastFeedStatus = PhotonFeeder::FeedResult::SUCCESS;
            return true;
        }
        else{
            _lastFeedStatus = PhotonFeeder::FeedResult::COULDNT_REACH;
            return false;
        }        
    }
    else{
        _lastFeedStatus = PhotonFeeder::FeedResult::COULDNT_REACH;
        return false;
    }
}

//-----------
//
//    FORWARD SEQUENCE BUILDING BLOCKS
//
//-----------

/* Position bookkeeping (used by all move sequences)
*   We can't just calculate the number of ticks we need to move for the given mm movement requested, and increment our tick count by that much.
*   Because the tick count is only ever an approximation of the precise mm position, any rounding done from the mm->tick conversion will
*   result in a significant amount of accrued error.
*
*   Instead, we need to use the _mm_ position as ground truth, and only ever use the ticks as only how we command the control loop. We do this by
*   first finding the new requested position, then converting this to ticks _based on the startup 0 tick position_. This is similar to
*   absolute positioning vs. relative positioning in Marlin. Every mm->tick calculation needs to be done based on the initial 0 tick position.
*/
PhotonFeeder::FeedGoal PhotonFeeder::feedGoal(signed long goal_mm) {
    FeedGoal goal;
    goal.mm = goal_mm;

    // calculating goal_tick based on absolute, not relative position
    goal.tick_precise = goal_mm * THOUSANDTHS_TICKS_PER_TENTH_MM;
    goal.tick = 0;

    if (goal.tick_precise >= 0) {
        goal.tick = (goal.tick_precise + 500) / 1000;   // round a positive number
    } else {
        goal.tick = (goal.tick_precise - 500) / 1000;   // round a negative integer
    }
    return goal;
}

void PhotonFeeder::adoptGoalPosition(const FeedGoal &goal) {
    // updating internal position to the goal position because we reached it
    _position = goal.mm;

    // Resetting internal position count so we dont creep up into our 2,147,483,647 limit on the variable
    // We can only do this when the exact tick we move to is a whole number so we don't accrue any drift
    if(goal.tick_precise == goal.tick * 1000){
        resetEncoderPosition(_encoder->getPosition() - goal.tick);
        setMmPosition(0);
    }
}

void PhotonFeeder::peelForForwardFeed(uint16_t tenths_mm) {
    int peel_delay = PEEL_TIME_PER_TENTH_MM * tenths_mm;

    int backlash_backdrive_value = 20;

    //drive backwards to counteract backlash
    driveRamp(false, 0, backlash_backdrive_value, 2);

    delay(100);

    int peel_tension_release_time = 400;

    // peel film for calculated time
    peelValue(true, 255);
    delay(peel_delay + peel_tension_release_time);

    peelValue(false, 255);
    delay(peel_tension_release_time);
    brakePeel();


    // ramp drive down to zero
    driveRamp(false, backlash_backdrive_value, 0, 2);
}

void PhotonFeeder::StallMonitor::reset() {
    static const int initial_history[20] = {1, 100, 1, 100, 1, 100, 1, 100, 1, 100, 1, 100, 1, 100, 1, 100, 1, 100, 1, 100};
    memcpy(tick_history, initial_history, sizeof(tick_history));
    tick_history_index = 0;
    delta = 5;
}

void PhotonFeeder::StallMonitor::record(int current_tick) {
    // Updating tick history for Stall Detection
    tick_history[tick_history_index] = current_tick;

    if(tick_history_index > 18){
        tick_history_index = 0;
    }
    else{
        tick_history_index++;
    }

    int max = *std::max_element(tick_history, tick_history + 20);
    int min = *std::min_element(tick_history, tick_history + 20);
    delta = max - min;
}

void PhotonFeeder::finishForwardMove(StallMonitor &stall, const FeedGoal &goal) {
    // We've reached the final position! We want to stop as hard and aggressively as possible here.

    // grab position for the sake of measuring coast
    int brakeTick = _encoder->getPosition();

    // capture time at ss settle start
    uint32_t ssStartTime = millis();

    // stop
    brakeDrive();

    // sample ticks until we've hit steady state
    while (stall.delta > 0 && ssStartTime + 500 > millis()){

        //watching for steady state ticks
        if(millis() > stall.last_sample_time + 1){

            //getting encoder position
            signed long current_tick = _encoder->getPosition();

            stall.last_sample_time = millis();
            stall.record(current_tick);
        }
    }            
    
    int ssTick = _encoder->getPosition();
    volatile int coast = ssTick - brakeTick;

    // BREAKPOINT BELOW TO MONITOR COAST
    volatile int test = 0;

    adoptGoalPosition(goal);
}

//-----------
//
//    FORWARD SEQUENCES
//
//-----------

/* moveForwardSequence*()
*   These functions actually handle the translation of the mm movement to driving the motor to the right position based on encoder ticks
*   (see feedGoal() for the position bookkeeping). Each contains all peeling and driving sequencing for its distance range;
*   they differ only in timeout, ease-in and drive phases.

    returns true if we reach position within timeout, returns false if we timeout. does NOT set _lastFeedStatus.
*/

bool PhotonFeeder::moveForwardSequenceLT2(uint16_t tenths_mm, bool first_attempt) {

    // ONLY USED FOR MOVEMENTS < 2mm!

    signed long timeout, current_tick;

    timeout = calculateExpectedFeedTime(20, true);

    const FeedGoal goal = feedGoal(_position + tenths_mm);

    peelForForwardFeed(tenths_mm);
    
    //prepping variables for stall detection
    StallMonitor stall;
    stall.reset();
    uint32_t stallCooldownTime = millis();
    bool stallCooldown = false;
    stall.last_sample_time = millis();

    // setting start time for measuring timeout
    uint32_t start_time = millis();

    int currentDriveValue = 20;

    // drive forward with ease in
    for(int i=30;i<currentDriveValue;i=i+3){
        driveValue(true, i);
        delay(1);
    }

    //monitor loop
    while(millis() < start_time + timeout){

        // Getting encoder position
        current_tick = _encoder->getPosition();
        
        // Calculating error
        int error = goal.tick - current_tick;

        // Updating tick history for Stall Detection
        if(millis() > stall.last_sample_time + 1){
            stall.last_sample_time = millis();
            stall.record(current_tick);
        }

        // if we're in beefy boi mode, just drive full tilt
        if(_beefy_boi){
            
            currentDriveValue = 255;
            driveBrakeValue(true, currentDriveValue);

        }

        // If we're not in Beefy Boi mode, and the motor easily moves the tape
        else{

            // //----------------
            // // PHASE TWO
            // //----------------
            // // This phase is if we're really close to the final position, just a bit more than standard coast.

            if (error > 0){

                // reset stallcooldown if it's been a minute, motor has had time to react to new drive value
                if(stallCooldownTime + 10 < millis()){
                    stallCooldown = false;
                }

                // if stall detected, and it's been a minute since we've adjusted currentDriveValue, increase currentDriveValue
                if(stall.delta < 1 && stallCooldown == false){ 

                    currentDriveValue = currentDriveValue + 3;
                    if(currentDriveValue > 255){
                        currentDriveValue = 255;
                    }

                    stallCooldown = true;
                    stallCooldownTime = millis();

                }

                driveBrakeValue(true, currentDriveValue);

            }
        }

        if(error < 1){
            finishForwardMove(stall, goal);
            return true;
        }

    }
    // brake to kill any coast
    halt();

    return false;
}

bool PhotonFeeder::moveForwardSequence2_4(uint16_t tenths_mm, bool first_attempt) {

    // ONLY USED FOR MOVEMENTS >= 2mm and <= 4mm!

    signed long timeout, current_tick;

    timeout = calculateExpectedFeedTime(40, true);

    const FeedGoal goal = feedGoal(_position + tenths_mm);

    peelForForwardFeed(tenths_mm);
    
    //prepping variables for stall detection
    StallMonitor stall;
    stall.reset();
    uint32_t stallCooldownTime = millis();
    bool stallCooldown = false;
    stall.last_sample_time = millis();

    bool first_time_phase_two = true;

    // setting start time for measuring timeout
    uint32_t start_time = millis();

    int currentDriveValue = 20;

    // drive forward with ease in
    for(int i=10;i<currentDriveValue;i=i+3){
        driveValue(true, i);
        delay(1);
    }

    //monitor loop
    while(millis() < start_time + timeout){

        // Getting encoder position
        current_tick = _encoder->getPosition();
        
        // Calculating error
        int error = goal.tick - current_tick;

        // Updating tick history for Stall Detection
        if(millis() > stall.last_sample_time + 1){
            stall.last_sample_time = millis();
            stall.record(current_tick);
        }

        // if we're in beefy boi mode, just drive full tilt
        if(_beefy_boi){
            
            currentDriveValue = 255;
            driveBrakeValue(true, currentDriveValue);
            

        }

        // If we're not in Beefy Boi mode, and the motor easily moves the tape
        else{

            //-------------
            // DRIVE PHASES
            //-------------

            //--------------
            // PHASE ONE
            //--------------
            // If we're still a good way away, we want to drive just above our friction threshold.
            // This means checking if we've stalled, and if we have, increase drive value.
            if (error > DRIVE_APPROACH_FINAL_TICKS){

                // reset stallcooldown if it's been a minute, motor has had time to react to new drive value
                if(stallCooldownTime + 20 < millis()){
                    stallCooldown = false;
                }

                // if stall detected, and it's been a minute since we've adjusted currentDriveValue, increase currentDriveValue
                if(stall.delta <= 5 && stallCooldown == false){ 

                    currentDriveValue = currentDriveValue + 5;
                    if(currentDriveValue > 255){
                        currentDriveValue = 255;
                    }

                    stallCooldown = true;
                    stallCooldownTime = millis();

                }

                //driving calculated value
                driveBrakeValue(true, currentDriveValue);

            }

            // //----------------
            // // PHASE TWO
            // //----------------
            // // This phase is if we're really close to the final position, just a bit more than standard coast.

            else if (error < DRIVE_APPROACH_FINAL_TICKS && error > 0){

                if(first_time_phase_two){

                    currentDriveValue = currentDriveValue - 10;
                    driveBrakeValue(true, currentDriveValue);
                    first_time_phase_two = false;

                }

                // reset stallcooldown if it's been a minute, motor has had time to react to new drive value
                if(stallCooldownTime + 20 < millis()){
                    stallCooldown = false;
                }

                // if stall detected, and it's been a minute since we've adjusted currentDriveValue, increase currentDriveValue
                if(stall.delta < 1 && stallCooldown == false){ 

                    currentDriveValue = currentDriveValue + 1;
                    if(currentDriveValue > 255){
                        currentDriveValue = 255;
                    }

                    stallCooldown = true;
                    stallCooldownTime = millis();

                }

                driveBrakeValue(true, currentDriveValue);

            }
        }

        if(error < 1){
            finishForwardMove(stall, goal);
            return true;
        }

    }
    // brake to kill any coast
    halt();

    return false;
}



bool PhotonFeeder::moveForwardSequenceGT4(uint16_t tenths_mm, bool first_attempt) {

    // USED FOR THE PIP CHUNKS OF MOVEMENTS > 4mm (tenths_mm <= 40 per call)

    signed long timeout, current_tick;

    timeout = tenths_mm * TIMEOUT_TIME_PER_TENTH_MM;

    const FeedGoal goal = feedGoal(_position + tenths_mm);

    peelForForwardFeed(tenths_mm);
    
    //prepping variables for stall detection
    StallMonitor stall;
    stall.reset();
    uint32_t stallCooldownTime = millis();
    bool stallCooldown = false;
    stall.last_sample_time = millis();

    bool first_time_phase_two = true;

    // setting start time for measuring timeout
    uint32_t start_time = millis();

    int currentDriveValue = 60;

    // drive forward with ease in
    for(int i=10;i<currentDriveValue;i=i+3){
        driveValue(true, i);
        delay(1);
    }

    //monitor loop
    while(millis() < start_time + timeout){

        // Getting encoder position
        current_tick = _encoder->getPosition();
        
        // Calculating error
        int error = goal.tick - current_tick;

        // Updating tick history for Stall Detection
        if(millis() > stall.last_sample_time + 1){
            stall.last_sample_time = millis();
            stall.record(current_tick);
        }

        if(_beefy_boi){
            currentDriveValue = 255;
            driveBrakeValue(true, currentDriveValue);
        }

        else{
            if (error > DRIVE_APPROACH_FINAL_TICKS){
                currentDriveValue = 255;
                driveBrakeValue(true, currentDriveValue);
            }

            else if (error < DRIVE_APPROACH_FINAL_TICKS && error > 0){
                if(first_time_phase_two){

                    currentDriveValue = 50;
                    driveBrakeValue(true, currentDriveValue);
                    first_time_phase_two = false;

                }

                // reset stallcooldown if it's been a minute, motor has had time to react to new drive value
                if(stallCooldownTime + 20 < millis()){
                    stallCooldown = false;
                }

                // if stall detected, and it's been a minute since we've adjusted currentDriveValue, increase currentDriveValue
                if(stall.delta < 1 && stallCooldown == false){ 

                    currentDriveValue = currentDriveValue + 4;
                    if(currentDriveValue > 255){
                        currentDriveValue = 255;
                    }

                    stallCooldown = true;
                    stallCooldownTime = millis();

                }

                driveBrakeValue(true, currentDriveValue);
            }

        }

        if(error < 1){
            finishForwardMove(stall, goal);
            return true;
        }

    }
    // brake to kill any coast
    halt();

    return false;
}


bool PhotonFeeder::moveBackwardSequence(bool forward, uint16_t tenths_mm) {
/* moveInternal()
*   This function actually handles the translation of the mm movement to driving the motor to the right position based on encoder ticks.
*   We can't just calculate the number of ticks we need to move for the given mm movement requested, and increment our tick count by that much.
*   Because the tick count is only ever an approximation of the precise mm position, any rounding done from the mm->tick conversion will
*   result in a signficiant amount of accrued error.
*
*   Instead, we need to use the _mm_ position as ground truth, and only ever use the ticks as only how we command the PID loop. We do this by
*   first finding the new requested position, then converting this to ticks _based on the startup 0 tick position_. This is similar to
*   absolute positioning vs. relative positioning in Marlin. Every mm->tick calculation needs to be done based on the initial 0 tick position.
*
*/

    signed long goal_mm, timeout, signed_mm, current_tick;

    timeout = tenths_mm * TIMEOUT_TIME_PER_TENTH_MM;

    // doing final position math
    if(forward){
        goal_mm = _position + tenths_mm;
    }
    else{
        goal_mm = _position - tenths_mm;
    }

    // calculating goal_tick based on absolute, not relative position (see feedGoal())
    const FeedGoal goal = feedGoal(goal_mm);

    uint32_t start_time = millis();

    // in the direction of the goal with ease in
    for(size_t i=0; i<255; i=i+5){
        driveValue(forward, i);
        delay(1);
    }
    drive(forward);

    while(millis() < start_time + timeout + 20){

        current_tick = _encoder->getPosition();
        int error;
        
        if(forward){
            error = goal.tick - current_tick;
        }
        else{
            error = current_tick - goal.tick;
        }
        if (error < 0){
            brakeDrive();

            //delay to let position settle
            delay(50);

            adoptGoalPosition(goal);

            return true;
        }

    }
    // brake to kill any coast
    brakeDrive();

    return false;
}