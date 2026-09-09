#!/usr/bin/env python3
"""
Conta bytes no fio a partir de uma captura, por canal.

Uso:
    ./scripts/pcap_bytes.py results/<run_id>/server/c2_telemetry.pcap
    ./scripts/pcap_bytes.py results/<run_id>/server/*.pcap

Existe por causa do canal C2: o libcoap gerencia os próprios sockets e não expõe
estatística de bytes, então é a única fonte de bytes no fio para OSCORE. Serve
de conferência independente para os outros três, que têm contadores internos.

Lê o formato pcap diretamente, sem depender de scapy ou libpcap: o cabeçalho de
cada registro traz o tamanho ORIGINAL do quadro, que é o que interessa, mesmo
quando a captura foi truncada.

O cabeçalho de enlace é descontado para que o número seja comparável com o dos
contadores internos, que enxergam a partir do IP. Ethernet tem 14 bytes;
LINUX_SLL2, usado quando se captura em "any", tem 20 — capturar em "any" e não
descontar infla o resultado em 6 bytes por pacote.
"""
import argparse
import struct
import sys
from pathlib import Path

# Tamanho do cabeçalho de enlace por link-type do pcap.
LINK_HEADER = {
    1: ("Ethernet", 14),
    113: ("LINUX_SLL", 16),
    276: ("LINUX_SLL2", 20),
    0: ("BSD loopback", 4),
    228: ("IPv4 cru", 0),
    101: ("IP cru", 0),
}

PCAP_MAGICS = {
    0xA1B2C3D4: ("<", 1),      # little-endian, microssegundos
    0xD4C3B2A1: (">", 1),
    0xA1B23C4D: ("<", 1000),   # little-endian, nanossegundos
    0x4D3CB2A1: (">", 1000),
}


def read_pcap(path):
    """Devolve (link_name, link_hdr_len, n_pacotes, bytes_de_quadro)."""
    data = path.read_bytes()
    if len(data) < 24:
        raise ValueError("arquivo curto demais para ser um pcap")

    magic = struct.unpack("<I", data[:4])[0]
    if magic not in PCAP_MAGICS:
        magic_be = struct.unpack(">I", data[:4])[0]
        if magic_be == 0x0A0D0D0A:
            raise ValueError("é pcapng, não pcap clássico; recapture com "
                             "tcpdump -w (que grava pcap)")
        raise ValueError(f"assinatura desconhecida: 0x{magic:08x}")

    endian, _ = PCAP_MAGICS[magic]
    link_type = struct.unpack(endian + "I", data[20:24])[0]
    link_name, link_hdr = LINK_HEADER.get(link_type, (f"link-type {link_type}", 0))

    off = 24
    n = 0
    total = 0
    rec = struct.Struct(endian + "IIII")

    while off + 16 <= len(data):
        _, _, incl_len, orig_len = rec.unpack_from(data, off)
        off += 16 + incl_len
        if off > len(data):
            break              # registro truncado no fim: captura interrompida
        n += 1
        total += orig_len

    return link_name, link_hdr, n, total


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", type=Path, nargs="+")
    ap.add_argument("--keep-link-header", action="store_true",
                    help="não descontar o cabeçalho de enlace")
    args = ap.parse_args()

    print(f"{'ARQUIVO':<24}{'ENLACE':<14}{'PACOTES':>10}{'BYTES IP+':>13}{'MÉDIA/PKT':>11}")
    print("-" * 72)

    for path in args.pcap:
        if not path.is_file():
            print(f"{path.name:<24}não encontrado", file=sys.stderr)
            continue
        try:
            link, hdr, n, total = read_pcap(path)
        except ValueError as e:
            print(f"{path.name:<24}{e}", file=sys.stderr)
            continue

        if not args.keep_link_header:
            total -= n * hdr

        media = (total / n) if n else 0
        print(f"{path.name:<24}{link:<14}{n:>10}{total:>13}{media:>11.1f}")

    if not args.keep_link_header:
        print()
        print("  BYTES IP+ desconta o cabeçalho de enlace: é o que o processo")
        print("  enxerga, comparável com os contadores internos dos canais.")


if __name__ == "__main__":
    main()
