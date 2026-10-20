#include "LoadOrderFile.h"
#include <stdexcept>

namespace LoadOrderFile {

nlohmann::json ToJson(const LoadOrder& order)
{
  auto files = nlohmann::json::array();
  for (const auto& [name, light] : order) {
    files.push_back({ { "name", name }, { "light", light } });
  }
  return nlohmann::json{ { "files", files } };
}

LoadOrder FromJson(const nlohmann::json& j)
{
  if (!j.is_object() || !j.contains("files") || !j.at("files").is_array()) {
    throw std::runtime_error(
      "load order: expected an object with a 'files' array");
  }
  LoadOrder order;
  for (const auto& file : j.at("files")) {
    if (!file.is_object() || !file.contains("name") ||
        !file.at("name").is_string()) {
      throw std::runtime_error("load order: every file needs a 'name'");
    }
    order.emplace_back(file.at("name").get<std::string>(),
                       file.value("light", false));
  }
  return order;
}

}
