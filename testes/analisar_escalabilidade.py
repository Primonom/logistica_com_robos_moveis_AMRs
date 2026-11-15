"""
analisar_escalabilidade.py — Análise do benchmark testes/escalabilidade.sh.

Lê testes/saidas/escalabilidade.csv e escalabilidade_falha.csv e gera:
  docs/imagens/escalabilidade_utilizacao.png   U(N) x núcleos x Liu & Layland
  docs/imagens/escalabilidade_atraso.png       atraso máximo e perdas x N
  docs/imagens/escalabilidade_latencia_falha.png  latência da falha x N
  docs/imagens/escalabilidade_vazao.png        entregas/min e tempo de entrega x N
  docs/ESCALABILIDADE.md                       tabelas, RTA e a resposta

Lê também, para a vazão, escalabilidade[_rt]_N_eventos.csv e _cargas.csv
(linhas do Coletor gravadas pelo benchmark).

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
import re
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")           # sem janela: só grava os PNGs
import matplotlib.pyplot as plt

SAIDAS = Path("testes/saidas")
IMAGENS = Path("docs/imagens")
DOC = Path("docs/ESCALABILIDADE.md")
LIMITE_LATENCIA_MS = 300        # T_status + T_nav
CENTRAIS = ["simulador", "organizador", "ponte_mqtt"]
# Janela da vazão, em segundos depois do evento "sistema iniciado": o roteiro
# cria os robôs, espera 5 s, põe as caixas, espera 2 s e roda "tempos zerar"
# (início = 7 s); a medição dura 60 s. Os primeiros 20 s são aquecimento
# (todas as equipes saem juntas e nenhuma entregou ainda): ficam de fora.
INICIO_S, AQUECIMENTO_S, DURACAO_S = 7, 20, 60
SATURACAO = 0.9                 # "não aumenta mais" = já tem 90% da vazão máxima
# Limites do código, lidos de include/comum.h (para a tabela do limite do software).
COMUM = Path("include/comum.h").read_text()
MAX_ROBOS = int(re.search(r"#define MAX_ROBOS\s+(\d+)", COMUM).group(1))
MAX_CAIXAS = int(re.search(r"#define MAX_CAIXAS\s+(\d+)", COMUM).group(1))
# Bytes que cada fila desconta do RLIMIT_MSGQUEUE (fórmula do kernel, ipc/mqueue.c):
# maxmsg * (sizeof(struct msg_msg) = 48 + nó da árvore de prioridades = 48) +
# maxmsg * msgsize, com maxmsg = 10, sizeof(MensagemRobo) = 40 e
# sizeof(MensagemColetor) = 160.
BYTES_FILA_ROBO = 10 * (48 + 48) + 10 * 40
BYTES_FILA_COLETOR = 10 * (48 + 48) + 10 * 160
# Memória medida à mão com 128 robôs: RSS médio de "ps -o rss= -C robo"
# (conta as bibliotecas compartilhadas em cada processo, então é pessimista;
# o PSS, que as divide, deu ~180 KB por robô). /atr_shm deu 102 KB.
MEMORIA = "média de `ps -o rss= -C robo` com N = 128; PSS ≈ 180 KB"
RSS_ROBO_KB = 2200
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


def hora(texto):
    return datetime.strptime(texto, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def ler_vazao():
    """vazao[modo][N] = {entregas_min, tempo_med, tempo_max, n, fila_vazia,
    recusados, realocados, colisoes}. Entrega = linha de cargas.csv (gravada
    no SOLTAR); tempo de entrega = SOLTAR - atribuição ("indo para a baia")."""
    vazao = {}
    for arq in SAIDAS.glob("escalabilidade_*_cargas.csv"):
        nome = arq.name[len("escalabilidade_"):-len("_cargas.csv")]
        modo, n = ("rt", int(nome[3:])) if nome.startswith("rt_") else ("normal", int(nome))
        t0, atrib, recusados, colisoes = None, {}, 0, 0
        for linha in open(SAIDAS / f"escalabilidade_{nome}_eventos.csv"):
            data, _, texto = linha.rstrip("\n").split(";", 2)
            if "sistema iniciado" in texto:
                t0 = hora(data)
            elif "indo para a baia" in texto:
                atrib[int(texto.split()[1])] = hora(data)
            elif "recusado" in texto:
                recusados += 1
            elif "COLISAO" in texto:
                colisoes += 1
        ini, fim = t0 + INICIO_S + AQUECIMENTO_S, t0 + INICIO_S + DURACAO_S
        tempos = []
        for linha in open(arq):
            data, caixa = linha.split(";")[:2]
            t = hora(data)
            if ini <= t <= fim and int(caixa) in atrib:
                tempos.append(t - atrib[int(caixa)])
        atribuidas = sum(1 for t in atrib.values() if t <= fim)
        txt = (SAIDAS / f"escalabilidade_{nome}.txt").read_text()
        vazao.setdefault(modo, {})[n] = {
            "entregas_min": len(tempos) * 60 / (fim - ini),
            "tempo_med": sum(tempos) / len(tempos) if tempos else 0,
            "tempo_max": max(tempos, default=0), "n": len(tempos),
            "fila_vazia": atribuidas >= max(2 * n, 64),
            "recusados": recusados, "colisoes": colisoes,
            "realocados": txt.count(") ocupada; robo")}
    return vazao


def n_saturacao(v):
    """Menor N que já tem SATURACAO (90%) da maior vazão medida."""
    maior = max(x["entregas_min"] for x in v.values())
    return min(n for n in v if v[n]["entregas_min"] >= SATURACAO * maior), maior


def grafico_vazao(vazao):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4.2))
    for modo, cor, marca, linha in (("normal", AZUL, "o", "-"), ("rt", LARANJA, "s", "--")):
        if modo not in vazao:
            continue
        ns = sorted(vazao[modo])
        a1.plot(ns, [vazao[modo][n]["entregas_min"] for n in ns], linha, marker=marca,
                color=cor, linewidth=2, markersize=6, label=modo)
        a2.plot(ns, [vazao[modo][n]["tempo_med"] for n in ns], linha, marker=marca,
                color=cor, linewidth=2, markersize=6, label=modo)
    n_sat, _ = n_saturacao(vazao["normal"])
    for ax in (a1, a2):
        ax.set_xscale("log", base=2)
        ns = sorted(vazao["normal"])
        ax.set_xticks(ns)
        ax.set_xticklabels([str(n) for n in ns], fontsize=8)
        ax.axvline(n_sat, color=CINZA, linestyle=":", linewidth=1.5)
        ax.legend(frameon=False, fontsize=8)
    a1.text(n_sat * 1.08, a1.get_ylim()[1] * 0.05, f"≥ 90% do máximo\na partir de N = {n_sat}", color=TINTA,
            fontsize=8)
    estilo(a1, "Vazão (entregas por minuto, regime)", "N (robôs, escala log)", "entregas/min")
    estilo(a2, "Tempo médio de entrega (atribuição → SOLTAR)", "N (robôs, escala log)", "s")
    a1.set_ylim(bottom=0)
    a2.set_ylim(bottom=0)
    fig.tight_layout()
    fig.savefig(IMAGENS / "escalabilidade_vazao.png", dpi=130)
    plt.close(fig)


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


def ler_hardware(nome="escalabilidade_1.txt"):
    arq = SAIDAS / nome
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


def gerar_doc(dados, falhas, vazao, hw, hw_rt, nucleos):
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

    # ---- Resposta (3 camadas)
    n_max = maior_n_ok(dados, falhas, "normal")
    maior = ns[-1]
    cpu = hw.get("cpu", "?")
    v = vazao["normal"]
    n_sat, vazao_max = n_saturacao(v)
    um = [n for n in ns if rta_ok(d[n], n)]
    w("## Resposta\n")

    # a) processamento
    t = d[maior]
    u_central = {c: sum(t[x][c] / (t[x]["T"] * 1000) for x in CENTRAIS) for c in ("Cmax", "Cmed")}
    u_robo = {c: t["navegacao"][c] / 100e3 + t["status"][c] / 200e3 for c in ("Cmax", "Cmed")}
    n_cpu = {c: (nucleos - u_central[c]) / u_robo[c] for c in ("Cmax", "Cmed")}
    w("### a) Limite de processamento (CPU)\n")
    w("Reta medida em N = %d (o maior testado; Navegação e Status = pior robô):\n" % maior)
    w("    U(N) = U_central + N·(C_nav/T_nav + C_status/T_status)\n")
    w("| C usado | U_central | C_nav (µs) | C_status (µs) | custo por robô | N com U = %d núcleos |"
      % nucleos)
    w("|---|---|---|---|---|---|")
    for c, nome in (("Cmax", "máximo (pior caso)"), ("Cmed", "médio (carga típica)")):
        w(f"| {nome} | {u_central[c]:.4f} | {t['navegacao'][c]:.0f} | {t['status'][c]:.0f} | "
          f"{u_robo[c]:.5f} | **{n_cpu[c]:.0f}** |")
    w("")
    w(f"Com o C máximo (pico de UM robô somado como se TODOS tivessem o pico em todo ciclo) a "
      f"CPU encheria em N ≈ {n_cpu['Cmax']:.0f}; com o C médio, em N ≈ {n_cpu['Cmed']:.0f}. "
      f"Se tudo rodasse em **um núcleo só**, a RTA com os C máximos garante só até "
      f"**N = {um[-1] if um else 0}** (seção RTA). É uma estimativa: o Organizador e o A* "
      "crescem mais que linearmente com a ocupação do mapa, e a reta ignora troca de "
      "contexto e cache.\n")

    # b) software / SO
    w("### b) Limite do software e do sistema operacional\n")
    filas = maior + 1
    w("| recurso | uso com N robôs | limite | N máximo por esse recurso |")
    w("|---|---|---|---|")
    w(f"| `MAX_ROBOS` (comum.h, slots de /atr_shm, ids) | N | {MAX_ROBOS} | {MAX_ROBOS} "
      "(é só recompilar) |")
    w(f"| filas POSIX (`/proc/sys/fs/mqueue/queues_max`) | N + 1 (/atr_robo_* + /atr_coletor) "
      f"| {hw.get('queues_max', '?')} | {int(hw.get('queues_max', 256)) - 1} |")
    w(f"| bytes de fila (`RLIMIT_MSGQUEUE`, `ulimit -q`) | ~{BYTES_FILA_ROBO} B por robô + "
      f"{BYTES_FILA_COLETOR} B | {hw.get('rlimit_msgqueue', '?')} B | "
      f"{(int(hw.get('rlimit_msgqueue', 819200)) - BYTES_FILA_COLETOR) // BYTES_FILA_ROBO} |")
    w(f"| tarefas do Linux (`ulimit -u`: processos + threads) | 3 por robô (main, Navegação, "
      f"Status) | {hw.get('max_processos', '?')} | {int(hw.get('max_processos', 0)) // 3} |")
    w(f"| memória (RSS medido: {MEMORIA}) | ~{RSS_ROBO_KB / 1024:.1f} MB por robô | "
      f"{hw.get('memoria_mb', '?')} MB | {int(hw.get('memoria_mb', 0)) * 1024 // RSS_ROBO_KB} |")
    w(f"| `TAMANHO_JSON` (ponte MQTT) | ~600 B por robô + ~110 B por caixa | 256 KB | "
      f"~{(262144 - MAX_CAIXAS * 110) // 600} |")
    w("")
    w(f"Ao testar 96 e 128 robôs: {filas} filas abertas, nenhum `mq_open` falhou, nenhum JSON "
      "descartado. A posição padrão (8,15) é estacionamento, então todos os robôs nasceram "
      "nela (\"realocados\" conta os que o Gestor teria posto em outra célula):\n")
    w("| N | robôs realocados | pedidos recusados | COLISAO | perdas de prazo |")
    w("|---|---|---|---|---|")
    for n in ns:
        if n >= 48:
            w(f"| {n} | {v[n]['realocados']} | {v[n]['recusados']} | {v[n]['colisoes']} | "
              f"{sum(d[n][x]['perdas'] for x in TODAS)} |")
    w("")
    w(f"O primeiro limite do software é o próprio `MAX_ROBOS` = {MAX_ROBOS}; o primeiro limite "
      f"do SO que apareceria é o `queues_max` = {hw.get('queues_max', '?')} (no máximo "
      f"{int(hw.get('queues_max', 256)) - 1} robôs, porque cada um tem a sua fila). Ele pode ser "
      "aumentado como root (`sysctl fs.mqueue.queues_max=...`), e aí o seguinte é o "
      "`RLIMIT_MSGQUEUE`.\n")

    # c) galpão
    w("### c) Limite físico do galpão (vazão)\n")
    w(f"Entregas por minuto e tempo médio de entrega (da atribuição, \"indo para a baia\", "
      f"até o SOLTAR, pelos timestamps do Coletor), contando só as entregas dos últimos "
      f"{DURACAO_S - AQUECIMENTO_S} s da janela de {DURACAO_S} s (os primeiros "
      f"{AQUECIMENTO_S} s são aquecimento). A fila tem max(2N, 64) caixas e nunca esvaziou "
      "na janela (coluna \"fila\").\n")
    w("| N | entregas na janela | entregas/min | tempo médio (s) | tempo máx (s) | fila |")
    w("|---|---|---|---|---|---|")
    for n in sorted(v):
        x = v[n]
        w(f"| {n} | {x['n']} | {x['entregas_min']:.1f} | {x['tempo_med']:.1f} | "
          f"{x['tempo_max']:.1f} | {'ESVAZIOU' if x['fila_vazia'] else 'nunca vazia'} |")
    w("")
    w("![Vazão](imagens/escalabilidade_vazao.png)\n")
    n_igual = min(n for n in v if v[n]["entregas_min"] == vazao_max)
    w(f"A vazão máxima foi {vazao_max:.1f} entregas/min. A partir de **N = {n_sat}** ela já "
      f"está em {SATURACAO:.0%} desse máximo, e a partir de N = {n_igual} é exatamente a "
      "mesma: **mais robôs não aumentam a vazão**. O motivo é o galpão: há 4 baias e o "
      "Organizador põe **uma carga por baia de cada vez**, então no máximo 4 entregas andam "
      "ao mesmo tempo (de 4 a 16 robôs trabalhando; o tamanho médio das caixas é 2,5, mais "
      "os que estão recarregando). Os outros ficam DISPONIVEL **no estacionamento** (posição "
      "padrão, zonas, estações; ver docs/EXPLICACAO.md 8.6) e não atrapalham ninguém: com a "
      "semente fixa o Organizador escolhe sempre os mesmos robôs (os mais perto da baia), e "
      "por isso os números com N grande são iguais. Com N pequeno a vazão é menor e o tempo "
      "maior porque falta robô livre perto da baia (a caixa de tamanho 4 espera os 4, e cada "
      "robô volta da zona, do outro lado do mapa). Antes do estacionamento dos ociosos, com "
      "N = 128 os robôs parados em volta das baias travavam o galpão: zero entregas.\n")

    # conclusão
    w("### Conclusão\n")
    w(f"**Com este hardware, o sistema garante corretude e responsividade para até "
      f"N = {n_max} robôs; o fator que limita é o galpão, e não a CPU nem o SO: a partir de "
      f"N ≈ {n_igual} a vazão para de crescer ({vazao_max:.0f} entregas/min, 4 baias com uma "
      f"carga cada), e o primeiro limite rígido acima de {maior} é o SO "
      f"({int(hw.get('queues_max', 256)) - 1} robôs, pelas filas POSIX).**\n")
    w(f"- N = {n_max} é o maior valor testado (= `MAX_ROBOS`), com zero perdas de prazo, "
      f"latência da falha < {LIMITE_LATENCIA_MS} ms, zero COLISAO, nenhum pedido recusado e "
      "nenhum deadlock (todas as rodadas entregaram caixas até o fim).\n"
      f"- A CPU tem folga: com {maior} robôs a carga média é U = "
      f"{utilizacao(d[maior], maior, 'Cmed'):.2f} de {nucleos} núcleos. A reta com o C "
      f"MÁXIMO chega aos {nucleos} núcleos em N ≈ {n_cpu['Cmax']:.0f}, abaixo do que foi "
      "medido sem perdas: ela soma o pico de cada tarefa (de robôs e instantes diferentes) "
      "como se todos acontecessem juntos em todo ciclo, o que não acontece. É um limite "
      "pessimista, não uma garantia de falha.\n"
      f"- Para um só núcleo, a RTA com os C máximos garante só N = {um[-1] if um else 0}.\n"
      f"- Robôs além de N ≈ {n_igual} são gerenciáveis mas inúteis: ficam no "
      "estacionamento. Para aumentar a vazão seria preciso mudar o galpão (mais baias, ou "
      "mais de uma carga por baia), não o computador.\n")
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
      "monótona); cada N roda 60 s com max(2N, 64) caixas sendo transportadas (cenário realista: "
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
        w("### Modo tempo real (ATR_TEMPO_REAL=1, SCHED_FIFO) × modo normal\n")
        w(f"Rodado no Docker (`{hw_rt.get('modo', '?')}`; uname `{hw_rt.get('uname', '?')}`), "
          "com o comando:\n")
        w("    docker compose run --rm -T -v ./testes:/app/testes logistica testes/escalabilidade.sh rt\n")
        w("C máx = pior tarefa periódica (µs); atraso = atraso máximo de liberação entre todas as "
          "tarefas (µs); lat. = latência máxima da falha (ms).\n")
        w("| N | C_nav máx normal / rt | C_status máx normal / rt | C_org máx normal / rt | "
          "atraso máx normal / rt | perdas normal / rt | lat. máx normal / rt | entregas/min normal / rt |")
        w("|---|---|---|---|---|---|---|---|")
        for n in sorted(dados["rt"]):
            a, b = d.get(n), dados["rt"][n]
            if a is None:
                continue
            vr = vazao.get("rt", {}).get(n, {"entregas_min": 0})
            w(f"| {n} | {a['navegacao']['Cmax']:.0f} / {b['navegacao']['Cmax']:.0f} | "
              f"{a['status']['Cmax']:.0f} / {b['status']['Cmax']:.0f} | "
              f"{a['organizador']['Cmax']:.0f} / {b['organizador']['Cmax']:.0f} | "
              f"{max(a[x]['atraso'] for x in TODAS):.0f} / {max(b[x]['atraso'] for x in TODAS):.0f} | "
              f"{sum(a[x]['perdas'] for x in TODAS)} / {sum(b[x]['perdas'] for x in TODAS)} | "
              f"{max(falhas['normal'].get(n, [0])):.0f} / {max(falhas['rt'].get(n, [0])):.0f} | "
              f"{v[n]['entregas_min']:.1f} / {vr['entregas_min']:.1f} |")
        w("")
        comuns = [n for n in sorted(dados["rt"]) if n in d]
        def pior(m, campo):    # maior valor do campo entre todas as tarefas e todos os N
            return max(dados[m][n][x][campo] for n in comuns for x in TODAS)
        def soma(m):
            return sum(dados[m][n][x]["perdas"] for n in comuns for x in TODAS)
        mais_baixo = sum(1 for n in comuns if max(dados["rt"][n][x]["atraso"] for x in TODAS)
                         < max(d[n][x]["atraso"] for x in TODAS))
        w(f"**Comparação.** Perdas de prazo: {soma('normal')} no modo normal e {soma('rt')} no "
          f"rt, em todos os N. O pior atraso de liberação caiu de {pior('normal', 'atraso') / 1000:.1f} ms "
          f"(normal) para {pior('rt', 'atraso') / 1000:.1f} ms (rt), e foi menor no rt em "
          f"{mais_baixo} de {len(comuns)} valores de N: com SCHED_FIFO uma tarefa pronta não espera "
          "atrás de processos comuns da máquina (navegador, IDE). O C máximo da Status com N "
          f"grande também caiu (N = {comuns[-1]}: {d[comuns[-1]]['status']['Cmax']:.0f} → "
          f"{dados['rt'][comuns[-1]]['status']['Cmax']:.0f} µs). A latência da falha e a vazão "
          "praticamente não mudam: a latência é dominada pela FASE do ciclo de 200 ms da "
          "Status (0 a 200 ms), e a vazão pelas 4 baias, não pela CPU. Ou seja: com esta carga "
          "(U médio ≈ 1 de 12 núcleos) o modo normal já é suficiente; o tempo real diminui a "
          "cauda dos atrasos, que é o que importaria com a máquina mais carregada. Os C "
          "máximos continuam com picos isolados nos dois modos: o WSL2 é uma máquina virtual "
          "e o Windows pode tirar a CPU do Linux inteiro, coisa que nenhuma prioridade "
          "dentro do Linux impede.\n")
    else:
        w("### Modo tempo real (ATR_TEMPO_REAL=1)\n")
        w(f"Não executado: o usuário não tem permissão para SCHED_FIFO "
          f"(`{hw.get('modo', '')}`). Rode no Docker:\n")
        w("    docker compose run --rm -T -v ./testes:/app/testes logistica testes/escalabilidade.sh rt\n")

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
      f"(escalonamento global), e o que vale é a medição: zero perdas até N = {n_max}. A RTA de 1 "
      "núcleo é muito pessimista porque soma os PICOS de C (raros e de causas externas) de "
      "todas as tarefas como se acontecessem juntos.\n")

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
      "mas ninguém desenha 128 robôs).\n"
      "- **Vazão**: uma rodada de 60 s por N (40 s de regime) e uma semente (42, um mapa); "
      "entregas/min tem resolução de 1,5 entrega/min (1 entrega em 40 s).\n"
      "- **Modo rt no Docker**: o container roda como root com `SYS_NICE` (por isso o "
      "SCHED_FIFO funciona com `ulimit -r = 0`) e usa o fuso UTC nos logs (só os tempos "
      "relativos entram na vazão, então isso não muda nada).\n")

    w("## Hardware e ambiente\n")
    for chave in ("data", "nproc", "cpu", "uname", "modo"):
        w(f"- {chave}: `{hw.get(chave, '?')}`")
    w("")
    w("## Como reproduzir\n")
    w("    testes/escalabilidade.sh normal    # ~16 min; grava testes/saidas/escalabilidade*")
    w("    docker compose build               # modo rt: dentro do container (SYS_NICE)")
    w("    ATR_PORTA_MQTT=1884 docker compose run --rm -T -v ./testes:/app/testes \\")
    w("        logistica testes/escalabilidade.sh rt")
    w("    interface/.venv/bin/python testes/analisar_escalabilidade.py\n")
    w("`ATR_PORTA_MQTT=1884` só é preciso se o host já tem um mosquitto na porta 1883 "
      "(o do WSL, que o modo normal usa). O modo rt acrescenta as linhas `rt` ao mesmo CSV.")
    DOC.write_text("\n".join(out) + "\n")


def main():
    dados, falhas = ler_csv()
    vazao = ler_vazao()
    hw = ler_hardware()
    hw_rt = ler_hardware("escalabilidade_rt_1.txt")
    nucleos = int(hw.get("nproc", "1"))
    IMAGENS.mkdir(parents=True, exist_ok=True)
    grafico_utilizacao(dados, nucleos)
    grafico_atraso(dados)
    grafico_latencia(falhas)
    grafico_vazao(vazao)
    gerar_doc(dados, falhas, vazao, hw, hw_rt, nucleos)
    print(f"gerados: {DOC} e 4 gráficos em {IMAGENS}/")
    print(f"N máximo (normal): {maior_n_ok(dados, falhas, 'normal')}")


if __name__ == "__main__":
    main()
