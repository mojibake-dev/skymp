#pragma once

#include <nlohmann/json_fwd.hpp>

#include "archives/JsonOutputArchive.h"
#include "archives/SimdJsonInputArchive.h"

namespace simdjson::dom {
class element;
}

// A message's one field list, Serialize(Archive&), drives its JSON form,
// which is the form a message has in-process (thuum ADR-019). On the wire it
// is skymp-wire's encoding of the same fields, rendered and recognized in
// Rust; the BitStream archives went with RakNet.
class IMessageBase
{
public:
  virtual ~IMessageBase() = default;

  virtual void WriteJson(nlohmann::json& json) const = 0;
  virtual void ReadJson(const simdjson::dom::element& json) = 0;
};

template <class Message>
class MessageBase : public IMessageBase
{
public:
  void WriteJson(nlohmann::json& json) const override
  {
    JsonOutputArchive archive;
    AsMessage().Serialize(archive);
    json = std::move(archive.j);
  }

  void ReadJson(const simdjson::dom::element& json) override
  {
    SimdJsonInputArchive archive(json);
    AsMessage().Serialize(archive);
  }

private:
  Message& AsMessage() const
  {
    return *const_cast<Message*>(reinterpret_cast<const Message*>(this));
  }
};
