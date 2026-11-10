#include "libespm/RecordHeader.h"
#include "libespm/RecordHeaderAccess.h"
#include "libespm/ScriptData.h"
#include "libespm/Utils.h"
#include <cstring>

namespace espm {

namespace {
// A VMAD's bytes read with every read bounded by the field's own end, so a
// tail that is not the layout below stops the read and never reads past the
// field (a game file's or a mod's QUST that writes something else there).
// Every value lands in a local first and is assigned from there: the records
// are packed (pragma pack 1), so nothing binds a reference to their fields
class VmadReader
{
public:
  VmadReader(const uint8_t* begin, const uint8_t* stop)
    : p(begin)
    , end(stop)
  {
  }

  bool AtEnd() const { return p == end; }

  // the next `n` bytes, the reader past them; nullptr when fewer are left
  const uint8_t* Take(size_t n)
  {
    if (static_cast<size_t>(end - p) < n) {
      return nullptr;
    }
    const uint8_t* at = p;
    p += n;
    return at;
  }

  template <class T>
  bool Read(T& out)
  {
    const uint8_t* at = Take(sizeof(T));
    if (!at) {
      return false;
    }
    std::memcpy(&out, at, sizeof(T));
    return true;
  }

  // a wstring: a uint16 length, then that many bytes
  const uint8_t* TakeString(uint16_t& length)
  {
    length = 0;
    return Read(length) ? Take(length) : nullptr;
  }

  bool ReadString(std::string& out)
  {
    uint16_t length = 0;
    const uint8_t* at = TakeString(length);
    if (!at) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(at), length);
    return true;
  }

  bool SkipString()
  {
    uint16_t length = 0;
    return TakeString(length) != nullptr;
  }

private:
  const uint8_t* p;
  const uint8_t* end;
};

// one property value of `type` as utils::ReadPropertyValue reads it (an
// object's form id at the place objFormat gives it: 1 its first four bytes,
// 2 its last four), every read bounded; false for a type the format lists
// none of
bool ReadValue(VmadReader& r, Property::Type type, int16_t objFormat,
               Property::Value& out)
{
  switch (type) {
    case Property::Type::Object: {
      uint32_t first = 0;
      uint32_t second = 0;
      if (!r.Read(first) || !r.Read(second) ||
          (objFormat != 1 && objFormat != 2)) {
        return false;
      }
      out.formId = objFormat == 1 ? first : second;
      return true;
    }
    case Property::Type::String: {
      uint16_t length = 0;
      const uint8_t* at = r.TakeString(length);
      if (!at) {
        return false;
      }
      out.str = { reinterpret_cast<const char*>(at), length };
      return true;
    }
    case Property::Type::Int: {
      int32_t integer = 0;
      if (!r.Read(integer)) {
        return false;
      }
      out.integer = integer;
      return true;
    }
    case Property::Type::Float: {
      float floatingPoint = 0;
      if (!r.Read(floatingPoint)) {
        return false;
      }
      out.floatingPoint = floatingPoint;
      return true;
    }
    case Property::Type::Bool: {
      int8_t boolean = 0;
      if (!r.Read(boolean)) {
        return false;
      }
      out.boolean = boolean;
      return true;
    }
    default:
      return false;
  }
}

// a script array as utils::FillScriptArray fills one (a uint16 count, then
// each script's name, its status byte from version 4 on, its properties:
// name, type, status byte from version 4 on, value; an array a uint32 count
// of its element type's values), every read bounded
bool ReadScripts(VmadReader& r, int16_t version, int16_t objFormat,
                 std::vector<Script>& out)
{
  uint16_t count = 0;
  if (!r.Read(count)) {
    return false;
  }
  for (uint16_t i = 0; i < count; ++i) {
    Script script;
    std::string scriptName;
    uint8_t scriptStatus = 0;
    uint16_t properties = 0;
    if (!r.ReadString(scriptName) || (version >= 4 && !r.Read(scriptStatus)) ||
        !r.Read(properties)) {
      return false;
    }
    script.scriptName = std::move(scriptName);
    script.status = scriptStatus;
    for (uint16_t j = 0; j < properties; ++j) {
      std::string name;
      uint8_t type = 0;
      uint8_t status = static_cast<std::underlying_type_t<Property::Status>>(
        Property::Status::Edited);
      if (!r.ReadString(name) || !r.Read(type) ||
          (version >= 4 && !r.Read(status))) {
        return false;
      }
      Property prop;
      prop.name = std::move(name);
      prop.type = static_cast<Property::Type>(type);
      prop.status = status;
      if (type >= 11 && type <= 15) {
        uint32_t length = 0;
        if (!r.Read(length)) {
          return false;
        }
        const auto element = utils::GetElementType(prop.type);
        for (uint32_t k = 0; k < length; ++k) {
          Property::Value value;
          if (!ReadValue(r, element, objFormat, value)) {
            return false;
          }
          prop.array.push_back(value);
        }
      } else {
        Property::Value value;
        if (!ReadValue(r, prop.type, objFormat, value)) {
          return false;
        }
        prop.value = value;
      }
      script.properties.insert(prop);
    }
    out.push_back(std::move(script));
  }
  return true;
}

// thuum ADR-068 rung 5: a QUST's alias scripts (UESP, "Skyrim Mod:Mod File
// Format/VMAD Field"; read whole across the 1,255 scripted QUSTs of
// Skyrim.esm, Dawnguard.esm and Dragonborn.esm, eight of them in
// RotfernEyes.esp's shape: no quest script, no fragment, alias scripts
// alone). After the primary scripts a QUST's VMAD holds its Fragments
// Section (an int8, a uint16 fragment count, the fragment script's file
// name, then per fragment a uint16 stage index, an int16, an int32, an
// int8, the script's name and the function's) and then its Alias Scripts
// Section (a uint16 count, then per alias its object, eight bytes in
// objFormat's layout naming the quest and the alias id, an int16 version,
// an int16 objFormat and its scripts). A VMAD that ends after its primary
// scripts has neither section
std::vector<ScriptData::AliasScripts> ReadQuestAliasScripts(
  const uint8_t* data, uint32_t dataSize)
{
  std::vector<ScriptData::AliasScripts> out;
  VmadReader r(data, data + dataSize);
  int16_t version = 0;
  int16_t objFormat = 0;
  std::vector<Script> primary;
  if (!r.Read(version) || !r.Read(objFormat) || version < 4 ||
      !ReadScripts(r, version, objFormat, primary) || r.AtEnd()) {
    return out;
  }
  uint8_t unknown = 0;
  uint16_t fragments = 0;
  if (!r.Read(unknown) || !r.Read(fragments) || !r.SkipString()) {
    return out;
  }
  for (uint16_t i = 0; i < fragments; ++i) {
    if (!r.Take(2 + 2 + 4 + 1) || !r.SkipString() || !r.SkipString()) {
      return out;
    }
  }
  uint16_t aliases = 0;
  if (!r.Read(aliases)) {
    return out;
  }
  for (uint16_t i = 0; i < aliases; ++i) {
    uint32_t first = 0;
    uint32_t second = 0;
    int16_t aliasVersion = 0;
    int16_t aliasFormat = 0;
    std::vector<Script> scripts;
    if (!r.Read(first) || !r.Read(second) || !r.Read(aliasVersion) ||
        !r.Read(aliasFormat) || aliasVersion < 4 ||
        !ReadScripts(r, aliasVersion, aliasFormat, scripts)) {
      return out;
    }
    ScriptData::AliasScripts alias;
    // the object's alias id: objFormat 1 keeps it in the second word's low
    // half, 2 in the first word's high half
    alias.aliasId =
      static_cast<int16_t>(objFormat == 1 ? (second & 0xffff) : (first >> 16));
    alias.scripts = std::move(scripts);
    out.push_back(std::move(alias));
  }
  return out;
}
}

const Type RecordHeader::GetType() const noexcept
{
  return Type{ reinterpret_cast<const char*>(this) - 8 };
}

uint32_t RecordHeader::GetFlags() const noexcept
{
  return flags;
}

uint32_t RecordHeader::GetId() const noexcept
{
  return id;
}

uint16_t RecordHeader::GetVersion() const noexcept
{
  return version;
}

const char* RecordHeader::GetEditorId(
  espm::CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  const char* result = "";
  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "EDID", 4))
        result = data;
    },
    compressedFieldsCache);
  return result;
}

void RecordHeader::GetScriptData(
  ScriptData* out, CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  ScriptData res;

  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "VMAD", 4)) {
        res.version = *reinterpret_cast<const uint16_t*>(data);
        res.objFormat = *reinterpret_cast<const uint16_t*>(
          (reinterpret_cast<const uint8_t*>(data) + 2));
        const uint16_t scriptCount = *reinterpret_cast<const uint16_t*>(
          (reinterpret_cast<const uint8_t*>(data) + 4));

        auto p = reinterpret_cast<const uint8_t*>(data) + 6;
        res.scripts.resize(scriptCount);
        utils::FillScriptArray(p, res.scripts, res.objFormat);
        // thuum ADR-068 rung 5: a quest's alias scripts, apart from its
        // own (a reader of `scripts` sees what it saw before)
        if (GetType() == "QUST") {
          res.aliases = ReadQuestAliasScripts(
            reinterpret_cast<const uint8_t*>(data), dataSize);
        }
      }
    },
    compressedFieldsCache);

  *out = res;
}

std::vector<uint32_t> RecordHeader::GetKeywordIds(
  CompressedFieldsCache& compressedFieldsCache) const noexcept
{
  std::vector<uint32_t> res;
  uint32_t count = 0;

  RecordHeaderAccess::IterateFields(
    this,
    [&](const char* type, uint32_t dataSize, const char* data) {
      if (!std::memcmp(type, "KSIZ", 4)) {
        count = *reinterpret_cast<const uint32_t*>(data);
      }
      if (!std::memcmp(type, "KWDA", 4)) {
        for (uint32_t i = 0; i < count; ++i)
          res.push_back(reinterpret_cast<const uint32_t*>(data)[i]);
      }
    },
    compressedFieldsCache);

  return res;
}

uint32_t RecordHeader::GetFieldsSizeSum() const noexcept
{
  const auto ptr = (reinterpret_cast<const int8_t*>(this)) - 4;
  return *reinterpret_cast<const uint32_t*>(ptr);
}

}
