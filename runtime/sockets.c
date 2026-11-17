// wsock32.dll (imported by ordinal) on BSD sockets, plus iphlpapi's adapter query.
// Ordinals 1-23 are alphabetical: ... 9 htons, 10 inet_addr, 11 inet_ntoa, 12 ioctlsocket, 13 listen ...
// SOCKET values are host fds. Windows sockaddr has a 16-bit family where macOS has sin_len + an 8-bit
// family; fd_set is a counted SOCKET array; timeval fields are 32-bit; SO_RCVTIMEO/SNDTIMEO are DWORD ms.
#include "rt.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define INVALID_SOCKET 0xFFFFFFFFFFFFFFFFull
#define SOCKET_ERROR 0xFFFFFFFFu
#define W_AF_INET6 23

static uint32_t wsa_err(int e) {
    switch (e) {
    case EWOULDBLOCK: return 10035;   // WSAEWOULDBLOCK (EAGAIN)
    case EINPROGRESS: return 10035;   // non-blocking connect: Windows reports WOULDBLOCK
    case EALREADY: return 10037;
    case ENOTSOCK: case EBADF: return 10038;
    case EMSGSIZE: return 10040;
    case EINVAL: return 10022;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EAFNOSUPPORT: return 10047;
    case EACCES: return 10013;
    default: return 10004 + 0 * e;   // WSAEINTR as a generic failure
    }
}
static uint32_t fail(void) { set_last_error(wsa_err(errno)); return SOCKET_ERROR; }

// guest sockaddr -> host; returns the host length (0 if unsupported)
static socklen_t addr_in(uint64_t p, uint32_t len, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (!p || len < 2) return 0;
    uint16_t fam = rd16(p);
    if (fam == AF_INET && len >= 16) {
        struct sockaddr_in *a = (struct sockaddr_in *)out;
        a->sin_len = sizeof *a; a->sin_family = AF_INET;
        memcpy(&a->sin_port, PTR(p + 2), 2); memcpy(&a->sin_addr, PTR(p + 4), 4);
        return sizeof *a;
    }
    if (fam == W_AF_INET6 && len >= 28) {   // sin6_port +2, sin6_flowinfo +4, sin6_addr +8, sin6_scope_id +24
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)out;
        a->sin6_len = sizeof *a; a->sin6_family = AF_INET6;
        memcpy(&a->sin6_port, PTR(p + 2), 2); memcpy(&a->sin6_flowinfo, PTR(p + 4), 4);
        memcpy(&a->sin6_addr, PTR(p + 8), 16); memcpy(&a->sin6_scope_id, PTR(p + 24), 4);
        return sizeof *a;
    }
    return 0;
}
// host sockaddr -> guest buffer with in/out length
static void addr_out(const struct sockaddr_storage *in, uint64_t p, uint64_t plen) {
    if (!p || !plen) return;
    uint32_t cap = rd32(plen);
    uint8_t buf[28] = { 0 }; uint32_t n = 0;
    if (in->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)in;
        buf[0] = AF_INET; memcpy(buf + 2, &a->sin_port, 2); memcpy(buf + 4, &a->sin_addr, 4); n = 16;
    } else if (in->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)in;
        buf[0] = W_AF_INET6; memcpy(buf + 2, &a->sin6_port, 2); memcpy(buf + 4, &a->sin6_flowinfo, 4);
        memcpy(buf + 8, &a->sin6_addr, 16); memcpy(buf + 24, &a->sin6_scope_id, 4); n = 28;
    }
    memcpy(PTR(p), buf, n < cap ? n : cap);
    wr32(plen, n);
}
static int sfd(uint64_t s) { return s == INVALID_SOCKET ? -1 : (int)s; }
// RT_NET_TRACE=1: log socket setup and the first receive on each socket (with its blocking mode)
static int net_trace = -1;
#define NT(...) do { if (net_trace < 0) net_trace = getenv("RT_NET_TRACE") != NULL; if (net_trace) rt_log(__VA_ARGS__); } while (0)

// #115 WSAStartup(version, WSADATA*) / #116 WSACleanup / #111 WSAGetLastError / #112 WSASetLastError
void I_wsock32_dll__115(CPU *c) {
    uint64_t d = A1;
    if (d) { memset(PTR(d), 0, 408); wr16(d, 0x0202); wr16(d + 2, 0x0202); wr16(d + 4, 32767); wr16(d + 6, 65467); strcpy(PTR(d + 16), "WinSock 2.0"); strcpy(PTR(d + 273), "Running"); }
    RET(0);
}
void I_wsock32_dll__116(CPU *c) { RET(0); }
void I_wsock32_dll__111(CPU *c) { RET(get_last_error()); }
void I_wsock32_dll__112(CPU *c) { set_last_error((uint32_t)A0); }
// #23 socket(af, type, protocol)
void I_wsock32_dll__23(CPU *c) {
    int af = (int)A0 == W_AF_INET6 ? AF_INET6 : (int)A0;
    int fd = socket(af, (int)A1, (int)A2);
    if (fd < 0) { set_last_error(wsa_err(errno)); RET(INVALID_SOCKET); return; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    NT("[net] socket(af %d, type %d, proto %d) -> %d\n", (int)A0, (int)A1, (int)A2, fd);
    RET((uint64_t)fd);
}
void I_wsock32_dll__3(CPU *c) { RET(close(sfd(A0)) ? fail() : 0); }   // closesocket
// #2 bind(s, addr, len) / #4 connect(s, addr, len)
static void addr_call(CPU *c, int connect_) {
    struct sockaddr_storage a; socklen_t n = addr_in(A1, (uint32_t)A2, &a);
    if (!n) { set_last_error(10047); RET(SOCKET_ERROR); return; }
    int r = connect_ ? connect(sfd(A0), (struct sockaddr *)&a, n) : bind(sfd(A0), (struct sockaddr *)&a, n);
    NT("[net] %s(%d) -> %d (errno %d)\n", connect_ ? "connect" : "bind", sfd(A0), r, r ? errno : 0);
    RET(r ? fail() : 0);
}
void I_wsock32_dll__2(CPU *c) { addr_call(c, 0); }
void I_wsock32_dll__4(CPU *c) { addr_call(c, 1); }
// #6 getsockname(s, addr, *len) / #5 getpeername
static void name_call(CPU *c, int peer) {
    struct sockaddr_storage a; socklen_t n = sizeof a;
    int r = peer ? getpeername(sfd(A0), (struct sockaddr *)&a, &n) : getsockname(sfd(A0), (struct sockaddr *)&a, &n);
    if (r) { RET(fail()); return; }
    addr_out(&a, A1, A2); RET(0);
}
void I_wsock32_dll__6(CPU *c) { name_call(c, 0); }
void I_wsock32_dll__5(CPU *c) { name_call(c, 1); }
// #12 ioctlsocket(s, cmd, *arg): FIONBIO, FIONREAD
void I_wsock32_dll__12(CPU *c) {
    int fd = sfd(A0); uint32_t cmd = (uint32_t)A1;
    NT("[net] ioctlsocket(%d, 0x%x, %u)\n", fd, cmd, A2 ? rd32(A2) : 0);
    if (cmd == 0x8004667Eu) {   // FIONBIO
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || fcntl(fd, F_SETFL, rd32(A2) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK))) { RET(fail()); return; }
        RET(0); return;
    }
    if (cmd == 0x4004667Fu) { int n = 0; if (ioctl(fd, FIONREAD, &n)) { RET(fail()); return; } wr32(A2, (uint32_t)n); RET(0); return; }
    set_last_error(10022); RET(SOCKET_ERROR);
}
// #21 setsockopt(s, level, name, value, len) / #7 getsockopt(s, level, name, value, *len)
static int map_opt(int level, int name, int *hl, int *hn) {
    *hl = level == 0xFFFF ? SOL_SOCKET : level; *hn = name;
    if (level == 0xFFFF) {
        switch (name) {
        case 0x0004: *hn = SO_REUSEADDR; return 1;  case 0x0020: *hn = SO_BROADCAST; return 1;
        case 0x0008: *hn = SO_KEEPALIVE; return 1;  case 0x0080: *hn = SO_LINGER; return 1;
        case 0x1001: *hn = SO_SNDBUF; return 1;     case 0x1002: *hn = SO_RCVBUF; return 1;
        case 0x1005: *hn = SO_SNDTIMEO; return 2;   case 0x1006: *hn = SO_RCVTIMEO; return 2;   // DWORD ms
        case 0x1007: *hn = SO_ERROR; return 1;      case 0x1008: *hn = SO_TYPE; return 1;
        case (int)0xFFFFFFFBu: return 0;            // SO_EXCLUSIVEADDRUSE: no equivalent; accept
        default: return 0;
        }
    }
    if (level == IPPROTO_TCP && name == 1) { *hn = TCP_NODELAY; return 1; }
    if (level == IPPROTO_IP && name == 4) { *hn = IP_TTL; return 1; }
    return 0;
}
void I_wsock32_dll__21(CPU *c) {
    int hl, hn, kind = map_opt((int)A1, (int)A2, &hl, &hn);
    if (!kind) { RET(0); return; }   // unknown options are accepted and ignored
    int r;
    if (kind == 2) { uint32_t ms = rd32(A3); struct timeval tv = { ms / 1000, (int)(ms % 1000) * 1000 }; r = setsockopt(sfd(A0), hl, hn, &tv, sizeof tv); }
    else if (hn == SO_LINGER) { struct linger l = { rd16(A3), rd16(A3 + 2) }; r = setsockopt(sfd(A0), hl, hn, &l, sizeof l); }
    else { int v = (uint32_t)AN(4) >= 4 ? (int)rd32(A3) : rd8(A3); r = setsockopt(sfd(A0), hl, hn, &v, sizeof v); }
    RET(r ? fail() : 0);
}
void I_wsock32_dll__7(CPU *c) {
    int hl, hn, kind = map_opt((int)A1, (int)A2, &hl, &hn);
    uint64_t out = A3, plen = AN(4);
    if (!kind) { if (rd32(plen) >= 4) { wr32(out, 0); wr32(plen, 4); } RET(0); return; }
    if (kind == 2) { struct timeval tv; socklen_t n = sizeof tv; if (getsockopt(sfd(A0), hl, hn, &tv, &n)) { RET(fail()); return; } wr32(out, (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000)); wr32(plen, 4); RET(0); return; }
    int v = 0; socklen_t n = sizeof v;
    if (getsockopt(sfd(A0), hl, hn, &v, &n)) { RET(fail()); return; }
    if (hn == SO_ERROR) v = v ? (int)wsa_err(v) : 0;
    wr32(out, (uint32_t)v); wr32(plen, 4); RET(0);
}
// #16 recv(s, buf, len, flags) / #19 send(s, buf, len, flags): MSG_OOB 1, MSG_PEEK 2 match
void I_wsock32_dll__16(CPU *c) { ssize_t r = recv(sfd(A0), PTR(A1), (size_t)(int32_t)A2, (int)A3 & 3); RET(r < 0 ? fail() : (uint32_t)r); }
void I_wsock32_dll__19(CPU *c) { ssize_t r = send(sfd(A0), PTR(A1), (size_t)(int32_t)A2, (int)A3 & 1); RET(r < 0 ? fail() : (uint32_t)r); }
// #17 recvfrom(s, buf, len, flags, from, *fromlen)
void I_wsock32_dll__17(CPU *c) {
    struct sockaddr_storage a; socklen_t n = sizeof a;
    { static uint8_t seen[1024]; int fd = sfd(A0);
      if (fd >= 0 && fd < 1024 && !seen[fd]) { seen[fd] = 1; NT("[net] first recvfrom(%d, flags %d): %s\n", fd, (int)A3, (fcntl(fd, F_GETFL) & O_NONBLOCK) ? "non-blocking" : "BLOCKING"); } }
    ssize_t r = recvfrom(sfd(A0), PTR(A1), (size_t)(int32_t)A2, (int)A3 & 3, (struct sockaddr *)&a, &n);
    if (r < 0) { RET(fail()); return; }
    addr_out(&a, AN(4), AN(5));
    RET((uint32_t)r);
}
// #20 sendto(s, buf, len, flags, to, tolen)
void I_wsock32_dll__20(CPU *c) {
    struct sockaddr_storage a; socklen_t n = addr_in(AN(4), (uint32_t)AN(5), &a);
    ssize_t r = n ? sendto(sfd(A0), PTR(A1), (size_t)(int32_t)A2, (int)A3 & 1, (struct sockaddr *)&a, n)
                  : send(sfd(A0), PTR(A1), (size_t)(int32_t)A2, (int)A3 & 1);
    RET(r < 0 ? fail() : (uint32_t)r);
}
// #18 select(nfds, read, write, except, timeval*): fd_set = { u_int fd_count; SOCKET fd_array[64] } (x64: array at +8)
static int set_in(uint64_t p, fd_set *s, int *maxfd) {
    FD_ZERO(s);
    if (!p) return 0;
    uint32_t n = rd32(p); if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) { int fd = (int)rd64(p + 8 + 8ull * i); if (fd >= 0 && fd < FD_SETSIZE) { FD_SET(fd, s); if (fd > *maxfd) *maxfd = fd; } }
    return 1;
}
static void set_out(uint64_t p, fd_set *s) {
    if (!p) return;
    uint32_t n = rd32(p), k = 0; if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) { uint64_t fd = rd64(p + 8 + 8ull * i); if ((int)fd >= 0 && (int)fd < FD_SETSIZE && FD_ISSET((int)fd, s)) wr64(p + 8 + 8ull * k++, fd); }
    wr32(p, k);
}
void I_wsock32_dll__18(CPU *c) {
    fd_set r, w, e; int maxfd = -1;
    { static int n; if (n < 5) { n++; NT("[net] select(read %u, write %u, except %u, timeout %s)\n", A1 ? rd32(A1) : 0, A2 ? rd32(A2) : 0, A3 ? rd32(A3) : 0, AN(4) ? "set" : "none"); } }
    set_in(A1, &r, &maxfd); set_in(A2, &w, &maxfd); set_in(A3, &e, &maxfd);
    struct timeval tv, *ptv = NULL;
    if (AN(4)) { tv.tv_sec = (int32_t)rd32(AN(4)); tv.tv_usec = (int32_t)rd32(AN(4) + 4); ptv = &tv; }
    int n = select(maxfd + 1, A1 ? &r : NULL, A2 ? &w : NULL, A3 ? &e : NULL, ptv);
    if (n < 0) { RET(fail()); return; }
    set_out(A1, &r); set_out(A2, &w); set_out(A3, &e);
    RET((uint32_t)n);
}
// #8 htonl, #9 htons, #14 ntohl, #15 ntohs
void I_wsock32_dll__8(CPU *c) { RET(__builtin_bswap32((uint32_t)A0)); }
void I_wsock32_dll__9(CPU *c) { RET(__builtin_bswap16((uint16_t)A0)); }
void I_wsock32_dll__14(CPU *c) { RET(__builtin_bswap32((uint32_t)A0)); }
void I_wsock32_dll__15(CPU *c) { RET(__builtin_bswap16((uint16_t)A0)); }
// #11 inet_ntoa(in_addr by value) -> per-thread static string
void I_wsock32_dll__11(CPU *c) {
    static __thread char buf[16];
    uint32_t a = (uint32_t)A0;
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    RET(G(buf));
}
// #10 inet_addr("a.b.c.d")
void I_wsock32_dll__10(CPU *c) { struct in_addr a; RET(inet_aton(PTR(A0), &a) ? a.s_addr : 0xFFFFFFFFu); }
// #52 gethostbyname(name): IPv4 only. hostent (x64): h_name +0, h_aliases +8, h_addrtype +16, h_length +18, h_addr_list +24
void I_wsock32_dll__52(CPU *c) {
    static __thread uint8_t he[32]; static __thread char name[256]; static __thread uint32_t addrs[8]; static __thread uint64_t list[9], aliases[1];
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    if (!A0 || getaddrinfo(PTR(A0), NULL, &hints, &res)) { set_last_error(11001); RET(0); return; }   // WSAHOST_NOT_FOUND
    int n = 0;
    for (struct addrinfo *r = res; r && n < 8; r = r->ai_next) addrs[n++] = ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    strlcpy(name, PTR(A0), sizeof name);
    for (int i = 0; i < n; i++) list[i] = G(&addrs[i]);
    list[n] = 0; aliases[0] = 0;
    uint64_t p = G(he);
    wr64(p, G(name)); wr64(p + 8, G(aliases)); wr16(p + 16, AF_INET); wr16(p + 18, 4); wr64(p + 24, G(list));
    RET(p);
}
// #57 gethostname(buf, len)
void I_wsock32_dll__57(CPU *c) { if (gethostname(PTR(A0), (size_t)(int32_t)A1)) { RET(fail()); return; } RET(0); }

// iphlpapi GetAdaptersInfo(IP_ADAPTER_INFO*, *size): the active IPv4 interfaces (not loopback), with their MAC
// address. Win64 IP_ADAPTER_INFO is 704 bytes: Next +0, ComboIndex +8, AdapterName[260] +12, Description[132]
// +272, AddressLength +404, Address[8] +408, Index +416, Type +420, DhcpEnabled +424, CurrentIpAddress +432,
// IpAddressList +440 (IP_ADDR_STRING: Next +0, IpAddress[16] +8, IpMask[16] +24, Context +40), GatewayList +488.
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <arpa/inet.h>
#define ADAPTER_SIZE 704
void I_iphlpapi_dll_GetAdaptersInfo(CPU *c) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs)) { RET(232); return; }   // ERROR_NO_DATA
    struct { char name[IFNAMSIZ]; struct in_addr ip, mask; uint8_t mac[6]; int hasmac; unsigned idx; } a[16]; int n = 0;
    for (struct ifaddrs *i = ifs; i && n < 16; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        snprintf(a[n].name, sizeof a[n].name, "%s", i->ifa_name);
        a[n].ip = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
        a[n].mask = i->ifa_netmask ? ((struct sockaddr_in *)i->ifa_netmask)->sin_addr : (struct in_addr){ 0 };
        a[n].hasmac = 0; a[n].idx = if_nametoindex(i->ifa_name);
        for (struct ifaddrs *l = ifs; l; l = l->ifa_next)
            if (l->ifa_addr && l->ifa_addr->sa_family == AF_LINK && !strcmp(l->ifa_name, i->ifa_name)) {
                struct sockaddr_dl *dl = (struct sockaddr_dl *)l->ifa_addr;
                if (dl->sdl_alen == 6) { memcpy(a[n].mac, LLADDR(dl), 6); a[n].hasmac = 1; }
            }
        n++;
    }
    freeifaddrs(ifs);
    if (!n) { RET(232); return; }
    uint32_t need = (uint32_t)n * ADAPTER_SIZE, have = A1 ? rd32(A1) : 0;
    if (A1) wr32(A1, need);
    if (!A0 || have < need) { RET(111); return; }   // ERROR_BUFFER_OVERFLOW
    memset(PTR(A0), 0, need);
    for (int k = 0; k < n; k++) {
        uint64_t p = A0 + (uint64_t)k * ADAPTER_SIZE;
        wr64(p, k + 1 < n ? p + ADAPTER_SIZE : 0);
        wr32(p + 8, a[k].idx);
        snprintf(PTR(p + 12), 260, "{%08X-0000-0000-0000-%012X}", a[k].idx, a[k].idx);
        snprintf(PTR(p + 272), 132, "%s", a[k].name);
        wr32(p + 404, a[k].hasmac ? 6 : 0);
        memcpy(PTR(p + 408), a[k].mac, 6);
        wr32(p + 416, a[k].idx);
        wr32(p + 420, 6);   // MIB_IF_TYPE_ETHERNET
        wr32(p + 424, 1);
        wr64(p + 432, p + 440);
        inet_ntop(AF_INET, &a[k].ip, PTR(p + 440 + 8), 16);
        inet_ntop(AF_INET, &a[k].mask, PTR(p + 440 + 24), 16);
        snprintf(PTR(p + 488 + 8), 16, "0.0.0.0");
    }
    RET(0);
}
