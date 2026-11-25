#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(sed -n 's/^Binarios locais: //p' data/m3/medicoes/execucao.txt | tail -1)
clear
echo 'M3 - Verificacao das prioridades e afinidade em execucao'
sudo -n "$work/servidor_sort" normal 3 5 2 > data/m3/medicoes/runtime-run.txt &
pid=$!
sleep 1
ps -eLo pid,tid,cls,rtprio,psr,comm | grep -E 'PID|servidor_sort' | tee data/m3/medicoes/runtime-policy.txt
wait "$pid"
tail -1 data/m3/medicoes/runtime-run.txt
echo 'FF = SCHED_FIFO; prioridades 60, 50, 49, 48, 47 e 40; PSR = processador'
