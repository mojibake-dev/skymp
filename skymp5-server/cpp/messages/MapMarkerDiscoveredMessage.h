#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// The player's engine discovered a location of this marker type (thuum
// docs/verbs/map-markers.md). The client does not say which marker: the
// server finds it in the master files near the player and records it.
struct MapMarkerDiscoveredMessage
  : public MessageBase<MapMarkerDiscoveredMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::MapMarkerDiscovered)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("markerType", markerType)
      .Serialize("canTravel", canTravel);
  }

  uint16_t markerType = 0; // the engine's MARKER_TYPE
  bool canTravel = false;  // fast travel allowed to it
};
