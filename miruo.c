#include "miruo.h"

miruopt opt;
void version()
{
  const char *libpcap = pcap_lib_version();
  printf("miruo version 0.9\n");
  if(libpcap){
    printf("%s\n", libpcap);
  }
}

void usage()
{
  version();
  printf("usage: miruo [option] [expression]\n");
  printf("  option\n");
  printf("   -h           # help\n");
  printf("   -V           # version\n");
  printf("   -v           # verbose\n");
  printf("   -vv          # more verbose\n");
  printf("   -vvv         # most verbose\n");
  printf("   -C0          # color off\n");
  printf("   -C1          # color on\n");
  printf("   -S0          # not search SYN retransmit\n");
  printf("   -S1          # search SYN retransmit(default)\n");
  printf("   -R0          # ignore RST break\n");
  printf("   -R1          # find   RST break (default)\n");
  printf("   -R2          # find   RST close\n");
  printf("   -D num       # show data packets\n");
  printf("   -a num       # active connection limit\n");
  printf("   -t time      # retransmit limit(Default 1000ms)\n");
  printf("   -T time      # long connection view time(Default 0ms = off)\n");
  printf("   -s interval  # statistics view interval(Default 60sec)\n");
  printf("   -r file      # read file(for tcpdump -w)\n");
  printf("   -i interface # \n");
  printf("\n");
  printf("  expression: see man tcpdump\n");
  printf("\n");
  printf("  ex)\n");
  printf("    miruo -m tcp host 192.168.0.100 and port 80\n");
  printf("    miruo -m tcp src host 192.168.0.1\n");
}

meminfo *get_memmory()
{
  int   i;
  int   f;
  char *r;
  char  buff[256];
  static meminfo mi;

  memset(buff, 0, sizeof(buff));
  mi.page_size = sysconf(_SC_PAGESIZE);
  f = open("/proc/self/statm", O_RDONLY);
  if(f == -1){
    memset(&mi, 0, sizeof(meminfo));
    return(NULL);
  }
  read(f, buff, sizeof(buff) - 1);
  close(f);
  i = 1;
  r = strtok(buff, " ");
  mi.vsz = atoi(r) * mi.page_size;
  while(r=strtok(NULL, " ")){
    switch(++i){
      case 2:
        mi.res = atoi(r) * mi.page_size;
        break;
      case 3:
        mi.share = atoi(r) * mi.page_size;
        break;
      case 4:
        mi.text = atoi(r) * mi.page_size;
        break;
      case 6:
        mi.data = atoi(r) * mi.page_size;
        break;
    }
  }
  return(&mi);
}


char *tcp_state_str(int state)
{
  static char *state_string[]={
    "-",
    "LISTEN",
    "SYN_SENT",
    "SYN_RECV",
    "ESTABLISHED",
    "FIN_WAIT1",
    "FIN_WAIT2",
    "CLOSE_WAIT",
    "LAST_ACK",
    "CLOSED",
    "TIME_WAIT",
  };
  return(state_string[state]);
}

char *tcp_flag_str(uint8_t flags)
{
  static char fstr[16];
  strcpy(fstr, "------");
  if(flags & 1){
    fstr[5]='F';
  }
  if(flags & 2){
    fstr[4]='S';
  }
  if(flags & 4){
    fstr[3]='R';
  }
  if(flags & 8){
    fstr[2]='P';
  }
  if(flags & 16){
    fstr[1]='A';
  }
  if(flags & 32){
    fstr[0]='U';
  }
  return(fstr);
}

char *tcp_opt_str(uint8_t *opt, uint8_t optsize)
{
  static uint8_t optstr[256];
  optstr[0]   = 0;
  uint8_t  t  = 0;
  uint8_t  l  = 0;
  uint32_t d0 = 0;
  uint32_t d1 = 0;
  uint8_t buf[32];
  while(optsize){
    t = *(opt++);
    if(t == 0){
      break;    /* END */
    }
    if(t == 1){
      continue; /* NO-OP */
    }
    d0 = 0;
    d1 = 0;
    l  = *(opt++);
    switch(l-2){
      case 1:
        d0 = (uint32_t)(*((uint8_t *)opt));
        break;
      case 2:
        d0 = (uint32_t)(ntohs(*((uint16_t *)opt)));
        break;
      case 4:
        d0 = (uint32_t)(ntohl(*((uint32_t *)opt)));
        break;
      case 8:
        d0 = ntohl(*((uint32_t *)(opt + 0)));
        d1 = ntohl(*((uint32_t *)(opt + 4)));
        break;
    }
    switch(t){
      case 2:
        sprintf(buf, "mss=%u", d0);
        break;
      case 3:
        sprintf(buf, "wscale=%u", d0);
        break;
      case 4:
        sprintf(buf, "sackOK");
        break;
      case 5:
        sprintf(buf, "sack 1"); /* len = 10 */
        break;
      case 8:
        sprintf(buf, "timestamp %u %u", d0, d1);
        break;
      default:
        sprintf(buf, "len=%hhu opt[%hhu]", l, t);
        break;
    }
    opt += (l - 2);
    optsize -= l;
    if(*optstr){
      strcat(optstr, ", ");
    }
    strcat(optstr, buf);
  }
  return(optstr);
}

uint32_t tcp_connection_time(tcpsession *c)
{
  uint32_t t;
  struct timeval ct;
  timerclear(&ct);
  if((c != NULL) && (c->last != NULL)){
    timersub(&(c->last->ts), &(c->ts), &ct);
  }
  t  = ct.tv_sec  * 1000;
  t += ct.tv_usec / 1000;
  return(t);
}

int is_tcp_retransmit(tcpsession *c, tcpsession *s)
{
  return((s->seqno == c->seqno) &&
         (s->ackno == c->ackno) && 
         (s->flags == c->flags) &&
         (s->psize == c->psize));
}

int is_tcp_retransmit_ignore(tcpsession *c, tcpsession *s)
{
  int64_t delay;
  if((c->flags & 2) != 0){
    return(opt.rsynfind == 0);
  }
  if(opt.rt_limit == 0){
    return(1);
  }
  delay  = c->ts.tv_sec;
  delay -= s->ts.tv_sec;
  delay *= 1000000;
  delay += c->ts.tv_usec;
  delay -= s->ts.tv_usec;
  delay /= 1000;
  delay  = (delay > 0) ? delay : -delay;
  return(delay < opt.rt_limit);
}

/***********************************************************************
 * ここからデバッグ用の関数だよ
***********************************************************************/
void debug_dumpdata(uint8_t *data, int size)
{
  int i;
  for(i=0;i<size;i++){
    printf("%02X ", (uint32_t)(*(data + i)));
    if((i % 16) == 15){
      printf("\n");
    }
  }
  printf("\n");
}

void debug_print_mac(char *mac)
{
  int i=0;
  printf("%02X",mac[i]);
  for(i=1;i<6;i++){
    printf(":%02X",mac[i]);
  }
}

void debug_print_iphdr(iphdr *h)
{
  int i;

  printf("======= IP HEADER =======\n");
  printf("Ver     : %hhu\n", h->Ver);
  printf("IHL     : %hhu\n", h->IHL);
  printf("TOS     : %hhu\n", h->TOS);
  printf("LEN     : %hu\n",  h->len);
  printf("ID      : %hu\n",  h->id);
  printf("Flags   : %hhu\n", h->flags);
  printf("Offset  : %hu\n",  h->offset);
  printf("TTL     : %hhu\n", h->TTL);
  printf("Protocol: %hhu\n", h->Protocol);
  printf("Checksum: %hX\n",  h->Checksum);
  printf("SrcAddr : %s\n",   inet_ntoa(h->src));
  printf("DstAddr : %s\n",   inet_ntoa(h->dst));
  for(i=0;i<(h->IHL - 20);i++){
    printf("option[%02d]: %02x\n", h->option[i]);
  }
}

void debug_print_tcphdr(tcphdr *h)
{
  char *flags = tcp_flag_str(h->flags);
  printf("======= TCP HEADER =======\n");
  printf("sport   : %hu\n",     h->sport);
  printf("dport   : %hu\n",     h->dport);
  printf("seqno   : %u\n",      h->seqno);
  printf("ackno   : %u\n",      h->ackno);
  printf("offset  : %hhu\n",    h->offset);
  printf("flags   : %s\n",      flags);
  printf("window  : %hu\n",     h->window);
  printf("checksum: 0x%04hx\n", h->checksum);
  debug_dumpdata(h->opt, h->offset - 20);
}
/***********************************************************************
 * ここまでデバッグ用の関数だよ
***********************************************************************/

u_char *read_ethhdr(ethhdr *h, u_char *p, uint32_t *l)
{
  memcpy(h, p, sizeof(ethhdr));
  h->type = ntohs(h->type);
  p  += sizeof(ethhdr);
  *l -= sizeof(ethhdr);
  return(p);
}

u_char *read_sllhdr(sllhdr *h, u_char *p, uint32_t *l)
{
  memcpy(h, p, sizeof(sllhdr));
  h->type = ntohs(h->type);
  p  += sizeof(sllhdr);
  *l -= sizeof(sllhdr);
  return(p);
}

uint8_t *read_l2hdr(l2hdr *hdr, u_char *p, uint32_t *l){
  switch(opt.lktype){
    case 1:
      return read_ethhdr(&(hdr->hdr.eth), p, l);
    case 113:
      return read_sllhdr(&(hdr->hdr.sll), p, l);
  }
  return(NULL);
}

u_char *read_iphdr(iphdr *h, u_char *p, int *l)
{
  int optlen;
  iphdr_row *hr = (iphdr_row *)p;
  if(*l < sizeof(iphdr_row)){
    return(NULL);
  }
  h->Ver        = (hr->vih & 0xf0) >> 4;
  h->IHL        = (hr->vih & 0x0f) << 2;
  h->TOS        = hr->tos;
  h->len        = ntohs(hr->len);
  h->id         = ntohs(hr->id);
  h->flags      = ntohs(hr->ffo) >> 13;
  h->offset     = ntohs(hr->ffo) & 0x1fff;
  h->TTL        = hr->ttl;
  h->Protocol   = hr->protocol;
  h->Checksum   = ntohs(hr->checksum);
  h->src.s_addr = hr->src;
  h->dst.s_addr = hr->dst;
  p  += sizeof(iphdr_row);
  *l -= sizeof(iphdr_row);
  if(optlen = h->IHL - sizeof(iphdr_row)){
    printf("len~%d IHL=%d, size=%d\n", optlen, h->IHL, sizeof(iphdr_row));
    if(*l < optlen){
      return(NULL);
    }
    memcpy(h->option, p, optlen);
    p  += optlen;
    *l -= optlen;
  }  
  return(p);
}

u_char *read_tcphdr(tcphdr *h, u_char *p, int *l)
{
  tcphdr *hr = (tcphdr *)p;
  if(*l < 20){
    return(NULL);
  }
  h->sport    = ntohs(hr->sport); 
  h->dport    = ntohs(hr->dport); 
  h->seqno    = ntohl(hr->seqno); 
  h->ackno    = ntohl(hr->ackno); 
  h->offset   = hr->offset >> 2;
  h->flags    = hr->flags;
  h->window   = ntohs(hr->window);
  h->checksum = ntohs(hr->checksum);
  h->urgent   = ntohs(hr->urgent);
  if(*l < h->offset){
    return(NULL);
  }
  memcpy(h->opt, hr->opt, h->offset - 20);
  p  += h->offset;
  *l -= h->offset;
  return(p);
}

int is_tcpsession_closed(tcpsession *c){
  if(c == NULL){
    return(0);
  }
  if((c->cs[0] == MIRUO_STATE_TCP_CLOSED) && (c->cs[1] == MIRUO_STATE_TCP_CLOSED)){
    return(1);
  }
  if((c->cs[0] == MIRUO_STATE_TCP_TIME_WAIT) && (c->cs[1] == MIRUO_STATE_TCP_CLOSED)){
    return(1);
  }
  if((c->cs[0] == MIRUO_STATE_TCP_CLOSED) && (c->cs[1] == MIRUO_STATE_TCP_TIME_WAIT)){
    return(1);
  }
  return(0);
}

tcpsession *get_tcpsession(tcpsession *c)
{
  tcpsession *s;
  tcpsession *t;

  c->sno = 0;
  c->rno = 1;
  for(s=opt.tsact;s;s=s->next){
    if((memcmp(&(c->src), &(s->src), sizeof(c->src)) == 0) && (memcmp(&(c->dst), &(s->dst), sizeof(c->dst)) == 0)){
      c->sno = 0;
      c->rno = 1;
      break;
    }
    if((memcmp(&(c->src), &(s->dst), sizeof(c->src)) == 0) && (memcmp(&(c->dst), &(s->src), sizeof(c->dst)) == 0)){
      c->sno = 1;
      c->rno = 0;
      break;
    }
  }
  for(t=s;t;t=t->stok){
    if(is_tcp_retransmit(t, c)){
      if(is_tcp_retransmit_ignore(t, c)){
        c->sno = 0;
        c->rno = 0;
        break;
      }
      s->views = 1;
      c->view  = 0;
      if((t->optsize != c->optsize) || (memcmp(t->opt, c->opt, c->optsize) != 0)){
        c->color = COLOR_MAGENTA;
      }else{
        c->color = COLOR_RED;
      }
      if(opt.verbose < 2){
        t->view  = 0;
        t->color = COLOR_GREEN;
        t = t->stok;
        while(t){
          if(t->color == 0){
            t->view  = 0;
            t->color = COLOR_CYAN;
          }
          t = t->stok;
        }
      }
      break;
    }  
  } 
  return(s);
}

tcpsession *malloc_tcpsession()
{
  tcpsession *s;
  if(s = opt.tspool.free){
    if(opt.tspool.free = s->next){
      s->next->prev = NULL;
      s->next = NULL;
    }
    opt.tspool.count--;
  }else{
    if(s = malloc(sizeof(tcpsession))){
      opt.count_ts++;
    }
  }
  return(s);
}

void free_tcpsession(tcpsession *c)
{
  while(c){
    tcpsession *s = c->stok;
    if(opt.tspool.count > 65535){
      free(c);
      opt.count_ts--;
    }else{
      if(c->next = opt.tspool.free){
        c->next->prev = c;
      }
      opt.tspool.free = c;
      opt.tspool.count++;
    }
    c = s;
  }
}

tcpsession *new_tcpsession(tcpsession *c)
{
  static uint16_t sid = 0;
  tcpsession *s = malloc_tcpsession();
  if(c){
    memcpy(s, c, sizeof(tcpsession));
  }else{
    memset(s, 0, sizeof(tcpsession));
  }
  sid = (sid > 9999) ? 1 : sid;
  s->sid   = sid++;
  s->stall = 1;
  return(s);
}

tcpsession *del_tcpsession(tcpsession *c)
{
  if(c == NULL){
    return;
  }
  tcpsession *p = c->prev; 
  tcpsession *n = c->next;
  if(c == opt.tsact){
    if(opt.tsact = n){
      opt.tsact->prev = NULL;
    }
  }else{
    if(p){
      p->next = n;
    }
    if(n){
      n->prev = p;
    }
  }
  opt.count_act--;
  free_tcpsession(c);
  return(n);
}

tcpsession *stok_tcpsession(tcpsession *c, tcpsession *s)
{
  tcpsession *t;
  tcpsession *r;
  if(c == NULL){
    return;
  }
  if(c->last){
    t = c->last;
  }else{
    t = c;
  }
  c->stcnt++;
  c->stall++;
  c->szall += s->psize;
  c->last = new_tcpsession(s);
  t->stok = c->last;
  t->stok->sid   = c->sid;
  t->stok->pno   = t->pno + 1;
  t->stok->st[0] = c->cs[s->sno];
  t->stok->st[1] = c->cs[s->rno];
  if(c->stcnt > 2048){
    r = c;
    s = c->stok;
    while(s){
      if(s->view == 0){
        r = s;
        s = s->stok;
        continue;
      }
      r->stok = s->stok;
      s->stok = NULL;
      if(c->last == s){
        c->last = r;
      }
      free_tcpsession(s);
      s = r->stok;
      c->stcnt--;
      if(c->stcnt < 1024){
        break;
      }
    }
  }
  return(c->last);
}

tcpsession *add_tcpsession(tcpsession *c)
{
  if(c == NULL){
    return(NULL);
  }
  if(opt.count_act < opt.actlimit){
    c = new_tcpsession(c);
  }else{
    opt.count_drop++;
    return(NULL);
  }
  while(c->next){
    c = c->next;
  }
  if(c->next = opt.tsact){
    c->next->prev = c;
  }
  opt.tsact = c;
  opt.count_act++;
  opt.count_total++;
  if(opt.count_actmax < opt.count_act){
    opt.count_actmax = opt.count_act;
  }
  return(c);
}

void print_acttcpsession(FILE *fp)
{
  struct tm *t;
  uint64_t  td;
  char st[256];
  char ts[2][64];
  char nd[2][64];
  tcpsession  *c;
  for(c=opt.tsact;c;c=c->next){
    t = localtime(&(c->ts.tv_sec));
    sprintf(st,    "%s/%s", tcp_state_str(c->cs[0]), tcp_state_str(c->cs[1]));
    sprintf(ts[0], "%02d:%02d:%02d.%03u", t->tm_hour, t->tm_min, t->tm_sec, c->ts.tv_usec / 1000);
    sprintf(nd[0], "%s:%u", inet_ntoa(c->src.in.sin_addr), c->src.in.sin_port);
    sprintf(nd[1], "%s:%u", inet_ntoa(c->dst.in.sin_addr), c->dst.in.sin_port);
    td = 0;
    if(c->last){
      td  = c->last->ts.tv_sec - c->ts.tv_sec;
      td *= 1000;
      td += c->last->ts.tv_usec / 1000;
      td -= c->ts.tv_usec / 1000;
    }
    sprintf(ts[1], "%u.%03us", (int)(td/1000), (int)(td%1000));
    fprintf(fp, "%04d %s(+%s) %s %s %-23s\n", c->stcnt, ts[0], ts[1], nd[0], nd[1], st);
  }
}

void print_tcpsession(FILE *fp, tcpsession *c)
{
  struct tm *t;               //
  char st[256];               // TCPステータス
  char ts[256];               // タイムスタンプ
  char cl[2][16];             // 色指定用ESCシーケンス
  char ip[2][64];             // IPアドレス
  char *allow[] = {">", "<"}; //

  if(c == NULL){
    return;
  }

  if(c->views == 0){
    return;
  }
  
  cl[0][0] = 0;
  cl[1][0] = 0;
  if(c->color && opt.color){
    sprintf(cl[0], "\x1b[3%dm", COLOR_YELLOW);
    sprintf(cl[1], "\x1b[39m");
  }
  if(opt.verbose < 2){
    uint32_t ct = tcp_connection_time(c);
    sprintf(ip[0], "%s:%u", inet_ntoa(c->src.in.sin_addr), c->src.in.sin_port);
    sprintf(ip[1], "%s:%u", inet_ntoa(c->dst.in.sin_addr), c->dst.in.sin_port);
    fprintf(fp, "%s[%04u] %13u.%03u |%21s == %-21s| Total %u pks, %u bytes%s\n",
      cl[0],
        c->sid, 
        ct / 1000,
        ct % 1000,
        ip[0], ip[1], 
        c->stall, c->szall, 
      cl[1]);
  }
  while(c){
    if(c->view == 0){
      cl[0][0] = 0;
      cl[1][0] = 0;
      if(c->color && opt.color){
        sprintf(cl[0], "\x1b[3%dm", c->color);
        sprintf(cl[1], "\x1b[39m");
      }
      t = localtime(&(c->ts.tv_sec));
      sprintf(ts, "%02d:%02d:%02d.%03u", t->tm_hour, t->tm_min, t->tm_sec, c->ts.tv_usec / 1000);
      if(opt.verbose < 2){
        fprintf(fp, "%s[%04u:%04u] %s |%18s %s%s%s %-18s| %08X/%08X %4u <%s>%s\n",
          cl[0], 
            c->sid, c->pno,
            ts, 
            tcp_state_str(c->st[c->sno]),
            allow[c->sno], tcp_flag_str(c->flags), allow[c->sno],  
            tcp_state_str(c->st[c->rno]),
            c->seqno, c->ackno,
            c->psize,
            tcp_opt_str(c->opt, c->optsize), 
          cl[1]);
        if(c->stok && c->stok->view){
          fprintf(fp, "[%04u:****] %12s |%46s|\n", c->sid, "", "");
        }
      }else{
        sprintf(ip[c->sno], "%s:%u", inet_ntoa(c->src.in.sin_addr), c->src.in.sin_port);
        sprintf(ip[c->rno], "%s:%u", inet_ntoa(c->dst.in.sin_addr), c->dst.in.sin_port);
        sprintf(st, "%s/%s", tcp_state_str(c->st[c->sno]), tcp_state_str(c->st[c->rno]));
        fprintf(fp, "%s[%04u:%04u] %s %s %s%s%s %s %-23s %08X/%08X <%s>%s\n",
          cl[0], 
            c->sid, c->pno,
            ts,
            ip[0],
            allow[c->sno], tcp_flag_str(c->flags), allow[c->sno],  
            ip[1],
            st,
            c->seqno, c->ackno, 
            tcp_opt_str(c->opt, c->optsize), 
          cl[1]);
      }
      c->view = 1;
    }
    c = c->stok;
  }
}

void miruo_tcp_session_statistics(int view)
{
  char tstr[32];
  char mstr[64];
  uint64_t size;
  uint32_t  cpu;
  uint64_t  rus;
  uint64_t  cus;
  uint64_t  mem;
  uint32_t   sc;
  tcpsession *t;
  tcpsession *s;
  tcpsession ts;
  meminfo   *mi;
  struct timeval  rtv; // 実時間 (now-old)
  struct timeval  ctv; // CPU時間(SYS+USR)
  struct timeval nctv; // CPU時間(SYS+USR)
  struct timeval octv; // CPU時間(SYS+USR)
  struct pcap_stat ps; //
  static int w = 0;
  if(opt.stattime == 0){
    return;
  }
  if((w > 0) && (view == 0)){
    w -= opt.itv.it_interval.tv_sec;
    if(w > 0){
      return;
    }
  }

  w = opt.stattime;
  size = opt.count_ts * sizeof(tcpsession);
  if(size > 1024 * 1024 * 1024){
    sprintf(mstr, "%lluGB", size / 1024 / 1024 / 1024);
  }else if(size > 1024 * 1024){
    sprintf(mstr, "%lluMB", size / 1024 / 1024);
  }else if(size > 1024){
    sprintf(mstr, "%lluKB", size / 1024);
  }else{
    sprintf(mstr, "%lluB", size);
  }
  if(opt.tsact){
    fprintf(stderr, "===== ACTIVE SESSIONLIST =====\n");
    print_acttcpsession(stderr);
  }
  memset(&ps, 0, sizeof(ps));
  pcap_stats(opt.p, &ps);

  timeradd(&(opt.now_rs.ru_stime), &(opt.now_rs.ru_utime), &nctv);
  timeradd(&(opt.old_rs.ru_stime), &(opt.old_rs.ru_utime), &octv);
  timersub(&(opt.now_tv), &(opt.old_tv), &rtv);
  timersub(&(nctv), &(octv), &ctv);
  cus  = ctv.tv_sec;
  cus *= 1000000;
  cus += ctv.tv_usec;
  rus  = rtv.tv_sec;
  rus *= 1000000;
  rus += rtv.tv_usec;
  cpu = cus * 1000 / rus;
  cpu = (cpu > 1000) ? 1000 : cpu;

  sprintf(tstr, "%02d:%02d:%02d", opt.tm.tm_hour, opt.tm.tm_min, opt.tm.tm_sec);
  fprintf(stderr, "===== Session Statistics =====\n");
  fprintf(stderr, "Current Time     : %s\n",      tstr);
  fprintf(stderr, "Total Session    : %llu\n",    opt.count_total);
  fprintf(stderr, "View Session     : %llu\n",    opt.count_view);
  fprintf(stderr, "Timeout Session  : %llu\n",    opt.count_timeout);
  fprintf(stderr, "RST Break Session: %llu\n",    opt.count_rstbreak);
  fprintf(stderr, "ActiveSession    : %u\n",      opt.count_act);
  fprintf(stderr, "ActiveSessionMax : %u\n",      opt.count_actmax);
  fprintf(stderr, "DropSession      : %u\n",      opt.count_drop);
  fprintf(stderr, "SessionPool(use) : %u\n",      opt.count_ts - opt.tspool.count);
  fprintf(stderr, "SessionPool(free): %u\n",      opt.tspool.count);
  fprintf(stderr, "CPU utilization  : %u.%u%%\n", cpu/10, cpu%10);
if(mi = get_memmory()){
  fprintf(stderr, "VmSize           : %lluKB\n", mi->vsz / 1024);
  fprintf(stderr, "VmRSS            : %lluKB\n", mi->res / 1024);
  fprintf(stderr, "VmData           : %lluKB\n", mi->res / 1024);
}
  fprintf(stderr, "===== Captcha Statistics =====\n");
  fprintf(stderr, "recv  : %u\n", ps.ps_recv);
  fprintf(stderr, "drop  : %u\n", ps.ps_drop);
  fprintf(stderr, "ifdrop: %u\n", ps.ps_ifdrop);
  fprintf(stderr, "===== Error Count Report =====\n");
  fprintf(stderr, "L2    : %d\n", opt.err_l2);
  fprintf(stderr, "IP    : %d\n", opt.err_ip);
  fprintf(stderr, "TCP   : %d\n", opt.err_tcp);
  fprintf(stderr, "==============================\n");
  memcpy(&(opt.old_tv), &(opt.now_tv), sizeof(struct timeval));
  memcpy(&(opt.old_rs), &(opt.now_rs), sizeof(struct rusage));
}

tcpsession *miruo_tcp_session_destroy(tcpsession *c, char *msg, char *reason)
{
  int  l;
  int  r;
  char sl[32];
  char sr[32];
  char ts[32];
  char sc[2][8];

  c->views = 1;
  opt.count_view++;
  print_tcpsession(stdout, c);
  l  = 46 - strlen(msg);
  r  = l / 2;
  l -= r;
  memset(sl, 0, sizeof(sl));
  memset(sr, 0, sizeof(sr));
  memset(sl, ' ', l);
  memset(sr, ' ', r);
  if(opt.color){
    sprintf(sc[0], "\x1b[31m");
    sprintf(sc[1], "\x1b[39m");
  }else{
    sc[0][0] = 0;
    sc[1][1] = 0;
  }
  sprintf(ts, "%02d:%02d:%02d.%03u", opt.tm.tm_hour, opt.tm.tm_min, opt.tm.tm_sec, opt.now_tv.tv_usec/1000);
  if(opt.verbose < 2){
    fprintf(stdout, "%s[%04u:%04u] %s |%s%s%s| %s%s\n", sc[0], c->sid, c->pno, ts, sl, msg, sr, reason, sc[1]);
  }else{
    fprintf(stdout, "%s[%04u:%04u] %s %s (%s)%s\n", sc[0], c->sid, c->pno, ts, msg, reason, sc[1]);
  }
  return(del_tcpsession(c));
}

void miruo_tcp_session_timeout()
{
  tcpsession *t;
  tcpsession *s;
  tcpsession *r;

  t = opt.tsact;
  if(t == NULL){
    return;
  }
  while(t){
    if((opt.now_tv.tv_sec - t->ts.tv_sec) > 30){
      switch(t->cs[0]){
        case MIRUO_STATE_TCP_SYN_SENT:
        case MIRUO_STATE_TCP_SYN_RECV:
        case MIRUO_STATE_TCP_FIN_WAIT1:
        case MIRUO_STATE_TCP_FIN_WAIT2:
        case MIRUO_STATE_TCP_CLOSE_WAIT:
        case MIRUO_STATE_TCP_LAST_ACK:
          opt.count_timeout++;
          t = miruo_tcp_session_destroy(t, "destroy session", "time out");
          continue;
      }
      switch(t->cs[1]){
        case MIRUO_STATE_TCP_SYN_SENT:
        case MIRUO_STATE_TCP_SYN_RECV:
        case MIRUO_STATE_TCP_FIN_WAIT1:
        case MIRUO_STATE_TCP_FIN_WAIT2:
        case MIRUO_STATE_TCP_CLOSE_WAIT:
        case MIRUO_STATE_TCP_LAST_ACK:
          opt.count_timeout++;
          t = miruo_tcp_session_destroy(t, "destroy session", "time out");
          continue;
      }
    }
    r = t;
    s = t->stok;
    while(s){
      if(s->view){
        // 再送時間の最長を暫定的に30秒として、それ以前に受け取ったパケットを破棄
        // そのため30秒以内の再送は検出できるがそれ以上かかった場合は検知できない
        // 30秒でも十分に大きすぎる気がするのでRTOをどうにか計算したほうがいいかな
        if((opt.now_tv.tv_sec - s->ts.tv_sec) > 30){
          r->stok = s->stok;
          s->stok = NULL;
          if(t->last == s){
            t->last = r;
          }
          free_tcpsession(s);
          s = r->stok;
          t->stcnt--;
          continue;
        }
      }
      r = s;
      s = s->stok;
    }
    t = t->next;
  }
}

tcpsession *miruo_tcp_syn(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    if(c = add_tcpsession(s)){
      c->color = COLOR_YELLOW;
    }else{
      return(NULL);
    }
  }else{
    if((c->seqno == s->seqno) && (c->ackno == s->ackno)){
      stok_tcpsession(c, s);
      return(c);
    }else{
      miruo_tcp_session_destroy(c, "error break", "Duplicate connection");
      if(c = add_tcpsession(s)){
        c->color = COLOR_RED;
        c->views = 1;
      }else{
        return(NULL);
      }
    }
  }
  c->sno = 0;
  c->rno = 1;
  c->st[0] = MIRUO_STATE_TCP_SYN_SENT;
  c->st[1] = MIRUO_STATE_TCP_SYN_RECV;
  c->cs[0] = MIRUO_STATE_TCP_SYN_SENT;
  c->cs[1] = MIRUO_STATE_TCP_SYN_RECV;
  return(c);
}

void miruo_tcp_synack(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  switch(s->st[0]){
    case MIRUO_STATE_TCP_SYN_RECV:
      break;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
      break;
  }
  switch(s->st[1]){
    case MIRUO_STATE_TCP_SYN_SENT:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_EST;
      break;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
      break;
  }
}

void miruo_tcp_ack(tcpsession *c, tcpsession *s)
{
  int f = 0;
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  if((s->pno <= opt.showdata) || (opt.verbose > 1)){
    s->view = 0;
  }else{
    s->view = (s->color == 0);
  }
  switch(s->st[0]){
    case MIRUO_STATE_TCP_EST:
      if(s->st[1] == MIRUO_STATE_TCP_FIN_WAIT1){
        s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_CLOSE_WAIT;
        s->view  = 0;
      }
      break;
    case MIRUO_STATE_TCP_SYN_RECV:
      s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_EST;
      s->view  = 0;
      break;
    case MIRUO_STATE_TCP_FIN_WAIT2:
      if(s->st[1] == MIRUO_STATE_TCP_LAST_ACK){
        s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_TIME_WAIT;
        s->view  = 0;
      }
      break;
  }
  switch(s->st[1]){
    case MIRUO_STATE_TCP_SYN_RECV:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_EST;
      s->view  = 0;
      break;
    case MIRUO_STATE_TCP_FIN_WAIT1:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_FIN_WAIT2;
      s->view  = 0;
      break;
    case MIRUO_STATE_TCP_FIN_WAIT2:
      s->view  = 0;
      break;
    case MIRUO_STATE_TCP_LAST_ACK:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_CLOSED;
      s->view  = 0;
      break;
  }
  if((s->st[0] == MIRUO_STATE_TCP_TIME_WAIT) && (s->st[1] == MIRUO_STATE_TCP_CLOSED)){
    s->color = COLOR_BLUE;
  }
}

void miruo_tcp_fin(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  switch(s->st[0]){
    case MIRUO_STATE_TCP_EST:
      s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_FIN_WAIT1;
      break;
    case MIRUO_STATE_TCP_CLOSE_WAIT:
      s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_LAST_ACK;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
      break;
  }
  switch(s->st[1]){
    case MIRUO_STATE_TCP_EST:
      break;
    case MIRUO_STATE_TCP_FIN_WAIT2:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_TIME_WAIT;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
      break;
  }
}

void miruo_tcp_finack(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  switch(s->st[0]){
    case MIRUO_STATE_TCP_SYN_RECV:
      s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_FIN_WAIT1;
      break;
    case MIRUO_STATE_TCP_EST:
      if(s->st[1] == MIRUO_STATE_TCP_EST){
        s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_FIN_WAIT1;
      }
      if(s->st[1] == MIRUO_STATE_TCP_FIN_WAIT1){
        s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_LAST_ACK;
      }
      break;
    case MIRUO_STATE_TCP_CLOSE_WAIT:
      s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_LAST_ACK;
      break;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
  }
  switch(s->st[1]){
    case MIRUO_STATE_TCP_EST:
      break;
    case MIRUO_STATE_TCP_FIN_WAIT1:
      s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_FIN_WAIT2;
      break;
    case MIRUO_STATE_TCP_FIN_WAIT2:
      break;
    default:
      c->views = 1;
      s->view  = 0;
      s->color = COLOR_RED;
  }
}

void miruo_tcp_rst(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_CLOSED;
  s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_CLOSED;
  s->color = COLOR_RED;
  c->views = (opt.rstmode > 0);
  if((s->st[0] == MIRUO_STATE_TCP_CLOSE_WAIT) && (s->st[1] == MIRUO_STATE_TCP_FIN_WAIT1)){
    c->views = (opt.rstmode > 1);
  }
  if((s->st[0] == MIRUO_STATE_TCP_CLOSE_WAIT) && (s->st[1] == MIRUO_STATE_TCP_FIN_WAIT2)){
    c->views = (opt.rstmode > 1);
  }
}

void miruo_tcp_rstack(tcpsession *c, tcpsession *s)
{
  if(c == NULL){
    return;
  }
  s = stok_tcpsession(c, s);
  c->views = (opt.rstmode > 0);
  if((s->st[0] == MIRUO_STATE_TCP_CLOSE_WAIT) && (s->st[1] == MIRUO_STATE_TCP_FIN_WAIT1)){
    c->views = (opt.rstmode > 1);
  }
  if((s->st[0] == MIRUO_STATE_TCP_CLOSE_WAIT) && (s->st[1] == MIRUO_STATE_TCP_FIN_WAIT2)){
    c->views = (opt.rstmode > 1);
  }
  s->st[0] = c->cs[s->sno] = MIRUO_STATE_TCP_CLOSED;
  s->st[1] = c->cs[s->rno] = MIRUO_STATE_TCP_CLOSED;
  s->color = COLOR_RED;
}

tcpsession *miruo_tcp_flags_switch(tcpsession *c, tcpsession *s)
{
  switch(s->flags & 23){
    case 2:
      c = miruo_tcp_syn(c, s);
      break;
    case 16:
      miruo_tcp_ack(c, s);
      break;
    case 18:
      miruo_tcp_synack(c, s);
      break;
    case 1:
      miruo_tcp_fin(c, s);
      break;
    case 17:
      miruo_tcp_finack(c, s);
      break;
    case 4:
      miruo_tcp_rst(c, s);
      break;
    case 20:
      miruo_tcp_rstack(c, s);
      break;
    default:
      if(c){
        c->views = 1;
        s->view  = 0;
        s->color = COLOR_RED;
        stok_tcpsession(c, s);
      }
      break;
  }
  return(c);
}

void hdr2tcpsession(tcpsession *s, iphdr *ih, tcphdr *th, const struct pcap_pkthdr *ph)
{
  memset(s, 0, sizeof(tcpsession));
  memcpy(&(s->ts), &(ph->ts), sizeof(struct timeval));
  memcpy(&(s->src.in.sin_addr), &(ih->src), sizeof(struct in_addr));
  memcpy(&(s->dst.in.sin_addr), &(ih->dst), sizeof(struct in_addr));
  s->src.in.sin_port = th->sport;
  s->dst.in.sin_port = th->dport;
  s->psize   = ph->len;
  s->flags   = th->flags;
  s->seqno   = th->seqno;
  s->ackno   = th->ackno;
  s->optsize = th->offset - 20;
  memcpy(s->opt, th->opt, s->optsize);
}

uint16_t get_l3type(l2hdr *hdr)
{
  switch(opt.lktype){
    case DLT_EN10MB:
      return(hdr->hdr.eth.type);
    case DLT_LINUX_SLL:
      return(hdr->hdr.sll.type);
  }
  return(0);
}

/***********************************************
 *
 * TCPセッションチェックの本体
 *
************************************************/
void miruo_tcp_session(u_char *u, const struct pcap_pkthdr *ph, const u_char *p)
{
  l2hdr      l2;
  iphdr      ih;
  tcphdr     th;
  u_char     *q;
  uint32_t    l;
  tcpsession  s;
  tcpsession *c;

  l = ph->caplen;
  q = (u_char *)p;
  q = read_l2hdr(&l2, q, &l);
  if(q == NULL){
    opt.err_l2++;
    return; // 不正なフレームは破棄
  }
  if(get_l3type(&l2) != 0x0800){
    return; // IP以外は破棄
  }

  q = read_iphdr(&ih, q, &l);
  if(q == NULL){
    opt.err_ip++;
    return; // 不正なIPヘッダ
  }
  if(ih.offset != 0){
    return; // フラグメントの先頭以外は破棄
  }
  if(ih.Protocol != 6){
    return; // TCP以外は破棄
  }

  q = read_tcphdr(&th, q, &l);
  if(q == NULL){
    opt.err_tcp++;
    return; // 不正なTCPヘッダ
  }

  hdr2tcpsession(&s, &ih, &th, ph);
  c = get_tcpsession(&s);
  if(s.sno == s.rno){
    return; // 無視すべき再送パケット
  }

  c = miruo_tcp_flags_switch(c, &s);
  if(is_tcpsession_closed(c)){
    if(opt.verbose > 0){
      c->views = 1;
    }
    if((opt.ct_limit > 0) && (tcp_connection_time(c) > opt.ct_limit)){
      c->views = 1;
    }
    if(c->views){
      opt.count_view++;
    }
    print_tcpsession(stdout, c);
    del_tcpsession(c);
    return;
  }
  if((opt.verbose > 1) && (c != NULL)){
    c->views = 1;
    print_tcpsession(stdout, c);
  }
}

void miruo_finish(int code)
{
  if(opt.p){
    pcap_close(opt.p);
    opt.p = NULL;
  }
  exit(code);
}

struct option *get_optlist()
{
  static struct option opt[4];
  opt[0].name    = "help";
  opt[0].has_arg = 0;
  opt[0].flag    = NULL;
  opt[0].val     = 'h';

  opt[1].name    = "version";
  opt[1].has_arg = 0;
  opt[1].flag    = NULL;
  opt[1].val     = 'V';

  opt[2].name    = "tcp";
  opt[2].has_arg = 1;
  opt[2].flag    = NULL;
  opt[2].val     = 'T';

  opt[3].name    = NULL;
  opt[3].has_arg = 0;
  opt[3].flag    = NULL;
  opt[3].val     = 0;
  return(opt);
}

void signal_int_handler()
{
  opt.loop = 0;
  pcap_breakloop(opt.p);
}

void signal_term_handler()
{
  opt.loop = 0;
  pcap_breakloop(opt.p);
}

void signal_handler(int n)
{
  switch(n){
    case SIGINT:
      signal_int_handler();
      break;
    case SIGTERM:
      signal_term_handler();
      break;
    case SIGPIPE:
      break;
    case SIGUSR1:
      break;
    case SIGUSR2:
      break;
    case SIGALRM:
      opt.alrm = 1;
      break;
  }
}

void miruo_signal()
{
  struct sigaction sig;
  memset(&sig, 0, sizeof(sig));
  sig.sa_handler = signal_handler;
  if(sigaction(SIGINT,  &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGINT\n", __func__);
    miruo_finish(1);
  }
  if(sigaction(SIGTERM, &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGTERM\n", __func__);
    miruo_finish(1);
  }
  if(sigaction(SIGPIPE, &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGPIPE\n", __func__);
    miruo_finish(1);
  }
  if(sigaction(SIGUSR1, &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGUSR1\n", __func__);
    miruo_finish(1);
  }
  if(sigaction(SIGUSR2, &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGUSR2\n", __func__);
    miruo_finish(1);
  }
  if(sigaction(SIGALRM, &sig, NULL) == -1){
    fprintf(stderr, "%s: sigaction error SIGALRM\n", __func__);
    miruo_finish(1);
  }
}

void miruo_timer()
{
  if(setitimer(ITIMER_REAL, &(opt.itv), NULL) == -1){
    fprintf(stderr, "%s: %s\n", __func__, strerror(errno));
    miruo_finish(1);
  }    
}

int miruo_init()
{
  memset(&opt, 0, sizeof(opt));
  opt.loop     = 1;
  opt.alrm     = 1;
  opt.promisc  = 1;
  opt.showdata = 0;
  opt.rsynfind = 1;
  opt.rstmode  = 1;
  opt.stattime = 60;
  opt.pksize   = 96;
  opt.ct_limit = 0;
  opt.rt_limit = 1000;
  opt.actlimit = 1024;
  opt.color    = isatty(fileno(stdout));
  opt.mode     = MIRUO_MODE_TCP_SESSION;
  opt.itv.it_interval.tv_sec = 1;
  opt.itv.it_value.tv_sec    = 1;
  gettimeofday(&(opt.old_tv), NULL);
  getrusage(RUSAGE_SELF, &(opt.old_rs));
}

void miruo_setopt(int argc, char *argv[])
{
  int   i;
  int   r;
  char *F[8];
  memset(F, 0, sizeof(F));
  F[MIRUO_MODE_TCP_SESSION] = "tcp";
  F[MIRUO_MODE_HTTP]        =  NULL;
  F[MIRUO_MODE_MYSQL]       =  NULL;
  while((r = getopt_long(argc, argv, "hVvR:S:C:D:a:T:t:s:i:m:r:", get_optlist(), NULL)) != -1){
    switch(r){
      case 'h':
        usage();
        miruo_finish(0);
      case 'V':
        version();
        miruo_finish(0);
      case 'v':
        opt.verbose++;
        break;
      case 'R':
        opt.rstmode = atoi(optarg);
        break;
      case 'S':
        opt.rsynfind = atoi(optarg);
        break;
      case 'D':
        opt.showdata = atoi(optarg);
        break;
      case 'C':
        opt.color = atoi(optarg);
        break;
      case 'a':
        if(atoi(optarg) > 0){
          opt.actlimit = atoi(optarg);
        }
        break;
      case 'T':
        opt.ct_limit = atoi(optarg);
        break;
      case 't':
        opt.rt_limit = atoi(optarg);
        break;
      case 's':
        opt.stattime = atoi(optarg);
        break;
      case 'r':
        strcpy(opt.file, optarg);
        break;
      case 'm':
        if(strcmp("tcp", optarg) == 0){
          opt.mode = MIRUO_MODE_TCP_SESSION;
        }
        break;
      case 'i':
        strcpy(opt.dev, optarg);
        break;
      case '?':
        usage();
        miruo_finish(1);
    }
  }
  for(i=optind;i<argc;i++){
    if(strlen(opt.exp)){
      strcat(opt.exp, " ");
    }
    strcat(opt.exp, argv[i]);
  }
  if(F[opt.mode]){
    if(strlen(opt.exp)){
      strcat(opt.exp, " and ");
    }
    strcat(opt.exp, F[opt.mode]);
  }
}

void miruo_pcap()
{
  const char *p;
  struct bpf_program pf;
  char errmsg[PCAP_ERRBUF_SIZE];

  if(strlen(opt.file)){
    opt.p = pcap_open_offline(opt.file, errmsg);
    if(opt.p == NULL){
      fprintf(stderr, "%s: [error] %s\n", __func__, errmsg);
      miruo_finish(1);
    }
  }else{
    if(strlen(opt.dev) == 0){
      if(p = pcap_lookupdev(errmsg)){
        strcpy(opt.dev, p);
      }else{
        fprintf(stderr,"%s. please run as root user.\n", errmsg);
        miruo_finish(1);
      }
    }
    opt.p = pcap_open_live(opt.dev, opt.pksize, opt.promisc, 1000, errmsg);
    if(opt.p == NULL){
      fprintf(stderr, "%s: [error] %s %s\n", __func__, errmsg, opt.dev);
      miruo_finish(1);
    }
  }
  if(pcap_compile(opt.p, &pf, opt.exp, 0, 0)){
    fprintf(stderr, "%s: [error] %s '%s'\n", __func__, pcap_geterr(opt.p), opt.exp);
    miruo_finish(1);
  }
  if(pcap_setfilter(opt.p, &pf)){
    fprintf(stderr, "%s: [error] %s\n", __func__, pcap_geterr(opt.p));
    miruo_finish(1);
  }

  opt.pksize = pcap_snapshot(opt.p);
  opt.lktype = pcap_datalink(opt.p);
  if(p = pcap_datalink_val_to_name(opt.lktype)){
    strcpy(opt.lkname, p);
  }
  if(p = pcap_datalink_val_to_description(opt.lktype)){
    strcpy(opt.lkdesc, p);
  }
  switch(opt.lktype){
    case DLT_EN10MB:
    case DLT_LINUX_SLL:
      break;
    default:
      fprintf(stderr, "%s: not support datalink %s(%s)\n", __func__, opt.lkname, opt.lkdesc);
      miruo_finish(1);
  }
}

void miruo_execute_tcp_session_offline()
{
  if(pcap_loop(opt.p, 0, miruo_tcp_session, NULL) == -1){
    fprintf(stderr, "%s: [error] %s\n", __func__, pcap_geterr(opt.p));
  }
  opt.stattime = 1;
  gettimeofday(&(opt.now_tv), NULL);
  getrusage(RUSAGE_SELF, &(opt.now_rs));
  memcpy(&(opt.tm), localtime(&(opt.now_tv.tv_sec)), sizeof(struct tm));
  miruo_tcp_session_statistics(1);
}

void miruo_execute_tcp_session_live(int p)
{
  fd_set fds;
  while(opt.loop){
    FD_ZERO(&fds);
    FD_SET(p,&fds);
    if(opt.alrm){
      opt.alrm = 0;
      gettimeofday(&(opt.now_tv), NULL);
      getrusage(RUSAGE_SELF, &(opt.now_rs));
      memcpy(&(opt.tm), localtime(&(opt.now_tv.tv_sec)), sizeof(struct tm));
      miruo_tcp_session_statistics(0);
      miruo_tcp_session_timeout();
    }
    if(select(1024, &fds, NULL, NULL, NULL) <= 0){
      continue;
    }
    if(FD_ISSET(p, &fds)){
      if(pcap_dispatch(opt.p, 0, miruo_tcp_session, NULL) == -1){
        fprintf(stderr, "%s: [error] %s\n", __func__, pcap_geterr(opt.p));
        break;
      }
    }
  }
  miruo_tcp_session_statistics(1);
}

void miruo_execute_tcp_session()
{
  int p = pcap_fileno(opt.p);
  if(p == 0){
    miruo_execute_tcp_session_offline();
  }else{
    miruo_execute_tcp_session_live(p);
  }
}

void miruo_execute()
{
  printf("listening on %s, link-type %s (%s), capture size %d bytes\n", opt.dev, opt.lkname, opt.lkdesc, opt.pksize);
  switch(opt.mode){
    case MIRUO_MODE_TCP_SESSION:
      miruo_execute_tcp_session();
      break;
    default:
      usage();
      break;
  }
}

int main(int argc, char *argv[])
{
  miruo_init();
  miruo_setopt(argc, argv);
  miruo_signal();
  miruo_timer();
  miruo_pcap();
  miruo_execute();
  miruo_finish(0);
  return(0);
}

