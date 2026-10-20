#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// The load order a world was saved under, kept beside its records (thuum
// docs/DECISIONS.md ADR-043 decision 2): {"files": [{"name", "light"}, ...]}
// in load order. The file driver keeps it as load-order.json in the world
// directory next to changeForms/, the zip driver as the entry of that name,
// the mongo driver as the one document of its loadOrder collection.
namespace LoadOrderFile {

using LoadOrder = std::vector<std::pair<std::string, bool>>;

inline constexpr const char* kName = "load-order.json";

nlohmann::json ToJson(const LoadOrder& order);

// Throws on anything that is not an order: a corrupt file must not pass as
// "none saved", or raw-shape records would be re-based from the wrong order
LoadOrder FromJson(const nlohmann::json& j);

}
