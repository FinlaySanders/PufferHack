/*Written by Timothy B. Terriberry (tterribe@xiph.org) 1999-2009
  CC0 (Public domain) - see http://creativecommons.org/publicdomain/zero/1.0/
  for details.
  Based on the public domain ISAAC implementation by Robert J. Jenkins Jr.*/

/*
 * Changes for NetHack:
 *      include config.h;
 *      skip rest of file if USE_ISAAC64 isn't defined there;
 *      re-do 'inline' handling.
 */
#include "config.h"

#ifdef USE_ISAAC64
#include <string.h>
#include "isaac64.h"

#define ISAAC64_MASK ((uint64_t)0xFFFFFFFFFFFFFFFFULL)

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#if !defined(HAS_INLINE)
#define HAS_INLINE
#endif
#else
# if (defined(__GNUC__) && __GNUC__ >= 2 && !defined(inline))
# define inline __inline__
# endif
#endif
#if !defined(HAS_INLINE) && !defined(inline)
#define inline /*empty*/
#endif

/* Extract ISAAC64_SZ_LOG bits (starting at bit 3). */
static inline uint32_t lower_bits(uint64_t x)
{
	return (x & ((ISAAC64_SZ-1) << 3)) >>3;
}

/* Extract next ISAAC64_SZ_LOG bits (starting at bit ISAAC64_SZ_LOG+2). */
static inline uint32_t upper_bits(uint64_t y)
{
	return (y >> (ISAAC64_SZ_LOG+3)) & (ISAAC64_SZ-1);
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__aarch64__))
# define ISAAC64_KEEP(_v) __asm__("":"+r"(_v))
# define ISAAC64_PREFETCH(_p) __builtin_prefetch(_p)
#else
# define ISAAC64_KEEP(_v) ((void)0)
# define ISAAC64_PREFETCH(_p) ((void)0)
#endif

/*b is carried as m[upper_bits(y)] plus the previous x (p), so the serial chain per word is one add, the index and one load.*/
#define ISAAC64_STEP(_mix,_off) \
  x=m[i]; \
  a=(_mix)+m[i _off]; \
  k=p+a+m[lower_bits(x)]; \
  ISAAC64_KEEP(k); \
  m[i]=y=b+k; \
  b=m[upper_bits(y)]; \
  r[i]=b+x; \
  p=x

static void isaac64_update(isaac64_ctx *_ctx){
  uint64_t *m;
  uint64_t *r;
  uint64_t  a;
  uint64_t  b;
  uint64_t  x;
  uint64_t  y;
  uint64_t  k;
  uint64_t  p;
  int       i;
  m=_ctx->m;
  r=_ctx->r;
  for(i=0;i<ISAAC64_SZ;i+=8)ISAAC64_PREFETCH(m+i);
  a=_ctx->a;
  b=_ctx->b+(++_ctx->c);
  p=0;
  for(i=0;i<ISAAC64_SZ/2;i++){
    ISAAC64_STEP(~(a^a<<21),+ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a>>5,+ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a<<12,+ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a>>33,+ISAAC64_SZ/2);
  }
  for(i=ISAAC64_SZ/2;i<ISAAC64_SZ;i++){
    ISAAC64_STEP(~(a^a<<21),-ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a>>5,-ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a<<12,-ISAAC64_SZ/2);
    i++;
    ISAAC64_STEP(a^a>>33,-ISAAC64_SZ/2);
  }
  _ctx->b=b+p;
  _ctx->a=a;
  _ctx->n=ISAAC64_SZ;
}

/*The eight words stay in registers across the seeding passes.*/
#define ISAAC64_MIX(a,b,c,d,e,f,g,h) \
  a-=e; f^=h>>9; h+=a; \
  b-=f; g^=a<<9; a+=b; \
  c-=g; h^=b>>23; b+=c; \
  d-=h; a^=c<<15; c+=d; \
  e-=a; b^=d>>14; d+=e; \
  f-=b; c^=e<<20; e+=f; \
  g-=c; d^=f>>17; f+=g; \
  h-=d; e^=g<<14; g+=h

void isaac64_init(isaac64_ctx *_ctx,const unsigned char *_seed,int _nseed){
  _ctx->a=_ctx->b=_ctx->c=0;
  memset(_ctx->r,0,sizeof(_ctx->r));
  isaac64_reseed(_ctx,_seed,_nseed);
}

void isaac64_reseed(isaac64_ctx *_ctx,const unsigned char *_seed,int _nseed){
  uint64_t *m;
  uint64_t *r;
  uint64_t  a,b,c,d,e,f,g,h;
  int       i;
  int       j;
  m=_ctx->m;
  r=_ctx->r;
  if(_nseed>ISAAC64_SEED_SZ_MAX)_nseed=ISAAC64_SEED_SZ_MAX;
  for(i=0;i<_nseed>>3;i++){
    r[i]^=(uint64_t)_seed[i<<3|7]<<56|(uint64_t)_seed[i<<3|6]<<48|
     (uint64_t)_seed[i<<3|5]<<40|(uint64_t)_seed[i<<3|4]<<32|
     (uint64_t)_seed[i<<3|3]<<24|(uint64_t)_seed[i<<3|2]<<16|
     (uint64_t)_seed[i<<3|1]<<8|_seed[i<<3];
  }
  _nseed-=i<<3;
  if(_nseed>0){
    uint64_t ri;
    ri=_seed[i<<3];
    for(j=1;j<_nseed;j++)ri|=(uint64_t)_seed[i<<3|j]<<(j<<3);
    r[i++]^=ri;
  }
  a=b=c=d=e=f=g=h=(uint64_t)0x9E3779B97F4A7C13ULL;
  for(i=0;i<4;i++){
    ISAAC64_MIX(a,b,c,d,e,f,g,h);
  }
  for(i=0;i<ISAAC64_SZ;i+=8){
    a+=r[i];b+=r[i+1];c+=r[i+2];d+=r[i+3];e+=r[i+4];f+=r[i+5];g+=r[i+6];h+=r[i+7];
    ISAAC64_MIX(a,b,c,d,e,f,g,h);
    m[i]=a;m[i+1]=b;m[i+2]=c;m[i+3]=d;m[i+4]=e;m[i+5]=f;m[i+6]=g;m[i+7]=h;
  }
  for(i=0;i<ISAAC64_SZ;i+=8){
    a+=m[i];b+=m[i+1];c+=m[i+2];d+=m[i+3];e+=m[i+4];f+=m[i+5];g+=m[i+6];h+=m[i+7];
    ISAAC64_MIX(a,b,c,d,e,f,g,h);
    m[i]=a;m[i+1]=b;m[i+2]=c;m[i+3]=d;m[i+4]=e;m[i+5]=f;m[i+6]=g;m[i+7]=h;
  }
  isaac64_update(_ctx);
}

uint64_t isaac64_next_uint64(isaac64_ctx *_ctx){
  if(!_ctx->n)isaac64_update(_ctx);
  return _ctx->r[--_ctx->n];
}

uint64_t isaac64_next_uint(isaac64_ctx *_ctx,uint64_t _n){
  uint64_t r;
  uint64_t v;
  uint64_t d;
  do{
    r=isaac64_next_uint64(_ctx);
    v=r%_n;
    d=r-v;
  }
  while(((d+_n-1)&ISAAC64_MASK)<d);
  return v;
}
#endif /* USE_ISAAC64 */

/*isaac64.c*/
