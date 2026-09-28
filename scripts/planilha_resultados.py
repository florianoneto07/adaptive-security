#!/usr/bin/env python3
"""
Compila os resumo.csv das campanhas numa planilha .xlsx.

    ./scripts/planilha_resultados.py                  # results/resultados-compilados.xlsx
    ./scripts/planilha_resultados.py --out ARQUIVO

É a mesma fonte das figuras e das tabelas do artigo: as campanhas estão
declaradas em `scripts/paper_tables.py` e importadas daqui, para que os três
geradores não possam divergir.

**Escreve o .xlsx sem biblioteca externa.** A VM de medição não tem openpyxl,
pandas, pip nem LibreOffice, e o repositório já decidiu (ver
`scripts/plot_resultados.py`) que os geradores de material do artigo rodam em
qualquer clone sem instalar nada — é o requisito de reprodutibilidade. Um
.xlsx é um ZIP de XML; o que está aqui é o subconjunto necessário: planilhas,
estilos, larguras de coluna e painel congelado.

As poucas colunas derivadas (acréscimo de memória, RTT) são **fórmulas**, não
valores calculados aqui, para recalcularem se alguém editar os dados. Como o
arquivo é escrito sem valores em cache, o workbook pede recálculo ao abrir
(`fullCalcOnLoad`); um leitor que só olhe o cache (pandas, csv) verá essas
células vazias até que a planilha seja aberta uma vez.

Unidades: os `resumo.csv` gravam tempo em NANOSSEGUNDOS, apesar de os nomes
das colunas dizerem `_ms` e `_us`. A conversão acontece aqui, e o cabeçalho de
cada coluna traz a unidade de verdade.
"""
import argparse
import csv
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paper_tables import MAIN, PLAIN, DELAY, LOSS, C2_PCAP  # noqa: E402

CH = ["C1 control", "C2 telemetry", "C3 media", "C4 bulk"]
CH_PLAIN = ["C1 claro", "C2 claro", "C3 claro", "C4 claro"]
CH_LABEL = ["C1 DTLS 1.3", "C2 OSCORE", "C3 DTLS-SRTP", "C4 TLS 1.3"]
CONDS = ["clean", "3gpp-c2", "handover"]
COND_LABEL = ["enlace limpo", "3GPP C2", "handover"]
LCONDS = ["loss-0", "loss-0.1", "loss-1", "loss-5", "loss-10", "loss-20"]
LPCT = [0, 0.1, 1, 5, 10, 20]
DCONDS = ["delay-0", "delay-25", "delay-50", "delay-100", "delay-200"]
DMS = [0, 25, 50, 100, 200]

# Estilos: índice em cellXfs, na ordem em que styles.xml os declara.
S_TXT, S_HDR, S_INT, S_2, S_3, S_TITLE, S_WRAP = 0, 1, 2, 3, 4, 5, 6


# --------------------------------------------------------------------------
# Escrita do .xlsx
# --------------------------------------------------------------------------

def esc(v):
    return (str(v).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def col_name(i):
    """0 -> A, 25 -> Z, 26 -> AA."""
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


class Sheet:
    def __init__(self, name, widths=None, freeze="A2"):
        # O Excel recusa nome de aba com : \ / ? * [ ] ou acima de 31 chars.
        self.name = name[:31]
        self.rows = []
        self.widths = widths or []
        self.freeze = freeze

    def add(self, cells):
        """cells: lista de (valor, estilo) ou (valor, estilo, "f") para fórmula."""
        self.rows.append(cells)

    def xml(self):
        out = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
               '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
               'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">']
        if self.freeze:
            out.append('<sheetViews><sheetView workbookViewId="0">'
                       f'<pane ySplit="1" topLeftCell="{self.freeze}" '
                       'activePane="bottomLeft" state="frozen"/>'
                       '</sheetView></sheetViews>')
        out.append('<sheetFormatPr defaultRowHeight="14.5"/>')
        if self.widths:
            out.append("<cols>" + "".join(
                f'<col min="{i+1}" max="{i+1}" width="{w}" customWidth="1"/>'
                for i, w in enumerate(self.widths)) + "</cols>")
        out.append("<sheetData>")
        for ri, cells in enumerate(self.rows, start=1):
            out.append(f'<row r="{ri}">')
            for ci, cell in enumerate(cells):
                val, style = cell[0], cell[1]
                formula = len(cell) > 2 and cell[2] == "f"
                ref = f"{col_name(ci)}{ri}"
                if val is None or val == "":
                    out.append(f'<c r="{ref}" s="{style}"/>')
                elif formula:
                    # Sem <v>: o workbook recalcula ao abrir (fullCalcOnLoad).
                    out.append(f'<c r="{ref}" s="{style}"><f>{esc(val)}</f></c>')
                elif isinstance(val, (int, float)) and not isinstance(val, bool):
                    out.append(f'<c r="{ref}" s="{style}"><v>{val}</v></c>')
                else:
                    out.append(f'<c r="{ref}" s="{style}" t="inlineStr">'
                               f'<is><t xml:space="preserve">{esc(val)}</t></is></c>')
            out.append("</row>")
        out.append("</sheetData></worksheet>")
        return "".join(out)


STYLES = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="1"><numFmt numFmtId="164" formatCode="0.000"/></numFmts>
<fonts count="3">
<font><sz val="10"/><name val="Arial"/></font>
<font><b/><sz val="10"/><color rgb="FFFFFFFF"/><name val="Arial"/></font>
<font><b/><sz val="12"/><name val="Arial"/></font>
</fonts>
<fills count="3">
<fill><patternFill patternType="none"/></fill>
<fill><patternFill patternType="gray125"/></fill>
<fill><patternFill patternType="solid"><fgColor rgb="FF44546A"/><bgColor indexed="64"/></patternFill></fill>
</fills>
<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="7">
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="1" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="2" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
<xf numFmtId="0" fontId="2" fillId="0" borderId="0" xfId="0" applyFont="1"/>
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf>
</cellXfs>
</styleSheet>'''


def write_xlsx(path, sheets):
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        types = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                 '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
                 '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>',
                 '<Default Extension="xml" ContentType="application/xml"/>',
                 '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>',
                 '<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>']
        for i in range(len(sheets)):
            types.append(f'<Override PartName="/xl/worksheets/sheet{i+1}.xml" '
                         'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>')
        types.append("</Types>")
        z.writestr("[Content_Types].xml", "".join(types))

        z.writestr("_rels/.rels",
                   '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
                   '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                   '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
                   '</Relationships>')

        wb = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
              '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
              'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets>']
        for i, s in enumerate(sheets):
            wb.append(f'<sheet name="{esc(s.name)}" sheetId="{i+1}" r:id="rId{i+1}"/>')
        # fullCalcOnLoad: as fórmulas são escritas sem valor em cache.
        wb.append('</sheets><calcPr calcId="0" fullCalcOnLoad="1"/></workbook>')
        z.writestr("xl/workbook.xml", "".join(wb))

        rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">']
        for i in range(len(sheets)):
            rels.append(f'<Relationship Id="rId{i+1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet{i+1}.xml"/>')
        rels.append(f'<Relationship Id="rId{len(sheets)+1}" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>')
        rels.append("</Relationships>")
        z.writestr("xl/_rels/workbook.xml.rels", "".join(rels))
        z.writestr("xl/styles.xml", STYLES)
        for i, s in enumerate(sheets):
            z.writestr(f"xl/worksheets/sheet{i+1}.xml", s.xml())


# --------------------------------------------------------------------------
# Leitura dos resumos
# --------------------------------------------------------------------------

def load(path):
    with open(path, newline="") as f:
        return {(r["condicao"], r["canal"][:2]): r for r in csv.DictReader(f)}


def val(rows, cond, ch, campo, div=1.0):
    r = rows.get((cond, ch[:2]))
    if r is None:
        return None
    v = r.get(campo, "")
    if v in ("", None):
        return None
    try:
        return round(float(v) / div, 6)
    except ValueError:
        return None


# Colunas dos despejos completos: (cabeçalho, campo, divisor, estilo).
DUMP = [
    ("condição", "condicao", None, S_TXT),
    ("canal", "canal", None, S_TXT),
    ("perfil", "perfil", None, S_TXT),
    ("repetições", "reps", 1, S_INT),
    ("reps sem dados", "reps_sem_dados", 1, S_INT),
    ("estabelecimento (ms)", "handshake_ms", 1e6, S_2),
    ("estab. IQR (ms)", "handshake_ms_iqr", 1e6, S_2),
    ("CPU do estab. (ms)", "handshake_cpu_ms", 1e6, S_3),
    ("bytes do estab.", "handshake_bytes", 1, S_INT),
    ("mensagens", "msgs", 1, S_INT),
    ("bytes de aplicação", "app_bytes", 1, S_INT),
    ("sobrecarga (B/msg)", "overhead_b", 1, S_2),
    ("origem do atraso", "lat_origem", None, S_TXT),
    ("p50 (ms)", "p50_ms", 1e6, S_2),
    ("p50 IQR (ms)", "p50_ms_iqr", 1e6, S_2),
    ("p95 (ms)", "p95_ms", 1e6, S_2),
    ("p99 (ms)", "p99_ms", 1e6, S_2),
    ("recebidas", "recv", 1, S_INT),
    ("entrega (%)", "entrega_pct", 1, S_2),
    ("vazão (Mbps)", "mbps", 1, S_3),
    ("vazão IQR (Mbps)", "mbps_iqr", 1, S_3),
    ("CPU/msg (µs)", "cpu_msg_us", 1e3, S_2),
    ("CPU/msg IQR (µs)", "cpu_msg_us_iqr", 1e3, S_2),
    ("retransmissões", "retx", 1, S_INT),
    ("CPU/retx (µs)", "cpu_retx_us", 1e3, S_2),
    ("CPU de verificação (µs)", "cpu_verify_us", 1e3, S_2),
    ("RSS de pico (MB)", "rss_kb", 1024, S_2),
]


def dump_sheet(name, rows, ordem):
    sh = Sheet(name, widths=[13, 15, 17, 10, 12, 17, 14, 15, 13, 11, 15, 15,
                             15, 11, 12, 11, 11, 11, 11, 13, 14, 13, 14, 13,
                             13, 16, 14])
    sh.add([(h, S_HDR) for h, _, _, _ in DUMP])
    for cond, ch in ordem:
        r = rows.get((cond, ch[:2]))
        if r is None:
            continue
        linha = []
        for _, campo, div, st in DUMP:
            if div is None:
                linha.append((r.get(campo, ""), st))
            else:
                v = r.get(campo, "")
                try:
                    linha.append((round(float(v) / div, 6), st))
                except (ValueError, TypeError):
                    linha.append(("", st))
        sh.add(linha)
    return sh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="results/resultados-compilados.xlsx")
    a = ap.parse_args()
    R = a.results

    principal = load(f"{R}/{MAIN}/resumo.csv")
    claro = load(f"{R}/{PLAIN}/resumo.csv")
    atraso = load(f"{R}/{DELAY}/resumo.csv")
    perda = {}
    for c in LOSS:
        perda.update(load(f"{R}/{c}/resumo.csv"))

    sheets = []

    # ---- Leia-me -------------------------------------------------------
    leia = Sheet("Leia-me", widths=[104], freeze=None)
    leia.add([("Resultados compilados do testbed de perfis de segurança por classe de aplicação UAV", S_TITLE)])
    for linha in [
        "",
        "Gerado por scripts/planilha_resultados.py a partir dos resumo.csv das campanhas. Não editar à mão:",
        "rode o script de novo depois de qualquer campanha nova.",
        "",
        "CAMPANHAS (os diretórios estão em results/, fora do controle de versão pelo tamanho):",
        f"  principal, 3 condições x 4 canais x 10 repetições .... {MAIN}",
        f"  baseline em claro, 3 condições x 4 canais x 5 reps ... {PLAIN}",
        f"  varredura de atraso, 5 pontos x 5 reps ............... {DELAY}",
        f"  varredura de perda, 6 pontos x 5 reps ................ {LOSS[0]}",
        f"                                                        {LOSS[1]} (loss-20, bulk de 1 MiB)",
        "",
        "ABAS:",
        "  Fig1..Fig5 ......... exatamente os números que cada figura do artigo desenha.",
        "  Campanha principal . resumo.csv completo das três condições, unidades convertidas.",
        "  Baseline em claro .. o mesmo para os canais sem segurança.",
        "  Varredura de perda / Varredura de atraso ... idem para as duas varreduras.",
        "",
        "UNIDADES: os resumo.csv gravam tempo em NANOSSEGUNDOS, embora os nomes das colunas digam",
        "_ms e _us. A conversão é feita aqui e o cabeçalho de cada coluna traz a unidade de verdade.",
        "A agregação entre repetições é MEDIANA, não média: latência tem cauda longa.",
        "",
        "RESSALVAS QUE MUDAM A LEITURA:",
        "  1. Todas estas campanhas são anteriores à build com aceleração de hardware (commit 41c7c9d).",
        "     AES-256-GCM a 138 MiB/s e ECDHE P-256 a 0,41 ms: os números de CPU são custo de software.",
        "  2. CPU/msg sob a condição 'enlace limpo' NÃO é comparável: o canal em claro mede mais CPU que",
        "     o seguro, o que é impossível como custo de cifra. Só a diferença seguro-claro SOB NETEM vale.",
        "  3. O canal C3 mede atraso UNIDIRECIONAL sem relógio comum entre as VMs. Valem as diferenças",
        "     entre percentis (p99-p50), não os valores absolutos.",
        "  4. As colunas de percentil do C4 trazem o tempo da transferência COMPLETA (32 MiB; 8 MiB nas",
        "     varreduras, 1 MiB em loss-20), uma amostra por repetição — não é latência por mensagem.",
        "  5. O C2 não tem contadores de bytes de fio (o libcoap não os expõe). Os bytes do C2 saem do",
        f"     pcap: requisição {C2_PCAP['secure_req']} B e resposta {C2_PCAP['secure_resp']} B com OSCORE, contra "
        f"{C2_PCAP['plain_req']} B e {C2_PCAP['plain_resp']} B em claro.",
        "  6. Células vazias em CPU/retx são o resultado, não instrumentação faltando: só o CoAP",
        "     confirmável retransmite na camada de aplicação.",
        "  7. 'reps sem dados' é a taxa de falha de estabelecimento: repetições cujo handshake não fechou,",
        "     excluídas das medianas. A 20% de perda independente, C1 falhou 3/5 e C3 2/5.",
        "",
        "As colunas marcadas como fórmula recalculam ao abrir a planilha; num leitor que só olhe o cache",
        "(pandas, csv) elas aparecem vazias até que o arquivo seja aberto uma vez num editor de planilha.",
    ]:
        leia.add([(linha, S_WRAP)])
    sheets.append(leia)

    # ---- Fig 1: estabelecimento ---------------------------------------
    s1 = Sheet("Fig1 estabelecimento", widths=[16] + [15, 14] * 3)
    s1.add([("canal", S_HDR)] + [(f"{c} — {x}", S_HDR)
                                 for c in COND_LABEL
                                 for x in ("estab. (ms)", "IQR (ms)")])
    for ch, lab in zip(CH, CH_LABEL):
        linha = [(lab, S_TXT)]
        for cond in CONDS:
            linha.append((val(principal, cond, ch, "handshake_ms", 1e6), S_2))
            linha.append((val(principal, cond, ch, "handshake_ms_iqr", 1e6), S_2))
        s1.add(linha)
    sheets.append(s1)

    # ---- Fig 2: CPU do estabelecimento ---------------------------------
    s2 = Sheet("Fig2 CPU", widths=[16, 16, 16, 16])
    s2.add([("canal", S_HDR)] + [(f"{c} — CPU (ms)", S_HDR) for c in COND_LABEL])
    for ch, lab in zip(CH, CH_LABEL):
        s2.add([(lab, S_TXT)] +
               [(val(principal, cond, ch, "handshake_cpu_ms", 1e6), S_3) for cond in CONDS])
    sheets.append(s2)

    # ---- Fig 3: memória ------------------------------------------------
    s3 = Sheet("Fig3 memoria", widths=[16, 17, 17, 17, 34])
    s3.add([("canal", S_HDR), ("seguro (MB)", S_HDR), ("em claro (MB)", S_HDR),
            ("acréscimo (MB)", S_HDR), ("o que o acréscimo é", S_HDR)])
    porque = ["wolfSSL", "libcoap já está nos dois lados; OSCORE é contexto, não pilha",
              "wolfSSL + libsrtp2", "wolfSSL"]
    for i, (ch, chp, lab) in enumerate(zip(CH, CH_PLAIN, CH_LABEL)):
        # Mediana das três condições: o RSS varia menos de 0,07 MB entre elas.
        seg = sorted(v for v in (val(principal, c, ch, "rss_kb", 1024) for c in CONDS) if v)[1]
        cla = sorted(v for v in (val(claro, c, chp, "rss_kb", 1024) for c in CONDS) if v)[1]
        r = i + 2
        s3.add([(lab, S_TXT), (seg, S_2), (cla, S_2), (f"B{r}-C{r}", S_2, "f"),
                (porque[i], S_TXT)])
    sheets.append(s3)

    # ---- Fig 4: entrega vs perda ---------------------------------------
    s4 = Sheet("Fig4 entrega-perda", widths=[12] + [15] * 4 + [15, 15])
    s4.add([("perda (%)", S_HDR)] + [(f"{l} — entrega (%)", S_HDR) for l in CH_LABEL]
           + [("C1 reps sem dados", S_HDR), ("C3 reps sem dados", S_HDR)])
    for cond, pct in zip(LCONDS, LPCT):
        s4.add([(pct, S_2)] +
               [(val(perda, cond, ch, "entrega_pct", 1), S_2) for ch in CH] +
               [(val(perda, cond, "C1", "reps_sem_dados", 1), S_INT),
                (val(perda, cond, "C3", "reps_sem_dados", 1), S_INT)])
    sheets.append(s4)

    # ---- Fig 5: estabelecimento vs RTT ---------------------------------
    s5 = Sheet("Fig5 estab-RTT", widths=[14, 12] + [16] * 4)
    s5.add([("atraso (ms)", S_HDR), ("RTT (ms)", S_HDR)] +
           [(f"{l} — estab. (ms)", S_HDR) for l in CH_LABEL])
    for i, (cond, d) in enumerate(zip(DCONDS, DMS)):
        r = i + 2
        # netem nas duas pontas: o RTT é o dobro do atraso configurado.
        s5.add([(d, S_INT), (f"2*A{r}", S_INT, "f")] +
               [(val(atraso, cond, ch, "handshake_ms", 1e6), S_2) for ch in CH])
    r0, r1 = 2, len(DCONDS) + 1
    s5.add([("", S_TXT)] * 6)
    s5.add([("inclinação (RTT)", S_TXT), ("", S_TXT)] +
           [(f"({col_name(2+i)}{r1}-{col_name(2+i)}{r0})/(B{r1}-B{r0})", S_3, "f")
            for i in range(4)])
    sheets.append(s5)

    # ---- Despejos completos --------------------------------------------
    sheets.append(dump_sheet("Campanha principal", principal,
                             [(c, ch) for c in CONDS for ch in CH]))
    sheets.append(dump_sheet("Baseline em claro", claro,
                             [(c, ch) for c in CONDS for ch in CH_PLAIN]))
    sheets.append(dump_sheet("Varredura de perda", perda,
                             [(c, ch) for c in LCONDS for ch in CH]))
    sheets.append(dump_sheet("Varredura de atraso", atraso,
                             [(c, ch) for c in DCONDS for ch in CH]))

    write_xlsx(a.out, sheets)
    print(f"{a.out}  ({len(sheets)} abas)")


if __name__ == "__main__":
    main()
