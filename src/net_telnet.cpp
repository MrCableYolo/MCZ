// net_telnet.cpp — see net_telnet.h. Deliberately does NOT #define Serial: every
// unqualified "Serial" below refers to the real hardware UART.
#include "net_telnet.h"

void TelnetSerial::begin(unsigned long baud){
  Serial.begin(baud);            // real UART, unchanged behaviour
  // The Telnet listener is started lazily from tick(), once WiFi is actually up (see below) -
  // starting a WiFiServer before the network interface exists is unreliable on ESP32.
}

void TelnetSerial::tick(){
  if (WiFi.status() != WL_CONNECTED) return;
  static bool started = false;
  if (!started){ server.begin(); server.setNoDelay(true); started = true; }
  if (server.hasClient()){
    if (!client || !client.connected()){
      client = server.available();
      Serial.println(">> Telnet client connected");
      client.println("MCZ Maestro BLE - telnet console ('help' for commands)");
    } else {
      server.available().stop();   // one client at a time -> politely reject a second
    }
  }
  if (client && !client.connected()) client.stop();
}

size_t TelnetSerial::write(uint8_t b){
  Serial.write(b);
  if (client && client.connected()) client.write(b);
  return 1;
}
size_t TelnetSerial::write(const uint8_t *buf, size_t size){
  Serial.write(buf, size);
  if (client && client.connected()) client.write(buf, size);
  return size;
}
int TelnetSerial::available(){
  int n = Serial.available();
  if (client && client.connected()) n += client.available();
  return n;
}
int TelnetSerial::read(){
  if (Serial.available()) return Serial.read();
  if (client && client.connected() && client.available()) return client.read();
  return -1;
}
int TelnetSerial::peek(){
  if (Serial.available()) return Serial.peek();
  if (client && client.connected() && client.available()) return client.peek();
  return -1;
}
void TelnetSerial::flush(){ Serial.flush(); }

TelnetSerial g_telnetSerial;
