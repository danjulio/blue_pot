/*
 * BM64 Bluetooth interface module - interface and state management for the
 * module and calling.
 *  - Bluetooth Connection State management
 *  - Phone Call state management
 *  - Bluetooth reconnect
 *  - Bluetooth pairing
 *  
 */


// ==============================================================================
// Pin assignments - BM64 HCI Serial connected to Teensy Serial1
//
const int PIN_RSTN = 5;   // Active Low BM64 Reset - Active High/Low
const int PIN_EAN  = 6;   // BM64 EAN configuration Input - Active High / Tri-state Low
const int PIN_P2_0 = 7;   // BM64 Configuration Input - Tri-state High / Active Low
const int PIN_MFB  = 8;   // Active High BM64 Multi-Function Button Input - Active High/Low



// ==============================================================================
// Constants
//
#define BT_EVAL_MSEC 20

// Number of digits to receive to initiate dialing
#define NUM_VALID_DIGITS 10

// Interval to attempt to reconnect to the specified pairing when bluettooth is disconnected
#define BT_RECONNECT_MSEC 60000



// ==============================================================================
// Variables
//

// Module evaluation state
unsigned long bt_prev_evalT;     // Previous evaluation time
bool bt_verbose_logging;
int bt_reconnect_count;          // Number of evaluation periods left while disconnected before attempting to reconnect
int bt_link_device_index;        // BM64 paired device index (0-7)

// Bluetooth state
const char* bt_state_name[] = {"DISCONNECTED", "CONNECTED-IDLE", "DIALING", "ACTIVE", "INITIATED", "OUTGOING", "RECEIVED"};
typedef enum {BT_DISCONNECTED, BT_CONNECTED_IDLE, BT_DIALING, BT_CALL_ACTIVE, BT_CALL_INITIATED, BT_CALL_OUTGOING, BT_CALL_RECEIVED} bt_stateT;
bt_stateT bt_state;

const char* bt_call_state_name[] = {"IDLE", "VOICEDIAL", "INCOMING", "OUTGOING", "ACTIVE"};
typedef enum {CALL_IDLE, CALL_VOICEDIAL, CALL_INCOMING, CALL_OUTGOING, CALL_ACTIVE} bt_call_stateT;
bt_call_stateT bt_call_state;
bool bt_in_service;               // Set when a HF Bluetooth connection exists, clear when nothing connected

// Incoming packet
typedef enum {BT_RX_IDLE, BT_RX_SYNC, BT_RX_LEN_H, BT_RX_LEN_L, BT_RX_CMD, BT_RX_DATA, BT_RX_CHKSUM} bt_rx_stateT;
bt_rx_stateT bt_rx_pkt_state;
char bt_rx_pkt_buf[32];
int bt_rx_pkt_index;
int bt_rx_pkt_len;
uint8_t bt_rx_pkt_exp_chksum;

// Outgoing packet
uint8_t bt_tx_pkt_buf[32];

// Dialing
int bt_dial_index;
int bt_dial_array[NUM_VALID_DIGITS];



// ==============================================================================
// Bluetooth API
//

void btInit(int n) {
  // Initialize variables
  bt_state = BT_DISCONNECTED;
  bt_call_state = CALL_IDLE;
  bt_in_service = false;
  bt_rx_pkt_state = BT_RX_IDLE;
  bt_rx_pkt_index = 0;
  bt_dial_index = 0;
  bt_verbose_logging = false;
  bt_reconnect_count = BT_RECONNECT_MSEC / BT_EVAL_MSEC;
  bt_link_device_index = n & 0x07;

  // Initialize hardware
  _btInitPins();
  _btSetMode(0);
  _btDoReset(true);

  // Finally initialize our evaluation time
  bt_prev_evalT = millis();
}


void btEval() {
  bool off_hook;
  int new_digit;
  
  // Look for serial data from the bluetooth module
  while (Serial1.available()) {
    _btProcessRxData((uint8_t) Serial1.read());
  }

  // Evaluate state
  if (_btEvalTimeout()) {
    switch (bt_state) {
      // No bluetooth connection
      case BT_DISCONNECTED:
        if (bt_in_service) {
          _btSetState(BT_CONNECTED_IDLE);
        } else if (--bt_reconnect_count <= 0) {
          // Attempt to connect to a device if it's in range
          Serial.printf("Connection attempt to device id %d\n", bt_link_device_index);
          _btSendLinkToSelectedDeviceIndex();
          bt_reconnect_count = BT_RECONNECT_MSEC / BT_EVAL_MSEC;
        }
        break;

      // Bluetooth connected; no activity
      case BT_CONNECTED_IDLE:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (potsHookChange(&off_hook)) {
          if (off_hook) {
            _btSetState(BT_DIALING);
          }
        } else if (bt_call_state == CALL_INCOMING) {
          _btSetState(BT_CALL_RECEIVED);
        }
        break;

      // Phone went off-hook with no incoming call so assume user wants to dial
      case BT_DIALING:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (potsDigitDialed(&new_digit)) {
          if ((new_digit == 0) && (bt_dial_index == 0)) {
            _btSendVoiceDial();
            _btSetState(BT_CALL_INITIATED);
            Serial.println("Voice Dial");
          } else {
            bt_dial_array[bt_dial_index++] = new_digit;
            if (bt_dial_index == NUM_VALID_DIGITS) {
              _btSendDialNumber();
              _btSetState(BT_CALL_INITIATED);
              _btPrintNumber(bt_dial_index);
            }
          }
        } else if (potsHookChange(&off_hook)) {
          if (!off_hook) {
            _btSetState(BT_CONNECTED_IDLE);
          }
        }
        break;

      // Call in progress
      case BT_CALL_ACTIVE:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (bt_call_state == CALL_IDLE) {
          _btSetState(BT_CONNECTED_IDLE);
        } else if (potsHookChange(&off_hook)) {
          if (!off_hook) {
            _btSendDropCall();
            _btSetState(BT_CONNECTED_IDLE);
          }
        }
        break;

      // Command sent to cellphone to dial a number but it hasn't yet acknowledged it's dialing the number
      case BT_CALL_INITIATED:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (bt_call_state == CALL_ACTIVE) {
          _btSetState(BT_CALL_ACTIVE);
        } else if (bt_call_state == CALL_OUTGOING) {
          _btSetState(BT_CALL_OUTGOING);
        } else if (potsHookChange(&off_hook)) {
          if (!off_hook) {
            _btSendDropCall();
            _btSetState(BT_CONNECTED_IDLE);
          }
        }
        break;

      // Cellphone is attempting to dial the number
      case BT_CALL_OUTGOING:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (bt_call_state == CALL_ACTIVE) {
          _btSetState(BT_CALL_ACTIVE);
        } else if (bt_call_state == CALL_IDLE) {
          _btSetState(BT_CONNECTED_IDLE);
        } else if (potsHookChange(&off_hook)) {
          if (!off_hook) {
            _btSendDropCall();
            _btSetState(BT_CONNECTED_IDLE);
          }
        }
        break;

      // Incoming phone call
      case BT_CALL_RECEIVED:
        if (!bt_in_service) {
          _btSetState(BT_DISCONNECTED);
        } else if (potsHookChange(&off_hook)) {
          if (off_hook) {
            _btSendAcceptCall();
            _btSetState(BT_CALL_ACTIVE);
          }
        } else if (bt_call_state != CALL_INCOMING) {
          // Call ended or user answered cellphone before picking up
          _btSetState(BT_CONNECTED_IDLE);
        }
        break;
    }
  }
}


void btSetPairingNumber(int n) {
  bt_link_device_index = n & 0x07;
}


void btSetVerboseLogging(bool b) {
  bt_verbose_logging = b;
}


void btSendPairingEnable() {
  bt_tx_pkt_buf[0] = 0x02;  // MMI Command
  bt_tx_pkt_buf[1] = 0x00;  // Database 0
  bt_tx_pkt_buf[2] = 0x5D;  // Fast enter pairing mode
  _btSendTxPacket(3);
}


void btSendGenericPacket(uint8_t* pktP, int l) {
  int i;

  for (i=0; i<l; i++) {
    bt_tx_pkt_buf[i] = *pktP++;
  }
  _btSendTxPacket(l);
}



// ==============================================================================
// Bluetooth subroutines
//

void _btInitPins() {
  pinMode(PIN_RSTN, OUTPUT);
  digitalWrite(PIN_RSTN, HIGH);
  pinMode(PIN_EAN, INPUT);
  pinMode(PIN_P2_0, INPUT);
  pinMode(PIN_MFB, OUTPUT);
  digitalWrite(PIN_MFB, LOW);
}


void _btSetResetPin(int v) {
    digitalWrite(PIN_RSTN, v);
}


void _btSetEanPin(int v) {
  if (v == HIGH) {
    pinMode(PIN_EAN, OUTPUT);
    digitalWrite(PIN_EAN, HIGH);
  } else {
    digitalWrite(PIN_EAN, LOW);
    pinMode(PIN_EAN, INPUT);
  }
}


void _btSetP2_0Pin(int v) {
  if (v == HIGH) {
    digitalWrite(PIN_P2_0, HIGH);
    pinMode(PIN_P2_0, INPUT);
  } else {
    pinMode(PIN_P2_0, OUTPUT);
    digitalWrite(PIN_P2_0, LOW);
  }
}


void _btSetMfbPin(int v) {
  digitalWrite(PIN_MFB, v);
}


void _btSetMode(int m) {
  switch (m) {
    case 1: // Flash IBDK
      _btSetEanPin(LOW);
      _btSetP2_0Pin(LOW);
      break;

    case 2: // ROM App
      _btSetEanPin(HIGH);
      _btSetP2_0Pin(HIGH);
      break;

    case 3: // ROM IBDK
      _btSetEanPin(HIGH);
      _btSetP2_0Pin(LOW);
      break;

    default: // Flash App
      _btSetEanPin(LOW);
      _btSetP2_0Pin(HIGH);
  }
}


void _btDoReset(bool set_mfb) {
  if (set_mfb) {
    _btSetMfbPin(LOW);
  }
  _btSetResetPin(LOW);

  delay(499);

  if (set_mfb) {
    _btSetMfbPin(HIGH);
  }

  delay(1);

  _btSetResetPin(HIGH);  
}


boolean _btEvalTimeout() {
  unsigned long curT = millis();
  unsigned long deltaT;
  
  if (curT > bt_prev_evalT) {
    deltaT = curT - bt_prev_evalT;
  } else {
    // Handle wrap
    deltaT = ~(bt_prev_evalT - curT) + 1;
  }
  
  if (deltaT >= BT_EVAL_MSEC) {
    bt_prev_evalT = curT;
    return true;
  }

  return false;
}


//void _btSetState(bt_stateT s) {
void _btSetState(int s) {
  switch ((bt_stateT) s) {
    case BT_DISCONNECTED:
      potsSetInService(false);
      potsSetInCall(false);
      potsSetRing(false);
      bt_reconnect_count = BT_RECONNECT_MSEC / BT_EVAL_MSEC;
      break;
        
    case BT_CONNECTED_IDLE:
      potsSetInService(true);
      potsSetInCall(false);
      potsSetRing(false);
      break;

    case BT_DIALING:
      bt_dial_index = 0;            // Reset the index when we enter this state
      break;
        
    case BT_CALL_ACTIVE:
      _btSendSetSpeakerGain(0x0E);  // Increased speaker gain seems necessary for iPhone
      potsSetInCall(true);
      potsSetRing(false);
      break;
        
    case BT_CALL_INITIATED:
      break;

    case BT_CALL_OUTGOING:
      break;
      
    case BT_CALL_RECEIVED:
      potsSetRing(true);
      break;
  }

  Serial.print("BT State: ");
  Serial.print(bt_state_name[(int) bt_state]);
  Serial.print(" -> ");
  Serial.println(bt_state_name[(int) s]);

  bt_state = (bt_stateT) s;
}



void _btProcessRxData(uint8_t d) {
  switch (bt_rx_pkt_state) {
    case BT_RX_IDLE:
      if (d == 0x00) {
        bt_rx_pkt_state = BT_RX_SYNC;
        bt_rx_pkt_index = 0;
        bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      }
      break;
      
    case BT_RX_SYNC:
      if (d == 0xAA) {
        bt_rx_pkt_state = BT_RX_LEN_H;
        bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      } else {
        bt_rx_pkt_state = BT_RX_IDLE;
      }
      break;
      
    case BT_RX_LEN_H:
      bt_rx_pkt_state = BT_RX_LEN_L;
      bt_rx_pkt_len = d * 0x100;
      bt_rx_pkt_exp_chksum = d;
      bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      break;
      
    case BT_RX_LEN_L:
      bt_rx_pkt_state = BT_RX_CMD;
      bt_rx_pkt_len += d;
      bt_rx_pkt_exp_chksum += d;
      bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      break;
      
    case BT_RX_CMD:
      bt_rx_pkt_state = BT_RX_DATA;
      bt_rx_pkt_exp_chksum += d;
      bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      break;
      
    case BT_RX_DATA:
      if (bt_rx_pkt_index == (bt_rx_pkt_len+3)) {
        bt_rx_pkt_state = BT_RX_CHKSUM;
      }
      bt_rx_pkt_exp_chksum += d;
      bt_rx_pkt_buf[bt_rx_pkt_index++] = d;
      break;
      
    case BT_RX_CHKSUM:
      bt_rx_pkt_state = BT_RX_IDLE;
      bt_rx_pkt_exp_chksum = ~bt_rx_pkt_exp_chksum + 1;  // 2's complement
      bt_rx_pkt_buf[bt_rx_pkt_index] = d;

      if (bt_verbose_logging) {
        if (bt_rx_pkt_exp_chksum != d) {
          Serial.print("BAD ");
        }
        Serial.print("RX: ");
        for (int i=0; i<(bt_rx_pkt_len+5); i++) {
          if (bt_rx_pkt_buf[i] < 0x10) {
            Serial.print("0");
          }
          Serial.print(bt_rx_pkt_buf[i], HEX);
          Serial.print(" ");
        }
        Serial.println("");
      }

      if ((bt_rx_pkt_buf[4] != 0x00) && (bt_rx_pkt_exp_chksum == d)) {
        // Display and process valid non-Command_ACK packets
        _btSendEventAck(bt_rx_pkt_buf[4]);
        _btProcessRxPkt();
      }
      break;
      
    default:
      bt_rx_pkt_state = BT_RX_IDLE;
  }
}


void _btProcessRxPkt() {
  switch (bt_rx_pkt_buf[4]) {
    case 0x01: // BTM_Status
      if (bt_rx_pkt_buf[5] == 0x05) {
        // HF/HS Link established
        bt_in_service = true;
      } else if (bt_rx_pkt_buf[5] == 0x07) {
        // HF Link disconnected
        bt_in_service = false;
      }
      break;

    case 0x02: // Call_Status
      switch (bt_rx_pkt_buf[6]) {
        case 0x00: // Idle
          bt_call_state = CALL_IDLE;
          break;
        case 0x01: // Voice Dialing
          bt_call_state = CALL_VOICEDIAL;
          break;
        case 0x02: // Incoming call
          bt_call_state = CALL_INCOMING;
          break;
        case 0x03: // Outgoing call
          bt_call_state = CALL_OUTGOING;
          break;
        case 0x04: // Call active
          bt_call_state = CALL_ACTIVE;
          break;
      }
      Serial.print("Call: ");
      Serial.println(bt_call_state_name[(int) bt_call_state]);
      break;


    case 0x03: // Caller_ID
      Serial.print("Caller ID: ");
      for (int i=0; i<(bt_rx_pkt_buf[3]-2); i++) {
        Serial.write(bt_rx_pkt_buf[i+6]);
      }
      Serial.println();
      break;
  }
}


void _btSendEventAck(uint8_t id) {
  bt_tx_pkt_buf[0] = 0x14;
  bt_tx_pkt_buf[1] = id;
  _btSendTxPacket(2);
}


void _btSendLinkToSelectedDeviceIndex() {
  bt_tx_pkt_buf[0] = 0x17;  // Profile_Link_Back
  bt_tx_pkt_buf[1] = 0x04;  // Initiate connection to specified index and profile
  bt_tx_pkt_buf[2] = (uint8_t) bt_link_device_index;
  bt_tx_pkt_buf[3] = 0x03;  // HS profile
  _btSendTxPacket(4);
}


void _btSendAcceptCall() {
  bt_tx_pkt_buf[0] = 0x02;
  bt_tx_pkt_buf[1] = 0x00;
  bt_tx_pkt_buf[2] = 0x04;
  _btSendTxPacket(3);
}


void _btSendDropCall() {
  bt_tx_pkt_buf[0] = 0x02;
  bt_tx_pkt_buf[1] = 0x00;
  bt_tx_pkt_buf[2] = 0x06;
  _btSendTxPacket(3);
}


void _btSendDialNumber() {
  int i;

  bt_tx_pkt_buf[0] = 0x00;
  bt_tx_pkt_buf[1] = 0x00;
  
  for (i=0; i<NUM_VALID_DIGITS; i++) {
    if (bt_dial_array[i] == 10) {
      bt_tx_pkt_buf[i+2] = '*';
    } else if (bt_dial_array[i] == 11) {
      bt_tx_pkt_buf[i+2] = '#';
    } else {
      bt_tx_pkt_buf[i+2] = '0' + bt_dial_array[i];
    }
  }
  _btSendTxPacket(12);
}


void _btSendVoiceDial() {
  bt_tx_pkt_buf[0] = 0x02;
  bt_tx_pkt_buf[1] = 0x00;
  bt_tx_pkt_buf[2] = 0x0A;
  _btSendTxPacket(3);
}


void _btSendSetSpeakerGain(uint8_t g) {
  bt_tx_pkt_buf[0] = 0x1B;
  bt_tx_pkt_buf[1] = 0x00;
  bt_tx_pkt_buf[2] = g & 0x0F;
  _btSendTxPacket(3);
}


void _btSendTxPacket(uint16_t l) {
  uint16_t i;
  uint8_t cs = (l >> 8);
  
  Serial1.write(0x00);
  Serial1.write(0xAA);
  Serial1.write(l >> 8);
  Serial1.write(l & 0xFF);
  cs += (l & 0xFF);

  if (bt_verbose_logging) {
    Serial.print("TX: ");
    _btPrintHex8(0x00);
    _btPrintHex8(0xAA);
    _btPrintHex8(l >> 8);
    _btPrintHex8(l & 0xFF);
  }

  for (i=0; i<l; i++) {
    Serial1.write(bt_tx_pkt_buf[i]);
    cs += bt_tx_pkt_buf[i];

    if (bt_verbose_logging) {
      _btPrintHex8(bt_tx_pkt_buf[i]);
    }
  }

  Serial1.write(~cs + 1);

  if (bt_verbose_logging) {
    _btPrintHex8(~cs + 1);
    Serial.println();
  }
}


void _btPrintHex8(uint8_t n) {
  if (n < 0x10) {
    Serial.print("0");
  }
  Serial.print(n, HEX);
  Serial.print(" ");
}


void _btPrintNumber(int n) {
  int i;

  Serial.print("Number: ");
  for (i=0; i<n; i++) {
    if (bt_dial_array[i] == 10) {
      Serial.print("*");
    } else if (bt_dial_array[i] == 11) {
      Serial.print("#");
    } else {
      Serial.print(bt_dial_array[i]);
    }
  }

  Serial.println();
}

