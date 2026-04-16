#pragma once

#include "coproc_link.hpp"

namespace alc {

// Dispatches a PeripheralData frame to the right per-device-type decoder
// and publishes the resulting JSON to MQTT.
// Matches the deviceType enum from ble_peripheral_standard.md / ble_auth_peripheral.cpp:
//   1 = FridgeMaster, 2 = DrawerMaster, 3 = RoomMaster, 4 = FlushMaster.
class PeripheralHandler
{
public:
  static PeripheralHandler& Instance();

  void Handle(const PeripheralRecord& rec);

private:
  PeripheralHandler() = default;

  void handleRoomMaster(const PeripheralRecord& rec);
};

}  // namespace alc
