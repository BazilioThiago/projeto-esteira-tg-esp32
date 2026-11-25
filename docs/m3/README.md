# M3 - Servidor periódico para a triagem SORT

A função SORT da M2 foi reproduzida em C com pthreads. Um gerador representa
os eventos de objeto e os comandos do PC. Os pedidos entram em uma fila FIFO
de 32 posições e são atendidos por uma thread servidor com período Ts e
orçamento Cs. O exemplo original está em `data/codigos_viel/`.

## Compilar e executar um teste

No terminal do Lubuntu, a partir da raiz do projeto:

```bash
gcc -std=c11 -O2 -Wall -Wextra -Werror m3/servidor_sort.c -o /tmp/servidor_sort -pthread
sudo /tmp/servidor_sort normal 6 50 20
sudo /tmp/servidor_sort normal 6 5 2
```

Argumentos: cenário, duração em segundos, Ts em ms e Cs em ms.
Cenários: `empty`, `normal`, `burst` e `overload`.
Os binários ficam em `/tmp`, pois a pasta compartilhada pode impedir execução.

## Repetir os ensaios do relatório

```bash
bash m3/run_tests.sh
python3 m3/analyze.py
```

São duas configurações (50/20 ms e 5/2 ms), quatro cenários e três repetições
de seis segundos: 24 arquivos CSV, um por execução. O roteiro grava os logs
em `data/m3/medicoes/`; o analisador gera `data/m3/resumo.csv`.
Executar novamente o roteiro substitui os registros de mesmo nome.

Os 11 prints em `data/m3/evidencias/` são os usados no relatório: compilação,
exemplo original, prioridades e oito telas com as três repetições de cada
combinação. Imagens intermediárias da revisão do Word ficam em `output/`.

## Entender os resultados

- `J`: pedido SORT; `P`: job de T1 a T4; `S`: janela do servidor.
- Tempos em nanossegundos, relativos ao começo do ensaio.
- Resposta = fim menos chegada/liberação; WCRT = maior resposta observada.
- Miss = resposta maior que o prazo; o prazo da SORT é 10 ms.
- Rejeitados não entraram na fila; pendentes foram aceitos e ainda não
  terminaram ao encerrar o teste.

Todas as threads usam o mesmo processador. As prioridades SCHED_FIFO são
60 para o servidor, 50/49/48/47 para T1 a T4 e 40 para o gerador.
O exemplo verifica o orçamento entre jobs; o último pode ultrapassar Cs.
O relatório apresenta essas ultrapassagens e os atrasos observados na VM.

A reprodução mantém a operação de triagem e o incremento de produção.
O LCG veio da base usada no início da M3; a versão final da M2 usa uma espera
ativa de 700 us na SORT. No Linux, a carga é complementada até 1 ms de CPU
e o contador é protegido por mutex. Esse custo é do teste no PC, não da ESP32. T1 a T4 mantêm os períodos 40/80/120/160 ms do exemplo,
com carga de 5 ms de CPU por job.
