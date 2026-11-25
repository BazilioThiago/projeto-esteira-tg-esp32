# Registros dos testes da M3

- `evidencias/`: 11 prints apresentados no relatório. Oito mostram as três
  repetições de cada combinação de configuração e cenário.
- `medicoes/`: 24 CSVs gerados pelo programa, um por ensaio, e cinco arquivos
  de texto de compilação, exemplo original, permissões e verificação em execução.
- `resumo.csv`: resultados calculados dos 24 CSVs por `m3/analyze.py`.

Os dados brutos foram preservados como registrados. Caminhos antigos nos
logs e prints correspondem à organização existente durante as execuções.
O código C atual tem o mesmo conteúdo do utilizado nos testes.

Os prints atendem ao requisito de evidências. Os CSVs permitem conferir
WCRT, misses, rejeições e pendências sem repetir o experimento.
