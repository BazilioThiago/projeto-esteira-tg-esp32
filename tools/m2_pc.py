"""Bancada M2, apenas biblioteca padrao. Nao executa ao importar.

python tools/m2_pc.py collect --ip IP --proto UDP --scenario melhor --out resultados/80_udp_r1.jsonl
python tools/m2_pc.py analyze resultados/80_udp_r1.jsonl --out resultados/80_udp_r1
"""
import argparse
import csv
import json
import math
from pathlib import Path
import socket
import statistics
import struct
import time


def pc_sntp(server):
    """Offset NTP com quatro timestamps; nao altera o relogio do Windows.

    Uma amostra no inicio: salvar RTT e offset; assimetria/drift limitam one-way.
    """
    delta = 2208988800
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(3)
        s.connect((server, 123))
        request = bytearray(48)
        request[0] = 0x23  # cliente NTP v4
        t1 = time.time()
        sec = int(t1) + delta
        frac = int((t1 % 1) * 2**32)
        struct.pack_into("!II", request, 40, sec, frac)
        s.send(request)
        reply = s.recv(512)
        t4 = time.time()
    if len(reply) < 48 or reply[0] >> 6 == 3 or reply[0] & 7 != 4 or not 1 <= reply[1] <= 15:
        raise ValueError("Resposta SNTP invalida/nao sincronizada")
    if reply[24:32] != request[40:48]:
        raise ValueError("Resposta SNTP nao corresponde a requisicao")
    def stamp(pos):
        a, b = struct.unpack_from("!II", reply, pos)
        return a - delta + b / 2**32
    t2, t3 = stamp(32), stamp(40)
    return {"ok": True, "server": server,
            "offset_ms": ((t2-t1)+(t3-t4))*500,
            "rtt_ms": ((t4-t1)-(t3-t2))*1000}


def collect(args):
    path = Path(args.out)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        sync = pc_sntp(args.ntp)
    except (OSError, ValueError) as exc:
        sync = {"ok": False, "error": str(exc), "offset_ms": 0}
    meta = {"type": "meta", "scenario": args.scenario, "proto": args.proto,
            "expected_freq": args.freq, "duration_s": args.seconds,
            "period_s": args.period, "pc_sync": sync, "ip": args.ip,
            "policy": "Custom", "preemption": "ON", "slicing": "ON",
            "unicore": True, "started_pc_ms": time.time_ns()//1000000}
    kind = socket.SOCK_DGRAM if args.proto == "UDP" else socket.SOCK_STREAM
    sent = {}
    received_acks = set()
    seq = 0
    pending = b""
    started = time.perf_counter()
    next_send = started
    offset_ms = sync["offset_ms"]
    with path.open("x", encoding="utf-8") as out, socket.socket(socket.AF_INET, kind) as s:
        def write(obj):
            out.write(json.dumps(obj, ensure_ascii=False) + "\n")
        write(meta)
        s.settimeout(5)
        s.connect((args.ip, args.port))
        s.settimeout(0.1)
        try:
            # Dois segundos finais sem novos comandos para drenar logs/acks.
            while time.perf_counter()-started < args.seconds + 2:
                now = time.perf_counter()
                if now < started+args.seconds and now >= next_send:
                    # Padrao reprodutivel: SORT, PING, SORT, PING, ESTOP.
                    cmd = ("SORT", "PING", "SORT", "PING", "ESTOP")[seq % 5]
                    if args.ping_only:
                        cmd = "PING"
                    pc_ms = time.time_ns()//1000000 + offset_ms
                    wire = f"{seq},{int(pc_ms)},{cmd}\n".encode("ascii")
                    sent[seq] = time.perf_counter_ns()
                    s.sendall(wire) if args.proto == "TCP" else s.send(wire)
                    write({"type": "sent", "cmd_seq": seq, "cmd": cmd,
                           "pc_ms": pc_ms, "bytes": len(wire)})
                    seq += 1
                    next_send += args.period
                try:
                    data = s.recv(65535)
                except socket.timeout:
                    continue
                if not data:
                    raise ConnectionError("ESP32 encerrou a conexao TCP")
                receive_ns = time.perf_counter_ns()
                pc_ms = time.time_ns()//1000000 + offset_ms
                if args.proto == "TCP":
                    pending += data
                    lines = pending.split(b"\n")
                    pending = lines.pop()
                    if len(pending) > 4096:
                        raise ValueError("Linha TCP ultrapassou limite")
                else:
                    lines = data.splitlines()
                for line in lines:
                    try:
                        msg = json.loads(line)
                    except (ValueError, UnicodeDecodeError):
                        write({"type": "invalid", "raw": line.hex(), "pc_rx_ms": pc_ms})
                        continue
                    msg["pc_rx_ms"] = pc_ms
                    msg["pc_synced"] = sync["ok"]
                    msg["actual_bytes"] = len(line)+1
                    if msg.get("type") == "ack" and msg.get("cmd_seq") in sent:
                        key = msg["cmd_seq"]
                        # Descartar duplicatas para RTT e contar separadamente.
                        msg["duplicate_ack"] = key in received_acks
                        received_acks.add(key)
                        msg["rtt_ms"] = (receive_ns-sent[key])/1e6
                        if msg.get("synced") and sync["ok"]:
                            msg["up_ms_estimate"] = msg["esp_ms"]-msg["pc_ms"]
                    if msg.get("synced") and sync["ok"]:
                        msg["down_ms_estimate"] = pc_ms-msg["esp_ms"]
                    write(msg)
        except (KeyboardInterrupt, OSError, ValueError) as exc:
            write({"type": "collection_error", "error": str(exc)})
        finally:
            write({"type": "collection_end", "sent": seq,
                   "ack_unique": len(received_acks), "unanswered": seq-len(received_acks),
                   "elapsed_s": time.perf_counter()-started})
    print(f"Coleta salva em {path}; analisar antes de tirar conclusoes.")


def percentile(values, fraction):
    """HWM pelo nearest-rank, sem interpolar o maximo observado."""
    if not values:
        return None
    return sorted(values)[max(0, math.ceil(len(values)*fraction)-1)]


def summary(values):
    return {"n": len(values), "mean": statistics.mean(values) if values else None,
            "p99": percentile(values, .99), "max": max(values) if values else None}


def firm(results, m, k):
    windows = [sum(results[i:i+k]) for i in range(len(results)-k+1)]
    return {"m": m, "k": k, "windows": len(windows),
            "violations": sum(n < m for n in windows),
            "minimum_met": min(windows) if windows else None}


def analyze(args):
    records = [json.loads(line) for line in Path(args.input).read_text(encoding="utf-8").splitlines() if line.strip()]
    meta = next((r for r in records if r.get("type") == "meta"), {})
    packets = [r for r in records if "seq" in r]
    # UDP pode duplicar/reordenar: nao contar o mesmo pacote duas vezes.
    unique = {r["seq"]: r for r in packets}
    duplicates = len(packets)-len(unique)
    seqs = sorted(unique)
    gaps = seqs[-1]-seqs[0]+1-len(seqs) if seqs else 0
    records_unique = list(unique.values())
    rows = []
    jobs_by_task = {}
    for r in records_unique:
        if r.get("type") == "job":
            jobs_by_task.setdefault(r["task"], []).append(r)
    for task in ("ENC", "CTRL", "SORT", "SAFETY", "MONITOR", "TIME"):
        jobs = sorted(jobs_by_task.get(task, []), key=lambda r: (r["evt"], r["start"]))
        responses = [(r["end"]-r["evt"])/1000 for r in jobs]
        execution = [(r["end"]-r["start"])/1000 for r in jobs]
        latency = [(r["start"]-r["evt"])/1000 for r in jobs]
        touch = [(r["start"]-r["evt"])/1000 for r in jobs if r["origin"] == "touch"]
        met = [r["end"]-r["evt"] <= r["d"] for r in jobs]
        period = {"ENC": 5, "CTRL": 5, "MONITOR": 500, "TIME": 1000}.get(task)
        intervals = [(b["start"]-a["start"])/1000 for a,b in zip(jobs,jobs[1:])]
        row = {"task": task, "class": "soft" if task in ("MONITOR", "TIME") else "hard",
               "jobs": len(jobs), "misses": len(jobs)-sum(met),
               "miss_pct": 100*(1-sum(met)/len(met)) if met else None,
               "wcrt_ms": max(responses) if responses else None,
               "hwm99_ms": percentile(responses,.99),
               "wall_exec_max_ms": max(execution) if execution else None,
               "latency_max_ms": max(latency) if latency else None,
               "touch_latency_max_ms": max(touch) if touch else None,
               "start_jitter_abs_max_ms": max((abs(x-period) for x in intervals), default=None) if period else None}
        row["firm"] = firm(met,args.m,args.k) if row["class"] == "soft" else None
        rows.append(row)
    acks = [r for r in records_unique if r.get("type") == "ack" and not r.get("duplicate_ack")]
    status = [r for r in records_unique if r.get("type") == "status"]
    counters = {key: max((r.get(key,0) for r in status),default=None)
                for key in ("log_drops", "input_drops", "send_errors", "disconnects")}
    issues = []
    if gaps or duplicates:
        issues.append("Lacunas/duplicatas de pacotes; conferir perdas e reordenacao UDP.")
    if not status:
        issues.append("Sem status de integridade/CPU.")
    if any(v for v in counters.values()):
        issues.append("Perdas/erros reportados pela ESP32; contagens de jobs nao representam toda a execucao.")
    if any(not r["jobs"] for r in rows):
        issues.append("Uma ou mais tasks sem amostras: falta de evidencia.")
    if any(r.get("type") in ("invalid", "collection_error") for r in records):
        issues.append("Coleta interrompida ou mensagem invalida.")
    if not meta.get("pc_sync",{}).get("ok") or any(not r.get("synced") for r in packets):
        issues.append("SNTP ausente em parte da coleta: nao usar one-way como evidencia nesse trecho.")
    if status and any(r["freq_mhz"] != meta.get("expected_freq") for r in status):
        issues.append("Frequencia do firmware diferente da declarada no cliente.")
    ending = next((r for r in reversed(records) if r.get("type") == "collection_end"),{})
    if ending.get("unanswered",0):
        issues.append("Comandos sem ACK; ausencia de ACK nao prova ausencia de acionamento.")
    report = {"meta": meta, "tasks": rows, "packet_gaps_internal": gaps,
              "duplicate_packets": duplicates, "esp_counters": counters,
              "rtt_ms": summary([r["rtt_ms"] for r in acks if "rtt_ms" in r]),
              "down_ms_estimate": summary([r["down_ms_estimate"] for r in records_unique if "down_ms_estimate" in r]),
              "cpu_pct": summary([r["cpu_pct"] for r in status if r["cpu_pct"] >= 0]),
              "collection_end": ending, "issues": issues,
              "note": "WCRT/HWM sao observados. Execucao e tempo de parede com preempcoes; nao WCET de CPU provado."}
    base = Path(args.out)
    base.parent.mkdir(parents=True,exist_ok=True)
    base.with_suffix(".json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    with base.with_suffix(".csv").open("w",newline="",encoding="utf-8-sig") as out:
        writer = csv.DictWriter(out,fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    lines = [f"# M2 — {meta.get('scenario','?')} / {meta.get('proto','?')}", "",
             "|Task|Tipo|Jobs|Misses|WCRT ms|HWM99 ms|Latencia max ms|", "|---|---|---:|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"|{r['task']}|{r['class']}|{r['jobs']}|{r['misses']}|{r['wcrt_ms']}|{r['hwm99_ms']}|{r['latency_max_ms']}|")
    lines += ["",f"RTT ms: {report['rtt_ms']}",f"CPU %: {report['cpu_pct']}",
              f"Integridade: {counters}; lacunas internas: {gaps}.","", "## Ressalvas", ""]
    lines += ["- "+x for x in issues] or ["- Nenhuma ressalva automatica; conferir protocolo de bancada e estimulos manuais."]
    lines += ["",report["note"],"As janelas firm e todas as metricas estao no JSON/CSV. Janelas com menos de k jobs nao sustentam conclusao."]
    base.with_suffix(".md").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print(f"Analise gravada em {base}.json/.csv/.md")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action",required=True)
    c = sub.add_parser("collect")
    c.add_argument("--ip",required=True)
    c.add_argument("--proto",choices=("UDP","TCP"),required=True)
    c.add_argument("--scenario",choices=("melhor","pior"),required=True)
    c.add_argument("--freq",type=int,choices=(80,160),required=True)
    c.add_argument("--port",type=int,default=5000)
    c.add_argument("--seconds",type=float,default=120)
    c.add_argument("--period",type=float,default=1)
    c.add_argument("--ntp",default="a.st1.ntp.br")
    c.add_argument("--ping-only",action="store_true")
    c.add_argument("--out",required=True)
    c.set_defaults(func=collect)
    a = sub.add_parser("analyze")
    a.add_argument("input")
    a.add_argument("--out",required=True)
    a.add_argument("--m",type=int,default=9)
    a.add_argument("--k",type=int,default=10)
    a.set_defaults(func=analyze)
    args = parser.parse_args()
    if args.action == "collect" and (args.seconds <= 0 or args.period < .5):
        parser.error("Use duracao positiva e periodo >= 0.5 s (capacidade de um comando por ciclo).")
    if args.action == "analyze" and not 1 <= args.m <= args.k:
        parser.error("Exigido 1 <= m <= k")
    args.func(args)


if __name__ == "__main__":
    main()
