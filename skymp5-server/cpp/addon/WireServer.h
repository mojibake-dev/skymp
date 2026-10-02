#pragma once
#include "NetworkingInterface.h"

#include <memory>

namespace prometheus {
class Registry;
}

// The server's network edge on the wire (thuum ADR-019): an IServer whose
// packets are SkyMP JSON that Rust has already recognized and validated,
// handed to the core as `0x86 + JSON`, the input MessageSerializer's JSON
// path reads for all 33 message types. The core's sends are the same shape
// and go back to Rust, which recognizes, checks and encodes them. No byte a
// client sent reaches C++.
std::shared_ptr<Networking::IServer> CreateWireServer(
  const char* listenAddress, unsigned short port,
  unsigned short maxConnections, const char* password,
  std::shared_ptr<prometheus::Registry> promRegistry);
