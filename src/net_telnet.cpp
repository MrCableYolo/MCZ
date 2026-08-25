// net_telnet.cpp — see net_telnet.h. Deliberately does NOT #define Serial: every
// unqualified "Serial" below refers to the real hardware UART.
#include "net_telnet.h"
#include <string.h>   // memcpy

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
  if (paused_){
    if (pauseLen_ < PAUSE_BUF_SIZE) pauseBuf_[pauseLen_++] = b;
    else pauseDropped_++;                       // buffer full: count what we couldn't keep
  } else if (client && client.connected()) client.write(b);
  return 1;
}
size_t TelnetSerial::write(const uint8_t *buf, size_t size){
  Serial.write(buf, size);
  if (paused_){
    size_t room = (PAUSE_BUF_SIZE > pauseLen_) ? (PAUSE_BUF_SIZE - pauseLen_) : 0;
    size_t take = size < room ? size : room;
    if (take){ memcpy(pauseBuf_ + pauseLen_, buf, take); pauseLen_ += take; }
    if (take < size) pauseDropped_ += (size - take);
  } else if (client && client.connected()) client.write(buf, size);
  return size;
}

void TelnetSerial::pauseOutput(){
  paused_ = true; pauseLen_ = 0; pauseDropped_ = 0;
}
void TelnetSerial::resumeOutput(){
  paused_ = false;
  if (client && client.connected() && pauseLen_) client.write(pauseBuf_, pauseLen_);
  pauseLen_ = 0;
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
