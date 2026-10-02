#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <simdjson.h>

#include "UpdateMovementMessage.h"

#include <fmt/ranges.h>

namespace {

UpdateMovementMessage MakeTestMovementMessage(std::string runMode,
                                              bool hasLookAt)
{
  UpdateMovementMessage result;
  result.idx = 1337;
  result.data.worldOrCell = 0x2077;
  result.data.pos = { 0.25, -100, 0 };
  result.data.rot = { 123, 0, 45 };
  result.data.direction = 270;
  result.data.healthPercentage = 0.5;
  result.data.speed = 400;
  result.data.runMode = "Running";
  result.data.isInJumpState = true;
  result.data.isSneaking = true;
  result.data.isBlocking = false;
  result.data.isWeapDrawn = true;
  result.data.isDead = false;
  result.data.lookAt = { { 1, 2, 3 } };

  result.data.runMode = runMode;
  if (!hasLookAt) {
    result.data.lookAt = std::nullopt;
  }
  return result;
}

std::unordered_map<std::string, UpdateMovementMessage>
MakeTestMovementMessageCases()
{
  std::unordered_map<std::string, UpdateMovementMessage> result;
  for (std::string runMode :
       { "Running", "Sprinting", "Standing", "Walking" }) {
    result.emplace(fmt::format("{},{}", runMode, false),
                   MakeTestMovementMessage(runMode, false));
    result.emplace(fmt::format("{},{}", runMode, true),
                   MakeTestMovementMessage(runMode, true));
  }
  return result;
}

}

TEST_CASE("MovementMessage correctly encoded and decoded to JSON",
          "[Serialization]")
{
  for (const auto& [name, movData] : MakeTestMovementMessageCases()) {
    SECTION(name)
    {
      nlohmann::json json;
      movData.WriteJson(json);

      simdjson::dom::parser sjParser;
      auto sjResult = sjParser.parse(nlohmann::to_string(json));
      UpdateMovementMessage movData2;
      movData2.ReadJson(sjResult.value());

      nlohmann::json json2;
      movData2.WriteJson(json2);

      CAPTURE(json.dump());
      CAPTURE(json2.dump());
      REQUIRE(json == json2);
      REQUIRE(json["t"].get<int>() ==
              static_cast<int>(UpdateMovementMessage::kMsgType.value));
      REQUIRE(json2["t"].get<int>() ==
              static_cast<int>(UpdateMovementMessage::kMsgType.value));
    }
  }
}
