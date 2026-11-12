#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// thuum ADR-068 rung 3 (the script record; docs/research/
// rung3-script-record-2026-11-06.md section 1(b)): one attached script's
// state as the owner's engine holds it, the shape ScriptState (MsgType 76)
// carries from the owner, ScriptStateApply (MsgType 77) carries to a seat,
// and the server keeps on the reference's record (ScriptStateRecord.h): the
// payload the two messages share, in its own header as the client's
// scriptStateTypes.ts is (linter-config.json's message pairs). The contract
// is skymp-wire's (wire-schema skymp.rs: ScriptHandle, ScriptValue,
// ScriptVar, ScriptObjectState; wire-validate check_script_state holds the
// kinds, the caps and every string's form: no NUL anywhere, since the
// receiver's BSFixedString reads a C string); a u8 or a u16 on the wire is a
// uint32_t here, the messages' idiom (ChangeFormRemapRow's). The server stores
// the values and never reads them: the owner's engine wrote them and a
// receiver's applies them (ApplyScriptState, the platform's).

// An object value's handle as the owner's read splits the VM's 64-bit
// handle (thuum docs/research/adr065-papyrus-state-2026-11-03.md Q1: a
// (kind, mid, form id) triple; a plain form's is kind 0, mid 0xFFFF), and the
// script type bound there, which a receiver's FindBoundObject takes
struct ScriptStateHandle
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("kind", kind)
      .Serialize("mid", mid)
      .Serialize("form", form)
      .Serialize("type", type);
  }
  auto ToTuple() const { return std::make_tuple(kind, mid, form, type); }
  friend bool operator==(const ScriptStateHandle& lhs,
                         const ScriptStateHandle& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ScriptStateHandle& lhs,
                        const ScriptStateHandle& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  uint32_t kind = 0; // 0, 1 or 2 (a u8 on the wire); 1 and 2 read as None
  uint32_t mid = 0;  // the handle's mid (a u16 on the wire)
  uint32_t form = 0; // the form id, never 0x14 nor 0xff000000 and up
  std::string type;  // the script type bound at the handle, never empty
};

// One value as the VM's Variable holds it: `k` its folded raw type (CL
// include/RE/T/TypeInfo.h: 0 None, 1 object, 2 string, 3 int, 4 float, 5
// bool) and the one field that kind carries (an object with no handle is a
// typed None)
struct ScriptStateValue
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("k", k)
      .Serialize("i", i)
      .Serialize("f", f)
      .Serialize("b", b)
      .Serialize("s", s)
      .Serialize("o", o);
  }
  auto ToTuple() const { return std::make_tuple(k, i, f, b, s, o); }
  friend bool operator==(const ScriptStateValue& lhs,
                         const ScriptStateValue& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ScriptStateValue& lhs,
                        const ScriptStateValue& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  uint32_t k = 0; // the raw type, 0 to 5 (a u8 on the wire)
  std::optional<int32_t> i;
  std::optional<float> f;
  std::optional<bool> b;
  std::optional<std::string> s;
  std::optional<ScriptStateHandle> o;
};

// One variable of a script, positional and parent-first: a value as
// ScriptStateValue holds one, or (`k` 11 to 15, the typed array raw types) an
// array of values of kind k - 10 in `a`, `elem` naming an object array's
// element type (k 11 alone); no `a` is a None array, and k 10, the None
// array's own kind, carries none
struct ScriptStateVar
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("k", k)
      .Serialize("i", i)
      .Serialize("f", f)
      .Serialize("b", b)
      .Serialize("s", s)
      .Serialize("o", o)
      .Serialize("elem", elem)
      .Serialize("a", a);
  }
  auto ToTuple() const { return std::make_tuple(k, i, f, b, s, o, elem, a); }
  friend bool operator==(const ScriptStateVar& lhs, const ScriptStateVar& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ScriptStateVar& lhs, const ScriptStateVar& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  uint32_t k = 0; // 0 to 5, or 10 to 15 for an array (a u8 on the wire)
  std::optional<int32_t> i;
  std::optional<float> f;
  std::optional<bool> b;
  std::optional<std::string> s;
  std::optional<ScriptStateHandle> o;
  std::optional<std::string> elem; // an object array's element type
  std::optional<std::vector<ScriptStateValue>> a; // at most 128 elements
};

// One attached script of the reference: its type (the loader's exact match on
// the receiver), its current state, Object::Save's flags byte (0x01
// constructed, 0x02 valid, 0x04 lock held, 0x08 initialized) and every
// variable the type's chain declares, in order
struct ScriptStateObject
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("type", type)
      .Serialize("state", state)
      .Serialize("flags", flags)
      .Serialize("vars", vars);
  }
  auto ToTuple() const { return std::make_tuple(type, state, flags, vars); }
  friend bool operator==(const ScriptStateObject& lhs,
                         const ScriptStateObject& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ScriptStateObject& lhs,
                        const ScriptStateObject& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  std::string type;                 // the script type, never empty
  std::string state;                // its current state, "" the empty state
  uint32_t flags = 0;               // Object::Save's byte (a u8 on the wire)
  std::vector<ScriptStateVar> vars; // at most 512
};

// thuum D-C3 A (PENDING Eli; docs/research/fifty-fixes-2026-11-07.md 1(a)):
// one behavior graph variable of a world object as its owner's script last
// set it (ObjectReference.SetAnimationVariableBool, Int or Float, kept on the
// reference with the call's sequence), served in ScriptStateApply beside the
// record's view: `k` takes ScriptStateValue's folded kinds, 3 int, 4 float,
// 5 bool, and the one field that kind carries; `seq` is the object's sequence
// at the call, so a receiver applies a variable above the one it holds
// (wire-schema skymp.rs, GraphVar; wire-validate refuses an empty name, a
// NUL, a name twice, a kind outside 3 to 5, a payload not its kind's and a
// seq of 0)
struct ScriptStateGraphVar
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("name", name)
      .Serialize("k", k)
      .Serialize("i", i)
      .Serialize("f", f)
      .Serialize("b", b)
      .Serialize("seq", seq);
  }
  auto ToTuple() const { return std::make_tuple(name, k, i, f, b, seq); }
  friend bool operator==(const ScriptStateGraphVar& lhs,
                         const ScriptStateGraphVar& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  friend bool operator<(const ScriptStateGraphVar& lhs,
                        const ScriptStateGraphVar& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
  std::string name; // the graph variable, never empty, at most 128 bytes
  uint32_t k = 0;   // 3 int, 4 float, 5 bool (a u8 on the wire)
  std::optional<int32_t> i;
  std::optional<float> f;
  std::optional<bool> b;
  uint32_t seq = 0; // the object's sequence at the call, never 0
};
