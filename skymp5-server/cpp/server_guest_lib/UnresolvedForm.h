#pragma once
#include "FormDesc.h"
#include <string>
#include <tuple>

// A stored form whose plugin the running load order does not have (thuum
// docs/DECISIONS.md ADR-043 decision 1: kept, not dropped). In memory it
// resolves to nothing; the record keeps it here so the next save writes it
// back where it was and the plugin's return restores it. `field` is the
// record field it came from: raceId, headTextureSetId or headpartIds of the
// appearance, leftSpell, rightSpell, voiceSpell or instantSpell of the
// equipment, learnedSpells, inv or equipment.inv. An inventory entry is kept
// whole in `entry`, as written.
struct UnresolvedForm
{
  std::string field;
  FormDesc form;
  std::string entry;

  friend bool operator==(const UnresolvedForm& lhs, const UnresolvedForm& rhs)
  {
    return std::tie(lhs.field, lhs.form, lhs.entry) ==
      std::tie(rhs.field, rhs.form, rhs.entry);
  }

  friend bool operator!=(const UnresolvedForm& lhs, const UnresolvedForm& rhs)
  {
    return !(lhs == rhs);
  }

  friend bool operator<(const UnresolvedForm& lhs, const UnresolvedForm& rhs)
  {
    return std::tie(lhs.field, lhs.form, lhs.entry) <
      std::tie(rhs.field, rhs.form, rhs.entry);
  }
};
