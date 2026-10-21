#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/cell-host.md (ADR-035): the server's grant. Server to one
// client, reliable, R0: the client's game hosts the cell from now, at this
// epoch, one counter per cell that goes up at every grant, so a grant or a
// report from an older epoch is dropped. Idempotent per (cell, epoch).
struct HostGrantMessage : public MessageBase<HostGrantMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::HostGrant)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("cell", cell)
      .Serialize("epoch", epoch);
  }

  uint32_t cell = 0;
  uint32_t epoch = 0;
};
