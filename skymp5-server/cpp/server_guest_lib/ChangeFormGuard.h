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
    NoRequestSave
  };

  void EditChangeForm(std::function<void(MpChangeForm&)> f,
                      Mode mode = Mode::RequestSave)
  {
    f(changeForm);
    if (!blockSaving && mode == Mode::RequestSave) {
      lastSaveRequest = std::chrono::system_clock::now();
      unsavedEdit = false;
      ChangeFormGuard_::RequestSave(self);
    } else if (!blockSaving) {
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
