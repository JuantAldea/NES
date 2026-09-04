#include "../include/device.h"

#include "../include/bus.h"

// Out of line so that device.h does not need Bus's definition: bus.h includes
// device.h, so the dependency cannot run the other way in a header.
//
// The null check is not defensive padding. Several unit tests construct a
// device standalone - `ROM rom(nullptr)` in memory_tests.cpp - to exercise
// loading without a whole machine around it. None of those reaches a register
// read, but a crash there would be a confusing way to find that out.
uint8_t Device::open_bus() const { return bus != nullptr ? bus->cpu_open_bus : 0; }

bool Device::continues_a_run() const { return bus != nullptr && bus->controller_read_is_continuation; }
