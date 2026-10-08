#include "FormDesc.h"
#include <cstdio>
#include <stdexcept>
#include <string>

std::string FormDesc::ToString(char delimiter) const
{
  auto fullFmt = "%0x%c%s";
  auto idFmt = "%0x";
  size_t size = !file.empty()
    ? std::snprintf(nullptr, 0, fullFmt, shortFormId, delimiter, file.c_str())
    : std::snprintf(nullptr, 0, idFmt, shortFormId);

  std::string buffer;
  buffer.resize(size + 1);

  if (!file.empty()) {
    std::sprintf(buffer.data(), fullFmt, shortFormId, delimiter, file.c_str());
  } else {
    std::sprintf(buffer.data(), idFmt, shortFormId);
  }
  buffer.resize(size); // remove extra null terminator
  return buffer;
}

FormDesc FormDesc::FromString(const std::string& str, char delimiter)
{
  FormDesc res;
  std::string id, file;

  if (str.find(delimiter) == std::string::npos) {
    std::sscanf(str.data(), "%x", &res.shortFormId);
    return res;
  }

  for (auto it = str.begin(); it != str.end(); ++it) {
    if (*it == delimiter) {
      id = { str.begin(), it };
      res.file = { it + 1, str.end() };
      break;
    }
  }

  std::sscanf(id.data(), "%x", &res.shortFormId);
  return res;
}

uint32_t FormDesc::ToFormId(const std::vector<std::string>& files) const
{
  // Workaround legacy tests throwing exceptions (drop support for PartOne
  // instances without espm to remove this)
  static const std::string kSkyrimEsm = "Skyrim.esm";
  if (shortFormId == 0x3c && file == kSkyrimEsm) {
    return 0x3c;
  }

  uint32_t realFormId;
  if (file.empty()) {
    realFormId = 0xff000000 + shortFormId;
  } else {
    int fileIdx = -1;
    int numFiles = static_cast<int>(files.size());
    for (int i = 0; i < numFiles; ++i) {
      if (files[i] == file) {
        fileIdx = i;
        break;
      }
    }
    if (fileIdx == -1) {
      throw std::runtime_error(file + " not found in loaded files");
    }

    realFormId = fileIdx * 0x01000000 + shortFormId;
  }
  return realFormId;
}

FormDesc FormDesc::FromFormId(uint32_t formId,
                              const std::vector<std::string>& files)
{
  // Workaround legacy tests throwing exceptions (drop support for PartOne
  // instances without espm to remove this)
  if (formId == 0x3c) {
    return FormDesc::Tamriel();
  }

  FormDesc res;
  if (formId < 0xff000000) {
    int fileIdx = formId / 0x01000000;
    if (fileIdx >= static_cast<int>(files.size())) {
      throw std::runtime_error("FromFormId failed due to invalid file index " +
                               std::to_string(fileIdx));
    }
    res.file = files[fileIdx];
    res.shortFormId = formId % 0x01000000;
  } else {
    res.shortFormId = formId - 0xff000000;
  }
  return res;
}

int FormDesc::FilePosition(uint32_t formId, const EspmFileList& files)
{
  if (formId >= 0xff000000) {
    return -1;
  }
  const uint32_t top = formId >> 24;
  const bool wantLight = top == 0xfe && !files.light.empty();
  const uint32_t wanted = wantLight ? (formId >> 12) & 0x0fff : top;
  uint32_t count = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files.IsLight(i) != wantLight) {
      continue;
    }
    if (count++ == wanted) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

FormDesc FormDesc::FromFormId(uint32_t formId, const EspmFileList& files)
{
  // Workaround legacy tests throwing exceptions (drop support for PartOne
  // instances without espm to remove this)
  if (formId == 0x3c) {
    return FormDesc::Tamriel();
  }

  FormDesc res;
  if (formId >= 0xff000000) {
    res.shortFormId = formId - 0xff000000;
    return res;
  }
  const int position = FilePosition(formId, files);
  if (position < 0) {
    throw std::runtime_error("FromFormId failed: no loaded file holds form " +
                             std::to_string(formId));
  }
  res.file = files[position];
  res.shortFormId =
    files.IsLight(position) ? (formId & 0x00000fff) : (formId & 0x00ffffff);
  return res;
}

uint32_t FormDesc::ToFormId(const EspmFileList& files) const
{
  // Workaround legacy tests throwing exceptions (drop support for PartOne
  // instances without espm to remove this)
  static const std::string kSkyrimEsm = "Skyrim.esm";
  if (shortFormId == 0x3c && file == kSkyrimEsm) {
    return 0x3c;
  }
  if (file.empty()) {
    return 0xff000000 + shortFormId;
  }
  uint32_t numFull = 0;
  uint32_t numLight = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const bool light = files.IsLight(i);
    if (files[i] == file) {
      return light ? (0xfe000000 | (numLight << 12) | (shortFormId & 0x0fff))
                   : (numFull * 0x01000000 + shortFormId);
    }
    if (light) {
      ++numLight;
    } else {
      ++numFull;
    }
  }
  throw std::runtime_error(file + " not found in loaded files");
}

static const FormDesc kTamriel = FormDesc::FromString("3c:Skyrim.esm");

FormDesc FormDesc::Tamriel()
{
  return kTamriel;
}
