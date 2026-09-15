#!/usr/bin/env python3
"""
Agrega os resultados de uma execução numa tabela comparável entre os perfis.

Uso:
    ./scripts/summarize.py results/<run_id>
    ./scripts/summarize.py results/<run_id> --csv resumo.csv

Numa campanha entre as duas VMs, os resultados do cliente e os do servidor
ficam em máquinas diferentes. Passe os dois diretórios e eles são mesclados:

    ./scripts/summarize.py results/<run_cliente> /caminho/do/<run_servidor>

Uma execução deixa um .summary por canal, por papel e por repetição. Compará-los
a olho não escala, e a comparação entre perfis é justamente o ponto do testbed.

Agrega por MEDIANA entre repetições, não por média: latência tem cauda longa, e
uma repetição em que o escalonador atrapalhou deslocaria a média sem dizer nada
sobre o perfil. Os percentis já vêm calculados de dentro de cada execução, sobre
todas as amostras daquela repetição.
"""
import argparse
import csv
import statistics
import sys
from pathlib import Path

# Canais na ordem do enunciado, com o nome curto usado no run.sh.
# slug, rótulo, transporte. O transporte decide como a razão de entrega é
# calculada: em fluxo (TCP) o receptor lê em pedaços de tamanho diferente do que
# o emissor escreveu, sem que byte algum se perca, e comparar contagens de
# mensagens produziria uma "perda" que não existe.
CHANNELS = [
    ("c1_control", "C1 control", "datagrama"),
    ("c2_telemetry", "C2 telemetry", "datagrama"),
    ("c3_media", "C3 media", "datagrama"),
    ("c4_bulk", "C4 bulk", "fluxo"),
]


def read_summary(path):
    """Lê um .summary em chave=valor. Números viram int quando possível."""
    out = {}
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        k, _, v = line.partition("=")
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    return out


def collect(run_dirs):
    """
    Devolve {slug: {"client": [summaries], "server": [summaries]}}.

    Aceita as duas disposições que o run.sh produz: por repetição (papel "both"
    e o cliente de uma campanha entre VMs) e a pasta server/ única, usada quando
    o servidor fica persistente para toda a campanha.
    """
    found = {slug: {"client": [], "server": []} for slug, _, _ in CHANNELS}

    paths = sorted(q for d in run_dirs for q in d.rglob("*.summary"))
    for path in paths:
        name = path.stem                      # ex.: c1_control_client
        for role in ("client", "server"):
            suffix = "_" + role
            if name.endswith(suffix):
                slug = name[: -len(suffix)]
                if slug in found:
                    found[slug][role].append(read_summary(path))
                break
    return found


def med(rows, key):
    """Mediana de uma chave entre repetições; None se ninguém a preencheu."""
    vals = [r[key] for r in rows if isinstance(r.get(key), int)]
    return statistics.median(vals) if vals else None


def total(rows, key):
    vals = [r[key] for r in rows if isinstance(r.get(key), int)]
    return sum(vals) if vals else None


def fmt(v, unit="", div=1.0, prec=1):
    if v is None:
        return "—"
    if div != 1.0:
        v = v / div
    # Inteiros saem sem casas decimais; o resto respeita a precisão pedida.
    if isinstance(v, float):
        return f"{v:.{prec}f}{unit}"
    return f"{v}{unit}"


def analyse(slug, label, transport, data):
    """Monta a linha de um canal. Devolve dict com valores já em unidades úteis."""
    cli, srv = data["client"], data["server"]
    if not cli and not srv:
        return None

    row = {"canal": label, "reps": len(cli) or len(srv)}
    ref = (cli or srv)[0]
    row["perfil"] = ref.get("profile", "?")

    # --- estabelecimento ---
    row["handshake_ms"] = med(cli, "handshake_wall_ns")
    row["handshake_cpu_ms"] = med(cli, "handshake_cpu_ns")
    hs_tx = med(cli, "handshake_wire_tx_bytes")
    hs_rx = med(cli, "handshake_wire_rx_bytes")
    row["handshake_bytes"] = (hs_tx + hs_rx) if (hs_tx and hs_rx) else None

    # --- volume e overhead ---
    sent = total(cli, "msgs_sent")
    app = total(cli, "app_bytes_tx")
    wire = total(cli, "wire_tx_bytes")
    hs_all = total(cli, "handshake_wire_tx_bytes") or 0
    row["msgs"] = sent
    row["app_bytes"] = app

    # Sem contadores de fio (caso do C2, que usa libcoap), o overhead por
    # mensagem não é calculável fora do pcap.
    instrumented = any(r.get("wire_instrumented") == 1 for r in cli)
    if instrumented and sent and app and wire:
        row["overhead_b"] = (wire - hs_all - app) / sent
    else:
        row["overhead_b"] = None

    # --- atraso ---
    # C1, C2 e C4 medem ida e volta no cliente. O C3 é unidirecional: quem
    # registra o atraso é o receptor, e olhar só o cliente daria zero.
    lat = cli if any(r.get("rtt_samples") for r in cli) else srv
    row["lat_origem"] = "cliente (RTT)" if lat is cli else "servidor (unidirecional)"
    row["p50_ms"] = med(lat, "rtt_p50_ns")
    row["p95_ms"] = med(lat, "rtt_p95_ns")
    row["p99_ms"] = med(lat, "rtt_p99_ns")

    # --- entrega ---
    recv = total(srv, "msgs_recv")
    row["recv"] = recv
    if transport == "fluxo":
        rx = total(srv, "app_bytes_rx")
        row["entrega_pct"] = (100.0 * rx / app) if (app and rx) else None
    else:
        row["entrega_pct"] = (100.0 * recv / sent) if (sent and recv is not None) else None

    # Throughput só faz sentido no canal de transferência volumosa. É a mediana
    # da vazão POR repetição — não os bytes somados de todas as repetições sobre
    # a duração de uma só, que inflava o valor pelo número de repetições.
    if transport == "fluxo":
        per_rep = [r["app_bytes_tx"] * 8.0 / (r["duration_ns"] / 1e9) / 1e6
                   for r in cli
                   if isinstance(r.get("app_bytes_tx"), int)
                   and isinstance(r.get("duration_ns"), int) and r["duration_ns"]]
        row["mbps"] = statistics.median(per_rep) if per_rep else None
    else:
        row["mbps"] = None

    # --- CPU por mensagem ---
    cpu_send = total(cli, "cpu_first_send_ns")
    row["cpu_msg_us"] = (cpu_send / sent) if (cpu_send and sent) else None

    retx = total(cli, "msgs_retx")
    cpu_retx = total(cli, "cpu_retransmit_ns")
    row["retx"] = retx
    row["cpu_retx_us"] = (cpu_retx / retx) if (retx and cpu_retx is not None) else None

    cpu_ver = total(srv, "cpu_verify_ns")
    row["cpu_verify_us"] = (cpu_ver / recv) if (cpu_ver and recv) else None

    row["rss_kb"] = med(cli, "rss_peak_kb")
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", type=Path, nargs="+",
                    help="diretório(s) da execução; passe o do cliente e o do "
                         "servidor para mesclá-los")
    ap.add_argument("--csv", type=Path, help="grava a tabela agregada neste arquivo")
    ap.add_argument("--label", default=None,
                    help="rótulo da condição (ex.: clean, 3gpp-c2); vira uma "
                         "coluna no CSV, para empilhar campanhas num gráfico")
    ap.add_argument("--append", action="store_true",
                    help="acrescenta ao CSV em vez de sobrescrever, sem repetir "
                         "o cabeçalho")
    args = ap.parse_args()

    for d in args.run_dir:
        if not d.is_dir():
            sys.exit(f"Diretório não encontrado: {d}")

    # None quando nenhum diretório traz manifesto — caso de uma campanha
    # interrompida antes do fim, em que os dados existem mas o manifesto não
    # chegou a ser escrito.
    manifest = next((d / "manifest.json" for d in args.run_dir
                     if (d / "manifest.json").is_file()), None)
    data = collect(args.run_dir)
    rows = [r for slug, label, transport in CHANNELS
            if (r := analyse(slug, label, transport, data[slug])) is not None]

    if not rows:
        sys.exit("Nenhum .summary encontrado em: "
                 + ", ".join(str(d) for d in args.run_dir))

    print(f"Execução: {', '.join(d.name for d in args.run_dir)}")
    if manifest is not None:
        import json
        m = json.loads(manifest.read_text())
        env = m.get("ambiente", {})
        netem = m.get("netem")
        if isinstance(netem, dict):
            netem = netem.get("efetivo", "?")
        print(f"  netem={netem}  wolfSSL={env.get('wolfssl')}  "
              f"libcoap={env.get('libcoap')}  kernel={env.get('kernel')}")
        if m.get("git", {}).get("dirty"):
            print("  ATENÇÃO: árvore git com alterações não commitadas")
    else:
        print("  ATENÇÃO: sem manifest.json — campanha interrompida antes do fim.")
        print("  Ambiente e parâmetros não ficaram registrados nesta execução.")
    print()

    hdr = (f"{'CANAL':<14}{'PERFIL':<22}{'REPS':>5}{'MSGS':>9}"
           f"{'OVERH.B':>9}{'p50 ms':>11}{'p95 ms':>11}{'p99 ms':>11}"
           f"{'ENTREGA':>9}{'CPU/msg':>10}{'CPU/retx':>10}")
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['canal']:<14}{r['perfil'][:21]:<22}{r['reps']:>5}"
              f"{fmt(r['msgs']):>9}"
              f"{fmt(r['overhead_b'], prec=1):>9}"
              # 11 colunas: o volumoso sob handover chegou a 1046490.90 ms,
              # dez caracteres, e com 9 as três colunas saíam coladas.
              f"{fmt(r['p50_ms'], div=1e6, prec=2):>11}"
              f"{fmt(r['p95_ms'], div=1e6, prec=2):>11}"
              f"{fmt(r['p99_ms'], div=1e6, prec=2):>11}"
              f"{fmt(r['entrega_pct'], unit='%', prec=1):>9}"
              f"{fmt(r['cpu_msg_us'], unit='us', div=1e3, prec=1):>10}"
              f"{fmt(r['cpu_retx_us'], unit='us', div=1e3, prec=1):>10}")

    print()
    print("Estabelecimento e memória:")
    print(f"  {'CANAL':<14}{'HANDSHAKE ms':>14}{'CPU ms':>10}{'BYTES':>9}{'RSS kB':>9}")
    for r in rows:
        print(f"  {r['canal']:<14}"
              f"{fmt(r['handshake_ms'], div=1e6, prec=2):>14}"
              f"{fmt(r['handshake_cpu_ms'], div=1e6, prec=2):>10}"
              f"{fmt(r['handshake_bytes']):>9}"
              f"{fmt(r['rss_kb']):>9}")

    for r in rows:
        if r.get("mbps") is not None:
            print()
            print(f"  {r['canal']}: transferência a {r['mbps']:.1f} Mbps. As colunas de")
            print("  percentil trazem o tempo da transferência COMPLETA, uma amostra por")
            print("  repetição — não são latência por mensagem como nos outros canais.")

    if any(r["lat_origem"].startswith("servidor") for r in rows):
        print()
        print("  O atraso do C3 é unidirecional, medido no receptor, e os relógios")
        print("  das duas VMs não são sincronizados: use as DIFERENÇAS entre")
        print("  percentis (p99 - p50), não os valores absolutos.")

    # Razão de entrega acima de 100% não é medição, é desencontro de contagem:
    # o servidor viu mais repetições do que o cliente registrou, tipicamente
    # porque a campanha foi interrompida entre o fim de uma transferência e a
    # gravação do resumo do cliente.
    for r in rows:
        if r["entrega_pct"] is not None and r["entrega_pct"] > 100.5:
            print()
            print(f"  {r['canal']}: entrega de {r['entrega_pct']:.1f}% — acima de 100%.")
            print("  O servidor contabilizou mais do que o cliente registrou: as duas")
            print("  pontas cobrem números diferentes de repetições. Campanha")
            print("  interrompida; este canal não é comparável com os demais.")
            r["entrega_pct"] = None

    notas = []
    if any(r["overhead_b"] is None for r in rows):
        notas.append("overhead vazio: canal sem contadores de fio (C2/libcoap); "
                     "use a captura em pcap")
    if any(r["cpu_retx_us"] is None for r in rows):
        notas.append("CPU/retx vazio: o protocolo não retransmite dados de "
                     "aplicação (DTLS e TLS); é o resultado, não lacuna")
    if any(r["entrega_pct"] is None for r in rows):
        notas.append("entrega vazia: faltam os .summary do servidor "
                     "(encerre-o com Ctrl+C para que sejam gravados)")
    if notas:
        print()
        for n in notas:
            print(f"  nota: {n}")

    if args.csv:
        if args.label:
            for r in rows:
                r["condicao"] = args.label
            # A condição primeiro: é por ela que os gráficos vão agrupar.
            rows = [{"condicao": r.pop("condicao"), **r} for r in rows]

        keys = list(rows[0].keys())
        exists = args.csv.exists() and args.csv.stat().st_size > 0
        mode = "a" if (args.append and exists) else "w"

        with args.csv.open(mode, newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=keys)
            if mode == "w":
                w.writeheader()
            w.writerows(rows)
        print(f"\nTabela agregada em {args.csv}")


if __name__ == "__main__":
    main()
