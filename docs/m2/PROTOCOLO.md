# Protocolo e limites das medidas

ESP inicia conexao TCP com o PC ou usa UDP conectado ao IP/porta do PC. Pacotes
de ate 1300 bytes incluem cabecalho B e corpo CSV. TCP trata fragmentacao,
coalescencia e escritas parciais; nao se assume um recv por send. Cada datagrama
UDP contem linhas completas. Todos os sockets da ESP sao nao bloqueantes.

```text
B,seq,protocolo,tamanho_bytes,esp_tx_utc_us,esp_tx_mono_us,sntp_valido,politica,preempcao,slicing,mhz,cores,log_drops,tx_drops
E,task,origem,job,evento_us,inicio_us,fim_us,deadline_us,espera_mutex_us
S,task,jobs,misses,wcrt_us,janelas_firm,violacoes_firm,event_drops
T,idade_sync_us,ciclos_desde_sync,iteracoes_TIME,geracao_sync,cpu_ocupada_permille
A,id,pc_tx_utc_us,esp_rx_mono_us,esp_rx_utc_us,aceito
```

IDs: 0 ENC, 1 CTRL, 2 SORT, 3 SAFETY, 4 MONITOR, 5 TIME, 6 HMI (trecho soft do
CTRL, nao uma setima tarefa). Origens: 0 periodica, 1 encadeada, 2 touch, 3 PC.
Tamanho e de aplicacao, incluindo B, sem cabecalhos de rede. UTC de envio e
capturado ao montar o pacote, antes de send; escritas adiadas acrescentam fila.

PC envia `CMD,id,SORT,pc_tx_utc_us` ou ESTOP/PING, com LF. Somente verbos
reconhecidos e linhas completas de ate 254 caracteres sao aceitos. No TCP a
linha pode chegar fragmentada. O PC associa ACK por ID e timestamp. Nao ha
deduplicacao no atuador: eventual duplicacao de comando UDP pode repetir SORT.

O PC salva `pc_rx_utc_us,pc_rx_mono_us,<linha ESP>` e acrescenta:

```text
N,id,verbo,rtt_us,ida_estimada_us,volta_estimada_us,aceito,sntp_valido
C,id,verbo
Z,enviados,acks_recebidos,pendentes,duracao_us
```

RTT usa apenas o relogio monotonic do PC. Inclui envio, rede, espera de MONITOR
e ACK. Ida estimada = UTC_rx_ESP - UTC_tx_PC; volta estimada = UTC_rx_PC -
UTC_tx_pacote_ESP. Exigem ambos os relogios sincronizados, incluem erro NTP e
buffering e nao equivalem a RTT/2. SNTP=1 certifica apenas que a ESP recebeu uma
sync, nao a precisao do PC. Valores negativos nao representam atraso fisico.

## Medidas

- R=fim-evento; miss se R>D; WCRT observado=max(R), sem garantia formal.
- Latencia=inicio-evento. Touch registra entrada na ISR, nao o contato fisico.
- Span=fim-inicio inclui interrupcoes, preempcao e bloqueio. Nao e CPU exclusiva
  C ou WCET provado. Meca C em condicao isolada se precisar desse parametro.
- Espera de mutex inclui eventual preempcao entre solicitar e obter o recurso.
- Jitter de inicio=abs((inicio_atual-inicio_anterior)-periodo); gaps invalidam
  a comparacao direta entre jobs consecutivos.
- CPU=1-delta(runtime_IDLE)/delta(runtime_total), com uxTaskGetSystemState.
  Inclui pilha Wi-Fi e aplicacao. CPU=-1 indica snapshot invalido; primeiro
  intervalo inclui tempo desde boot. Nao some spans para estimar ocupacao.
- HWM100=max; HWM99 e nearest rank ceil(0,99*n). Hard exige examinar HWM100.
- (9,10)-firm exige nove sucessos em cada janela deslizante de dez jobs soft,
  nao apenas 90% global. PC interrompe janelas em gaps de IDs; firmware mantem
  contagem cumulativa. N/A quando faltam dez amostras.

esp_timer libera periodicamente ENC/MONITOR/TIME a cada 5/500/1000 ms. Callback
marca evento antes de enfileirar, incluindo espera na fila em R. Jitter do
servico esp_timer anterior ao callback nao esta incluido; avalie separadamente.
CTRL tem D=10 ms desde fim do encoder. SAFETY por PC tem D=5 ms desde interpretar
o comando na ESP; desde enviar no PC pode haver cerca de 500 ms de polling.

TIME inicia SNTP uma vez e monitora atualizacoes assincronas; nao bloqueia uma
vez por segundo esperando servidor. O callback reseta idade, ciclos e iteracoes.
CCOUNT de 32 bits e acumulado em 64 bits no core unico, com frequencia fixa;
TIME deve amostrar antes do wrap (~17,9 s a 240 MHz). Inanicao maior invalida
essa contagem. Ciclos de CPU e iteracoes TIME sao registrados separadamente.

## Interferencia, bloqueio e perdas

Mutex do estado possui heranca de prioridade. Nenhum workload/print/socket
fica dentro dele. Uma tarefa de maior prioridade pode bloquear por ENC/CTRL/SORT
que detenha o recurso; heranca limita inversao sem elimina-la. A espera medida
e um limite observado que tambem pode incluir preempcao. Latch/PWM usam secao
critica curta compartilhada com a ISR. Filas possuem regioes criticas internas.

Cooperativo usa configUSE_PREEMPTION=0; touch nao solicita yield na ISR. As
tarefas bloqueiam nas filas e cedem entre jobs de evento. Drivers/ISRs do SDK
podem solicitar yields explicitos, inclusive Wi-Fi. Documente esse comportamento
do port ao comparar; nao e uma simulacao sem interrupcoes.

Eventos possuem filas de 32 itens; logs, 512. S registra descartes de eventos,
B registra perdas de logs/transporte. Nao trate descartes como sucesso. Touch
aciona o latch mesmo se a fila SAFETY lotar; LED depende da tarefa. Comando PC
rejeitado recebe aceito=0. Sem rede os logs podem lotar enquanto tarefas continuam;
S preserva estatisticas agregadas, mas nao recupera amostras perdidas.
