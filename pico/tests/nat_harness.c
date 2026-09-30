#include "../nat_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef union { max_align_t align; unsigned char bytes[40000]; } store_t;
static store_t store;
static nat_t *n = (nat_t *)&store;
static unsigned emitted, connects, writes, forwards, recved;
static unsigned char last[NAT_MTU]; static size_t last_len;
static int write_ok = 1;
static uint16_t fold(const unsigned char *p, size_t z, unsigned s) { while(z>1){s+=((unsigned)p[0]<<8)|p[1];p+=2;z-=2;}if(z)s+=(unsigned)*p<<8;while(s>>16)s=(s&65535)+(s>>16);return (unsigned short)~s; }
static void emit(void *x,const unsigned char *p,size_t z){(void)x;++emitted;last_len=z;memcpy(last,p,z);}
static bool connect_(void*x,unsigned i,uint32_t a,uint16_t p){(void)x;(void)i;(void)a;(void)p;++connects;return true;}
static bool write_(void*x,unsigned i,const unsigned char*p,size_t z){(void)x;(void)i;(void)p;(void)z;++writes;return write_ok;}
static void no(void*x,unsigned i){(void)x;(void)i;}
static bool udp(void*x,unsigned i,uint32_t a,uint16_t p,const unsigned char*d,size_t z){(void)x;(void)i;(void)a;(void)p;(void)d;(void)z;return true;}
static bool fadd(void*x,uint16_t p){(void)x;(void)p;++forwards;return true;}
static void fdel(void*x,uint16_t p){(void)x;(void)p;}
static void got(void*x,unsigned i,size_t z){(void)x;(void)i;recved+=z;}
static void ip(unsigned char *p,unsigned proto,uint32_t dst,const unsigned char *b,size_t z){memset(p,0,20);p[0]=0x45;p[2]=(20+z)>>8;p[3]=20+z;p[8]=64;p[9]=proto;p[12]=10;p[14]=2;p[15]=15;p[16]=dst>>24;p[17]=dst>>16;p[18]=dst>>8;p[19]=dst;unsigned c=fold(p,20,0);p[10]=c>>8;p[11]=c;memcpy(p+20,b,z);}
static void tcp(unsigned char *b,unsigned flags,unsigned seq,unsigned ack,const char *d){size_t z=d?strlen(d):0;memset(b,0,20+z);b[0]=0x13;b[1]=0x88;b[2]=0;b[3]=80;b[4]=seq>>24;b[5]=seq>>16;b[6]=seq>>8;b[7]=seq;b[8]=ack>>24;b[9]=ack>>16;b[10]=ack>>8;b[11]=ack;b[12]=0x50;b[13]=flags;b[14]=4;b[15]=0; if(d)memcpy(b+20,d,z);unsigned s=0x0a00+0x020f+0x7f00+1+6+20+z, c=fold(b,20+z,s);b[16]=c>>8;b[17]=c;}
int main(void){nat_ops_t o={emit,connect_,write_,no,no,udp,fadd,fdel,got,NULL};nat_core_init(n,&o);unsigned char p[NAT_MTU],b[NAT_MTU];
    memset(b,0,9);b[0]=8;b[4]=0x53;b[5]=0x41;b[8]=1;unsigned c=fold(b,9,0);b[2]=c>>8;b[3]=c;ip(p,1,0x0a000202,b,9);nat_core_input(n,p,29,1);if(emitted!=1){fprintf(stderr,"icmp emit=%u drop=%u\n",emitted,nat_core_stats(n).drop);return 1;}assert(last[20]==0);
    b[0]=0x11;b[1]=0x11;b[2]=0;b[3]=53;b[4]=0;b[5]=11;b[6]=b[7]=0;memcpy(b+8,"dns",3);ip(p,17,0x0a000203,b,11);nat_core_input(n,p,31,2);nat_core_udp_received(n,0,(const unsigned char *)"ok",2,3);assert(last[12]==10&&last[15]==3);
    tcp(b,2,0x1111,0,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,4);assert(connects==1);nat_core_tcp_connected(n,0,0,5);assert(last[33]&0x12);
    tcp(b,16,0x1112,0x3001,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,6);assert(nat_core_tcp_received(n,0,(const unsigned char *)"UP",2,7));nat_core_poll(n,8);assert(last_len>40&&last[40]=='U');tcp(b,16,0x1112,0x3003,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,9);assert(recved==2);
    write_ok=0;emitted=0;tcp(b,8|16,0x1112,0x3003,"hold");ip(p,6,0x7f000001,b,24);nat_core_input(n,p,44,10);assert(!emitted);write_ok=1;
    emitted=0;tcp(b,16,1,1,NULL);b[2]=0;b[3]=81;b[16]=b[17]=0;c=fold(b,20,0x0a00+0x020f+0x7f00+1+6+20);b[16]=c>>8;b[17]=c;ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,10);assert(emitted&&last[33]&4);
    /* A bare SYN reuses the tuple; a failed real connect becomes RST. */
    tcp(b,2,0x2222,0,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,11);assert(connects==2);emitted=0;nat_core_tcp_connected(n,0,1,12);assert(emitted&&last[33]&4);
    /* Re-open, then carry FIN after data and let the remote EOF close it. */
    tcp(b,2,0x3333,0,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,13);nat_core_tcp_connected(n,0,0,14);tcp(b,16,0x3334,0x5001,NULL);ip(p,6,0x7f000001,b,20);nat_core_input(n,p,40,15);
    tcp(b,8|16|1,0x3334,0x5001,"bye");ip(p,6,0x7f000001,b,23);emitted=0;nat_core_input(n,p,43,16);assert(emitted&&last[33]&16);nat_core_tcp_eof(n,0,17);nat_core_poll(n,1000);assert(last[33]&1);
    assert(nat_core_forward_add(n,8080,1));for(unsigned i=0;i<4;++i)assert(nat_core_inbound_accept(n,8080,20+i)>=0);assert(nat_core_inbound_accept(n,8080,30)<0);
    nat_core_udp_received(n,0,b,sizeof(b),40);assert(nat_core_stats(n).drop);
    nat_core_input(n,(const unsigned char *)"",0,42);puts("nat: icmp udp dns tcp data+fin stray refused forward forward-cap syn-reuse udp-oversize backpressure garbage passed");return 0;}
