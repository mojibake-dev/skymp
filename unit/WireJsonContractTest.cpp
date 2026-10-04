// thuum ADR-019: the JSON contract between the wire and the core. The Rust
// edge (skymp-wire, wire-json) renders every inbound message as SkyMP's JSON
// and recognizes the core's; its fixtures, two per message type, are what
// it renders. Here the core reads each fixture through its own reader and
// writes it back through its own writer: the result must be the fixture, so
// a field name, a type or an optional the two sides disagree on fails here
// as well as in `cargo test -p wire-json` (fixtures_round_trip).
#include "MessageSerializerFactory.h"
#include "MinPacketId.h"

#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("Every wire fixture reads and writes back unchanged",
          "[WireContract]")
{
  auto serializer = MessageSerializerFactory::CreateMessageSerializer();
  size_t checked = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(WIRE_FIXTURES_DIR)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::ifstream f(entry.path());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    std::string packet;
    packet.push_back(static_cast<char>(Networking::MinPacketId));
    packet += text;

    INFO(entry.path().filename().string());
    auto result = serializer->Deserialize(
      reinterpret_cast<const uint8_t*>(packet.data()), packet.size());
    REQUIRE(result.has_value());

    nlohmann::json written;
    result->message->WriteJson(written);
    REQUIRE(written == nlohmann::json::parse(text));
    ++checked;
  }
  REQUIRE(checked >= 70);
}
