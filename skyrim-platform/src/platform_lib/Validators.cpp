#include "Validators.h"

// A plugin's file name may hold spaces ("Apocalypse - Magic of Skyrim.esp",
// USSEP's; thuum docs/verbs/ghostwalk.md: skymp5-client's load-order check
// asks getFileInfo for every plugin, and the first spaced name threw here,
// 2026-10-14). A space traverses nothing, so a file name (allowDots, the
// getFileInfo and text callers) takes it; a Skyrim Platform plugin's name
// (the dev and console callers, no dots) stays as strict as before. A plus
// sign the same way ("DigitigradeKhajiit+ArgonianRaptor.esp", 2026-10-22:
// the refusal was the login's one C++ exception, and it unwound through
// V8's frames from the unwrapped getFileInfo; docs/verbs/npc-puppet.md,
// the landing hang).
bool ValidateFilename(std::string_view filename, bool allowDots)
{
  for (char c : filename) {
    if (!(('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') ||
          ('a' <= c && c <= 'z') || (c == '.' && allowDots) || c == '-' ||
          c == '_' || (c == ' ' && allowDots) || (c == '+' && allowDots))) {
      return false;
    }
  }
  return true;
}

bool ValidateRelativePath(std::string_view path)
{
  for (size_t i = 0; i < path.size(); ++i) {
    // Forbid everything including ':' and null character
    const char& c = path[i];
    if (!(('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') ||
          ('a' <= c && c <= 'z') || c == '.' || c == '-' || c == '_' ||
          c == '/' || c == '\\')) {
      return false;
    }
    if (i > 0 && path[i - 1] == '.' && path[i] == '.') {
      return false;
    }
  }
  return true;
}
