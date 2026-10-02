#pragma once

constexpr auto kMaxPlayers = MAX_PLAYERS;

// The messaging protocol's version gate is netcode's protocol id, derived
// from skymp-wire's SCHEMA_VERSION (thuum ADR-019); RakNet's password prefix
// ("7_") went with RakNet.
