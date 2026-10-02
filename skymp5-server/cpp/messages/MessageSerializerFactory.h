#pragma once

#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace simdjson::dom {
class element;
}

class MessageSerializer;

class MessageSerializerFactory
{
public:
  static std::shared_ptr<MessageSerializer> CreateMessageSerializer();
};

enum class DeserializeInputFormat
{
  Json
};

struct DeserializeResult
{
  MsgType msgType = MsgType::Invalid;
  std::unique_ptr<IMessageBase> message;
  DeserializeInputFormat format = DeserializeInputFormat::Json;
};

class MessageSerializer
{
  friend class MessageSerializerFactory;

public:
  // A packet is the 0x86 packet id and SkyMP's JSON form of a message
  // (thuum ADR-019): the network edge (skymp-wire, Rust) renders inbound
  // messages that way and recognizes outbound ones.
  void Serialize(const char* jsonContent, std::string& output);

  void Serialize(const IMessageBase& message, std::string& output);

  std::optional<DeserializeResult> Deserialize(const uint8_t* packet,
                                               size_t length);

private:
  typedef std::optional<DeserializeResult> (*DeserializeFn)(
    const simdjson::dom::element& parsedJson);

  explicit MessageSerializer(std::vector<DeserializeFn> deserializerFns);

  const std::vector<DeserializeFn> deserializerFns;
};
