// chrono.h — MCZ weekly scheduler ("cronotermostato") reader.
// Self-contained: reads the chrono registers via bleReadRegs() (ble_api.h) and prints
// the programmed schedule.
//   temp_eco     = 0x097C (2428)              eco setpoint (/10 C)
//   temp_comfort = 0x097D (2429)              comfort setpoint (/10 C)
//   program_g<D>_int<N> = 2430 + (D-1)*48 + (N-1)
//     D = 1..7 = Monday..Sunday ; N = 1..48 = half-hour slot (int1 = 00:00-00:30)
#pragma once

// Serial command 'getchrono': read the known chrono registers and print them (analogous
// to 'settime'). Blocks briefly while reading (like a manual read).
void chronoPrint();
