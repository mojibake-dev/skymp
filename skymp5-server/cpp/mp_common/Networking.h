#pragma once
#include "Config.h"
#include "NetworkingInterface.h"

// The network edge is skymp-wire (Rust, thuum ADR-019): the server's
// IServer is addon/WireServer.cpp, the client's is MpClientPlugin.dll, built
// from skymp-wire/crates/wire-client-ffi. What remains here in C++ is the
// interface (NetworkingInterface.h), the in-process mock and the combiner.
