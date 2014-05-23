//EtherCard library for ENC28J60
#include <EtherCard.h>

// DHT22 library for RHT03
#include <DHT22.h>

#define FEED "your_feed_here"
#define APIKEY "your_apikey_here"
char website[] PROGMEM = "api.xively.com";

// ethernet interface mac address, must be unique on the LAN
byte mymac[] = { 
  0x74,0x69,0x69,0x2D,0x30,0x31 };

byte Ethernet::buffer[700];

// Set up timers
uint32_t timer1;
uint32_t timer2;
uint32_t timer3;
uint32_t timer4;

// Stash used to store the string that is sent to the web
Stash stash;

// RHT03 data wire is plugged into port 7 on the Arduino
// Connect a 22K resistor between VCC and the data pin
#define DHT22_PIN 7

// Setup a DHT22 instance
DHT22 myDHT22(DHT22_PIN);

// Pin 9 and 8 have a LED
int led1 = 9;
int led2 = 8;

// Create variables
long passMillis; // Millis of the last pass of the KWH meter
int Watt; // Current wattage
long irSensorValue; // IR sensor value
const int numReadings = 100;
int irSensorReadings[numReadings]; // The readings from the analog input
int irSensorIndex = 0; // The index of the current reading
int irSensorTotal = 0; // The running total
int irSensorAverage = 0; // IR sensor average
int irState = 2; // IR Sensor debounce. Black = 0, Shiny = 1, a value of 2 is used to initially set irState
float tempC; // Temperature sensor value
float humPCT; // Humiditiy sensor value
static char statusstr[10]; // Stores value to be sent to web
int channel; // Set up a variable for the Xively channel
float fadeValue; // Used to fade led1

// Analog input
const int analogInPin = A0;  // Analog input pin that the IR Sensor is attached to

void setup () {
  Serial.begin(57600);
  Serial.println("\n[Energy Monitor]");
  pinMode (led1, OUTPUT);
  pinMode (led2, OUTPUT);

  if (ether.begin(sizeof Ethernet::buffer, mymac, 10) == 0)
    Serial.println("Failed to access Ethernet controller");
  if (!ether.dhcpSetup())
    Serial.println("DHCP failed");

  ether.printIp("IP: ", ether.myip);
  ether.printIp("GW: ", ether.gwip);
  ether.printIp("DNS: ", ether.dnsip);

  if (!ether.dnsLookup(website))
    Serial.println("DNS failed");

  ether.printIp("SRV: ", ether.hisip);

  // Timers are set 30 seconds apart
  timer1 = millis();
  timer2 = millis() + 30000;
  timer3 = millis() + 60000;

  // IR Sensor avarage array
  for (int irSensorReading = 0; irSensorReading < numReadings; irSensorReading++)
    irSensorReadings[irSensorReading] = 0;
}

void loop () {
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);

  // Read the analog in value:
  irSensorValue = analogRead(analogInPin);

  // Sample the average
  if (timer4 < millis()) {
    irSensorTotal = irSensorTotal - irSensorReadings[irSensorIndex]; // Subtract the last reading
    irSensorReadings[irSensorIndex] = irSensorValue; // Add the reading to the total
    irSensorTotal = irSensorTotal + irSensorReadings[irSensorIndex]; // Advance to the next position in the array
    irSensorIndex = irSensorIndex + 1; // Advance to the next position in the array

    if (irSensorIndex >= numReadings) { // If we're at the end of the array...
      irSensorIndex = 0; // Wrap around to the beginning
    }

    // Calculate the average:
    irSensorAverage = irSensorTotal / numReadings;

    // Dataplotter
    Serial.print(irSensorValue);
    Serial.print(',');
    Serial.println(irSensorAverage);
    timer4 = millis() + 25;
  }

  if (irState != 1) {
    if (irSensorValue >= irSensorAverage) {
      irState = 1;
    }
  }

  if (irState != 0) {  
    if (irSensorValue < irSensorAverage - (irSensorAverage * 0.15)) {
      fadeValue = 255;
      analogWrite(led1, fadeValue);
      // Time between cycles to watt. Calculate the number of rotations per
      // hour. Devide by 600 rotations per KWH, multiply by 1000 for watts.
      Watt = ((100000/(millis() - passMillis)*3600)/600)*10;
      passMillis = millis();
      irState = 0;
    }    
  }

  if (fadeValue > 1) {
    fadeValue = fadeValue - 0.1;
    analogWrite(led1, fadeValue);
  }

  if (millis() > timer1) {
    timer1 = millis() + 90000;
    // Get temperature
    readsensor();
    tempC=(myDHT22.getTemperatureC());
    if ( tempC > 0) {
      Serial.print("Temperature: ");
      Serial.println(tempC);
      // Prepare string
      dtostrf(tempC,3,1,statusstr);
      channel = 116;
      // Send to web
      sendtoweb();
    }
  }

  if (millis() > timer2) {
    timer2 = millis() + 90000;
    // Send humidity
    readsensor();
    humPCT=(myDHT22.getHumidity());
    if (humPCT > 0) {
      Serial.print("Humidity: ");
      Serial.println(humPCT);
      dtostrf(humPCT,3,1,statusstr);
      channel = 117;
      sendtoweb();
    }
  }

  if (millis() > timer3) {
    timer3 = millis() + 90000;
    // Send watts
    Serial.print("Watt: ");
    Serial.println(Watt);
    dtostrf(Watt,3,1,statusstr);
    channel = 118;
    sendtoweb();
  }
}

void sendtoweb() {
  // Determine the size of the generated message ahead of time
  byte sd = stash.create();
  stash.print(channel);
  stash.print(",");
  stash.println(statusstr);
  stash.save();

  // Generate the header with payload - note that the stash size is used,
  // and that a "stash descriptor" is passed in as argument using "$H"
  Stash::prepare(PSTR("PUT http://$F/v2/feeds/$F.csv HTTP/1.0" "\r\n"
    "Host: $F" "\r\n"
    "X-PachubeApiKey: $F" "\r\n"
    "Content-Length: $D" "\r\n"
    "\r\n"
    "$H"),
  website, PSTR(FEED), website, PSTR(APIKEY), stash.size(), sd);

  // Send the packet - this also releases all stash buffers once done
  ether.tcpSend();
  return;
}

void readsensor() {
  // Read RHT03
  DHT22_ERROR_t errorCode;
  errorCode = myDHT22.readData();
  switch(errorCode)
  {
  case DHT_ERROR_NONE:
    break;
  case DHT_ERROR_CHECKSUM:
    Serial.print("check sum error ");
    break;
  case DHT_BUS_HUNG:
    Serial.println("BUS Hung ");
    break;
  case DHT_ERROR_NOT_PRESENT:
    Serial.println("Not Present ");
    break;
  case DHT_ERROR_ACK_TOO_LONG:
    Serial.println("ACK time out ");
    break;
  case DHT_ERROR_SYNC_TIMEOUT:
    Serial.println("Sync Timeout ");
    break;
  case DHT_ERROR_DATA_TIMEOUT:
    Serial.println("Data Timeout ");
    break;
  case DHT_ERROR_TOOQUICK:
    Serial.println("Polled to quick ");
    break;
    return;
  }
}















