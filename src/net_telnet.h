// net_telnet.h — WiFi Telnet bridge for the serial console (port 23, one client at a time).
//
// Mirrors the log output AND accepts remote command input, so the board can be
// monitored/controlled over WiFi instead of (or in addition to) a USB cable.
//
// Usage in main.cpp: include this header, then
//     #define Serial g_telnetSerial
// right after the other includes. Every existing Serial.print/println/printf/available/
// read call in that file is then transparently mirrored to Telnet too, with NO other
// code changes needed. The real hardware UART is untouched underneath (net_telnet.cpp
// itself does NOT redefine Serial), so the USB serial monitor keeps working exactly
// as before, at the same time.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

class TelnetSerial : public Stream {
public:
  void begin(unsigned long baud);   // starts the real UART (baud) AND the Telnet listener
  void tick();                      // call once per loop(): accept a new client / drop a stale one

  // Stream/Print interface: every call is mirrored to the real UART + the Telnet client (if any)
  size_t write(uint8_t b) override;
  size_t write(const uint8_t *buf, size_t size) override;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override;

private:
  WiFiServer server{23};
  WiFiClient client;
};

extern TelnetSerial g_telnetSerial;
