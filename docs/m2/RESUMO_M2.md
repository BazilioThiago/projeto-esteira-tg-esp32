# Resumo M2 — versão curta v3

Mantidos o PI, a dinâmica simulada, touch B/C/D (GPIO27/33/32), prioridades Custom e ações das quatro tasks da M1. Exemplos do professor preservados em `codigos_viel`. A M2 fica principalmente em `main/m2.c`; configurações de WiFi/protocolo permanecem no menuconfig.

## O que a versão atual acrescenta

- MONITOR: T=D=500 ms, prioridade 1/core 0. WiFi, TCP ou UDP, comandos remotos SORT/ESTOP, resumos periódicos e recuperação dos registros finais.
- TIME: T=D=1 s, prioridade 1/core 0. Lê a hora mantida por SNTP, registra sincronizações, idade e ciclos desde a última atualização. SNTP assíncrono a cada 60 s por padrão, sem aguardar Internet dentro da task.
- Metodologia curta: START do PC define uma janela de 20 s e zera somente a instrumentação. A placa encerra a janela automaticamente e congela o resultado após 1 s de margem para jobs pendentes. Consultar novamente não refaz o experimento. Novo START com outro identificador inicia outra coleta.
- Métricas em RAM: WCRT/HWM(100%), misses, tempos de parede, latências, jitter e (9,10)-firm das duas tasks soft. Não dependem de receber cada job via WiFi. O PC calcula RTT dos comandos, registra perdas e produz JSON/CSV/Markdown automaticamente.
- Logs de evidência: últimas oito execuções válidas e timestamps do maior R por task, com evento/início/fim/origem. Não se retém a série completa nem se calcula HWM(99%). Contadores, máximos e firm são atualizados em todas as execuções medidas.
- Configuração/diagnóstico: número de jobs periódicos esperado pelo relógio absoluto, inválidos, perdas de ativação, erros de envio, errno e tentativas de reenvio. Falha transitória de socket retém o pacote para tentar depois; perdas de rede não apagam os agregados locais.
- Extra opcional: LED do E-stop se o GPIO correto for configurado. Padrão -1 desabilita por variar conforme a placa.

Não há prints de HMI ou métricas na janela de medição. O IP aparece apenas no aquecimento; logs normais do ESP-IDF são silenciados no START. Timestamps são capturados em variáveis antes da atualização das métricas em RAM. A formatação/envio pertence à MONITOR e integra seu próprio tempo medido. Instrumentação tem custo; não se declara perturbação zero.

## Correções e mudança de liberação periódica

As tentativas anteriores do grupo expuseram saturação da transmissão, sessão iniciada antes da coleta e timestamps de tick indisponíveis. Os arquivos que ainda estão em `resultados` foram preservados nesta alteração; não são recalculados nem sobrescritos automaticamente.

A v3 remove as filas de logs e o histórico de ticks. ENC, MONITOR e TIME usam notificações contadas de um `esp_timer` periódico com primeiro alarme absoluto (`esp_timer_start_periodic_at`, disponível no ESP-IDF 6.1 instalado). O timestamp nominal do evento é exatamente o instante passado ao alarme, no mesmo relógio de início/fim. Atrasos do callback e do escalonamento entram em R/latência; não são escondidos substituindo o evento pelo início. Avisos periódicos acumulados são consumidos um a um. T, D e prioridades são preservados, mas o mecanismo de liberação muda em relação à M1 e deve ser declarado no relatório. A task interna do timer também interfere.

O fluxo ENC→CTRL da M1 permanece com notificação e última amostra; notificações coalescidas entram em `input_drops`, assim como filas de eventos cheias. Não declarar atendimento integral quando esses contadores mostram perda de ativações. Os jobs cujo evento está fora da janela não entram; os pertencentes à janela podem concluir durante a margem de 1 s. Jobs periódicos ausentes são detectados comparando a quantidade concluída com liberações nominais. WCRT é observado sobre jobs concluídos, não um limite formal para qualquer estímulo.

A fila de E-stop com timestamp/origem, a proteção contra restaurar PWM após SAFETY e a criação da CTRL antes da ENC continuam sendo os pequenos ajustes da integração M2. HMI mantém a carga simulada de 400 us, sem UART, e entra no timestamp final da CTRL.

## Análise temporal para o relatório

ENC, CTRL, SORT e SAFETY são hard pelas consequências físicas de atraso. MONITOR e TIME são soft para supervisão e referência de hora. Um comando remoto de emergência passa pela MONITOR soft: sua deadline hard de 5 ms conta do despacho local/ISR até o fim da SAFETY, não desde o envio no PC. RTT é medido separadamente e inclui essa espera de supervisão.

Interferência entre tasks da aplicação: SAFETY(5) → SORT(4) → ENC(3) → CTRL(2) → MONITOR/TIME(1). As duas últimas compartilham prioridade e slicing. WiFi, TCP/IP, timer e interrupções também interferem, inclusive com prioridade maior que SAFETY.

Não há um mutex de aplicação mantido por uma task baixa enquanto uma alta espera por ele no caminho hard. Existem seções críticas curtas de estatísticas/IPC/estado, e mecanismos internos da rede podem consumir CPU ou bloquear. Não declarar bloqueio zero. O busy loop original usa tempo decorrido, não quantidade fixa de instruções; `fim-início` inclui preempções. Ocupação de CPU usa runtime/idle, evitando somar intervalos sobrepostos.

Perdas na comunicação são resultados a comparar, não motivo automático para descartar métricas locais íntegras. Se houver misses, declará-los. Se faltar resultado final ou houver inconsistência de timestamp/contagem, a avaliação é inconclusiva. Quatro casos de 20 s são a proposta de bancada, não duração obrigatória do PDF nem garantia de atingir os critérios de sucesso.

## Entrega e validação

Código e roteiro preparados; o assistente não compilou, gravou ou executou esta revisão, conforme orientação do usuário. O grupo deve rodar `docs/TESTES_M2.md`, preencher as tabelas, discutir qual protocolo foi melhor/pior e montar o relatório PDF com evidências reais. Compilação presencial, demonstração e postagem continuam pendentes ao grupo. A restrição anterior de não executar testes pelo assistente foi mantida.
