#pragma once
#include "NetworkingInterface.h"

#include <memory>
#include <string>

namespace prometheus {
class Registry;
}

// The server's network edge on the wire (thuum ADR-019): an IServer whose
// packets are SkyMP JSON that Rust has already recognized and validated,
// handed to the core as `0x86 + JSON`, the input MessageSerializer's JSON
// path reads for all 33 message types. The core's sends are the same shape
// and go back to Rust, which recognizes, checks and encodes them. No byte a
// client sent reaches C++.
// The mod lock (thuum docs/verbs/mod-lock.md): the wire judges each
// client's Hello against the manifest the TypeScript layer writes from the
// load order (manifest.json in the data dir). `enabled` false welcomes every
// Hello and logs the verdict; `requireHello` refuses a login from a
// connection that sent no Hello (false until every client sends one).
struct ModLockSettings
{
  std::string manifestPath;
  bool enabled = true;
  bool requireHello = false;
};

std::shared_ptr<Networking::IServer> CreateWireServer(
  const char* listenAddress, unsigned short port,
  unsigned short maxConnections, const char* password,
  std::shared_ptr<prometheus::Registry> promRegistry,
  const ModLockSettings& modLock);
