#pragma once
#include "FormDesc.h"
#include <tuple>

// A map marker a player discovered (thuum docs/verbs/map-markers.md): the
// marker reference and whether fast travel to it is allowed.
struct MapMarker
{
  FormDesc refr;
  bool canTravel = false;

  auto ToTuple() const { return std::make_tuple(refr, canTravel); }

  friend bool operator==(const MapMarker& lhs, const MapMarker& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const MapMarker& lhs, const MapMarker& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
