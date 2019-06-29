/*
 * Blue POT prototype - quick&dirty code to experiment with interfacing the
 * BM64 Bluetooth module with the AG1171 SLIC.  Designed to get the main
 * state machines figured out.  Includes a simple USB Serial command interface.
 * 
 * Simplified dialing model:
 *   - dial '0' as first/only digit -> activates voice dialing (e.g. Siri or Google Voice "operator")
 *   - dial 10 digits -> dial number
 *   
 * Teensy 3.2 Connections
 * ----------------------
 *   D0  - RX1 - BM64 TX
 *   D1  - TX1 - BM64 RX
 *   D2  - AG1171 FR
 *   D3  - AG1171 RM
 *   D4  - AG1171 SHK
 *   D5  - BM64 RSTN
 *   D6  - BM64 EAN
 *   D7  - BM64 P2_0
 *   D8  - BM64 MFB
 *   D13 - Teensy built-in LED
 *   D23 - Jumper Input (to ground)
 *   DAC - AG1171 VIN via 1 uF capacitor
 *   A2  - AG1171 VOUT via 1 uF capacitor with 0.6v bias circuit
 *   
 * BM64 AOHPR connected to AG1171 VIN via 1 uF capacitor
 * BM64 MIC_P1 connected to AG1171 VOUT via 0.1 uF capacitor and voltage divider
 * 
 * Teensy 3.2 built-in PTC fuse bypassed.
 * 
 * Jumper
 *   - Inserted at power-on puts the BM64 into Mode 1 (Flash IBDK) and echos serial data
 *     between USB serial and BM64 to enable configuring the BM64 using its software
 *     running on a PC.
 *     
 * Uses EEPROM to store one of eight possible Bluetooth pairings to use to link to.
 * 
 * See the "DisplayUsage" subroutine for the current Command Interface command set and arguments.
 * 
 * Copyright (c) 2019 Dan Julio
 * 
 * Date     Version   Description
 * ------------------------------------------------------------------------------
 * 6-21-19  1.0       Initial version
 * 
 * Distributed "as-is"; No warranty given.  But hopefully it's helpful to you.  Let me know
 * if you make one or if you find bugs.
 *    
 */
#include <EEPROM.h>

// ==============================================================================
// Pin assignments - main control
//
const int PIN_FUNC = 23;  // Active low on startup enable of BM64 EEPROM update mode (pulled-high)



// ==============================================================================
// Constants
//

// Version string
#define VERSION     "1.0"

// Command processing
#define CMD_ST_IDLE 0
#define CMD_ST_CMD  1
#define CMD_ST_VAL1 2
#define CMD_ST_VAL2 3

#define TERM_CHAR 0x0D

// EEPROM Locations
#define EEP_DEV_ID  0



// ==============================================================================
// Variables
//
// Command processor
int CmdState = CMD_ST_IDLE;
byte CurCmd;
int CmdArg;
int CmdValIndex;
uint8_t CmdVal[32];
int CmdValNum;
boolean CmdHasVal;
char CmdBuf[32];


// Jumper detection
bool transparentMode;

// Device ID to pair with
int pairID;



// ==============================================================================
// Arduino entry-points
//

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  // Look at jumper initially
  pinMode(PIN_FUNC, INPUT_PULLUP);
  delay(500);
  if (digitalRead(PIN_FUNC) == LOW) {
    // Update mode
    _btSetMode(1);  // SetMode(3) for entry into firmware update
    _btDoReset(true);
    transparentMode = true;
  } else {
    // Normal start-up
    transparentMode = false;
    eepInit();  // First to get pairID
    btInit(pairID);
    cmdInit();
    potsInit();
  }
}


void loop() {
  if (transparentMode) {
    // Echo data between HOST USB and BM64
    if (Serial.available()) Serial1.write(Serial.read());
    if (Serial1.available()) Serial.write(Serial1.read());
  } else {
    cmdEval();
    btEval();
    potsEval();
  }
}



// ==============================================================================
// Command processor
//
void cmdInit() {
  CmdState = CMD_ST_IDLE;
}


void cmdEval() {
  char c;
  int  v;
  
  while (Serial.available()) {
    c = Serial.read();
    
    switch (CmdState) {
      case CMD_ST_IDLE:
        if (ValidateCommand(c)) {
          CurCmd = c;
          CmdHasVal = false;
          CmdArg = 0;
          CmdState = CMD_ST_CMD;
        }
        break;
        
      case CMD_ST_CMD:
        if (c == TERM_CHAR) {
          ProcessCommand();
          CmdState = CMD_ST_IDLE;
        } else {
          if ((v = IsValidHex(c)) >= 0) {
            CmdArg = (CmdArg*16) + v;
          } else if (c == '=') {
            CmdState = CMD_ST_VAL1;
            CmdValNum = 0;
            CmdValIndex = 0;
            CmdVal[CmdValIndex] = 0;
          } else {
            CmdState = CMD_ST_IDLE;
            Serial.println("Illegal command");
          }
        }
        break;
        
      case CMD_ST_VAL1: // Waiting for first numeric character in a value
        if (c == TERM_CHAR) {
          ProcessCommand();
          CmdState = CMD_ST_IDLE;
        } else {
          if ((v = IsValidHex(c)) >= 0) {
            CmdVal[CmdValIndex] = v;
            CmdValNum++;
            CmdHasVal = true;
            CmdState = CMD_ST_VAL2;
          } else {
            CmdState = CMD_ST_IDLE;
            Serial.println("Illegal command");
          }
        }
        break;

      case CMD_ST_VAL2: // In the middle of a value
        if (c == TERM_CHAR) {
          ProcessCommand();
          CmdState = CMD_ST_IDLE;
        } else if (c == ' ') {
          // Increment to next value (note this will blow up if the user enters more than the length of CmdVal array)
          CmdValIndex++;
          CmdVal[CmdValIndex] = 0;
          CmdState = CMD_ST_VAL1;
        } else {
          if ((v = IsValidHex(c)) >= 0) {
            CmdVal[CmdValIndex] = (CmdVal[CmdValIndex]*16) + v;
          } else {
            CmdState = CMD_ST_IDLE;
            Serial.println("Illegal command");
          }
        }
        break;

      default:
        CmdState = CMD_ST_IDLE;
    }
  }
}


boolean ValidateCommand(char c) {
  switch (c) {
    case 'D':
    case 'H':
    case 'L':
    case 'P':
    case 'R':
    case 'V':
      return true;
      break;
    
    default:
      return false;
  }
}


// Returns -1 for invalid hex character, 0 - 15 for valid hex
int IsValidHex(char c) {
  if ((c >= '0') && (c <= '9')) {
    return (c - '0');
  } else if ((c >= 'a') && (c <= 'f')) {
    return (c - 'a' + 10);
  } else if ((c >= 'A') && (c <= 'F')) {
    return (c - 'A' + 10);
  }
  return -1;
}


void ProcessCommand() {
  bool b;
  
  switch (CurCmd) {
    case 'D':
      if (CmdHasVal) {
        if (CmdVal[0] > 7) {
          Serial.println("Illegal Device ID");
        } else {
          if (CmdVal[0] != pairID) {
            pairID = CmdVal[0];
            EEPROM.write(EEP_DEV_ID, pairID);
            btSetPairingNumber(pairID);
          }
        }
      }
      Serial.printf("Pairing Device ID = %d\n", pairID);
      break;
      
    case 'L':
      btSendPairingEnable();
      Serial.println("Enable Pairing");
      break;
      
    case 'P':
      if (CmdHasVal) {
        btSendGenericPacket(CmdVal, CmdValNum);
      }
      break;

    case 'R':
      btInit(pairID);
      Serial.println("Reset BM64");
      break;

    case 'V':
      if (CmdHasVal) {
        b = (CmdVal[0] != 0);
        btSetVerboseLogging(b);
        Serial.printf("Verbose = %d\n", CmdVal[0]);
      }
      break;

    case 'H':
      DisplayUsage();
      break;
  }
}


void DisplayUsage() {
  Serial.print("Command Interface for version ");
  Serial.println(VERSION);
  Serial.println("   D                : Display the current Bluetooth pairing ID (0-7)");
  Serial.println("   D=<N>            : Set the current Bluetooth pairing ID (0-7) - ");
  Serial.println("                      This is the device ID of the bluetooth connection to try to make");
  Serial.println("   L                : Initiate Bluetooth pairing (the BM64 will eventually time-out if no pairing is made)");
  Serial.println("   P=[Packet Bytes] : Send packet.  Packet bytes are a list of the hex bytes making up the Op Code");
  Serial.println("                      and optional data bytes, excluding sync, length and checksum bytes");
  Serial.println("   R                : Reset BM64");
  Serial.println("   V=<N>            : 1: Set/0: clear verbose mode for received packets from BM64");
  Serial.println("   H                : Usage");
  Serial.println("");
  Serial.println("  All arguments are specified as hex numbers (0 - FF)");
  Serial.println("");
  Serial.println("  Received packets are displayed:");
  Serial.println("    RX: 00 AA <Len_H> <Len_L> <Opcode> <Data> ... <Data> <Checksum>");
  Serial.println("");
}



// ==============================================================================
// EEPROM Storage
//
void eepInit() {
  pairID = EEPROM.read(EEP_DEV_ID);

  if (pairID > 7) {
    pairID = 0;
    EEPROM.write(EEP_DEV_ID, pairID);
  }

  Serial.printf("pairID = %d\n", pairID);
}

