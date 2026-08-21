// ble_api.h — thin interface of the BLE layer (main.cpp) for the display menu.
// Oven search: collects visible MCZ_EP devices WITHOUT connecting automatically.
#pragma once
#include <Arduino.h>

void bleScanListStart();                 // disconnect active connection, start collection scan
void bleScanListStop();                  // leave collection mode (normal auto-connect active again)
bool bleScanListBusy();                  // is a scan running right now?
int  bleScanListCount();                 // number of ovens found
bool bleScanListGet(int i, String& mac, String& name);

// Synchronous Modbus block read (function 03): send the read and wait for the response,
// writing 'count' 16-bit registers into dst. Returns false on timeout / not connected.
// For diagnostic/read-only modules (e.g. chrono.cpp). Blocks up to ~1.5 s; call from loop
// context (serial command), NOT from a BLE callback.
bool bleReadRegs(uint16_t reg, uint16_t count, uint16_t* dst);
