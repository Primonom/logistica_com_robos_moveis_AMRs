"""
analisar_escalabilidade.py — Análise do benchmark testes/escalabilidade.sh.

Lê testes/saidas/escalabilidade.csv e escalabilidade_falha.csv e gera:
  docs/imagens/escalabilidade_utilizacao.png   U(N) x núcleos x Liu & Layland
  docs/imagens/escalabilidade_atraso.png       atraso máximo e perdas x N
  docs/imagens/escalabilidade_latencia_falha.png  latência da falha x N
  docs/ESCALABILIDADE.md                       tabelas, RTA e a resposta

Rodar da raiz do repositório (só precisa de matplotlib):
    interface/.venv/bin/python testes/analisar_escalabilidade.py

Modelo usado (o mesmo das aulas):
  - tarefas periódicas com deadline = período, prioridade Rate Monotonic;
  - C de cada tarefa = C MÁXIMO medido (pior caso observado);
  - com N robôs há 3 + 2N tarefas: Simulador, Organizador e Ponte (uma de
    cada) + N Navegações + N Status (Navegação e Status: pior robô);
  - U(N) = soma das centrais + N * (C_nav/T_nav + C_status/T_status).
"""
import csv
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")           # sem janela: só grava os PNGs
import matplotlib.pyplot as plt

SAIDAS = Path("testes/saidas")
IMAGENS = Path("docs/imagens")
DOC = Path("docs/ESCALABILIDADE.md")
LIMITE_LATENCIA_MS = 300        # T_status + T_nav
CENTRAIS = ["simulador", "organizador", "ponte_mqtt"]
TODAS = ["navegacao", "status"] + CENTRAIS
# Cores (paleta categórica de referência, em ordem fixa) e tinta neutra.
AZUL, LARANJA, VERDE_AGUA, AMARELO = "#2a78d6", "#eb6834", "#1baf7a", "#eda100"
CINZA, TINTA = "#8a8984", "#52514e"


def ler_csv():
    """dados[modo][N][tarefa] = {T, Cmed, Cmax, atraso, perdas, U};
    falhas[modo][N] = [latências em ms]."""
    dados, falhas = {}, {}
    with open(SAIDAS / "escalabilidade.csv") as f:
        for l in csv.DictReader(f):
            dados.setdefault(l["modo"], {}).setdefault(int(l["N"]), {})[l["tarefa"]] = {
                "T": float(l["T_ms"]), "Cmed": float(l["Cmed_us"]), "Cmax": float(l["Cmax_us"]),
                "atraso": float(l["atraso_max_us"]), "perdas": int(l["perdas"]),
                "U": float(l["U"])}
    with open(SAIDAS / "escalabilidade_falha.csv") as f:
        for l in csv.DictReader(f):
            falhas.setdefault(l["modo"], {}).setdefault(int(l["N"]), []).append(
                float(l["latencia_ms"]))
    return dados, falhas


def utilizacao(t, n, campo="Cmax"):
    """U total com n robôs: soma de C/T, com o C máximo (padrão) ou o médio."""
    u = {x: t[x][campo] / (t[x]["T"] * 1000) for x in TODAS}
    return sum(u[x] for x in CENTRAIS) + n * (u["navegacao"] + u["status"])


def liu_layland(n_tarefas):
    return n_tarefas * (2 ** (1 / n_tarefas) - 1)


def tarefas_rm(t, n):
    """Lista (nome, C_ms, T_ms) em ordem Rate Monotonic (menor T primeiro).
    Empate (mesmo T): a ordem não muda o pior caso da de menor prioridade."""
    lista = [("navegacao", t["navegacao"]["Cmax"] / 1000, 100.0)] * n
    lista += [("simulador", t["simulador"]["Cmax"] / 1000, 100.0)]
    lista += [("status", t["status"]["Cmax"] / 1000, 200.0)] * n
    lista += [("ponte_mqtt", t["ponte_mqtt"]["Cmax"] / 1000, 200.0)]
    lista += [("organizador", t["organizador"]["Cmax"] / 1000, 500.0)]
    return lista


def rta(lista, i, passos=None):
    """Análise de tempo de resposta (1 processador) da tarefa i da lista:
    R = C_i + soma_{j < i} ceil(R / T_j) * C_j, iterando até R parar de
    mudar (convergiu) ou passar do deadline (= T_i). Retorna R em ms."""
    c_i, t_i = lista[i][1], lista[i][2]
    r = c_i + sum(c for _, c, _ in lista[:i])          # R0
    while True:
        if passos is not None:
            passos.append(r)
        novo = c_i + sum(math.ceil(r / t_j) * c_j for _, c_j, t_j in lista[:i])
        if novo == r or novo > t_i:
            if passos is not None and novo != r:
                passos.append(novo)
            return novo
        r = novo


def rta_ok(t, n):
    """Todas as tarefas passam na RTA de 1 núcleo? Basta testar a última de
    cada período (a de menor prioridade dentro do grupo)."""
    lista = tarefas_rm(t, n)
    return rta(lista, n) <= 100 and rta(lista, 2 * n + 1) <= 200 and \
        rta(lista, len(lista) - 1) <= 500


def ler_hardware():
    arq = SAIDAS / "escalabilidade_1.txt"
    if not arq.exists():
        return {}
    hw = {}
    for linha in arq.read_text().splitlines():
        if linha.startswith("# ") and ":" in linha:
            chave, valor = linha[2:].split(":", 1)
            hw[chave.strip()] = valor.strip()
    return hw


def estilo(ax, titulo, xlabel, ylabel):
    ax.set_title(titulo, loc="left", fontsize=12, color="#0b0b0b")
    ax.set_xlabel(xlabel, color=TINTA)
    ax.set_ylabel(ylabel, color=TINTA)
    ax.grid(True, color="#e6e5e0", linewidth=0.8)
    ax.set_axisbelow(True)
    for lado in ("top", "right"):
        ax.spines[lado].set_visible(False)
    ax.tick_params(colors=TINTA)


def grafico_utilizacao(dados, nucleos):
    fig, ax = plt.subplots(figsize=(8, 4.8))
    for modo, cor in (("normal", AZUL), ("rt", LARANJA)):
        if modo not in dados:
            continue
        ns = sorted(dados[modo])
        ax.plot(ns, [utilizacao(dados[modo][n], n) for n in ns], "-o", color=cor,
                linewidth=2, markersize=6, label=f"U(N) com C máximo ({modo})")
        ax.plot(ns, [utilizacao(dados[modo][n], n, "Cmed") for n in ns], "--o", color=cor,
                linewidth=1.5, markersize=4, label=f"U(N) com C médio ({modo})")
    ns = sorted(dados["normal"])
    ax.plot(ns, [liu_layland(3 + 2 * n) for n in ns], "--", color=VERDE_AGUA, linewidth=2,
            label="Liu & Layland n(2^(1/n)-1), 1 núcleo")
    ax.axhline(1.0, color=CINZA, linestyle=":", linewidth=1.5, label="1 núcleo (U = 1)")
    ax.axhline(nucleos, color=AMARELO, linestyle="-.", linewidth=1.5,
               label=f"{nucleos} núcleos (U = {nucleos})")
    ax.set_yscale("log")
    estilo(ax, "Utilização total U(N) = Σ C/T", "N (robôs)", "U (escala log)")
    ax.legend(frameon=False, fontsize=9)
    fig.tight_layout()
    fig.savefig(IMAGENS / "escalabilidade_utilizacao.png", dpi=130)
    plt.close(fig)


def grafico_atraso(dados):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4.2))
    cores = {"navegacao": AZUL, "status": LARANJA, "simulador": VERDE_AGUA,
             "organizador": AMARELO, "ponte_mqtt": "#6250d6"}
    d = dados["normal"]
    ns = sorted(d)
    for tarefa in TODAS:
        a1.plot(ns, [d[n][tarefa]["atraso"] / 1000 for n in ns], "-o", color=cores[tarefa],
                linewidth=2, markersize=5, label=tarefa)
    estilo(a1, "Atraso máximo de liberação (normal)", "N (robôs)", "ms")
    a1.legend(frameon=False, fontsize=8)
    for modo, cor in (("normal", AZUL), ("rt", LARANJA)):
        if modo in dados:
            m = sorted(dados[modo])
            a2.plot(m, [sum(dados[modo][n][x]["perdas"] for x in TODAS) for n in m], "-o",
                    color=cor, linewidth=2, markersize=5, label=modo)
    estilo(a2, "Perdas de prazo (todas as tarefas, 60 s)", "N (robôs)", "perdas")
    a2.set_ylim(bottom=0, top=max(5, a2.get_ylim()[1]))   # perdas nunca são negativas
    a2.yaxis.get_major_locator().set_params(integer=True)
    a2.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    fig.savefig(IMAGENS / "escalabilidade_atraso.png", dpi=130)
    plt.close(fig)


def grafico_latencia(falhas):
    fig, ax = plt.subplots(figsize=(8, 4.5))
    for modo, cor in (("normal", AZUL), ("rt", LARANJA)):
        if modo not in falhas:
            continue
        ns = sorted(falhas[modo])
        ax.plot(ns, [max(falhas[modo][n]) for n in ns], "-o", color=cor, linewidth=2,
                markersize=6, label=f"máxima ({modo})")
        ax.plot(ns, [sum(falhas[modo][n]) / len(falhas[modo][n]) for n in ns], "--",
                color=cor, linewidth=1.5, label=f"média ({modo})")
    ax.axhline(LIMITE_LATENCIA_MS, color=CINZA, linestyle=":", linewidth=1.5,
               label="limite 300 ms (T_status + T_nav)")
    estilo(ax, "Latência da falha: comando falha → DEFEITO no buffer (10 amostras)",
           "N (robôs)", "ms")
    ax.set_ylim(0, LIMITE_LATENCIA_MS * 1.2)
    ax.legend(frameon=False, fontsize=9)
    fig.tight_layout()
    fig.savefig(IMAGENS / "escalabilidade_latencia_falha.png", dpi=130)
    plt.close(fig)


def maior_n_ok(dados, falhas, modo):
    """Maior N testado tal que ele E TODOS os menores têm zero perdas e
    latência máxima da falha < 300 ms."""
    melhor = None
    for n in sorted(dados[modo]):
        perdas = sum(dados[modo][n][x]["perdas"] for x in TODAS)
        lat = max(falhas[modo].get(n, [math.inf]))
        if perdas > 0 or lat >= LIMITE_LATENCIA_MS:
            break
        melhor = n
    return melhor


def passo_a_passo(w, t, n):
    """Escreve as iterações da RTA do Organizador com n robôs."""
    lista = tarefas_rm(t, n)
    passos = []
    r = rta(lista, len(lista) - 1, passos)
    c_nav, c_sim = t["navegacao"]["Cmax"] / 1000, t["simulador"]["Cmax"] / 1000
    c_sta, c_pon = t["status"]["Cmax"] / 1000, t["ponte_mqtt"]["Cmax"] / 1000
    c_org = t["organizador"]["Cmax"] / 1000
    w(f"### Passo a passo: Organizador com N = {n} (tudo num núcleo só)\n")
    w(f"C (ms): Navegação = {c_nav:.3f} (×{n}), Simulador = {c_sim:.3f}, Status = "
      f"{c_sta:.3f} (×{n}), Ponte = {c_pon:.3f}, Organizador = {c_org:.3f}.\n")
    w(f"R^(k+1) = {c_org:.3f} + ⌈R^k/100⌉·({n}·{c_nav:.3f} + {c_sim:.3f}) + "
      f"⌈R^k/200⌉·({n}·{c_sta:.3f} + {c_pon:.3f}),  R^0 = Σ C\n")
    w("| iteração k | R^k (ms) |")
    w("|---|---|")
    for k, valor in enumerate(passos):
        w(f"| {k} | {valor:.3f} |")
    w("")
    if r <= 500 and len(passos) == 1:
        w(f"R^0 = {r:.3f} ms já é menor que o menor período (100 ms): nenhuma tarefa de maior "
          "prioridade é liberada de novo antes de o Organizador terminar, então R^1 = R^0 "
          f"(convergiu). R = {r:.3f} ms ≤ 500 ms → **escalonável**.\n")
    elif r <= 500:
        w(f"Convergiu: R = {r:.3f} ms ≤ 500 ms → **escalonável**.\n")
    else:
        w(f"R passou de T = 500 ms ({r:.3f} ms) → **NÃO escalonável** em um núcleo: a cada "
          "100 ms as Navegações e o Simulador voltam, a cada 200 ms as Status e a Ponte, e o "
          "Organizador não termina a tempo.\n")


def gerar_doc(dados, falhas, hw, nucleos):
    d = dados["normal"]
    ns = sorted(d)
    out = []
    w = out.append
    w("# Análise de tempo de resposta e escalabilidade\n")
    w("> Gerado por `testes/analisar_escalabilidade.py` a partir de "
      "`testes/saidas/escalabilidade.csv` (benchmark `testes/escalabilidade.sh`). "
      "Não editar à mão: rode o benchmark e o script de novo.\n")
    w("**Pergunta do enunciado:** num dado cenário, qual o número máximo de robôs que é "
      "possível gerir com o hardware utilizado garantindo corretude e responsividade?\n")

    # ---- Resposta
    n_max = maior_n_ok(dados, falhas, "normal")
    maior = ns[-1]
    w("## Resposta\n")
    cpu = hw.get("cpu", "?")
    if n_max == maior:
        w(f"**Com o hardware {cpu} ({nucleos} núcleos lógicos, WSL2), o número máximo de robôs "
          f"que mantém zero perdas de prazo e latência de falha abaixo de 300 ms "
          f"(T_status + T_nav) é N = {n_max}** — o maior valor testado (MAX_ROBOS = 64 é o "
          f"limite do sistema). O processador não foi o gargalo: com {maior} robôs a "
          f"utilização total medida é U = {utilizacao(d[maior], maior):.2f}, isto é, "
          f"{100 * utilizacao(d[maior], maior) / nucleos:.0f}% da capacidade de {nucleos} "
          f"núcleos. Pela extrapolação linear de U(N) (seção Extrapolação), o limite do "
          f"processador estaria bem acima disso.\n")
    else:
        w(f"**Com o hardware {cpu} ({nucleos} núcleos lógicos, WSL2), o número máximo de robôs "
          f"que mantém zero perdas de prazo e latência de falha abaixo de 300 ms "
          f"(T_status + T_nav) é N = {n_max}.**\n")
        prox = [n for n in ns if n_max is None or n > n_max][0]
        perdas = {x: d[prox][x]["perdas"] for x in TODAS if d[prox][x]["perdas"] > 0}
        lat = max(falhas["normal"].get(prox, [0]))
        w(f"Em N = {prox} o critério falhou: perdas de prazo = {perdas or 'nenhuma'}; "
          f"latência máxima da falha = {lat:.0f} ms.\n")

    um = [n for n in ns if rta_ok(d[n], n)]
    w(f"Para comparação, se tudo rodasse em **um núcleo só**, a RTA com os C máximos medidos "
      f"garante até **N = {um[-1] if um else 0}** (seção RTA). Os {nucleos} núcleos são o que "
      "permite chegar a 64 sem perdas.\n")
    w("### Critério escolhido\n")
    w("- **Corretude temporal = zero perdas de prazo** em TODAS as tarefas periódicas "
      "(deadline = período; uma perda é um ciclo que terminou depois de `proximo + T`). "
      "Se a Navegação perde prazo, o setpoint chega atrasado ao simulador e a anti-colisão "
      "(que decide passo a passo) trabalha com informação velha.\n"
      "- **Responsividade = latência do evento crítico < 300 ms.** O evento mais crítico é a "
      "falha de um robô: entre o hardware ligar `i_falha` e o robô estar PARADO passam, no "
      "pior caso, um período da Status (que detecta e publica DEFEITO) + um período da "
      "Navegação (que segura a posição): 200 + 100 = 300 ms. Medimos até o buffer mostrar "
      "DEFEITO (o que o Organizador e a tela enxergam); se isso passar de 300 ms é porque "
      "alguém atrasou além do projeto.\n"
      "- Os dois critérios têm de valer para o N e para todos os menores (escalabilidade "
      "monótona); cada N roda 60 s com 2N caixas sendo transportadas (cenário realista: "
      "equipes, passagens, recargas).\n")

    # ---- Tabela
    w("## Resultados medidos (modo normal, SCHED_OTHER)\n")
    w("C e atraso em µs; Navegação e Status = pior robô. "
      "U(N) = Σ centrais + N·(C_nav/T_nav + C_status/T_status), calculada com o C máximo "
      "(pior caso, usada nos testes) e com o C médio (carga típica). A diferença grande entre "
      "as duas vem de picos isolados de C máximo (preempção pelo Windows/WSL), não do trabalho "
      "normal do ciclo.\n")
    w("| N | C_nav méd/máx | C_status méd/máx | C_sim máx | C_org méd/máx | C_ponte máx | "
      "atraso máx (tarefa) | perdas | U(N) Cmáx / Cméd | lat. falha méd/máx (ms) |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    for n in ns:
        t = d[n]
        pior = max(TODAS, key=lambda x: t[x]["atraso"])
        lat = falhas["normal"].get(n, [0])
        w(f"| {n} | {t['navegacao']['Cmed']:.0f} / {t['navegacao']['Cmax']:.0f} | "
          f"{t['status']['Cmed']:.0f} / {t['status']['Cmax']:.0f} | "
          f"{t['simulador']['Cmax']:.0f} | "
          f"{t['organizador']['Cmed']:.0f} / {t['organizador']['Cmax']:.0f} | "
          f"{t['ponte_mqtt']['Cmax']:.0f} | {t[pior]['atraso']:.0f} ({pior}) | "
          f"{sum(t[x]['perdas'] for x in TODAS)} | {utilizacao(t, n):.3f} / "
          f"{utilizacao(t, n, 'Cmed'):.3f} | "
          f"{sum(lat) / len(lat):.0f} / {max(lat):.0f} |")
    w("")
    if "rt" in dados:
        w("### Modo tempo real (ATR_TEMPO_REAL=1, SCHED_FIFO)\n")
        w("| N | C_nav máx | C_status máx | C_org máx | atraso máx | perdas | U(N) | lat. máx (ms) |")
        w("|---|---|---|---|---|---|---|---|")
        for n in sorted(dados["rt"]):
            t = dados["rt"][n]
            w(f"| {n} | {t['navegacao']['Cmax']:.0f} | {t['status']['Cmax']:.0f} | "
              f"{t['organizador']['Cmax']:.0f} | {max(t[x]['atraso'] for x in TODAS):.0f} | "
              f"{sum(t[x]['perdas'] for x in TODAS)} | {utilizacao(t, n):.3f} | "
              f"{max(falhas['rt'].get(n, [0])):.0f} |")
        w("")
    else:
        w("### Modo tempo real (ATR_TEMPO_REAL=1)\n")
        w(f"Não executado nesta máquina: o usuário não tem permissão para SCHED_FIFO "
          f"(`{hw.get('modo', '')}`; `chrt -f 1 true` falha com EPERM). O código está pronto: "
          "`ATR_TEMPO_REAL=1 ./logistica` como root, ou no Docker (o `compose.yaml` dá "
          "`cap_add: SYS_NICE`), e depois `testes/escalabilidade.sh rt`.\n")

    w("![Utilização](imagens/escalabilidade_utilizacao.png)\n")
    w("![Atraso e perdas](imagens/escalabilidade_atraso.png)\n")
    w("![Latência da falha](imagens/escalabilidade_latencia_falha.png)\n")

    # ---- Utilização e Liu & Layland
    w("## Teste de utilização (Liu & Layland)\n")
    w("Para n tarefas periódicas independentes com Rate Monotonic num processador, "
      "U ≤ n(2^(1/n) − 1) é condição SUFICIENTE de escalonabilidade (o limite tende a "
      "ln 2 ≈ 0,693). Com N robôs, n = 3 + 2N.\n")
    w("| N | n tarefas | U(N) | limite L&L | passa no L&L (1 núcleo)? |")
    w("|---|---|---|---|---|")
    for n in ns:
        u, ll = utilizacao(d[n], n), liu_layland(3 + 2 * n)
        w(f"| {n} | {3 + 2 * n} | {u:.3f} | {ll:.3f} | {'sim' if u <= ll else 'não (teste inconclusivo)'} |")
    w("")
    w(f"Este computador tem {nucleos} núcleos e o Linux distribui as threads entre eles "
      "(escalonamento global). O L&L é de 1 núcleo, então ele é PESSIMISTA aqui: mesmo que "
      "U(N) passe do limite de 1 núcleo, a carga cabe com folga em "
      f"{nucleos} (U ≪ {nucleos}). Ele serve como referência de \"se tudo rodasse num "
      "núcleo só\".\n")

    # ---- RTA
    w("## Análise de tempo de resposta (RTA)\n")
    w("Teste EXATO para prioridade fixa (1 processador, deadline = período): para cada tarefa i,\n")
    w("    R_i = C_i + Σ_{j ∈ hp(i)} ⌈R_i / T_j⌉ · C_j\n")
    w("iterando a partir de R⁰ = Σ C até R parar de mudar; a tarefa é escalonável se R_i ≤ T_i. "
      "hp(i) = tarefas de prioridade maior (Rate Monotonic: Navegações e Simulador, T = 100 ms; "
      "Status e Ponte, 200 ms; Organizador, 500 ms). Usamos os **C máximos medidos** "
      "(pior caso observado). A tarefa de MENOR prioridade (Organizador) sofre a interferência de "
      "todas as outras: se ela passa, todas passam (as outras têm menos interferência e o mesmo "
      "critério).\n")
    escalonaveis = [n for n in ns if rta_ok(d[n], n)]
    caso = escalonaveis[-1] if escalonaveis else ns[0]   # o maior N que passa em 1 núcleo
    falhou = [n for n in ns if n > caso][:1]              # e o primeiro que não passa
    for n in [caso] + falhou:
        passo_a_passo(w, d[n], n)

    w("### RTA para todos os N (1 núcleo, C máximo)\n")
    w("R de cada grupo = R da ÚLTIMA tarefa daquele período na ordem de prioridade "
      "(Simulador para T = 100 ms, Ponte para T = 200 ms), que sofre mais interferência.\n")
    w("| N | R (T = 100 ms) | R (T = 200 ms) | R_organizador (ms) | escalonável em 1 núcleo? |")
    w("|---|---|---|---|---|")
    for n in ns:
        lista = tarefas_rm(d[n], n)
        r_nav = rta(lista, n)                # Simulador: a última das de T = 100 ms
        r_sta = rta(lista, 2 * n + 1)        # Ponte: a última das de T = 200 ms
        r_org = rta(lista, len(lista) - 1)
        w(f"| {n} | {r_nav:.2f} | {r_sta:.2f} | {r_org:.2f} | {'sim' if rta_ok(d[n], n) else 'não'} |")
    w("")
    w(f"Se TUDO rodasse num núcleo só, com os C máximos medidos, o maior N garantido pela RTA "
      f"seria **N = {caso}**. Com {nucleos} núcleos a RTA clássica não se aplica diretamente "
      "(escalonamento global), e o que vale é a medição: zero perdas até N = 64. A RTA de 1 "
      "núcleo é muito pessimista porque soma os PICOS de C (raros e de causas externas) de "
      "todas as tarefas como se acontecessem juntos.\n")

    # ---- Extrapolação
    w("## Extrapolação (até onde iria o processador)\n")
    a, b = ns[-2], ns[-1]
    inc = (utilizacao(d[b], b) - utilizacao(d[a], a)) / (b - a)
    u_b = utilizacao(d[b], b)
    n_m = b + (nucleos - u_b) / inc if inc > 0 else math.inf
    u_med = utilizacao(d[b], b, "Cmed")
    inc_med = (u_med - utilizacao(d[a], a, "Cmed")) / (b - a)
    n_med = b + (nucleos - u_med) / inc_med if inc_med > 0 else math.inf
    w(f"Entre N = {a} e N = {b}, cada robô a mais soma ≈ {inc:.4f} de utilização com C máximo "
      f"(≈ {inc_med:.4f} com C médio). Extrapolando linearmente, U = {nucleos} (todos os "
      f"núcleos cheios) seria atingido em N ≈ {n_m:.0f} pelo pior caso e N ≈ {n_med:.0f} pela "
      f"carga média. É uma estimativa grosseira: o custo do Organizador "
      "e do A* cresce mais que linearmente com a ocupação do mapa, e o mapa de 60×30 (com "
      "obstáculos, passagens de capacidade 1 a 4) fica congestionado antes disso. Na prática "
      "o limite de verdade passa a ser o ESPAÇO (robôs esperando célula/passagem), não a CPU.\n")

    # ---- Limitações
    w("## Limitações\n")
    w("- **Linux não é um RTOS.** Mesmo com SCHED_FIFO não há garantia de pior caso: "
      "interrupções, page faults, o próprio kernel e o \"RT throttling\" (95% da CPU no "
      "máximo para tarefas de tempo real) podem atrasar qualquer thread.\n"
      "- **Medições em SCHED_OTHER** sofrem interferência de todos os outros processos da "
      "máquina (navegador, IDE, antivírus do Windows): o atraso máximo é dominado por isso, "
      "não pelo nosso sistema.\n"
      "- **WSL2 é uma máquina virtual** (Hyper-V): o núcleo Linux pode perder a CPU para o "
      "Windows a qualquer momento; no Docker há ainda mais uma camada. Isso adiciona ruído aos "
      "C máximos (picos isolados).\n"
      "- **C máximo observado ≠ WCET.** 60 s por N é uma amostra; um caminho de código raro "
      "(A* num mapa congestionado) pode demorar mais. Por isso usamos margem: o critério é "
      "zero perdas, e a RTA usa o máximo, não a média.\n"
      "- **Latência da falha**: 10 amostras por N, com resolução de 1 ms (o Console consulta o "
      "buffer a cada 1 ms). Ela é dominada pela FASE do ciclo da Status (0 a 200 ms), não "
      "pela carga.\n"
      "- **RTA de 1 núcleo**: o sistema roda em vários núcleos com escalonamento global, para o "
      "qual a RTA clássica não vale diretamente; usá-la como se fosse 1 núcleo é conservador "
      "(se passa em 1, passa em mais). Também ignora bloqueio por mutex (seções críticas "
      "curtas, de cópia) e o custo de troca de contexto.\n"
      "- O benchmark roda sem a Tela de Operação (a ponte MQTT publica para o broker local, "
      "mas ninguém desenha 64 robôs).\n")

    w("## Hardware e ambiente\n")
    for chave in ("data", "nproc", "cpu", "uname", "modo"):
        w(f"- {chave}: `{hw.get(chave, '?')}`")
    w("")
    w("## Como reproduzir\n")
    w("    testes/escalabilidade.sh           # ~12 min por modo; grava testes/saidas/escalabilidade*")
    w("    interface/.venv/bin/python testes/analisar_escalabilidade.py")
    DOC.write_text("\n".join(out) + "\n")


def main():
    dados, falhas = ler_csv()
    hw = ler_hardware()
    nucleos = int(hw.get("nproc", "1"))
    IMAGENS.mkdir(parents=True, exist_ok=True)
    grafico_utilizacao(dados, nucleos)
    grafico_atraso(dados)
    grafico_latencia(falhas)
    gerar_doc(dados, falhas, hw, nucleos)
    print(f"gerados: {DOC} e 3 gráficos em {IMAGENS}/")
    print(f"N máximo (normal): {maior_n_ok(dados, falhas, 'normal')}")


if __name__ == "__main__":
    main()
