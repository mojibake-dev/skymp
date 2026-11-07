#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <optional>
#include <type_traits>

struct ActivateMessage : public MessageBase<ActivateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::Activate)>{};

  struct Data
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("t", kMsgType)
        .Serialize("caster", caster)
        .Serialize("target", target)
        .Serialize("isSecondActivation", isSecondActivation)
        .Serialize("entry", entry);
    }

    uint64_t caster = 0;
    uint64_t target = 0;
    bool isSecondActivation = false;
    // thuum D-W1 and D-C2 (SCHEMA_VERSION 40): the presser's owner's report
    // of an entry into a trigger whose world script runs on the cell's host,
    // 1 OnTriggerEnter, 2 OnTriggerLeave (a u8 on the wire); `caster` is the
    // presser (0x14 for the sender's own player, or an actor it simulates)
    // and `target` the trigger. Absent on every activation
    // (ActionListener::OnTriggerEntry; wire-schema skymp.rs, ActivateData)
    std::optional<uint32_t> entry;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("data", data);
  }

  Data data;
};
