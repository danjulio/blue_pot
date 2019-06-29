/*
 * AG1171/Phone interface module - state management and control for the telephone
 *  - Off/On Hook Detection
 *  - Ring Control
 *  - Dial Tone or No Service tone generation
 *  - Receiver Off Hook tone generation
 *  - Rotary Dial or DTMF dialing support
 * 
 */
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>


// ==============================================================================
// Pin assignments for AG1171 SLIC
//
const int PIN_FR = 2;              // Tip/Ring signal Forward/nReverse control
const int PIN_RM = 3;              // Bias Control: High for Ring Mode, Low for other modes
const int PIN_SHK = 4;             // Switch hook detection:
                                   //   Break: Low when on-hook (or dial pulse)
                                   //   Make : High when off-hook
const int PIN_OFF_HOOK_LED = 13;   // LED output to indicate switch hook state visually



// ==============================================================================
// Constants
//

// State machine evaluation interval
#define POTS_EVAL_MSEC           10

// Make period required to move from off-hook back to on-hook (differentiate from rotary dialing pulses)
#define POTS_ON_HOOK_DETECT_MSEC 500

// Period to detect individual DTMF presses
#define POTS_DTMF_DRIVEN_MSEC    30
#define POTS_DTMF_SILENT_MSEC    30

// Audio Library Tone decoder thresholds for DTMF detection
#define POTS_DTMF_ROW_THRESHOLD  0.2
#define POTS_DTMF_COL_THRESHOLD  0.2

// Special DTMF key values
#define POTS_DTMF_ASTERISK_VAL   10
#define POTS_DTMF_POUND_VAL      11

// Ringing timing
#define POTS_RING_ON_MSEC        1000
#define POTS_RING_OFF_MSEC       3000
#define POTS_RING_FREQ_HZ        25

// Rotary dialing pulse detection
//   Break is maximum period to detect a pulse in a digit
//   Make is minimum period between digits
#define POTS_ROT_BREAK_MSEC      100
#define POTS_ROT_MAKE_MSEC       100

// No Service Tone timing
#define POTS_NS_TONE_ON_MSEC     300
#define POTS_NS_TONE_OFF_MSEC    200

// Off-Hook Tone timing
#define POTS_OH_TONE_ON_MSEC     100
#define POTS_OH_TONE_OFF_MSEC    100

// Receiver off-hook detection timing threshold
#define POTS_RCV_OFF_HOOK_MSEC   60000

// Tone frequency sets for the four waveform generators (HZ)
const float pots_dial_tone_hz[4] = {350, 440, 0, 0};
const float pots_no_service_tone_hz[4] = {480, 620, 0, 0};
const float pots_off_hook_tone_hz[4] = {1400, 2060, 2450, 2600};

// Tone amplitude sets for the four waveform generators (0-1 - sum all active generators to 1)
const float pots_dial_tone_ampl[4] = {0.5, 0.5, 0, 0};
const float pots_no_service_tone_ampl[4] = {0.5, 0.5, 0, 0};
const float pots_off_hook_tone_ampl[4] = {0.25, 0.25, 0.25, 0.25};



// ==============================================================================
// Variables
//

// Module evaluation state
unsigned long pots_prev_evalT;    // Previous evaluation time in system mSec

// Main phone process state
typedef enum {ON_HOOK, OFF_HOOK, ON_HOOK_PROVISIONAL, RINGING} pots_stateT;
pots_stateT pots_state;
int pots_state_count;             // Down counter for phone state change detection
bool pots_in_service;             // Set if the phone can make a call, clear if it isn't in service
bool pots_in_call;                // Set when the external process has a connected phone call (used to suppress off-hook tone)

// Hook logic
bool pots_prev_off_hook;          // Previous state for off-hook debounce
bool pots_cur_off_hook;           // Debounced off-hook state
bool pots_saw_hook_state_change;  // For API notification

// Ring logic
typedef enum {RING_IDLE, RING_PULSE_ON, RING_PULSE_OFF, RING_BETWEEN} pots_ring_stateT;
pots_ring_stateT pots_ring_state;
int pots_ring_period_count;       // Counts down evaluation cycles for each ringing state
int pots_ring_pulse_count;        // Counts down pulses in one ring

// Dialing logic
typedef enum {DIAL_IDLE, DIAL_BREAK, DIAL_MAKE, DIAL_DTMF_ON, DIAL_DTMF_OFF} pots_dial_stateT;
pots_dial_stateT pots_dial_state;
int pots_dial_period_count;       // Counts up evaluation cycles for each dialing state
int pots_dial_pulse_count;        // Counts pulses from the rotary dial for one digit
bool pots_dial_new_digit;         // Set true when a digit has been detected dialed
int pots_dial_cur_digit;          // 0 - 9, 10 ('*'), 11 ('#')
int pots_dial_prev_digit;         // -1 indicates no previous DTMF digit, 0 or positive number contains prev for debounce

// Tone generation logic
typedef enum {TONE_IDLE, TONE_OFF, TONE_DIAL, TONE_NO_SERVICE_ON, TONE_NO_SERVICE_OFF, TONE_OFF_HOOK_ON, TONE_OFF_HOOK_OFF} pots_tone_stateT;
pots_tone_stateT pots_tone_state;
int pots_tone_period_count;       // Counts up evaluation cycles for tone states



// ==============================================================================
// Audio System
//

// Audio Input - DTMF decoder
AudioInputAnalog         adc1;
AudioAnalyzeToneDetect   row1;
AudioAnalyzeToneDetect   row2;
AudioAnalyzeToneDetect   row4;
AudioAnalyzeToneDetect   row3;
AudioAnalyzeToneDetect   column1;
AudioAnalyzeToneDetect   column2;
AudioAnalyzeToneDetect   column3;
AudioConnection          patchCord1(adc1, row1);
AudioConnection          patchCord2(adc1, row2);
AudioConnection          patchCord3(adc1, row3);
AudioConnection          patchCord4(adc1, row4);
AudioConnection          patchCord5(adc1, column1);
AudioConnection          patchCord6(adc1, column2);
AudioConnection          patchCord7(adc1, column3);

// Audio Output - Up to 4 frequencies
AudioSynthWaveform       waveform1;
AudioSynthWaveform       waveform2;
AudioSynthWaveform       waveform3;
AudioSynthWaveform       waveform4;
AudioMixer4              mixer1;
AudioOutputAnalog        dac1;
AudioConnection          patchCord8(waveform1, 0, mixer1, 0);
AudioConnection          patchCord9(waveform2, 0, mixer1, 1);
AudioConnection          patchCord10(waveform3, 0, mixer1, 2);
AudioConnection          patchCord11(waveform4, 0, mixer1, 3);
AudioConnection          patchCord12(mixer1, dac1);



// ==============================================================================
// API Routines
//

/*
 * Initialize POTS module
 */
void potsInit() {
  // Init state
  pots_state = ON_HOOK;
  pots_ring_state = RING_IDLE;
  pots_dial_state = DIAL_IDLE;
  pots_tone_state = TONE_IDLE;
  pots_in_service = false;
  pots_in_call = false;
  pots_prev_off_hook = false;
  pots_cur_off_hook = false;
  pots_saw_hook_state_change = false;
  pots_dial_new_digit = false;
  
  // Init subsystems
  _potsInitHardware();
  _potsInitAudio();

  // Finally initialize our evaluation time
  pots_prev_evalT = millis();
}


/*
 * Evaluate POTS module - call repeatedly from loop() at a sub-millisecond rate
 */
void potsEval() {
  bool hook_changed;  // Debounced hook output changed
  bool digit_dialed;  // Set when a digit is detected having been dialed
  
  if (_potsEvalTimeout()) {
    // Evaluate hardware for changes
    hook_changed = _potsEvalHook();

    // Evaluate our output state
    _potsEvalRinger(hook_changed);
    digit_dialed = _potsEvalDialer(hook_changed);
    _potsEvalTone(hook_changed, digit_dialed);
    
    // Finally evaluate the overall phone system state
    _potsEvalPhoneState(hook_changed);
  }
}


/*
 * Configure service level
 *   Set true to enable dial-tone generation indicating phone is in service
 *   Set false to enable no service tone generation indicating phone is out of service
 */
void potsSetInService(bool en) {
  pots_in_service = en;
}


/*
 * Initiate or end ringing
 *   Set true to initiate ringing
 *   Set false to terminate ringing
 * 
 * Ringing may also be ended automatically upon off-hook detection
 */
void potsSetRing(bool en) {
  if (en) {
    // Only allow ringing if the phone is on hook
    if ((pots_state == ON_HOOK) && (pots_ring_state == RING_IDLE)) {
      _potsStartRing();
    }
  } else {
    // Immediately end ringing
    if (pots_ring_state != RING_IDLE) {
      _potsEndRing();
    }
  }
}


/*
 * Set call progress status
 *   Set true when a call is connected
 *   Set false when a call has not yet been established or ends
 */
void potsSetInCall(bool en) {
  pots_in_call = en;
}


/*
 * Return true if a on-hook/off-hook transition was just detected
 *  *offHook set true if the phone is now off-hook, false if it is now on-hook
 */
boolean potsHookChange(bool* offHook) {
  if (pots_saw_hook_state_change) {
    pots_saw_hook_state_change = false;
    *offHook = (pots_state != ON_HOOK);
    return true;
  }

  return false;
}


/*
 * Return true if a digit was just detected dialed
 *   digitNum set for dialed digit: 0 - 9, 10 = '*', 11 = '#'
 */
boolean potsDigitDialed(int* digitNum) {
  if (pots_dial_new_digit) {
    pots_dial_new_digit = false;  // Consumed
    *digitNum = pots_dial_cur_digit;
    return true;
  }

  return false;
}



// ==============================================================================
// Internal Routines
//

void _potsInitHardware() {
  pinMode(PIN_FR, OUTPUT);
  digitalWrite(PIN_FR, HIGH);
  pinMode(PIN_RM, OUTPUT);
  digitalWrite(PIN_RM, LOW);
  pinMode(PIN_SHK, INPUT);
  pinMode(PIN_OFF_HOOK_LED, OUTPUT);
  digitalWrite(PIN_OFF_HOOK_LED, LOW);
}


void _potsInitAudio() {
  AudioMemory(6);

  // Configure the tone detectors with the frequency and number
  // of cycles to match.  These numbers were picked for match
  // times of approx 30 ms.  Longer times are more precise.
  row1.frequency(697, 21);
  row2.frequency(770, 23);
  row3.frequency(852, 25);
  row4.frequency(941, 28);
  column1.frequency(1209, 36);
  column2.frequency(1336, 40);
  column3.frequency(1477, 44);

  // Configure the waveform generators
  waveform1.begin(WAVEFORM_SINE, 0, 0);
  waveform2.begin(WAVEFORM_SINE, 0, 0);
  waveform3.begin(WAVEFORM_SINE, 0, 0);
  waveform4.begin(WAVEFORM_SINE, 0, 0);
  mixer1.gain(0, 0);
  mixer1.gain(1, 0);
  mixer1.gain(2, 0);
  mixer1.gain(3, 0);
}


// Updates current hook switch state
//   returns true if the state changes, false otherwise
bool _potsEvalHook() {
  bool cur_hw_off_hook;
  bool changed_detected = false;

  // Get current hardware signal
  cur_hw_off_hook = (digitalRead(PIN_SHK) == HIGH);

  // Look for debounced transitions
  if (cur_hw_off_hook && pots_prev_off_hook && !pots_cur_off_hook) {
    changed_detected = true;
    pots_cur_off_hook = true;
    digitalWrite(PIN_OFF_HOOK_LED, HIGH);
  } else if (!cur_hw_off_hook && !pots_prev_off_hook && pots_cur_off_hook) {
    changed_detected = true;
    pots_cur_off_hook = false;
    digitalWrite(PIN_OFF_HOOK_LED, LOW);
  }

  // Update state
  pots_prev_off_hook = cur_hw_off_hook;

  return changed_detected;
}


void _potsEvalPhoneState(bool hookChange) {
  switch (pots_state) {
    case ON_HOOK:
      if (hookChange && pots_cur_off_hook) {
        pots_state = OFF_HOOK;
        pots_saw_hook_state_change = true;
      } else if (pots_ring_state != RING_IDLE) {
        pots_state = RINGING;
      }
      break;
      
    case OFF_HOOK:
      if (hookChange && !pots_cur_off_hook) {
        // Back on-hook - it could be permanent or the start of a rotary dial
        pots_state = ON_HOOK_PROVISIONAL;
        pots_state_count = 0; // Initialize end-of-call (back on-hook) timer
      }
      break;
      
    case ON_HOOK_PROVISIONAL:
      // Increment timer so we can decide where to go on next action
      ++pots_state_count;

      if (hookChange && pots_cur_off_hook) {
        pots_state = OFF_HOOK;
      } else {
        if (pots_state_count >= (POTS_ON_HOOK_DETECT_MSEC / POTS_EVAL_MSEC)) {
          // Call has ended
          pots_state = ON_HOOK;
          pots_saw_hook_state_change = true;
        }
      }
      break;
      
    case RINGING:
      if (pots_ring_state == RING_IDLE) {
        if (pots_cur_off_hook) {
          // User picked up
          pots_state = OFF_HOOK;
          pots_saw_hook_state_change = true;
        } else {
          // Call cancelled
          pots_state = ON_HOOK;
        }
      }
      break;
  }
}


void _potsEvalRinger(bool hookChange) {
  if (hookChange && pots_cur_off_hook && (pots_ring_state != RING_IDLE)) {
    // End ringing if phone just went off hook
    _potsEndRing();
  }
  
  switch (pots_ring_state) {
    case RING_IDLE:
      // Do nothing as some external process will initiate ringing through potsSetRing()
      break;

    case RING_PULSE_ON:
      // Decrement ring-on timer
      pots_ring_period_count--;
      if (--pots_ring_pulse_count <= 0) {
        // Ring pulse done
        digitalWrite(PIN_FR, HIGH);
        pots_ring_state = RING_PULSE_OFF;
        pots_ring_pulse_count = ((POTS_RING_ON_MSEC / POTS_RING_FREQ_HZ) / 2) / POTS_EVAL_MSEC;  // Off for half a pulse
      }
      break;

    case RING_PULSE_OFF:
      // Decrement ring-off timer and pulse timer
      pots_ring_period_count--;
      pots_ring_pulse_count--;
      // Either timer expiring ends this half of the pulse
      if ((pots_ring_period_count <= 0) || (pots_ring_pulse_count <= 0)) {
        if (pots_ring_period_count <= 0) {
          // Setup silence between rings
          pots_ring_state = RING_BETWEEN;
          pots_ring_period_count = POTS_RING_OFF_MSEC / POTS_EVAL_MSEC;
        } else {
          // Setup next ring pulse in this ring
          pots_ring_state = RING_PULSE_ON;
          pots_ring_pulse_count = ((POTS_RING_ON_MSEC / POTS_RING_FREQ_HZ) / 2) / POTS_EVAL_MSEC;
          digitalWrite(PIN_FR, LOW);
        }
      }
      break;

    case RING_BETWEEN:
      // Look for end of silence between rings
      if (--pots_ring_period_count <= 0) {
        // Start next ring
        _potsStartRing();
      }
      break;
  }
}


void _potsStartRing() {
  pots_ring_state = RING_PULSE_ON;
  pots_ring_period_count = POTS_RING_ON_MSEC / POTS_EVAL_MSEC;
  pots_ring_pulse_count = ((POTS_RING_ON_MSEC / POTS_RING_FREQ_HZ) / 2) / POTS_EVAL_MSEC;  // On for half a pulse
  digitalWrite(PIN_RM, HIGH);  // Cause the line to enter ring mode
  digitalWrite(PIN_FR, LOW);   // Toggle the line (reverse) to start this pulse of the ring
}


void _potsEndRing() {
  pots_ring_state = RING_IDLE;
  digitalWrite(PIN_FR, HIGH);  // Make sure tip/ring are not reversed
  digitalWrite(PIN_RM, LOW);   // Exit ring mode
}


// Assumes that we'll only have one source (DTMF or rotary) dialing at a time
// Returns true a digit was detected dialed
bool _potsEvalDialer(bool hookChange) {
  int cur_dtmf_digit;
  bool digit_dialed_detected = false;
  
  // Evaluate the DTMF tone detector logic every time
  cur_dtmf_digit = _potsDtmfDigitFound();

  switch (pots_dial_state) {
    case DIAL_IDLE:
      if (pots_state != ON_HOOK) {
        if (hookChange && !pots_cur_off_hook) {
          pots_dial_state = DIAL_BREAK;
          pots_dial_pulse_count = 0;
          pots_dial_period_count = 0;  // Start timer to detect rotary dial break
        } else if (cur_dtmf_digit >= 0) {
          pots_dial_state = DIAL_DTMF_ON;
          pots_dial_prev_digit = cur_dtmf_digit;
          pots_dial_period_count = 0;  // Start timer to detect valid DTMF tone
        }
      } else {
        // Cancel any dialed digit left laying around (e.g. dialed after the call was placed)
        pots_dial_new_digit = false;
      }
      break;
      
    case DIAL_BREAK:
      // Increment timer
      ++pots_dial_period_count;

      if (pots_dial_period_count > (POTS_ROT_BREAK_MSEC / POTS_EVAL_MSEC)) {
        // Too long for a rotary dialer so this must be the switch hook going back on-hook
        pots_dial_state = DIAL_IDLE;
      } else if (hookChange && pots_cur_off_hook) {
        // Valid rotary pulse
        if (pots_dial_pulse_count < 10) ++pots_dial_pulse_count;
        pots_dial_state = DIAL_MAKE;
        pots_dial_period_count = 0;  // Start timer to detect rotary dial make action (either end of digit or inner-pulse)
      }
      break;
      
    case DIAL_MAKE:
      // Increment timer
      ++pots_dial_period_count;

      if (pots_dial_period_count > (POTS_ROT_MAKE_MSEC / POTS_EVAL_MSEC)) {
        // End of one rotary dial - note we have a digit
        pots_dial_new_digit = true;
        digit_dialed_detected = true;
        if (pots_dial_pulse_count == 10) {
          // Special case of '0' since it is actually 10 pulses from the rotary dial
          pots_dial_cur_digit = 0;
        } else {
          pots_dial_cur_digit = pots_dial_pulse_count;
        }
        pots_dial_state = DIAL_IDLE;
      } else if (hookChange && !pots_cur_off_hook) {
        // Start of next rotary pulse in this dial
        pots_dial_state = DIAL_BREAK;
        pots_dial_period_count = 0;  // Start timer to detect rotary dial break
      }
      break;
      
    case DIAL_DTMF_ON:
      // Increment timer
      ++pots_dial_period_count;

      if (cur_dtmf_digit < 0) {
        // End of tone - check if we saw it long enough
        if (pots_dial_period_count >= (POTS_DTMF_DRIVEN_MSEC / POTS_EVAL_MSEC)) {
          // Possible valid DTMF digit
          pots_dial_state = DIAL_DTMF_OFF;
          pots_dial_period_count = 0;  // Start timer to detect valid DTMF inner-tone period
        } else {
          // Tone length too short
          pots_dial_state = DIAL_IDLE;
        }
      } else if (cur_dtmf_digit != pots_dial_prev_digit) {
        // Invalid DTMF tone (it changed part-way through)
        pots_dial_state = DIAL_IDLE;
      }
      break;
      
    case DIAL_DTMF_OFF:
      // Increment timer
      ++pots_dial_period_count;

      if (pots_dial_period_count >= (POTS_DTMF_SILENT_MSEC / POTS_EVAL_MSEC)) {
        // Valid inner-digit delay so we have a valid DTMF digit
        pots_dial_new_digit = true;
        digit_dialed_detected = true;
        pots_dial_cur_digit = pots_dial_prev_digit;
        pots_dial_state = DIAL_IDLE;
      } else if (cur_dtmf_digit >= 0) {
        // Tone started again before inner-digit delay expired, see if it's a continuation of previous tone
        if (cur_dtmf_digit == pots_dial_prev_digit) {
          // Restart digit detection with existing timer value
          pots_dial_state = DIAL_DTMF_ON;
        } else {
          // Restart digit detection with new digit and timer
          pots_dial_state = DIAL_DTMF_ON;
          pots_dial_prev_digit = cur_dtmf_digit;
          pots_dial_period_count = 0;  // Start timer to detect valid DTMF tone
        }
      }
      break;
  }

  return digit_dialed_detected;
}


void _potsEvalTone(bool hookChange, bool digitDialed) {
  switch (pots_tone_state) {
    case TONE_IDLE:
      if (hookChange && pots_cur_off_hook) {
        if (pots_state == RINGING) {
          // Answering: No tone so audio from the remote party can be heard
          pots_tone_state = TONE_OFF;
          _potsSetAudioOutput(TONE_OFF);
          pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;  // receiver off hook (too long) detection
        } else if (pots_in_service) {
          // Dialing: Give the user some dial tone
          pots_tone_state = TONE_DIAL;
          _potsSetAudioOutput(TONE_DIAL);
          pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;  // receiver off hook (too long) detection
        } else {
          // Dialing: Start that fast beep telling the user there's no service
          pots_tone_state = TONE_NO_SERVICE_ON;
          _potsSetAudioOutput(TONE_NO_SERVICE_ON);
          pots_tone_period_count = POTS_NS_TONE_ON_MSEC / POTS_EVAL_MSEC;   // On portion of fast beep
        }
      }
      break;
      
    case TONE_OFF:
      if (pots_state == ON_HOOK) {
        // Call ended and we've detected the phone back on-hook
        pots_tone_state = TONE_IDLE;
        _potsSetAudioOutput(TONE_IDLE);
      } else if (!pots_in_call) {
        // Remote side ended call but phone is off-hook so check if it's time to give the receiver off hook tone
        if (--pots_tone_period_count <= 0) {
          pots_tone_state = TONE_OFF_HOOK_ON;
          _potsSetAudioOutput(TONE_OFF_HOOK_ON);
          pots_tone_period_count = POTS_OH_TONE_ON_MSEC / POTS_EVAL_MSEC;   // On portion of fast beep
        }
      } else {
        // Hold receiver off hook timer in reset while in a call
        pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;
      }
      break;
      
    case TONE_DIAL:
      if ((hookChange && !pots_cur_off_hook) || digitDialed) {
        // End dial tone because either they are hanging up or dialing a number
        pots_tone_state = TONE_OFF;
        _potsSetAudioOutput(TONE_OFF);
        pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;  // receiver off hook (too long) detection
      } else if (--pots_tone_period_count <= 0) {
        pots_tone_state = TONE_OFF_HOOK_ON;
        _potsSetAudioOutput(TONE_OFF_HOOK_ON);
        pots_tone_period_count = POTS_OH_TONE_ON_MSEC / POTS_EVAL_MSEC;
      }
      break;
      
    case TONE_NO_SERVICE_ON:
      if (--pots_tone_period_count <= 0) {
        pots_tone_state = TONE_NO_SERVICE_OFF;
        _potsSetAudioOutput(TONE_NO_SERVICE_OFF);
        pots_tone_period_count = POTS_NS_TONE_OFF_MSEC / POTS_EVAL_MSEC;   // Off portion of fast beep
      }
      break;
      
    case TONE_NO_SERVICE_OFF:
      if (pots_state == ON_HOOK) {
        // End beep because phone has been hung up
        pots_tone_state = TONE_IDLE;
        _potsSetAudioOutput(TONE_IDLE);
      } else if (--pots_tone_period_count <= 0) {
        pots_tone_state = TONE_NO_SERVICE_ON;
        _potsSetAudioOutput(TONE_NO_SERVICE_ON);
        pots_tone_period_count = POTS_NS_TONE_ON_MSEC / POTS_EVAL_MSEC;   // On portion of fast beep
      }
      break;

    case TONE_OFF_HOOK_ON:
      if (digitDialed) {
        // End tone because they are trying to dial...after all this time! They must have had to run out of the
        // room to find some long lost phone number and left the receiver just laying there, so we'll be nice
        // and let them dial (I don't think the phone company would have allowed this).
        pots_tone_state = TONE_OFF;
        _potsSetAudioOutput(TONE_OFF);
        pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;  // reset receiver off hook detection
      } else  if (--pots_tone_period_count <= 0) {
        pots_tone_state = TONE_OFF_HOOK_OFF;
        _potsSetAudioOutput(TONE_OFF_HOOK_OFF);
        pots_tone_period_count = POTS_OH_TONE_OFF_MSEC / POTS_EVAL_MSEC;
      }
      break;
            
    case TONE_OFF_HOOK_OFF:
      if (digitDialed) {
        pots_tone_state = TONE_OFF;
        _potsSetAudioOutput(TONE_OFF);
        pots_tone_period_count = POTS_RCV_OFF_HOOK_MSEC / POTS_EVAL_MSEC;  // reset receiver off hook detection
      } else if (pots_state == ON_HOOK) {
        // End tone because phone has been hung up
        pots_tone_state = TONE_IDLE;
        _potsSetAudioOutput(TONE_IDLE);
      } else if (--pots_tone_period_count <= 0) {
        pots_tone_state = TONE_OFF_HOOK_ON;
        _potsSetAudioOutput(TONE_OFF_HOOK_ON);
        pots_tone_period_count = POTS_OH_TONE_ON_MSEC / POTS_EVAL_MSEC;
      }
      break;
  }
}


//void _potsSetAudioOutput(pots_tone_stateT s) {
void _potsSetAudioOutput(int s) {
  int i;
  float freq[4];
  float ampl[4];

  switch ((pots_tone_stateT) s) {
    case TONE_DIAL:
      for (i=0; i<4; i++) {
        freq[i] = pots_dial_tone_hz[i];
        ampl[i] = pots_dial_tone_ampl[i];
      }
      break;
      
    case TONE_NO_SERVICE_ON:
      for (i=0; i<4; i++) {
        freq[i] = pots_no_service_tone_hz[i];
        ampl[i] = pots_no_service_tone_ampl[i];
      }
      break;
      
    case TONE_OFF_HOOK_ON:
      for (i=0; i<4; i++) {
        freq[i] = pots_off_hook_tone_hz[i];
        ampl[i] = pots_off_hook_tone_ampl[i];
      }
      break;
      
    case TONE_IDLE:
    case TONE_OFF:
    case TONE_NO_SERVICE_OFF:
    case TONE_OFF_HOOK_OFF:
    default:
      // No tone
      for (i=0; i<4; i++) {
        freq[i] = 0;
        ampl[i] = 0;
      }
  }

  AudioNoInterrupts();  // disable audio library momentarily
  if (ampl[0] == 0) {
    waveform1.amplitude(0);
  } else {
    waveform1.frequency(freq[0]);
    waveform1.amplitude(1.0);
  }

  if (ampl[1] == 0) {
    waveform2.amplitude(0);
  } else {
    waveform2.frequency(freq[1]);
    waveform2.amplitude(1.0);
  }

  if (ampl[2] == 0) {
    waveform3.amplitude(0);
  } else {
    waveform3.frequency(freq[2]);
    waveform3.amplitude(1.0);
  }

  if (ampl[3] == 0) {
    waveform4.amplitude(0);
  } else {
    waveform4.frequency(freq[3]);
    waveform4.amplitude(1.0);
  }

  for (i=0; i<4; i++) {
    mixer1.gain(i, ampl[i]);
  }
  AudioInterrupts();    // enable so tones will start together
}


// Returns -1 if no digit is found
int _potsDtmfDigitFound() {
  int digit = -1;   // Set to a postive digit or value for non-numeric character if one is found
  float r1, r2, r3, r4, c1, c2, c3;

  // read the tone detectors
  r1 = row1.read();
  r2 = row2.read();
  r3 = row3.read();
  r4 = row4.read();
  c1 = column1.read();
  c2 = column2.read();
  c3 = column3.read();

  // check all 12 combinations for key press
  if (r1 >= POTS_DTMF_ROW_THRESHOLD) {
    if (c1 > POTS_DTMF_COL_THRESHOLD) {
      digit = 1;
    } else if (c2 > POTS_DTMF_COL_THRESHOLD) {
      digit = 2;
    } else if (c3 > POTS_DTMF_COL_THRESHOLD) {
      digit = 3;
    }
  } else if (r2 >= POTS_DTMF_ROW_THRESHOLD) {
    if (c1 > POTS_DTMF_COL_THRESHOLD) {
      digit = 4;
    } else if (c2 > POTS_DTMF_COL_THRESHOLD) {
      digit = 5;
    } else if (c3 > POTS_DTMF_COL_THRESHOLD) {
      digit = 6;
    }
  } else if (r3 >= POTS_DTMF_ROW_THRESHOLD) {
    if (c1 > POTS_DTMF_COL_THRESHOLD) {
      digit = 7;
    } else if (c2 > POTS_DTMF_COL_THRESHOLD) {
      digit = 8;
    } else if (c3 > POTS_DTMF_COL_THRESHOLD) {
      digit = 9;
    }
  } else if (r4 >= POTS_DTMF_ROW_THRESHOLD) {
    if (c1 > POTS_DTMF_COL_THRESHOLD) {
      digit = POTS_DTMF_ASTERISK_VAL;
    } else if (c2 > POTS_DTMF_COL_THRESHOLD) {
      digit = 0;
    } else if (c3 > POTS_DTMF_COL_THRESHOLD) {
      digit = POTS_DTMF_POUND_VAL;
    }
  }

  return digit;
}


boolean _potsEvalTimeout() {
  unsigned long curT = millis();
  unsigned long deltaT;
  
  if (curT > pots_prev_evalT) {
    deltaT = curT - pots_prev_evalT;
  } else {
    // Handle wrap
    deltaT = ~(pots_prev_evalT - curT) + 1;
  }
  
  if (deltaT >= POTS_EVAL_MSEC) {
    pots_prev_evalT = curT;
    return true;
  }

  return false;
}
