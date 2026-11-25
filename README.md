# Esteira industrial - M2 e M3

Guilherme Santana e Thiago Bazilio — Sistemas em Tempo Real.

Este repositório reúne a versão final da M2 e a reprodução em pthreads da M3.
São duas execuções independentes no mesmo projeto.

## M2 na ESP32

O firmware final está em `main/`, com base no commit `be16289` do antigo
branch `teste_para_m2`, usado nos quatro ensaios do relatório da M2.
No terminal ESP-IDF 6.1, a partir desta pasta:

```powershell
idf.py menuconfig
idf.py build
idf.py -p COM3 flash monitor
```

Em **Esteira - M2**, configure a rede Wi-Fi e escolha TCP ou UDP.
A configuração local preservada está em TCP, 160 MHz, porta 5000 e LED GPIO2.
Ajuste a porta serial e a rede conforme a bancada.

O coletor do PC é `tools/m2_pc.py`. Exemplo para a configuração atual:

```powershell
python tools/m2_pc.py collect --ip IP_DA_ESP --proto TCP --scenario pior --freq 160
```

Substitua `IP_DA_ESP` pelo endereço mostrado no monitor serial.
O roteiro está em `docs/m2/TESTES_M2.md`; os resultados anteriores, em
`data/resultados/`; o relatório entregue, em `docs/m2/`.

## M3 no Lubuntu

Os códigos estão em `m3/`. Eles não entram no build do ESP-IDF.
Na pasta compartilhada da VM:

```bash
cd /media/sf_projeto
gcc -std=c11 -O2 -Wall -Wextra -Werror m3/servidor_sort.c -o /tmp/servidor_sort -pthread
sudo /tmp/servidor_sort normal 6 5 2
```

Para repetir a bateria completa e calcular os resultados:

```bash
bash m3/run_tests.sh
python3 m3/analyze.py
```

O roteiro realiza 24 ensaios: duas configurações, quatro cenários e três
repetições de seis segundos. Uma nova execução substitui os registros de
mesmo nome em `data/m3/medicoes/`.

A M3 adapta a operação SORT (carga de triagem e incremento de produção).
A carga sintética de 1 ms e o mutex são da reprodução no PC; na M2 final,
SORT usa uma espera ativa de 700 us. Não são medições equivalentes de CPU.

O roteiro da M3 está em `docs/m3/README.md`, as evidências em `data/m3/`
e o relatório em `docs/m3/Trabalho Esteira M3 - Guilherme e Thiago B.docx`.

## Organização

- `main/`: firmware ESP32 da M2.
- `m3/`: servidor periódico e scripts para Linux.
- `tools/`: coletor e análise da M2.
- `data/`: exemplos do professor e resultados das etapas.
- `docs/m2/` e `docs/m3/`: documentos de cada etapa.
- `output/`: arquivos locais de compilação auxiliar, revisão e backup, ignorados pelo Git.
