#!/bin/bash
# empacotar.sh — Gera o ZIP da entrega no Moodle: ATR_TrabalhoFinal.zip.
#
# Conteúdo: tudo o que está versionado no git (código C e Python, Makefile,
# Docker, config, docs com os PDFs, testes e as saídas em testes/saidas).
# Ficam de fora: .git, executáveis e objetos (logistica, robo, *.o), o
# ambiente Python (interface/.venv), logs/, __pycache__ e arquivos não
# versionados. Por isso os PDFs (docs/*.pdf) precisam estar commitados.
#
# Usa "git archive" (já respeita o que é versionado), então não precisa do
# programa zip. Uso (de qualquer pasta): ./empacotar.sh

cd "$(dirname "$0")" || exit 1   # raiz do repositório
SAIDA=ATR_TrabalhoFinal.zip

if [ -n "$(git status --porcelain -- . ':!ATR_TrabalhoFinal.zip' ':!Claude outputs')" ]; then
    echo "AVISO: há mudanças não commitadas; o ZIP leva só o que está no último commit."
fi
for pdf in docs/RELATORIO.pdf docs/INSTRUCOES.pdf; do
    git ls-files --error-unmatch "$pdf" > /dev/null 2>&1 ||
        echo "AVISO: $pdf não está commitado (gere com: interface/.venv/bin/python docs/gerar_pdf.py)"
done

rm -f "$SAIDA"
git archive --format=zip --prefix=ATR_TrabalhoFinal/ -o "$SAIDA" HEAD || exit 1
echo "Gerado $SAIDA ($(du -h "$SAIDA" | cut -f1)), $(git ls-files | wc -l) arquivos."
