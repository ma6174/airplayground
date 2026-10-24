#!/usr/bin/env python3
"""Let the Mac see the phone -> projector AirPlay traffic, then capture it.

Runs ARP spoofing in both directions so the projector's peer (the phone) sends
its packets via this Mac, enables IP forwarding, and prints the tcpdump command
to use in a second terminal. Ctrl-C restores the real ARP entries.

Usage: sudo python3 mitm_capture.py <phone-ip> <projector-ip> [interface]

Everything here is a diagnostic aid: after Ctrl-C the network is back to normal.
"""

import fcntl
import signal
import socket
import struct
import subprocess
import sys
import time

ETH_P_ARP = 0x0806
ETH_P_IP = 0x0800
ARP_REQUEST = 1
ARP_REPLY = 2


def mac_bytes(text):
    return bytes(int(part, 16) for part in text.split(":"))


def get_iface_mac(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        info = fcntl.ioctl(sock.fileno(), 0x8927, struct.pack("256s", ifname[:15].encode()))
    return info[18:24]


def get_iface_ip(ifname):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        info = fcntl.ioctl(sock.fileno(), 0x8915, struct.pack("256s", ifname[:15].encode()))
    return socket.inet_ntoa(info[20:24])


def send_arp(ifname, src_mac, dst_mac, sender_ip, sender_mac, target_ip, target_mac, op):
    frame = struct.pack("!6s6sH", dst_mac, src_mac, ETH_P_ARP)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        1,             # ethernet
        ETH_P_IP,      # IPv4
        6,             # hardware length
        4,             # protocol length
        op,
        sender_mac,
        socket.inet_aton(sender_ip),
        target_mac,
        socket.inet_aton(target_ip),
    )
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW) as sock:
        sock.bind((ifname, 0))
        sock.send(frame + arp)


def real_mac(ip):
    out = subprocess.run(["arp", "-n", ip], capture_output=True, text=True).stdout
    for token in out.split():
        if token.count(":") == 5 and len(token) == 17:
            return token
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    phone_ip, target_ip = sys.argv[1], sys.argv[2]
    ifname = sys.argv[3] if len(sys.argv) > 3 else "en0"

    my_ip = get_iface_ip(ifname)
    my_mac = get_iface_mac(ifname)
    phone_mac = mac_bytes(real_mac(phone_ip))
    target_mac = mac_bytes(real_mac(target_ip))

    print(f"interface {ifname}  my ip {my_ip}  my mac {my_mac.hex(':')}")
    print(f"phone {phone_ip} {phone_mac.hex(':')}   projector {target_ip} {target_mac.hex(':')}")
    print("\nnow start the capture in another terminal:")
    print(f"  sudo tcpdump -i {ifname} -s 0 -w ~/airplay-phone.pcap host {target_ip}\n")
    print("keep this running while the phone mirrors, then Ctrl-C here.\n")

    subprocess.run(["sysctl", "-w", "net.inet.ip.forwarding=1"], check=False, capture_output=True)

    def restore(*_):
        for _ in range(3):
            # Hand the real MACs back to both parties.
            send_arp(ifname, target_mac, phone_mac, target_ip, target_mac, phone_ip, phone_mac, ARP_REPLY)
            send_arp(ifname, phone_mac, target_mac, phone_ip, phone_mac, target_ip, target_mac, ARP_REPLY)
            time.sleep(0.3)
        print("\nARP restored")
        sys.exit(0)

    signal.signal(signal.SIGINT, restore)
    signal.signal(signal.SIGTERM, restore)

    while True:
        # Tell each side that the other's address lives at this Mac.
        send_arp(ifname, target_mac, phone_mac, target_ip, target_mac, phone_ip, my_mac, ARP_REPLY)
        send_arp(ifname, my_mac, target_mac, phone_ip, my_mac, target_ip, target_mac, ARP_REPLY)
        time.sleep(1)


if __name__ == "__main__":
    main()
