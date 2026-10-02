# wire-legacy fixtures

`smoke-233455.hex`: SkyMP packets from the green smoke-two-players run
20261001-233455 (lab capture `lab.pcap`, two clients, 3 minutes), one per
line as `<c2s|s2c> <hex>`, every distinct packet of the rarer types and a
sample of the frequent ones. They hold form ids, positions, the lab
characters' looks and inventories: nothing licensed.

Extraction: RakNet datagrams parsed from `tshark -T fields -e udp.payload`
(reliability header, ordering, split reassembly; the pcap holds both sides of
docker's NAT, so only packets to or from 10.10.70.10:7777 count), keeping the
user messages that start with 0x86.
