#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/cell-host.md (ADR-035): the server's release. Server to
// one client, reliable, R0: the client's game stops hosting the cell; the
// epoch is the grant that ends. Idempotent per (cell, epoch).
struct HostReleaseMessage : public MessageBase<HostReleaseMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::HostRelease)>{};

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
