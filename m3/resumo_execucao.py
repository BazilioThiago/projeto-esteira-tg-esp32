"""Exibe o resumo de um ensaio individual sem alterar seus registros.

Uso: python3 m3/resumo_execucao.py /tmp/video-50.csv
"""
import sys
from pathlib import Path
from analyze import analyze


def show(path):
    result = analyze(path)
    config, total = result['config'], result['total']
    sort, server = result['sort'], result['server']
    print(f"Cenario: {config['scenario']} | Duracao: {config['seconds']} s")
    print(f"Servidor: Ts={config['Ts_ms']} ms | Cs={config['Cs_ms']} ms")
    print(f"Gerados: {total['produced']} | Aceitos: {total['accepted']} | Concluidos: {total['completed']}")
    print(f"Rejeitados: {total['rejected']} | Pendentes: {total['pending']} | Fila maxima: {total['max_depth']}")
    if sort['n']:
        print(f"Maior resposta SORT (WCRT observado): {sort['wcrt_ms']:.3f} ms")
        print(f"Resposta media SORT: {sort['mean_response_ms']:.3f} ms")
        print(f"Maior espera ate iniciar SORT: {sort['max_wait_ms']:.3f} ms")
        print(f"Prazo SORT: 10 ms | Misses: {sort['misses']} de {sort['n']} concluidos")
    else:
        print('Nenhum pedido SORT concluido.')
    print('Misses T1/T2/T3/T4: ' + ' / '.join(str(p.get('misses', 0)) for p in result['periodic']))
    print(f"Maior excesso da janela sobre Cs: {server['max_overrun_ms']:.3f} ms")
    print(f"Producao: {total['production']} | Registros perdidos: {total['log_lost']}")
    print('Contadores, ordem FIFO e timestamps conferidos.')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Uso: python3 m3/resumo_execucao.py ARQUIVO.csv')
    try:
        show(Path(sys.argv[1]))
    except (OSError, ValueError, AssertionError, KeyError) as exc:
        raise SystemExit(f'Nao foi possivel analisar o ensaio: {exc}') from exc
