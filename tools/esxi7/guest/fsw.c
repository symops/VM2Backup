/* fsw: писатель/проверяющий файлов на ext4 диске данных гостя (проверка quiesce, раздел docs/ESXI.md «Раунд 3»).
 *   fsw write DIR [--rate N]   последовательность записей seq = 1, 2, ...  На каждую запись:
 *       класс s: файл DIR/s/<seq>.dat (псевдослучайный размер и содержимое от seq), write + fsync + rename + fsync каталога (долговечен до следующей записи);
 *       класс l: файл DIR/l/<seq>.dat, write + close БЕЗ fsync (лежит в кэше ОС: теряется при crash-consistent копии, сохраняется при quiesce);
 *     окно: файлы с seq <= текущий - 400 удаляются.  Каждые 20 записей строка «W seq» на serial (/dev/ttyS0), каждые 200 - sync() и «SYNC seq».
 *   fsw verify DIR             проверка: печатает «FSVERIFY s: n= min= max= gaps= bad= | l: n= min= max= gaps= bad= short=»
 * Файл: заголовок «VM2BFS1 <seq> <class> <size> <crc32 данных>\n» + данные (splitmix64 от seq и класса). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
static uint64_t mix(uint64_t z){ z+=0x9E3779B97F4A7C15ULL; z=(z^(z>>30))*0xBF58476D1CE4E5B9ULL; z=(z^(z>>27))*0x94D049BB133111EBULL; return z^(z>>31); }
static uint32_t crct[256];
static void crcinit(void){ for(uint32_t i=0;i<256;i++){ uint32_t c=i; for(int k=0;k<8;k++) c=c&1?0xEDB88320u^(c>>1):c>>1; crct[i]=c; } }
static uint32_t crc(const unsigned char*p,size_t n){ uint32_t c=~0u; for(size_t i=0;i<n;i++) c=crct[(c^p[i])&255]^(c>>8); return ~c; }
static size_t sizeof_seq(uint64_t seq,int cls){ return 600 + mix(seq*2+cls) % 24000; }
static void fill(unsigned char*b,size_t n,uint64_t seq,int cls){ uint64_t s=mix(seq*7+cls); for(size_t i=0;i<n;i++){ if(i%8==0) s=mix(s); b[i]=(unsigned char)(s>>((i%8)*8)); } }
static int tty=-1;
static void say(const char*fmt,...);
static void say(const char*fmt,...){ char b[300]; __builtin_va_list a; __builtin_va_start(a,fmt); int n=vsnprintf(b,sizeof b,fmt,a); __builtin_va_end(a); if(tty>=0) { if(write(tty,b,n)<0){} } else fputs(b,stdout); }
static int put(const char*dir,uint64_t seq,int cls,int sync_it){
  size_t n=sizeof_seq(seq,cls); unsigned char*d=malloc(n+64); fill(d+64,n,seq,cls);
  char hdr[64]; int h=snprintf(hdr,64,"VM2BFS1 %llu %c %zu %08x\n",(unsigned long long)seq,cls?'l':'s',n,crc(d+64,n)); memcpy(d+64-h,hdr,h);
  char tmp[300],fin[300]; snprintf(tmp,300,"%s/%c/%010llu.tmp",dir,cls?'l':'s',(unsigned long long)seq); snprintf(fin,300,"%s/%c/%010llu.dat",dir,cls?'l':'s',(unsigned long long)seq);
  int fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd<0){ say("FSW open %s errno=%d\n",tmp,errno); if(tty>=0)  return -1; }
  if(write(fd,d+64-h,n+h)!=(ssize_t)(n+h)){ close(fd); free(d); return -1; }
  if(sync_it) fsync(fd); close(fd); free(d);
  if(rename(tmp,fin)){ say("FSW rename %s errno=%d\n",tmp,errno); return -1; }
  if(sync_it){ char dd[300]; snprintf(dd,300,"%s/%c",dir,cls?'l':'s'); int f=open(dd,O_RDONLY); if(f>=0){ fsync(f); close(f);} }
  return 0; }
struct st { long n; long long min,max; long gaps,bad,shortf; };
static void scan(const char*dir,int cls,struct st*s){
  memset(s,0,sizeof *s); s->min=-1; char dd[300]; snprintf(dd,300,"%s/%c",dir,cls?'l':'s'); DIR*D=opendir(dd); if(!D) return; struct dirent*e; long long prev=-1;
  long long *seqs=malloc(sizeof(long long)*100000); long cnt=0;
  while((e=readdir(D))){ long long q; char ex[8]; if(sscanf(e->d_name,"%lld.%7s",&q,ex)!=2||strcmp(ex,"dat")) continue; seqs[cnt++]=q; if(cnt>=100000) break; }
  closedir(D);
  for(long i=0;i<cnt;i++) for(long j=i+1;j<cnt;j++) if(seqs[j]<seqs[i]){ long long t=seqs[i]; seqs[i]=seqs[j]; seqs[j]=t; }
  for(long i=0;i<cnt;i++){ long long q=seqs[i]; char f[300]; snprintf(f,300,"%s/%010lld.dat",dd,q); s->n++; if(s->min<0) s->min=q; s->max=q; if(prev>=0&&q!=prev+1) s->gaps++; prev=q;
    int fd=open(f,O_RDONLY); if(fd<0){ s->bad++; continue; } struct stat sb; fstat(fd,&sb); unsigned char*b=malloc(sb.st_size+1); ssize_t r=read(fd,b,sb.st_size); close(fd);
    if(r<=0){ s->shortf++; s->bad++; free(b); continue; } b[r]=0; unsigned long long sq; char c; size_t sz; unsigned cr; int hl=0;
    if(sscanf((char*)b,"VM2BFS1 %llu %c %zu %x\n%n",&sq,&c,&sz,&cr,&hl)<4){ s->bad++; free(b); continue; }
    char*nl=memchr(b,'\n',r); hl=nl?(int)(nl-(char*)b)+1:0;
    if(sq!=(unsigned long long)q||(size_t)(r-hl)!=sz){ if((size_t)(r-hl)<sz) s->shortf++; s->bad++; free(b); continue; }
    unsigned char*e2=malloc(sz); fill(e2,sz,q,cls); if(memcmp(e2,b+hl,sz)||crc(b+hl,sz)!=cr) s->bad++; free(e2); free(b); }
  free(seqs); }
int main(int argc,char**argv){
  crcinit(); if(argc<3){ fprintf(stderr,"fsw write|verify DIR [--rate N]\n"); return 2; } const char*dir=argv[2];
  if(getenv("VM2B_TTY")) tty=open("/dev/ttyS0",O_WRONLY|O_NOCTTY);
  if(!strcmp(argv[1],"verify")){ struct st a,b; scan(dir,0,&a); scan(dir,1,&b);
    say("FSVERIFY s: n=%ld min=%lld max=%lld gaps=%ld bad=%ld | l: n=%ld min=%lld max=%lld gaps=%ld bad=%ld short=%ld\n",a.n,a.min,a.max,a.gaps,a.bad,b.n,b.min,b.max,b.gaps,b.bad,b.shortf); return 0; }
  int rate=10; for(int i=3;i<argc;i++) if(!strcmp(argv[i],"--rate")&&i+1<argc) rate=atoi(argv[++i]); if(rate<1) rate=1;
  char p[300]; mkdir(dir,0755); snprintf(p,300,"%s/s",dir); mkdir(p,0755); snprintf(p,300,"%s/l",dir); mkdir(p,0755);
  struct st a; scan(dir,0,&a); uint64_t seq=a.max>0?(uint64_t)a.max:0; say("FSW start next=%llu\n",(unsigned long long)seq+1);
  for(;;){ seq++; if(put(dir,seq,0,1)||put(dir,seq,1,0)){ say("FSW error seq=%llu errno=%d\n",(unsigned long long)seq,errno); return 1; }
    if(seq>400){ char f[300]; snprintf(f,300,"%s/s/%010llu.dat",dir,(unsigned long long)(seq-400)); unlink(f); snprintf(f,300,"%s/l/%010llu.dat",dir,(unsigned long long)(seq-400)); unlink(f); }
    if(seq%20==0){ struct timespec t; clock_gettime(CLOCK_REALTIME,&t); say("W %llu %ld.%03ld\n",(unsigned long long)seq,(long)t.tv_sec,t.tv_nsec/1000000); }
    if(seq%200==0){ sync(); say("SYNC %llu\n",(unsigned long long)seq); }
    usleep(1000000/rate); } }
