# difftest/tools

- fakeclient-stub.py: a stand-in for the fork's `fakeclient` binary
  (skymp5-server/cpp/fakeclient) that prints the same event lines without a
  network, so `cargo test -p difftest` exercises the legacy driver here. The
  real binary is built by the fork's server image; on sky-srv,
  `DIFFTEST_FAKECLIENT=/srv/skymp/fakeclient DIFFTEST_LEGACY_ADDR=127.0.0.1:7777`
  point the driver at it and at the legacy server.
- fakeclient-flood.py: a stand-in whose login prints 292 KB, more than a pipe
  holds, and whose profile 1 waits for profile 2 to have printed all of its
  own. difftest's drain test runs two of them through the driver, which must
  read every client's stdout while the others run (rung 5's logins printed
  about 91 KB each, 2026-11-10).
- pcap2session: turns a lab `lab.pcap` into a session YAML using Wireshark's
  RakNet dissector (`tshark -Y raknet -T json`), then maps legacy message
  bodies onto wire-schema names. Written in M0 once the first lab pcap exists;
  hand-written sessions cover rejection paths the corpus never hits.
