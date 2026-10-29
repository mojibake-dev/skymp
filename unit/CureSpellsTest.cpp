#include "MsgType.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum memo D56 A (Eli, 2026-10-28): the listed cure spells leave a
// player's game at its login by the engine's own RemoveSpell (Sanguinare
// Vampiris, Skyrim.esm SPEL 0xb8780, lab/esm.py), so nobody turns from a
// seat-side disease until the vampirism slice is built
namespace {
constexpr uint32_t kActor = 0xff000e40;
constexpr uint32_t kSanguinare = 0x000b8780;

size_t CuresSent(PartOne& p, Networking::UserId user, uint32_t spellId)
{
  size_t n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
        m.j["class"] == "Actor" && m.j["function"] == "RemoveSpell" &&
        m.j.contains("arguments") && !m.j["arguments"].empty() &&
        m.j["arguments"][0].contains("formId") &&
        m.j["arguments"][0]["formId"] == spellId) {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("The listed cure spells are removed from a player's game at its "
          "login by the engine's own RemoveSpell, and an empty list sends "
          "nothing",
          "[CureSpells][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCureSpellsSettings(R"(["b8780:Skyrim.esm"])");
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.Messages().clear();
  p.SetUserActor(0, kActor);
  p.Tick(); // the deferred snippets
  REQUIRE(CuresSent(p, 0, kSanguinare) == 1);
  DoDisconnect(p, 0);
  p.DestroyActor(kActor);

  p.SetCureSpellsSettings("[]");
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.Messages().clear();
  p.SetUserActor(0, kActor);
  p.Tick();
  REQUIRE(CuresSent(p, 0, kSanguinare) == 0);
  DoDisconnect(p, 0);
  p.DestroyActor(kActor);
}

TEST_CASE("A cure list that is not a list of strings stops the server, named",
          "[CureSpells]")
{
  PartOne& p = GetPartOne();
  REQUIRE_THROWS(p.SetCureSpellsSettings(R"({"spell": 1})"));
  REQUIRE_THROWS(p.SetCureSpellsSettings(R"([1])"));
  p.SetCureSpellsSettings("[]");
}
