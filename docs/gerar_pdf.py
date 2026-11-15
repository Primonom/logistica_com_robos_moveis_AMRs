"""
gerar_pdf.py — Gera a figura da arquitetura e os PDFs da entrega.

  1) docs/imagens/arquitetura.dot (Graphviz) -> docs/imagens/arquitetura.png
  2) docs/RELATORIO.md                       -> docs/RELATORIO.pdf
  3) README.md (como compilar e rodar)       -> docs/INSTRUCOES.pdf

Como: pandoc converte o Markdown em HTML (com as figuras embutidas e o
estilo de docs/estilo_pdf.css) e o Chromium headless do Playwright
"imprime" o HTML em PDF. A figura Graphviz também é desenhada no Chromium,
com o Graphviz compilado para WebAssembly (@hpcc-js/wasm-graphviz, baixado
do CDN jsdelivr: precisa de internet), para não exigir o pacote graphviz.

Não faz parte do sistema (só da documentação). Dependências (uma vez):
    interface/.venv/bin/pip install -r interface/requirements-dev.txt
    interface/.venv/bin/playwright install --with-deps chromium
Rodar (da raiz do repositório):
    interface/.venv/bin/python docs/gerar_pdf.py
"""
from pathlib import Path
import tempfile

import pypandoc
from playwright.sync_api import sync_playwright

RAIZ = Path(__file__).resolve().parent.parent
DOCS = RAIZ / "docs"
CSS = DOCS / "estilo_pdf.css"
GRAPHVIZ_JS = "https://cdn.jsdelivr.net/npm/@hpcc-js/wasm-graphviz@1.6.1/dist/index.js"

# (entrada, saída, título usado só se o Markdown não tiver YAML)
DOCUMENTOS = [
    (DOCS / "RELATORIO.md", DOCS / "RELATORIO.pdf", None),
    (RAIZ / "README.md", DOCS / "INSTRUCOES.pdf",
     "Logística com AMRs — Instruções de compilação e execução"),
]

RODAPE = ('<div style="font-size:8px;width:100%;text-align:center;color:#666">'
          '<span class="pageNumber"></span> / <span class="totalPages"></span></div>')


def desenhar_arquitetura(navegador):
    """Graphviz (dot) -> SVG (WebAssembly no Chromium) -> PNG (escala 2)."""
    fonte = (DOCS / "imagens" / "arquitetura.dot").read_text(encoding="utf-8")
    pagina = navegador.new_page(device_scale_factor=2, viewport={"width": 1800, "height": 1200})
    pagina.set_content('<html><body style="background:white;margin:0"><div id="fig"></div></body></html>')
    pagina.add_script_tag(type="module", content=(
        f'import {{ Graphviz }} from "{GRAPHVIZ_JS}";'
        'const g = await Graphviz.load();'
        f'document.getElementById("fig").innerHTML = g.dot({fonte!r});'))
    pagina.wait_for_selector("#fig svg", timeout=30000)
    pagina.locator("#fig svg").screenshot(path=str(DOCS / "imagens" / "arquitetura.png"))
    pagina.close()
    print("docs/imagens/arquitetura.png")


def markdown_para_pdf(navegador, entrada, saida, titulo):
    """Markdown -> HTML (pandoc) -> PDF (Chromium)."""
    argumentos = ["--standalone", "--embed-resources", "--toc", "--toc-depth=2",
                  f"--css={CSS}", f"--resource-path={entrada.parent}"]
    if titulo:
        argumentos += ["--metadata", f"title={titulo}"]
    with tempfile.TemporaryDirectory() as tmp:
        html = Path(tmp) / "doc.html"
        pypandoc.convert_file(str(entrada), "html5", format="gfm+yaml_metadata_block",
                              outputfile=str(html), extra_args=argumentos)
        pagina = navegador.new_page()
        pagina.goto(html.as_uri())
        pagina.pdf(path=str(saida), format="A4", print_background=True,
                   margin={"top": "18mm", "bottom": "18mm", "left": "16mm", "right": "16mm"},
                   display_header_footer=True, header_template="<span></span>",
                   footer_template=RODAPE)
        pagina.close()
    print(saida.relative_to(RAIZ))


def main():
    with sync_playwright() as p:
        navegador = p.chromium.launch()
        desenhar_arquitetura(navegador)
        for entrada, saida, titulo in DOCUMENTOS:
            markdown_para_pdf(navegador, entrada, saida, titulo)
        navegador.close()


if __name__ == "__main__":
    main()
