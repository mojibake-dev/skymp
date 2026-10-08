#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The server's plugins in load order, and per file whether the engine loads
// it as a light plugin (thuum docs/verbs/light-plugins.md): full and light
// plugins are numbered apart, a full one's forms at its index among full
// plugins << 24, a light one's at 0xFE000000 | its index among light plugins
// << 12. Without light flags every file is full, as before.
struct EspmFileList : public std::vector<std::string>
{
  using std::vector<std::string>::vector;
  using std::vector<std::string>::operator=;
  EspmFileList() = default;
  EspmFileList(std::vector<std::string> names, std::vector<bool> light_)
    : std::vector<std::string>(std::move(names))
    , light(std::move(light_))
  {
  }

  std::vector<bool> light;

  bool IsLight(size_t i) const noexcept
  {
    return i < light.size() && light[i];
  }
};

class FormDesc
{
public:
  FormDesc() = default;
  FormDesc(uint32_t shortFormId_, std::string file_)
    : shortFormId(shortFormId_)
    , file(file_)
  {
  }

  std::string ToString(char delimiter = ':') const;
  static FormDesc FromString(const std::string& str, char delimiter = ':');

  uint32_t ToFormId(const std::vector<std::string>& files) const;
  static FormDesc FromFormId(uint32_t formId,
                             const std::vector<std::string>& files);
  // The same, light plugins numbered as the engine numbers them
  uint32_t ToFormId(const EspmFileList& files) const;
  static FormDesc FromFormId(uint32_t formId, const EspmFileList& files);
  // The load order position of the file a form id belongs to, -1 for none
  static int FilePosition(uint32_t formId, const EspmFileList& files);

  friend bool operator==(const FormDesc& left, const FormDesc& right)
  {
    return std::make_tuple(left.shortFormId, left.file) ==
      std::make_tuple(right.shortFormId, right.file);
  }

  friend bool operator!=(const FormDesc& left, const FormDesc& right)
  {
    return !(left == right);
  }

  friend bool operator<(const FormDesc& left, const FormDesc& right)
  {
    return std::make_tuple(left.shortFormId, left.file) <
      std::make_tuple(right.shortFormId, right.file);
  }

  static FormDesc Tamriel();

  uint32_t shortFormId = 0;
  std::string file;
};
