#!/usr/bin/env bash
# Apenas exibe logs existentes no terminal para registrar prints reais.
set -euo pipefail
cd "$(dirname "$0")/.."
clear
if [ "${1:-}" = ambiente ]; then
    echo 'M3 - Compilacao, validacao e permissao de tempo real'
    gcc --version | head -1
    echo 'gcc -std=c11 -O2 -Wall -Wextra -Werror ... -pthread'
    echo 'Compilacao concluida sem avisos; detalhes em execucao.txt'
    sed -n '/=== Testes de parametros/,/=== Autorizar/p' data/m3/medicoes/execucao.txt
elif [ "${1:-}" = original ]; then
    echo 'M3 - Exemplo original do professor - execucao de 10 segundos'
    head -12 data/m3/medicoes/exemplo-viel.txt
    echo '...'
    tail -8 data/m3/medicoes/exemplo-viel.txt
else
    echo "M3 - Resultados reais - configuracao $1 - cenario $2"
    echo 'Tres repeticoes independentes de 6 segundos'
    for rep in 1 2 3; do
        file="data/m3/medicoes/$1-$2-r$rep.csv"
        echo
        echo "REPETICAO $rep"
        head -1 "$file"
        tail -1 "$file"
    done
    echo
    echo 'WCRT, misses e orcamento: consultar data/m3/resumo.csv'
fi
