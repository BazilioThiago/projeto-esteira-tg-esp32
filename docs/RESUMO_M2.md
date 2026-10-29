# Resumo das adições da M2

O PI, a dinâmica simulada, os touch pads B/C/D, os períodos/deadlines das tarefas originais e as prioridades Custom da M1 foram mantidos. A implementação da M2 fica principalmente em `main/m2.c`, com configurações em `main/Kconfig.projbuild`. Os exemplos do professor em `codigos_viel` foram consultados e preservados.

## O que foi acrescentado

- **MONITOR_TASK:** T=D=500 ms, prioridade 1, core 0. Inicializa WiFi, recebe comandos `SORT`, `ESTOP` e `PING`, transmite logs das seis tarefas e estado da instrumentação. UDP e TCP selecionáveis no menuconfig, um protocolo por ensaio, porta padrão 5000. Ambos usam ESP32 como servidor e PC como cliente, para manter o sentido dos comandos e o tráfego comparáveis. Essa topologia adapta os exemplos do professor; o exemplo UDP original envia da ESP32 para o PC.
- **TIME_TASK:** T=D=1 s, prioridade 1, core 0. Inicia SNTP após obter IP, lê a hora local atualizada por SNTP e mantém ciclos desde a última sincronização efetiva. A atualização SNTP é assíncrona, padrão 60 s, independente da execução periódica da TIME. Callback registra sincronização, idade e quantidade de atualizações. A task não bloqueia esperando um servidor de Internet.
- **Logs:** evento, início, fim, deadline, tarefa e origem (`touch`, `pc`, `encoder` ou `periodic`). Pacotes incluem sequência, protocolo, tamanho em bytes, timestamp de envio UTC em ms, tempo monotônico e indicação de sincronização. Estado inclui ocupação da CPU via idle/runtime, perdas das filas, erros de envio e ciclos SNTP.
- **Cliente e análise:** `tools/m2_pc.py` coleta JSONL e calcula RTT com relógio monotônico do PC; salva timestamp do PC no envio e na recepção. Pode consultar SNTP no PC sem mudar o relógio do Windows. A análise gera JSON, CSV e Markdown com WCRT observado, HWM(99%), misses, latências, jitter, ocupação e janelas deslizantes (9,10)-firm das duas tasks soft.
- **Extra opcional:** LED acende no E-stop local ou remoto, se o GPIO correto for habilitado no menuconfig. O padrão é desabilitado porque o LED varia conforme a placa.

## Pequenas mudanças necessárias na M1

1. Chamadas para registrar métricas nas quatro tasks. Nenhuma task imprime logs de medição na UART, inclusive HMI. Os timestamps de início/fim são capturados em variáveis antes da cópia binária para uma fila em RAM; a MONITOR transmite esses registros ao PC. Ao iniciar a sessão, os logs normais do ESP-IDF são silenciados. O aviso de IP é emitido somente no aquecimento, fora da coleta.
2. E-stop passa de semáforo binário para fila com timestamp/origem. Assim, eventos acumulados não sobrescrevem o timestamp de um evento anterior; estouros são contados.
3. A escrita final do PWM verifica novamente `g_estop` em uma seção crítica curta. Evita que o controle retome PWM quando SAFETY o interrompeu durante o PI.
4. CTRL é criada antes de ENC para que a primeira notificação tenha destinatário. Notificações acumuladas descartadas são contadas; o timestamp da última amostra é lido com proteção.
5. CMake inclui o módulo e dependências de WiFi, rede e NVS. Mantidas as estatísticas runtime que já estavam habilitadas no sdkconfig da M1.

## Classificação temporal e interpretação

ENC, CTRL, SORT e SAFETY permanecem **hard** pelas consequências físicas dos atrasos na aplicação descrita. MONITOR é **soft** para supervisão/telemetria, e TIME é **soft** porque uma atualização perdida degrada a referência de hora sem impedir diretamente o PI. O comando remoto de emergência trafega pela supervisão soft e não tem garantia ponta a ponta de 5 ms: a deadline hard de SAFETY é contada do evento local (ISR ou despacho do comando) até o fim da ação. Essa distinção deve aparecer no relatório.

O semáforo da HMI ainda representa trabalho soft dentro da CTRL. Mantida a carga simulada de 400 us quando há solicitação, sem prints. O timestamp final da CTRL e seu contador de misses agora incluem esse trecho, usando o mesmo limite do log M2.

Conforme orientação do professor repassada pelo usuário, a medição usa variáveis: `C_parede = fim - início`, `resposta = fim - evento` e `latência = início - evento`. Os timestamps vêm de `esp_timer_get_time()`, em microssegundos; não são contadores de ciclos. A comparação ocorre depois no PC, sem impressão serial durante a coleta. A leitura do relógio e a instrumentação ainda têm um custo pequeno; não se declara perturbação zero.

O envio dos logs não acontece no trecho cronometrado de ENC/CTRL/SORT/SAFETY/TIME: cada uma salva seu fim antes de enfileirar o registro. Na MONITOR, formatação e envio de rede são sua função real e entram em `end-start`; retirar esse custo esconderia parte do trabalho exigido pela M2. A fila e a comunicação podem interferir em outras ativações, o que é parte do cenário com WiFi. Não há uma impressão serial de resultado dentro de nenhuma dessas medições.

Interferência das tasks da aplicação, em ordem de prioridade: SAFETY(5) → SORT(4) → ENC(3) → CTRL(2) → MONITOR/TIME(1). As duas últimas compartilham prioridade e sofrem time slicing. Tasks internas do WiFi, TCP/IP, timers e interrupções também interferem e podem ter prioridade maior que SAFETY. A prioridade 5 da segurança não é a maior do sistema inteiro.

Não há mutex mantido por uma task baixa enquanto uma alta espera por ele no caminho hard. Assim, não foi introduzido um caso clássico de inversão de prioridade por mutex. Existem seções críticas curtas nas filas/notificações e acesso a timestamps; podem adiar interrupções e execução. A implementação interna da pilha de rede também pode bloquear ou consumir CPU. Isso deve ser discutido a partir dos dados, sem declarar bloqueio zero.

O busy loop original mede tempo decorrido, não uma quantidade fixa de instruções: pode terminar após uma preempção sem consumir todo aquele tempo de CPU. Ele foi preservado conforme o pedido de alteração mínima. `end-start` inclui preempções e não constitui WCET de CPU isolado. A ocupação usa runtime/idle justamente para evitar somar intervalos de parede sobrepostos.

## Situação da entrega

Código, roadmap e roteiro preparados. **Sem testes executados e sem firmware validado**, conforme orientação do usuário. Antes dessa orientação, houve uma tentativa de build que parou no ambiente/compilador por acesso negado; a tentativa fora do sandbox foi recusada. Não houve compilação concluída, gravação, coleta em ESP32, ensaio sintético ou resultado experimental produzido.

Ainda cabe ao grupo compilar e executar os cenários, preencher as tabelas, interpretar qual protocolo foi melhor/pior e montar o PDF final com evidências. Nenhum protocolo pode ser declarado vencedor antes das medições. WCRT observado não é garantia formal de pior caso para todas as execuções. Os dados da M1 já continham hard misses, inclusive no melhor cenário; não equivalem a atendimento integral das deadlines.
