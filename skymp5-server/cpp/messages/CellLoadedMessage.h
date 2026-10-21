#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/cell-host.md (ADR-035): the client's game reports a cell
// fully loaded, or gone from its loaded set. Client to server, reliable (a
// load then an unload keep their order), R1: the server's election reads it
// against its own grid. Idempotent: it sets a bit.
struct CellLoadedMessage : public MessageBase<CellLoadedMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::CellLoaded)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("cell", cell)
      .Serialize("loaded", loaded);
  }

  uint32_t cell = 0; // the cell's form id in the load order both ends share
  bool loaded = false;
};
