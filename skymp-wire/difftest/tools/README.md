# difftest/tools

- pcap2session: turns a lab `lab.pcap` into a session YAML using Wireshark's
  RakNet dissector (`tshark -Y raknet -T json`), then maps legacy message
  bodies onto wire-schema names. Written in M0. Every lab run adds to the
  corpus; hand-written sessions cover rejection paths the corpus never hits.
