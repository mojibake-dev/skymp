#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <string>
#include <type_traits>

// The line a console command the player sent prints in its console (thuum
// docs/verbs/console-commands.md): the game's own wording where it prints
// one, or why the server refused the command. Server to client.
struct ConsoleOutputMessage : public MessageBase<ConsoleOutputMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ConsoleOutput)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("text", text)
      .Serialize("refused", refused);
  }

  std::string text;
  bool refused = false;
};
