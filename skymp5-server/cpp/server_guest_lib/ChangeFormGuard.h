#pragma once
#include "MpChangeForms.h"
#include <chrono>
#include <optional>

class MpObjectReference;

namespace ChangeFormGuard_ {
void RequestSave(MpObjectReference* self);
}

class ChangeFormGuard
{
public:
  ChangeFormGuard(const MpChangeForm& changeForm_, MpObjectReference* self_)
    : changeForm(changeForm_)
    , self(self_)
  {
  }

  enum class Mode
  {
    RequestSave,
    NoRequestSave,
    // thuum docs/verbs/cell-host.md: an edit the saving policy deferred (a
    // movement sample inside IsLocationSavingNeeded's 30 s, MakeMode): no
    // save request now, the record marked unsaved so a stop flushes it.
    // NoRequestSave alone (a load's apply, an attach's Init, a respawn's
    // bookkeeping) neither requests a save nor marks the record, so a stop
    // writes no record for a reference the session only loaded (playtest
    // 49: a restart wrote 13,851 flora records, every NoRequestSave edit
    // having been marked since 6f39d9a7)
    Deferred
  };

  void EditChangeForm(std::function<void(MpChangeForm&)> f,
                      Mode mode = Mode::RequestSave)
  {
    f(changeForm);
    if (!blockSaving && mode == Mode::RequestSave) {
      lastSaveRequest = std::chrono::system_clock::now();
      unsavedEdit = false;
      ChangeFormGuard_::RequestSave(self);
    } else if (!blockSaving && mode == Mode::Deferred) {
      // thuum docs/verbs/cell-host.md: an edit the saving policy skipped (a
      // movement sample inside IsLocationSavingNeeded's 30 s) stays in the
      // record alone until the next requested save; a stop flushes it
      // (WorldState::FlushSaveStorage), else a resident walking at a restart
      // comes back where the save last caught her (c-riverwood 2026-11-01)
      unsavedEdit = true;
    }
  }

  const MpChangeForm& ChangeForm() const noexcept { return changeForm; }

  auto GetLastSaveRequestMoment() const { return lastSaveRequest; }

  // whether the record holds an edit no save request carried yet
  bool HasUnsavedEdit() const noexcept { return unsavedEdit; }

  bool blockSaving = false;

private:
  MpChangeForm changeForm;
  MpObjectReference* const self;
  std::optional<std::chrono::system_clock::time_point> lastSaveRequest;
  bool unsavedEdit = false;
};
