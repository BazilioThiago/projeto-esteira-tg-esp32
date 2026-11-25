#!/usr/bin/env bash
# Rodar de dentro da pasta compartilhada: bash m3/run_tests.sh
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p data/m3/medicoes data/m3/evidencias
work=$(mktemp -d /tmp/esteira-m3.XXXXXX)
out="$PWD/data/m3/medicoes"
exec > >(tee "$out/execucao.txt") 2>&1
echo '=== M3: ambiente e compilacao ==='
date --iso-8601=seconds
uname -a
cat /etc/os-release
gcc --version
lscpu | head -25
echo 'gcc -std=c11 -O2 -Wall -Wextra -Werror m3/servidor_sort.c -o servidor_sort -pthread'
gcc -std=c11 -O2 -Wall -Wextra -Werror m3/servidor_sort.c -o "$work/servidor_sort" -pthread
sha256sum m3/servidor_sort.c data/codigos_viel/Aperiodic_Server_ex.c
echo '=== Testes de parametros invalidos ==='
for args in 'normal 0 50 20' 'normal 1 10 20' 'invalido 1 50 20' 'normal abc 50 20'; do
    if "$work/servidor_sort" $args; then echo "ERRO: aceitou $args"; exit 1; fi
done
echo '=== Permissao SCHED_FIFO (sem sudo deve falhar se usuario nao tem privilegio RT) ==='
if "$work/servidor_sort" empty 1 50 20 > "$out/sem-sudo.txt" 2>&1; then
    echo 'Usuario ja possui privilegio RT; consultar sem-sudo.txt'
else
    cat "$out/sem-sudo.txt"
fi
echo '=== Autorizar execucao dos ensaios SCHED_FIFO ==='
sudo -v
echo '=== Exemplo original do professor ==='
gcc -O2 -Wall data/codigos_viel/Aperiodic_Server_ex.c -o "$work/exemplo_viel" -pthread
sudo "$work/exemplo_viel" > "$out/exemplo-viel.txt"
echo 'Exemplo original concluido por 10 segundos; ver exemplo-viel.txt'
for config in '50 20 exemplo' '5 2 ajustado'; do
    read -r ts cs label <<< "$config"
    for mode in empty normal burst overload; do
        for rep in 1 2 3; do
            name="$label-$mode-r$rep"
            echo "EXECUTANDO $name: Ts=$ts ms Cs=$cs ms (6 segundos)"
            sudo "$work/servidor_sort" "$mode" 6 "$ts" "$cs" > "$out/$name.csv"
            head -1 "$out/$name.csv"
            tail -1 "$out/$name.csv"
        done
    done
done
printf 'TODOS OS ENSAIOS CONCLUIDOS\nBinarios locais: %s\n' "$work"
