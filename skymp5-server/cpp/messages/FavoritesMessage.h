#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// A player's favorites, the whole list (thuum docs/verbs/favorites.md). Client
// to server after the inventory, magic or favorites menu closed and the list
// changed; server to client after a login, for the client's engine to mark.
struct FavoritesMessage : public MessageBase<FavoritesMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::Favorites)>{};

  struct Entry
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("form", form).Serialize("hotkey", hotkey);
    }

    friend bool operator==(const Entry& lhs, const Entry& rhs)
    {
      return lhs.form == rhs.form && lhs.hotkey == rhs.hotkey;
    }

    uint32_t form = 0;  // the form id, as the sender knows it
    int8_t hotkey = -1; // -1 for none, 0 to 7 for the keys 1 to 8
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("entries", entries);
  }

  std::vector<Entry> entries;
};
