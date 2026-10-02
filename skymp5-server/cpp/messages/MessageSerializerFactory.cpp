#include "MessageSerializerFactory.h"
#include "Messages.h"
#include "MinPacketId.h"
#include "MsgType.h"
#include <cstring>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

// Packets are 0x86 + SkyMP's JSON form (thuum ADR-019). The network edge is
// Rust (skymp-wire): it renders every inbound message in this form, after
// recognizing and validating it, and recognizes every outbound one, so this
// file never sees a byte a client sent. The binary BitStream form and RakNet
// went with the port.

namespace {
template <class Message>
std::optional<DeserializeResult> Deserialize(
  const simdjson::dom::element& parsedJson)
{
  Message message;
  message.ReadJson(parsedJson);

  DeserializeResult result;
  result.msgType = static_cast<MsgType>(Message::kMsgType.value);
  result.message = std::make_unique<Message>(std::move(message));
  result.format = DeserializeInputFormat::Json;
  return result;
}
} // namespace

#define REGISTER_MESSAGE(Message)                                             \
  deserializeFns[static_cast<size_t>(Message::kMsgType)] =                    \
    Deserialize<Message>;

std::shared_ptr<MessageSerializer>
MessageSerializerFactory::CreateMessageSerializer()
{
  constexpr auto kDeserializeFnMax = static_cast<size_t>(MsgType::Max);
  std::vector<MessageSerializer::DeserializeFn> deserializeFns(
    kDeserializeFnMax);

  REGISTER_MESSAGES

  // make_shared isn't working for private constructors
  return std::shared_ptr<MessageSerializer>(
    new MessageSerializer(deserializeFns));
}

MessageSerializer::MessageSerializer(
  std::vector<DeserializeFn> deserializerFns_)
  : deserializerFns(deserializerFns_)
{
}

void MessageSerializer::Serialize(const char* jsonContent, std::string& output)
{
  output.clear();
  output.push_back(static_cast<char>(Networking::MinPacketId));
  output.append(jsonContent, strlen(jsonContent));
}

void MessageSerializer::Serialize(const IMessageBase& message,
                                  std::string& output)
{
  nlohmann::json j;
  message.WriteJson(j);
  output.clear();
  output.push_back(static_cast<char>(Networking::MinPacketId));
  output += j.dump();
}

std::optional<DeserializeResult> MessageSerializer::Deserialize(
  const uint8_t* packet, size_t length)
{
  if (length < 2 || packet[1] != '{') {
    spdlog::trace("MessageSerializer::Deserialize - not a JSON packet");
    return std::nullopt;
  }

  // Read "t" once and hand the message to its own reader.
  simdjson::dom::parser sjParser;
  auto parsed =
    sjParser.parse(reinterpret_cast<const char*>(packet) + 1, length - 1);
  if (auto err = parsed.error()) {
    throw std::runtime_error(
      fmt::format("failed to parse message, simdjson error: {}",
                  simdjson::error_message(err)));
  }
  const simdjson::dom::element parsedJson = parsed.value_unsafe();

  auto t = parsedJson.at_key("t").get_uint64();
  if (t.error() == simdjson::NO_SUCH_FIELD) {
    // Messages produced by the server use string "type" instead of integer
    // "t"; PacketParser handles them
    return std::nullopt;
  }
  if (auto err = t.error()) {
    throw std::runtime_error(
      fmt::format("failed to get message type, simdjson error: {}",
                  simdjson::error_message(err)));
  }
  const auto index = t.value_unsafe();
  if (index >= deserializerFns.size() || !deserializerFns[index]) {
    spdlog::trace("MessageSerializer::Deserialize - no reader for t={}, "
                  "falling back to PacketParser.cpp",
                  index);
    return std::nullopt;
  }
  return deserializerFns[index](parsedJson);
}
