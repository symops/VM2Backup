// Native (Linux, AddressSanitizer) fuzzer of the key-file parsers of libssh2: tools/fuzz-ssh-keys.sh builds and runs it.
//   fuzz_keys <seed> <iterations> <key dir> <scratch file>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libssh2.h>
#include <unistd.h>
#include <signal.h>
int _libssh2_pub_priv_keyfile(LIBSSH2_SESSION*, unsigned char **m, size_t*, unsigned char **p, size_t*, const char*, const char*);
int _libssh2_rsa_new_private(void **, LIBSSH2_SESSION*, const char*, const unsigned char*);
int _libssh2_ecdsa_new_private(void **, LIBSSH2_SESSION*, const char*, const unsigned char*);
int _libssh2_ed25519_new_private(void **, LIBSSH2_SESSION*, const char*, const unsigned char*);
static uint64_t s;
static uint32_t rnd(uint32_t n){ s = s*6364136223846793005ULL+1442695040888963407ULL; return n ? (uint32_t)((s>>33)%n) : 0; }
int main(int argc, char** argv){
  s = strtoull(argv[1],0,10); int iters = atoi(argv[2]); const char* dir=argv[3];
  const char* files[] = {"u_ed25519","u_ed25519_pw","u_rsa","u_rsa_pem","u_rsa_pkcs8","u_ecdsa256","u_ecdsa384_pw","u_ed25519_pkcs8","u_ecdsa_pem","u_ecdsa521"};
  LIBSSH2_SESSION* ses = libssh2_session_init();
  for (int it=0; it<iters; ++it) for (int fi=0; fi<10; ++fi) {
    char path[512]; snprintf(path,sizeof path,"%s/%s",dir,files[fi]);
    FILE*f=fopen(path,"rb"); unsigned char* o=malloc(20000); size_t n=fread(o,1,20000,f); fclose(f);
    unsigned char* m=malloc(n+100); memcpy(m,o,n); size_t mn=n;
    int kind=rnd(4);
    if(kind==0){int k=1+rnd(5); for(int j=0;j<k;++j) m[rnd(mn)]=rnd(256);}
    else if(kind==1){mn=rnd(mn);}
    else if(kind==2){size_t p=rnd(mn); size_t l=rnd(40); memmove(m+p+l,m+p,mn-p); for(size_t j=0;j<l;++j) m[p+j]=rnd(256); mn+=l;}
    else {size_t a=rnd(mn),b=rnd(mn); if(a>b){size_t t=a;a=b;b=t;} for(size_t j=a;j<b;++j) m[j]='A';}
    const char* tp=argv[4]; f=fopen(tp,"wb"); fwrite(m,1,mn,f); fclose(f);
    alarm(20); unsigned char *me=0,*pd=0; size_t ml=0,pl=0;
    _libssh2_pub_priv_keyfile(ses,&me,&ml,&pd,&pl,tp,"pass phrase 123");
    if(pd) free(pd); if(me) free(me);
    void* c=0;
    if(_libssh2_rsa_new_private(&c,ses,tp,(const unsigned char*)"pass phrase 123")==0 && c) {}
    c=0; _libssh2_ecdsa_new_private(&c,ses,tp,(const unsigned char*)"pass phrase 123");
    c=0; _libssh2_ed25519_new_private(&c,ses,tp,(const unsigned char*)"pass phrase 123");
    alarm(0); free(o); free(m);
  }
  puts("done"); return 0; }
