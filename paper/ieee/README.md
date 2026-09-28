# Artigo IEEE (conferência, 5–6 páginas)

Esqueleto criado em 2026-09-18. Texto do artigo em inglês; comentários e
`TODO` em português. Prazo: ver `docs/ANALISE-KO2024-E-CAMINHO-DE-PUBLICACAO.md` §7.

```
paper/ieee/
├── main.tex        esqueleto IEEEtran (conference)
├── refs.bib        bibliografia; entradas "VERIFICAR" precisam de conferência
├── tables/*.tex    GERADAS por scripts/paper_tables.py — não editar à mão
└── figures/*.svg   GERADAS por scripts/plot_resultados.py — hoje com rótulos
                    em português (falta a versão em inglês)
```

## Regenerar tabelas e figuras

Na VM servidora, onde estão os `results/` (não versionados):

```bash
./scripts/paper_tables.py --out paper/ieee/tables
```

```bash
./scripts/plot_resultados.py --out paper/ieee/figures
```

Todo número das tabelas é rastreável a um `resumo.csv`, que é rastreável aos
CSVs brutos e ao `manifest.json` da campanha. As campanhas usadas estão
fixadas no cabeçalho do `paper_tables.py`. Uma exceção marcada: os bytes no
fio do C2 vêm do pcap (`pcap_bytes.py --split-port 5002`) e ficam na
constante `C2_PCAP`.

## Compilar

Esta VM não tem TeX. Duas saídas:

**Overleaf** (recomendado): subir a pasta inteira. O pacote `svg` converte os
`.svg` com o inkscape do Overleaf; `IEEEtran` já está lá. Compilador:
pdfLaTeX, com shell-escape (padrão no Overleaf).

**Local**:

```bash
sudo apt install -y texlive-latex-extra texlive-bibtex-extra texlive-science inkscape
```

```bash
cd paper/ieee && pdflatex -shell-escape main && bibtex main && pdflatex -shell-escape main && pdflatex -shell-escape main
```

## O que falta (ordem sugerida)

1. Figuras com rótulos em inglês (`plot_resultados.py`, opção de idioma).
2. Fig. 1, o diagrama do testbed (não existe).
3. Texto: Introdução, §II.A–B, Trabalhos relacionados, Conclusão — os
   `TODO` no `main.tex` dizem o que cada um cobre e de onde vem.
4. Conferir as entradas `VERIFICAR` do `refs.bib`.
5. Título, autores, URL do repositório, limite de páginas do venue.
6. Rodar `\usepackage[disable]{todonotes}` antes de submeter.
