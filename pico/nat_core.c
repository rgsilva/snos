#include "nat_core.h"
#include <string.h>

#define SNES 0x0a00020fu
#define GW 0x0a000202u
#define DNS 0x0a000203u
#define ICMP 1
#define TCP 6
#define UDP 17
#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16
#define MSS 1460
#define WINDOW 2048
#define RTO 2500u
#define DATA_RETRIES 120u
#define MAX_RETRIES 8u
#define TCP_IDLE 300000u
#define UDP_IDLE 60000u
#define TIME_WAIT 4000u
#define FIN_GRACE 750u
#define INBOUND_CAP 4u

typedef enum { FREE, CONNECTING, SYN_RCVD, SYN_SENT, EST, FIN_SENT, DONE } state_t;
typedef struct {
    state_t state; uint16_t sport, dport; uint32_t dip, iss, snd_nxt, snd_una, rcv_nxt;
    uint16_t peer_wnd; uint16_t unacked, out_len, in_len; uint8_t retries;
    bool peer_fin, sock_eof, fin_sent, inbound, close_write;
    uint32_t touched, sent_at, eof_at, last_console, closed_at;
    uint8_t out[MSS], in[MSS];
} tcp_t;
typedef struct { bool used; uint16_t sport, dport; uint32_t reply_from, touched; } udp_t;
typedef struct { bool used; uint16_t port; uint8_t owners; } fwd_t;
struct nat { nat_ops_t op; tcp_t tcp[NAT_TCP_MAX]; udp_t udp[NAT_UDP_MAX]; fwd_t fwd[NAT_TCP_MAX];
    uint32_t ident, iss, resolver; uint16_t next_eport; nat_stats_t stat; };

static uint16_t rd16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)rd16(p) << 16) | rd16(p + 2); }
static void wr16(uint8_t *p, uint16_t n) { p[0] = n >> 8; p[1] = n; }
static void wr32(uint8_t *p, uint32_t n) { wr16(p, n >> 16); wr16(p + 2, n); }
static uint16_t sum(const uint8_t *p, size_t n, uint32_t initial) {
    uint32_t s = initial; while (n > 1) { s += rd16(p); p += 2; n -= 2; }
    if (n) s += (uint16_t)*p << 8;
    while (s >> 16) s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)~s;
}
static uint16_t pseudo(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *p, size_t n) {
    uint32_t s = (src >> 16) + (src & 0xffffu) + (dst >> 16) + (dst & 0xffffu) + proto + n;
    return sum(p, n, s);
}
static bool expired(uint32_t now, uint32_t then, uint32_t span) { return (uint32_t)(now - then) > span; }
static void count(nat_t *n) { n->stat.tcp = n->stat.udp = n->stat.forwards = 0;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) if (n->tcp[i].state != FREE) ++n->stat.tcp;
    for (unsigned i = 0; i < NAT_UDP_MAX; ++i) if (n->udp[i].used) ++n->stat.udp;
    for (unsigned i = 0; i < NAT_TCP_MAX; ++i) if (n->fwd[i].used) ++n->stat.forwards; }
static void emit(nat_t *n, uint32_t src, uint8_t proto, const uint8_t *body, size_t len) {
    uint8_t p[NAT_MTU]; if (len + 20 > sizeof(p)) { ++n->stat.drop; return; }
    memset(p, 0, 20); p[0] = 0x45; wr16(p + 2, (uint16_t)(20 + len)); wr16(p + 4, ++n->ident);
    wr16(p + 6, 0x4000); p[8] = 64; p[9] = proto; wr32(p + 12, src); wr32(p + 16, SNES);
    wr16(p + 10, sum(p, 20, 0)); memcpy(p + 20, body, len); ++n->stat.out; n->op.emit(n->op.arg, p, len + 20);
}
static void tcp_out(nat_t *n, tcp_t *t, uint8_t flags, const uint8_t *data, size_t len, uint32_t seq) {
    uint8_t b[MSS + 24]; size_t h = (flags & SYN) ? 24 : 20; memset(b, 0, h);
    wr16(b, t->dport); wr16(b + 2, t->sport); wr32(b + 4, seq); wr32(b + 8, t->rcv_nxt);
    b[12] = (uint8_t)(h / 4 << 4); b[13] = flags; wr16(b + 14, WINDOW);
    if (flags & SYN) { b[20] = 2; b[21] = 4; wr16(b + 22, MSS); }
    if (len) memcpy(b + h, data, len);
    wr16(b + 16, pseudo(t->dip, SNES, TCP, b, h + len));
    emit(n, t->dip, TCP, b, h + len);
}
static void rst(nat_t *n, uint32_t dip, uint16_t sport, uint16_t dport, uint32_t next, uint32_t ack, bool had_ack) {
    uint8_t b[20] = {0}; wr16(b, dport); wr16(b + 2, sport); wr32(b + 4, had_ack ? ack : 0); wr32(b + 8, next);
    b[12] = 0x50; b[13] = RST | ACK; wr16(b + 16, pseudo(dip, SNES, TCP, b, sizeof(b))); emit(n, dip, TCP, b, sizeof(b));
}
static int find_tcp(nat_t *n, uint16_t sport, uint32_t dip, uint16_t dport) { for (unsigned i=0;i<NAT_TCP_MAX;++i) { tcp_t *t=&n->tcp[i]; if(t->state!=FREE&&t->sport==sport&&t->dip==dip&&t->dport==dport)return (int)i;} return -1; }
static int free_tcp(nat_t *n) { for(unsigned i=0;i<NAT_TCP_MAX;++i)if(n->tcp[i].state==FREE)return (int)i; return -1; }
static void close_tcp(nat_t *n, unsigned i) { if (n->tcp[i].state != FREE) n->op.tcp_abort(n->op.arg, i); memset(&n->tcp[i], 0, sizeof(n->tcp[i])); }
static void tcp_reset(nat_t *n, unsigned i) { tcp_t *t=&n->tcp[i]; rst(n,t->dip,t->sport,t->dport,t->rcv_nxt,t->snd_nxt,true); close_tcp(n,i); }
static void try_write(nat_t *n, unsigned i) { tcp_t *t=&n->tcp[i]; if (!t->in_len) return; if (!n->op.tcp_write(n->op.arg,i,t->in,t->in_len)) return;
    t->rcv_nxt += t->in_len; t->in_len=0; tcp_out(n,t,ACK,NULL,0,t->snd_nxt); if(t->close_write){n->op.tcp_shutdown(n->op.arg,i);t->close_write=false;} }
static void in_tcp(nat_t *n, uint32_t dip, const uint8_t *b, size_t len, uint32_t now) {
    if(len<20){++n->stat.drop;return;} uint16_t sport=rd16(b),dport=rd16(b+2),wnd=rd16(b+14); uint32_t seq=rd32(b+4),ack=rd32(b+8);
    size_t off=(b[12]>>4)*4; uint8_t flags=b[13]; if(off<20||off>len||pseudo(SNES,dip,TCP,b,len)){++n->stat.drop;return;} const uint8_t *data=b+off; size_t dn=len-off;
    int slot=find_tcp(n,sport,dip,dport); if(slot>=0&&(flags&SYN)&&!(flags&ACK)&&(n->tcp[slot].state==EST||n->tcp[slot].state==FIN_SENT||n->tcp[slot].state==DONE)){close_tcp(n,slot);slot=-1;}
    if(slot<0){ if(flags&SYN){ slot=free_tcp(n); if(slot<0){++n->stat.drop;return;} tcp_t *t=&n->tcp[slot]; memset(t,0,sizeof(*t));t->state=CONNECTING;t->sport=sport;t->dport=dport;t->dip=dip;t->iss=(n->iss+=0x1000);t->snd_nxt=t->snd_una=t->iss;t->rcv_nxt=seq+1;t->touched=now; if(!n->op.tcp_connect(n->op.arg,slot,dip,dport))tcp_reset(n,slot); } else if(!(flags&RST)) rst(n,dip,sport,dport,seq+(uint32_t)dn+((flags&FIN)?1:0),ack,flags&ACK); return; }
    tcp_t *t=&n->tcp[slot]; t->touched=now;t->peer_wnd=wnd; if(flags&RST){close_tcp(n,slot);return;} if(t->state==CONNECTING)return;
    if(t->state==SYN_RCVD){if(!(flags&ACK)||ack!=t->iss+1)return;t->state=EST;t->snd_una=ack;} if(t->state==SYN_SENT){if((flags&(SYN|ACK))==(SYN|ACK)&&ack==t->iss+1){t->rcv_nxt=seq+1;t->snd_una=ack;t->state=EST;tcp_out(n,t,ACK,NULL,0,t->snd_nxt);}return;}
    if(flags&ACK){uint32_t a=ack-t->snd_una; if(a&&a<=t->unacked+(uint32_t)(t->fin_sent?1:0)){uint16_t d=a<t->unacked?(uint16_t)a:t->unacked; if(d){memmove(t->out,t->out+d,t->out_len-d);t->out_len-=d;t->unacked-=d;n->op.tcp_recved(n->op.arg,(unsigned)slot,d);} t->snd_una=ack;t->retries=0;if(t->fin_sent&&a>d){t->state=DONE;t->closed_at=now;}}}
    if(dn){if(t->in_len){} else if(seq==t->rcv_nxt){if(dn>MSS){++n->stat.drop;return;}memcpy(t->in,data,dn);t->in_len=dn;t->last_console=now;try_write(n,(unsigned)slot);}else tcp_out(n,t,ACK,NULL,0,t->snd_nxt);}
    if(flags&FIN){uint32_t fs=seq+(uint32_t)dn;if(!t->peer_fin&&fs==t->rcv_nxt){t->rcv_nxt++;t->peer_fin=true;tcp_out(n,t,ACK,NULL,0,t->snd_nxt);if(!t->in_len)n->op.tcp_shutdown(n->op.arg,(unsigned)slot);else t->close_write=true;}else if(t->peer_fin&&fs+1==t->rcv_nxt)tcp_out(n,t,ACK,NULL,0,t->snd_nxt);}
}
static void in_udp(nat_t *n,uint32_t dip,const uint8_t *b,size_t len,uint32_t now){if(len<8||rd16(b+4)<8||rd16(b+4)>len||(rd16(b+6)&&pseudo(SNES,dip,UDP,b,rd16(b+4)))){++n->stat.drop;return;}uint16_t sp=rd16(b),dp=rd16(b+2),ln=rd16(b+4);int s=-1;for(unsigned i=0;i<NAT_UDP_MAX;++i)if(n->udp[i].used&&n->udp[i].sport==sp&&n->udp[i].reply_from==dip&&n->udp[i].dport==dp)s=i;if(s<0)for(unsigned i=0;i<NAT_UDP_MAX;++i)if(!n->udp[i].used){s=i;break;}if(s<0){++n->stat.drop;return;}udp_t *u=&n->udp[s];if(!u->used)*u=(udp_t){.used=true,.sport=sp,.dport=dp,.reply_from=dip};u->touched=now;uint32_t target=dip;if(dip==DNS)target=n->resolver?n->resolver:0x08080808u;if(!n->op.udp_send(n->op.arg,(unsigned)s,target,dp,b+8,ln-8))++n->stat.drop;}
static void in_icmp(nat_t *n,uint32_t dip,const uint8_t *b,size_t len){if(len<8||sum(b,len,0)||b[0]!=8){++n->stat.drop;return;}if(dip==GW||dip==DNS){uint8_t r[NAT_MTU];memcpy(r,b,len);r[0]=0;wr16(r+2,0);wr16(r+2,sum(r,len,0));emit(n,dip,ICMP,r,len);}else ++n->stat.drop;}
size_t nat_core_size(void) { return sizeof(nat_t); }
void nat_core_init(nat_t *n,const nat_ops_t *op){memset(n,0,sizeof(*n));n->op=*op;n->iss=0x2000;n->next_eport=40000;}
void nat_core_set_resolver(nat_t *n,uint32_t address){n->resolver=address;}
void nat_core_reset(nat_t *n){nat_ops_t op=n->op;for(unsigned i=0;i<NAT_TCP_MAX;++i)if(n->tcp[i].state!=FREE)op.tcp_abort(op.arg,i);nat_core_init(n,&op);}
void nat_core_input(nat_t *n,const uint8_t *p,size_t len,uint32_t now){if(len<20||(p[0]>>4)!=4){++n->stat.drop;return;}size_t h=(p[0]&15)*4,total=rd16(p+2);if(h<20||total<h||total>len||sum(p,h,0)||rd32(p+12)!=SNES){++n->stat.drop;return;}++n->stat.in;switch(p[9]){case ICMP:in_icmp(n,rd32(p+16),p+h,total-h);break;case UDP:in_udp(n,rd32(p+16),p+h,total-h,now);break;case TCP:in_tcp(n,rd32(p+16),p+h,total-h,now);break;default:++n->stat.drop;}}
void nat_core_tcp_connected(nat_t *n,unsigned i,int error,uint32_t now){if(i>=NAT_TCP_MAX||n->tcp[i].state!=CONNECTING)return;if(error){tcp_reset(n,i);return;}tcp_t *t=&n->tcp[i];t->state=SYN_RCVD;tcp_out(n,t,SYN|ACK,NULL,0,t->iss);t->snd_nxt=t->iss+1;t->sent_at=now;}
bool nat_core_tcp_received(nat_t *n,unsigned i,const uint8_t *d,size_t len,uint32_t now){if(i>=NAT_TCP_MAX||n->tcp[i].state==FREE||len>MSS||n->tcp[i].out_len)return false;tcp_t *t=&n->tcp[i];memcpy(t->out,d,len);t->out_len=len;t->touched=now;return true;}
void nat_core_tcp_eof(nat_t *n,unsigned i,uint32_t now){if(i<NAT_TCP_MAX&&n->tcp[i].state!=FREE){n->tcp[i].sock_eof=true;n->tcp[i].eof_at=now;}}
void nat_core_tcp_error(nat_t *n,unsigned i){if(i<NAT_TCP_MAX)close_tcp(n,i);}
void nat_core_udp_received(nat_t *n,unsigned i,const uint8_t *d,size_t len,uint32_t now){if(i>=NAT_UDP_MAX||!n->udp[i].used)return;if(len>NAT_MTU-28){++n->stat.drop;return;}udp_t *u=&n->udp[i];uint8_t b[NAT_MTU];wr16(b,u->dport);wr16(b+2,u->sport);wr16(b+4,(uint16_t)(8+len));wr16(b+6,0);memcpy(b+8,d,len);uint16_t c=pseudo(u->reply_from,SNES,UDP,b,8+len);wr16(b+6,c?c:0xffff);u->touched=now;emit(n,u->reply_from,UDP,b,8+len);}
int nat_core_inbound_accept(nat_t *n,uint16_t port,uint32_t now){unsigned live=0;for(unsigned i=0;i<NAT_TCP_MAX;++i)if(n->tcp[i].state!=FREE&&n->tcp[i].inbound&&n->tcp[i].sport==port&&n->tcp[i].state!=DONE)++live;if(live>=INBOUND_CAP)return -1;int i=free_tcp(n);if(i<0)return -1;tcp_t *t=&n->tcp[i];memset(t,0,sizeof(*t));t->state=SYN_SENT;t->inbound=true;t->sport=port;t->dport=n->next_eport++;if(n->next_eport>=60000)n->next_eport=40000;t->dip=GW;t->iss=(n->iss+=0x1000);t->snd_nxt=t->snd_una=t->iss;t->sent_at=now;tcp_out(n,t,SYN,NULL,0,t->iss);t->snd_nxt++;return i;}
bool nat_core_forward_add(nat_t *n,uint16_t port,uint8_t ch){for(unsigned i=0;i<NAT_TCP_MAX;++i)if(n->fwd[i].used&&n->fwd[i].port==port){n->fwd[i].owners|=(uint8_t)(1u<<(ch&7));return true;}for(unsigned i=0;i<NAT_TCP_MAX;++i)if(!n->fwd[i].used){if(!n->op.forward_add(n->op.arg,port))return false;n->fwd[i]=(fwd_t){true,port,(uint8_t)(1u<<(ch&7))};return true;}return false;}
void nat_core_forward_del(nat_t *n,uint16_t port,uint8_t ch){for(unsigned i=0;i<NAT_TCP_MAX;++i)if(n->fwd[i].used&&n->fwd[i].port==port){n->fwd[i].owners&=(uint8_t)~(1u<<(ch&7));if(!n->fwd[i].owners){n->op.forward_del(n->op.arg,port);n->fwd[i].used=false;}return;}}
void nat_core_poll(nat_t *n,uint32_t now){for(unsigned i=0;i<NAT_TCP_MAX;++i){tcp_t *t=&n->tcp[i];if(t->state==FREE)continue;if(t->state==DONE){if(expired(now,t->closed_at,TIME_WAIT))close_tcp(n,i);continue;}if(expired(now,t->touched,TCP_IDLE)){close_tcp(n,i);continue;}try_write(n,i);if((t->state==SYN_RCVD||t->state==SYN_SENT)&&expired(now,t->sent_at,RTO)){if(++t->retries>MAX_RETRIES){tcp_reset(n,i);continue;}tcp_out(n,t,t->state==SYN_RCVD?(SYN|ACK):SYN,NULL,0,t->iss);t->sent_at=now;continue;}if(t->out_len&&t->unacked==0&&t->peer_wnd>=t->out_len){t->unacked=t->out_len;tcp_out(n,t,PSH|ACK,t->out,t->out_len,t->snd_una);t->snd_nxt=t->snd_una+t->out_len;t->sent_at=now;t->retries=0;}else if(t->unacked&&expired(now,t->sent_at,RTO)){if(++t->retries>DATA_RETRIES){tcp_reset(n,i);continue;}tcp_out(n,t,PSH|ACK,t->out,t->unacked,t->snd_una);t->sent_at=now;}else if(t->sock_eof&&!t->fin_sent&&!t->out_len&&(!t->inbound||expired(now,t->eof_at>t->last_console?t->eof_at:t->last_console,FIN_GRACE))){t->fin_sent=true;tcp_out(n,t,FIN|ACK,NULL,0,t->snd_nxt++);t->sent_at=now;t->retries=0;}else if(t->fin_sent&&expired(now,t->sent_at,RTO)){if(++t->retries>MAX_RETRIES){close_tcp(n,i);continue;}tcp_out(n,t,FIN|ACK,NULL,0,t->snd_nxt-1);t->sent_at=now;}}
for(unsigned i=0;i<NAT_UDP_MAX;++i)if(n->udp[i].used&&expired(now,n->udp[i].touched,UDP_IDLE))n->udp[i].used=false;
count(n);}
nat_stats_t nat_core_stats(const nat_t *n){return n->stat;}
