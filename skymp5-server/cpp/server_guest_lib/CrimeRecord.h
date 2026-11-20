#pragma once
#include "FormDesc.h"
#include "Inventory.h"
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>

// thuum tracker #97, ADR-079 (Eli, 2026-11-20: "yes real jail time"): a
// player's standing with the law, kept per player as the engine keeps it.
// The engine holds crime gold per crime faction on the player itself, a
// violent and a non-violent sum (CommonLibSSE-NG
// include/RE/P/PlayerCharacter.h:169-178, the crimeGoldMap), so the bounty is
// per player and per hold already; this is its server record (D-G6 A, R0),
// given back at login. The jail term is the sentence the player's own game
// set at its arrest (ADR-063), its end a time on the shared clock (ADR-021):
// never skipped for one player. Stored as "crimeRecord"; a record without it
// reads as none (rule 6's migration).

// One hold's bounty on the player
struct CrimeGold
{
  FormDesc faction; // the hold's crime faction (a FACT with crime data)
  uint32_t violent = 0;
  uint32_t nonViolent = 0;

  auto ToTuple() const
  {
    return std::make_tuple(faction, violent, nonViolent);
  }

  friend bool operator==(const CrimeGold& lhs, const CrimeGold& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const CrimeGold& lhs, const CrimeGold& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};

// The sentence the player is serving
struct JailTerm
{
  FormDesc faction;     // the crime faction that jailed the player
  uint32_t days = 0;    // the sentence the engine set, in game days
  double releaseAt = 0; // the shared clock's daysPassed at which it ends
  Inventory belongings; // what the prisoner's game put into the faction's
                        // prisoner-belongings chest, given back at release
  bool arrived = false; // the prisoner stood in its jail's cell since the
                        // arrest; only then is leaving it an escape

  auto ToTuple() const
  {
    return std::make_tuple(faction, days, releaseAt, belongings.ToJson(),
                           arrived);
  }

  friend bool operator==(const JailTerm& lhs, const JailTerm& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const JailTerm& lhs, const JailTerm& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};

struct CrimeRecord
{
  std::vector<CrimeGold> crimeGold; // one entry per hold with a bounty
  std::optional<JailTerm> jail;     // none while the player is free

  bool Empty() const { return crimeGold.empty() && !jail.has_value(); }

  // the hold's bounty, or none
  const CrimeGold* Find(const FormDesc& faction) const
  {
    for (const auto& gold : crimeGold) {
      if (gold.faction == faction) {
        return &gold;
      }
    }
    return nullptr;
  }

  // sets the hold's bounty; a zero bounty drops the hold's entry
  void Set(const FormDesc& faction, uint32_t violent, uint32_t nonViolent)
  {
    for (auto it = crimeGold.begin(); it != crimeGold.end(); ++it) {
      if (it->faction == faction) {
        if (violent == 0 && nonViolent == 0) {
          crimeGold.erase(it);
        } else {
          it->violent = violent;
          it->nonViolent = nonViolent;
        }
        return;
      }
    }
    if (violent != 0 || nonViolent != 0) {
      crimeGold.push_back(CrimeGold{ faction, violent, nonViolent });
    }
  }

  auto ToTuple() const { return std::make_tuple(crimeGold, jail); }

  friend bool operator==(const CrimeRecord& lhs, const CrimeRecord& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const CrimeRecord& lhs, const CrimeRecord& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
