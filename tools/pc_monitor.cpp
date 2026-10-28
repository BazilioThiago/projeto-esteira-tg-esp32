// C++17, standard library + OS sockets only. No third-party networking library.
// Windows: cl /EHsc /std:c++17 pc_monitor.cpp ws2_32.lib
// Linux:   c++ -std=c++17 -O2 pc_monitor.cpp -o pc_monitor
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
using socket_t = int;
constexpr int INVALID_SOCKET = -1;
static void close_socket(socket_t s) { close(s); }
#endif
using i64 = long long;
static i64 mono_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static i64 utc_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static std::vector<std::string> split(const std::string &line) {
    std::vector<std::string> v;
    std::istringstream s(line); std::string t;
    while (std::getline(s,t,',')) v.push_back(t);
    return v;
}
static bool readable(socket_t s, int milliseconds) {
    fd_set set; FD_ZERO(&set); FD_SET(s,&set);
    timeval t{milliseconds/1000,(milliseconds%1000)*1000};
    return select(static_cast<int>(s+1),&set,nullptr,nullptr,&t)>0;
}
static bool writable(socket_t s) {
    fd_set set; FD_ZERO(&set); FD_SET(s,&set);
    timeval t{0,0}; return select(static_cast<int>(s+1),nullptr,&set,nullptr,&t)>0;
}
struct Pending { i64 mono,utc; std::string verb; };
static int collect(const std::string &protocol, int port, int seconds,
                   const std::string &path, const std::string &mode) {
    bool tcp = protocol=="tcp";
    if (!tcp && protocol!="udp") throw std::runtime_error("Use tcp or udp");
    if (mode!="ping" && mode!="sort" && mode!="estop" && mode!="mixed")
        throw std::runtime_error("Mode: ping, sort, estop or mixed");
    if (seconds<30 || port<1024 || port>65535) throw std::runtime_error("Use >=30 s, port 1024..65535");
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot create log file");
    out << "pc_utc_us,pc_mono_us,type,fields...\n";
    socket_t listener = socket(AF_INET,tcp?SOCK_STREAM:SOCK_DGRAM,0);
    if (listener==INVALID_SOCKET) throw std::runtime_error("socket failed");
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    if (bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0)
        throw std::runtime_error("bind failed; check port/firewall");
    socket_t conn=listener;
    sockaddr_in peer{}; bool have_peer=false;
    std::cout << "Listening " << protocol << ':' << port << "; waiting for ESP32...\n";
    if (tcp) {
        if (listen(listener,1)!=0) throw std::runtime_error("listen failed");
        if (!readable(listener,120000)) throw std::runtime_error("No connection in 120 s");
        conn=accept(listener,nullptr,nullptr);
        if (conn==INVALID_SOCKET) throw std::runtime_error("accept failed");
        have_peer=true;
    }
    i64 begin=0, next_cmd=0, deadline=mono_us()+120000000;
    i64 packet_utc=0; bool sync=false;
    unsigned seq=0, received=0;
    std::map<unsigned,Pending> pending;
    std::string buffer, send_buffer;
    size_t send_offset=0;
    while (mono_us()<deadline) {
        if (readable(conn,50)) {
            char data[8192];
            int n;
            if (tcp) n=recv(conn,data,sizeof(data),0);
            else {
                sockaddr_in from{};
#ifdef _WIN32
                int len=sizeof(from);
#else
                socklen_t len=sizeof(from);
#endif
                n=recvfrom(conn,data,sizeof(data),0,reinterpret_cast<sockaddr*>(&from),&len);
                if (have_peer && (from.sin_addr.s_addr!=peer.sin_addr.s_addr || from.sin_port!=peer.sin_port)) continue;
                peer=from; have_peer=true;
            }
            if (n<=0) { if (tcp) break; else continue; }
            i64 pc_rx_utc=utc_us(), pc_rx_mono=mono_us();
            if (!begin) { begin=pc_rx_mono; deadline=begin+seconds*1000000LL; next_cmd=begin; }
            if (!tcp) buffer.clear();
            buffer.append(data,n);
            if (buffer.size()>65536) throw std::runtime_error("Invalid stream: missing line delimiter");
            size_t pos;
            while ((pos=buffer.find('\n'))!=std::string::npos) {
                std::string line=buffer.substr(0,pos); buffer.erase(0,pos+1);
                if (!line.empty() && line.back()=='\r') line.pop_back();
                out << pc_rx_utc << ',' << pc_rx_mono << ',' << line << '\n';
                auto f=split(line);
                if (f.size()==14 && f[0]=="B") {
                    packet_utc=std::stoll(f[4]); sync=f[6]=="1";
                } else if (f.size()==6 && f[0]=="A") {
                    unsigned id=static_cast<unsigned>(std::stoul(f[1]));
                    auto it=pending.find(id);
                    if (it!=pending.end() && std::stoll(f[2])==it->second.utc) {
                        const auto &p=it->second;
                        out << pc_rx_utc << ',' << pc_rx_mono << ",N," << id << ',' << p.verb << ','
                            << pc_rx_mono-p.mono << ',' << std::stoll(f[4])-p.utc << ','
                            << pc_rx_utc-packet_utc << ',' << f[5] << ',' << sync << '\n';
                        pending.erase(it); ++received;
                    }
                }
            }
        }
        // Commands start on first ESP data, each 500 ms. Keep payload identical
        // between protocols. TCP retains partial sends; UDP has one full command.
        i64 now=mono_us();
        if (begin && have_peer && now>=next_cmd && send_buffer.empty()) {
            std::string verb="PING";
            if (mode=="sort") verb="SORT";
            if (mode=="estop") verb="ESTOP";
            if (mode=="mixed") verb=(seq%3==0?"ESTOP":seq%3==1?"SORT":"PING");
            ++seq; i64 utc=utc_us();
            pending.emplace(seq,Pending{now,utc,verb});
            send_buffer="CMD,"+std::to_string(seq)+","+verb+","+std::to_string(utc)+"\n";
            send_offset=0; next_cmd=now+500000;
            out << utc << ',' << now << ",C," << seq << ',' << verb << '\n';
        }
        if (!send_buffer.empty() && writable(conn)) {
            int n=tcp ? send(conn,send_buffer.data()+send_offset,static_cast<int>(send_buffer.size()-send_offset),0)
                      : sendto(conn,send_buffer.data(),static_cast<int>(send_buffer.size()),0,
                               reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
            if (n<=0 || (!tcp && static_cast<size_t>(n)!=send_buffer.size())) break;
            send_offset+=n;
            if (send_offset==send_buffer.size()) { send_buffer.clear(); send_offset=0; }
        }
    }
    out << utc_us() << ',' << mono_us() << ",Z," << seq << ',' << received << ','
        << pending.size() << ',' << (begin?mono_us()-begin:0) << '\n';
    if (tcp) close_socket(conn);
    close_socket(listener);
    std::cout << "Saved " << path << "; commands=" << seq << " ACKs=" << received
              << " outstanding at end=" << pending.size() << '\n';
    return begin?0:2;
}
struct Task {
    std::vector<double> response, latency, span, block, jitter;
    unsigned misses=0, previous=0, gaps=0, history=0, consecutive=0, windows=0, violations=0;
    i64 last_event=0, deadline=0;
};
static double maximum(const std::vector<double> &v) { return v.empty()?0:*std::max_element(v.begin(),v.end()); }
static double percentile(std::vector<double> v,double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(),v.end()); return v[static_cast<size_t>(std::ceil(p*v.size()))-1];
}
static int analyze(const std::string &path,const std::string &prefix) {
    std::ifstream in(path); if (!in) throw std::runtime_error("Cannot open input");
    const char *names[]={"ENC_SENSE","SPD_CTRL","SORT_ACT","SAFETY_TASK","MONITOR_TASK","TIME_TASK","HMI"};
    Task task[7]; std::vector<double> rtt,cpu;
    std::string line, metadata;
    unsigned packets=0,seq_gap=0,last_seq=0,log_loss=0,tx_loss=0,reordered=0;
    unsigned queue_drops[7]={}, fw_jobs[7]={}, fw_misses[7]={};
    unsigned fw_windows[7]={}, fw_violations[7]={};
    double fw_wcrt[7]={};
    i64 first=0,last=0; bool have_sync=false;
    while (std::getline(in,line)) {
        auto f=split(line);
        // UART baseline accepts plain firmware CSV. Ignore IDF boot/monitor text.
        if (!f.empty() && (f[0]=="B"||f[0]=="E"||f[0]=="S"||f[0]=="T")) {
            std::string receipt=f[0]=="B" && f.size()>5?f[5]:std::to_string(last);
            f.insert(f.begin(),receipt); f.insert(f.begin(),"0");
        }
        if (f.size()<3 || f[0]=="pc_utc_us" || f[0].empty() || !std::isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        i64 receipt=std::stoll(f[1]); if (!first) first=receipt; last=receipt;
        if (f[2]=="B" && f.size()==16) {
            unsigned seq=static_cast<unsigned>(std::stoul(f[3]));
            if (last_seq && seq>last_seq+1) seq_gap+=seq-last_seq-1;
            if (last_seq && seq<=last_seq) ++reordered;
            last_seq=std::max(last_seq,seq); ++packets;
            log_loss=std::max(log_loss,static_cast<unsigned>(std::stoul(f[14])));
            tx_loss=std::max(tx_loss,static_cast<unsigned>(std::stoul(f[15])));
            have_sync|=f[8]=="1";
            metadata=f[4]+","+f[9]+","+f[10]+","+f[11]+","+f[12];
        } else if (f[2]=="E" && f.size()==11) {
            unsigned id=static_cast<unsigned>(std::stoul(f[3])); if (id>=7) continue;
            auto &t=task[id]; unsigned job=static_cast<unsigned>(std::stoul(f[5]));
            // Missing jobs break (m,k) windows; never bridge lost samples.
            if (t.previous && job!=t.previous+1) { ++t.gaps; t.consecutive=0; t.history=0; }
            bool gap=t.previous && job!=t.previous+1;
            t.previous=job;
            i64 evt=std::stoll(f[6]),start=std::stoll(f[7]),end=std::stoll(f[8]);
            t.deadline=std::stoll(f[9]); bool pass=end-evt<=t.deadline;
            if (evt>start || start>end) throw std::runtime_error("Invalid event chronology");
            t.response.push_back((end-evt)/1000.0); t.latency.push_back((start-evt)/1000.0);
            t.span.push_back((end-start)/1000.0); t.block.push_back(std::stoll(f[10])/1000.0);
            t.misses+=!pass; t.history=((t.history<<1)|pass)&1023;
            ++t.consecutive;
            if (id>=4 && t.consecutive>=10) {
                ++t.windows; unsigned bits=t.history, count=0;
                while (bits) { count+=bits&1U; bits>>=1; }
                if (count<9) ++t.violations;
            }
            if ((id==0||id==4||id==5) && t.last_event && !gap) {
                double period=id==0?5000:id==4?500000:1000000;
                t.jitter.push_back(std::abs((start-t.last_event)-period)/1000.0);
            }
            t.last_event=start;
        } else if (f[2]=="N" && f.size()==10) rtt.push_back(std::stoll(f[5])/1000.0);
        else if (f[2]=="T" && f.size()==8 && std::stoi(f[7])>=0) cpu.push_back(std::stoi(f[7])/10.0);
        else if (f[2]=="S" && f.size()==10) {
            unsigned id=static_cast<unsigned>(std::stoul(f[3])); if (id>=7) continue;
            fw_jobs[id]=std::max(fw_jobs[id],static_cast<unsigned>(std::stoul(f[4])));
            fw_misses[id]=std::max(fw_misses[id],static_cast<unsigned>(std::stoul(f[5])));
            fw_wcrt[id]=std::max(fw_wcrt[id],std::stoll(f[6])/1000.0);
            fw_windows[id]=std::max(fw_windows[id],static_cast<unsigned>(std::stoul(f[7])));
            fw_violations[id]=std::max(fw_violations[id],static_cast<unsigned>(std::stoul(f[8])));
            queue_drops[id]=std::max(queue_drops[id],static_cast<unsigned>(std::stoul(f[9])));
        }
    }
    std::ofstream csv(prefix+"_tasks.csv"), report(prefix+"_summary.md");
    if (!csv || !report) throw std::runtime_error("Cannot create analysis output");
    csv << "task,n,misses,wcrt_ms,hwm99_ms,max_latency_ms,max_span_ms,max_mutex_wait_ms,max_start_jitter_ms,firm_windows,firm_violations,job_gaps,firmware_jobs,firmware_misses,event_drops,firmware_wcrt_ms,firmware_firm_windows,firmware_firm_violations\n";
    report << "# Analise da coleta\n\nOrigem: " << path << "\n\nProtocolo, politica, preempcao, slicing, MHz: " << metadata
           << "\n\nDuracao recebida: " << (last-first)/1000000.0 << " s. Pacotes: " << packets
           << "; lacunas de sequencia: " << seq_gap << "; fora de ordem/duplicados: " << reordered
           << "; logs descartados: " << log_loss << "; falhas de transporte: " << tx_loss
           << ". SNTP observado: " << (have_sync?"sim":"nao") << ".\n\n"
           << "| Tarefa | n | Misses | WCRT ms | HWM99 ms | Latencia max ms | (9,10) violacoes/janelas |\n"
           << "|---|---:|---:|---:|---:|---:|---:|\n";
    for (unsigned i=0;i<7;++i) {
        const auto &t=task[i];
        csv << names[i] << ',' << t.response.size() << ',' << t.misses << ',' << maximum(t.response) << ','
            << percentile(t.response,.99) << ',' << maximum(t.latency) << ',' << maximum(t.span) << ','
            << maximum(t.block) << ',' << maximum(t.jitter) << ',' << t.windows << ',' << t.violations << ','
            << t.gaps << ',' << fw_jobs[i] << ',' << fw_misses[i] << ',' << queue_drops[i] << ','
            << fw_wcrt[i] << ',' << fw_windows[i] << ',' << fw_violations[i] << '\n';
        report << '|' << names[i] << '|' << t.response.size() << '|' << t.misses << '|'
               << maximum(t.response) << '|' << percentile(t.response,.99) << '|' << maximum(t.latency) << '|';
        if (i>=4 && t.windows) report << t.violations << '/' << t.windows;
        else report << "N/A";
        report << "|\n";
    }
    report << "\nRTT: n=" << rtt.size() << ", p99=" << percentile(rtt,.99) << " ms, max=" << maximum(rtt)
           << " ms. CPU ocupada: p99=" << percentile(cpu,.99) << "%, max=" << maximum(cpu)
           << "%.\n\nWCRT e o maior tempo observado, sem garantia formal. Span inclui preempcoes. "
           << "Janelas firm exigem 10 jobs consecutivos completos. Valores zero com n=0 nao sao evidencias. "
           << "Lacunas UDP podem incluir reordenacao; comparar contadores e repetir se houver perdas. "
           << "O resumo cumulativo do firmware pode incluir aquecimento anterior a coleta.\n";
    std::cout << "Saved " << prefix << "_tasks.csv and _summary.md\n";
    return 0;
}
int main(int argc,char **argv) {
#ifdef _WIN32
    WSADATA w; if (WSAStartup(MAKEWORD(2,2),&w)) return 1;
#endif
    try {
        int result;
        if (argc==4 && std::string(argv[1])=="analyze") result=analyze(argv[2],argv[3]);
        else if (argc==6) result=collect(argv[1],std::stoi(argv[2]),std::stoi(argv[3]),argv[4],argv[5]);
        else {
            std::cerr << "Usage:\n  pc_monitor udp|tcp PORT SECONDS LOG.csv ping|sort|estop|mixed\n"
                      << "  pc_monitor analyze LOG.csv OUTPUT_PREFIX\n"; return 1;
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return result;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
