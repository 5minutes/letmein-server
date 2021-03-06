/* 
  LetMeIn door opener. When OPEN command is received on serial port door is opened.
  eZ430-RF2500T wireless board is connected to digital input, when that digital in is high door should be opened.
  ENC28J60 ethernet module is connected to arduino and is listening for http requests that match GET /door/open. If that GET request is received door is opened
 */
 
#include <EtherCard.h>
#include <EEPROMex.h>

#define STATIC 1  // set to 1 to disable DHCP (adjust myip/gwip values below)
#define INACTIVE_STATE 2 //state when relay is off but is not accepting open commands

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;
const int RELAY_SIGNAL_DURATION = 250;
const int INACTIVE_STATE_DURATION = 3000;
const uint32_t PING_INTERVAL_MICROS = 5000000;
const int PING_COUNT_LIMIT = 5;
const int relay = 4;
const int MSP430 = 2;
const int pinAddress = 0;
const int led = 13;

int state = RELAY_OFF;
unsigned long stateStartMilis = 0;

#if STATIC
// ethernet interface ip address
static byte myip[] = { 10,5,1,230 };
// gateway ip address
static byte gwip[] = { 10,5,1,1 };
#endif

// ethernet mac address - must be unique on your network
static byte MAC[] = { 0x74,0x69,0x69,0x2D,0x30,0x38 };
static byte pingServer[] = { 10,5,1,1 };
static uint32_t pingTimer;
int pingCounter = PING_COUNT_LIMIT;

byte Ethernet::buffer[500]; // tcp/ip send and receive buffer
char replyOK[] PROGMEM = "HTTP/1.1 200 OK\r\n";
char replyUnauthorized[] PROGMEM = "HTTP/1.1 401 Unauthorized\r\n";
char replyBadRequest[] PROGMEM = "HTTP/1.1 400 Bad Request\r\n";

String PIN;

void(* resetFunc) (void) = 0;

void setup() {
  digitalWrite(relay, RELAY_OFF);
  pinMode(relay, OUTPUT);
  pinMode(led, OUTPUT);
  
  Serial.begin(9600);
  Serial.println("Program start.");

  //writePinToEEPROM("1234");
  PIN = readPinFromEEPROM();
  Serial.println("PIN: " + PIN);  
  
  MSP430Setup();
  ethernetSetup();
}

void MSP430Setup() {
  pinMode(MSP430, INPUT);
}

void ethernetSetup() {
  if (ether.begin(sizeof Ethernet::buffer, MAC) == 0) {
    Serial.println( "Failed to access Ethernet controller");
  }
#if STATIC
  ether.staticSetup(myip, gwip);
#else
  if (!ether.dhcpSetup())
    Serial.println("DHCP failed");
#endif

  //call this to report others pinging us
  ether.registerPingCallback(pingHandler);
  
  Serial.print("MAC: ");
  for (byte i = 0; i < 6; ++i) {
    Serial.print(MAC[i], HEX);
    if (i < 5)
      Serial.print(':');
  }
  Serial.println();
  
  ether.printIp("IP:  ", ether.myip);
  ether.printIp("GW:  ", ether.gwip);  
  ether.printIp("DNS: ", ether.dnsip); 
  ether.printIp("pingServer: ", pingServer);
  
  pingTimer = -9999999; // start timing out right away
}

void loop(){
  turnRelayOff();
  checkMSP430();
  checkEthernet();
}

void serialEvent() {
  boolean found = Serial.find("OPEN");
  if (found) {
    Serial.println("Received OPEN command via Serial");
    turnRelayOn();
  }
}

void turnRelayOn() {
  if (state == RELAY_OFF) {
    Serial.println("RELAY_ON");
    state = RELAY_ON;
    digitalWrite(relay, RELAY_ON);
    stateStartMilis = millis();
    digitalWrite(led, HIGH);
  }
  else {
    Serial.println("Relay is not in off state");
  }
}

void turnRelayOff() {
  if (state == RELAY_ON) {
    unsigned long currentMillis = millis();
    if(currentMillis - stateStartMilis > RELAY_SIGNAL_DURATION) {
      Serial.println("INACTIVE_STATE");
      state = INACTIVE_STATE;
      digitalWrite(relay, RELAY_OFF);
      stateStartMilis = millis();
      digitalWrite(led, LOW);
    }
  }
  else if (state == INACTIVE_STATE) {
    unsigned long currentMillis = millis();
    if(currentMillis - stateStartMilis > INACTIVE_STATE_DURATION) {
      Serial.println("RELAY_OFF");
      state = RELAY_OFF;
    }
  }
}

void checkMSP430() {
  int inputValue = digitalRead(MSP430);
  if (inputValue == HIGH) {
    Serial.println("Received OPEN command from MSP430 via RF");
    turnRelayOn();
    delay(10);
  }
}

void checkEthernet() {
  
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  
  // report whenever a reply to our outgoing ping comes back
  if (len > 0 && ether.packetLoopIcmpCheckReply(pingServer)) {
    //Serial.print("PingServer reply");
    //Serial.print("  ");
    //Serial.print((micros() - pingTimer) * 0.001, 3);
    //Serial.println(" ms");
    //reset counter
    pingCounter = PING_COUNT_LIMIT;
  }
  else if (pos) {
    String data = (char *) Ethernet::buffer + pos;
    Serial.println("**********************");
    
    Serial.println(data);
    

    boolean openCommand = data.startsWith("POST /letmein/door/open HTTP");
    boolean validateCommand = data.startsWith("POST /letmein/door/validate HTTP");
    boolean correctPin = data.indexOf("pin=" + PIN) > -1;
    
    boolean changePinCommand = data.startsWith("POST /letmein/door/changePin HTTP");
    boolean correctOldPin = data.indexOf("oldPin=" + PIN) > -1;

    int replyCode = 401;
    if(correctPin && (openCommand || validateCommand)) {
      if (openCommand) {
        Serial.println("Received OPEN command via Ethernet");
        turnRelayOn();
      }
      if (validateCommand) {
        Serial.println("Validated pin via Ethernet");
      }
      replyCode = 200;
    }
    else if (changePinCommand && correctOldPin) {
      replyCode = 400;
      int start = data.indexOf("newPin=");
      if (start > -1) {
        String newPin = data.substring(start + 7, start + 7 + 4);
        if (newPin.length() == 4) {
          Serial.println("Changed PIN");
          writePinToEEPROM(newPin);
          PIN = readPinFromEEPROM();
          replyCode = 200;
        }
      }
    }

    ethernetReply(replyCode);
  }
  
  if (pingCounter <= 0 && state != RELAY_ON) {
    Serial.println("RESET!");
    delay(1000);
    resetFunc(); //call reset 
  }
  // ping a remote server once every few seconds
  if (micros() - pingTimer >= PING_INTERVAL_MICROS) {
    //Serial.print("Pings left: ");
    //Serial.println(pingCounter);
    //ether.printIp("Pinging: ", pingServer);
    
    if (pingCounter < PING_COUNT_LIMIT) {
        Serial.print("Pings left: ");
        Serial.println(pingCounter);
    }
    
    pingCounter--;
    pingTimer = micros();
    ether.clientIcmpRequest(pingServer);
  }
}

void ethernetReply(int replyCode) {
  Serial.println("ethernetReply");
  Serial.println(replyCode);
  if (replyCode == 200) {
    Serial.println("replyCode == 200");
    memcpy_P(ether.tcpOffset(), replyOK, sizeof replyOK);
    ether.httpServerReply(sizeof replyOK - 1);
  }
  else if (replyCode == 400) {
    Serial.println("replyCode == 400");
    memcpy_P(ether.tcpOffset(), replyBadRequest, sizeof replyBadRequest);
    ether.httpServerReply(sizeof replyBadRequest - 1);
  }
  else {
    Serial.println("replyCode else");
    memcpy_P(ether.tcpOffset(), replyUnauthorized, sizeof replyUnauthorized);
    ether.httpServerReply(sizeof replyUnauthorized - 1);
  }
}

void writePinToEEPROM(String pin) {
  char data[5];
  pin.toCharArray(data, 5);
  EEPROM.writeBlock(pinAddress, data);
}

String readPinFromEEPROM() {
  char data[5];
  EEPROM.readBlock(pinAddress, data);
  String pin = data;
  return pin;
}

// called when a ping comes in (replies to it are automatic)
static void pingHandler (byte* ptr) {
  ether.printIp(">>> ping from: ", ptr);
}
