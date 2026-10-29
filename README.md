# Esteira industrial — ESP32 / FreeRTOS — M1 + M2

Projeto de Guilherme Santana e Thiago Bazilio para Sistemas em Tempo Real.

A M1 permanece em `main/hello_world_main.c`: encoder simulado, PI, triagem,
E-stop e HMI com touch. A M2 acrescenta `main/m2.c`: MONITOR/TIME, WiFi,
TCP/UDP, SNTP e logs temporais. Os exemplos de Felipe Viel ficam preservados
em `codigos_viel`.

Durante a coleta, início/fim são salvos em variáveis e registros em RAM, sem
prints seriais de HMI ou medição. A MONITOR transmite os registros pelo WiFi
e o PC calcula as diferenças de tempo. O aviso de IP aparece só no aquecimento.

- [Roadmap](docs/ROADMAP_M2.md)
- [Resumo das mudanças e limites](docs/RESUMO_M2.md)
- [Passo a passo da bancada e métricas](docs/TESTES_M2.md)

No terminal ESP-IDF, usar `idf.py menuconfig` → **Esteira - M2** para configurar
WiFi/protocolo. Depois compilar/gravar conforme o roteiro. O firmware usa ESP32
com touch, unicore e tick de 1 ms; a comparação usa 80/160 MHz da M1.

O cliente/analisador `tools/m2_pc.py` usa Python padrão e gera JSONL, JSON, CSV e
Markdown. Nenhum resultado experimental acompanha esta alteração. Por solicitação
do usuário, não foram executados testes nem concluída a compilação.

`pytest_hello_world.py` é o arquivo legado do exemplo ESP-IDF; suas expectativas
de “Hello world” não validam esta aplicação. Não foi usado nesta alteração.

Não publicar credenciais reais do WiFi presentes no sdkconfig.
