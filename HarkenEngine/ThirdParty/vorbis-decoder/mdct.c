/* Derived from libvorbis 1.3.7; modified for private packet decoding and explicit memory ownership. */
#include "private.h"
/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2009             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: normalized modified discrete cosine transform
           power of two length transform only [64 <= n ]

 Original algorithm adapted long ago from _The use of multirate filter
 banks for coding of high quality digital audio_, by T. Sporer,
 K. Brandenburg and B. Edler, collection of the European Signal
 Processing Conference (EUSIPCO), Amsterdam, June 1992, Vol.1, pp
 211-214

 The below code implements an algorithm that no longer looks much like
 that presented in the paper, but the basic structure remains if you
 dig deep enough to see it.

 This module DOES NOT INCLUDE code to generate/apply the window
 function.  Everybody has their own weird favorite including me... I
 happen to like the properties of y=sin(.5PI*sin^2(x)), but others may
 vehemently disagree.

 ********************************************************************/

/* this can also be run as an integer transform by uncommenting a
   define in mdct.h; the integerization is a first pass and although
   it's likely stable for Vorbis, the dynamic range is constrained and
   roundoff isn't done (so it's noisy).  Consider it functional, but
   only a starting point.  There's no point on a machine with an FPU */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vorbis/codec.h"
#include "mdct.h"
#if !defined(MDCT_INTEGERIZED) && (defined(__x86_64__) || defined(_M_X64))
#include <xmmintrin.h>
#define HV_MDCT_STAGE_VECTOR 1
#endif
#include "os.h"
#include "misc.h"

/* build lookups for trig functions; also pre-figure scaling and
   some window function algebra. */

void mdct_init(HvAllocator *allocator, mdct_lookup *lookup,int n){
  int   *bitrev=hv_allocate(allocator, sizeof(*bitrev)*(n/4));
  DATA_TYPE *T=hv_allocate(allocator, sizeof(*T)*(n+n/4));

  int i;
  int n2=n>>1;
  int log2n=lookup->log2n=rint(log((float)n)/log(2.f));
  lookup->n=n;
  lookup->trig=T;
  lookup->bitrev=bitrev;

/* trig lookups... */

  for(i=0;i<n/4;i++){
    T[i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i)));
    T[i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i)));
    T[n2+i*2]=FLOAT_CONV(cos((M_PI/(2*n))*(2*i+1)));
    T[n2+i*2+1]=FLOAT_CONV(sin((M_PI/(2*n))*(2*i+1)));
  }
  for(i=0;i<n/8;i++){
    T[n+i*2]=FLOAT_CONV(cos((M_PI/n)*(4*i+2))*.5);
    T[n+i*2+1]=FLOAT_CONV(-sin((M_PI/n)*(4*i+2))*.5);
  }

  /* bitreverse lookup... */

  {
    int mask=(1<<(log2n-1))-1,i,j;
    int msb=1<<(log2n-2);
    for(i=0;i<n/8;i++){
      int acc=0;
      for(j=0;msb>>j;j++)
        if((msb>>j)&i)acc|=1<<j;
      bitrev[i*2]=((~acc)&mask)-1;
      bitrev[i*2+1]=acc;

    }
  }
  lookup->scale=FLOAT_CONV(4.f/n);
}

/* 8 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_8(DATA_TYPE *x){
  REG_TYPE r0   = x[6] + x[2];
  REG_TYPE r1   = x[6] - x[2];
  REG_TYPE r2   = x[4] + x[0];
  REG_TYPE r3   = x[4] - x[0];

           x[6] = r0   + r2;
           x[4] = r0   - r2;

           r0   = x[5] - x[1];
           r2   = x[7] - x[3];
           x[0] = r1   + r0;
           x[2] = r1   - r0;

           r0   = x[5] + x[1];
           r1   = x[7] + x[3];
           x[3] = r2   + r3;
           x[1] = r2   - r3;
           x[7] = r1   + r0;
           x[5] = r1   - r0;

}

/* 16 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_16(DATA_TYPE *x){
  REG_TYPE r0     = x[1]  - x[9];
  REG_TYPE r1     = x[0]  - x[8];

           x[8]  += x[0];
           x[9]  += x[1];
           x[0]   = MULT_NORM((r0   + r1) * cPI2_8);
           x[1]   = MULT_NORM((r0   - r1) * cPI2_8);

           r0     = x[3]  - x[11];
           r1     = x[10] - x[2];
           x[10] += x[2];
           x[11] += x[3];
           x[2]   = r0;
           x[3]   = r1;

           r0     = x[12] - x[4];
           r1     = x[13] - x[5];
           x[12] += x[4];
           x[13] += x[5];
           x[4]   = MULT_NORM((r0   - r1) * cPI2_8);
           x[5]   = MULT_NORM((r0   + r1) * cPI2_8);

           r0     = x[14] - x[6];
           r1     = x[15] - x[7];
           x[14] += x[6];
           x[15] += x[7];
           x[6]  = r0;
           x[7]  = r1;

           mdct_butterfly_8(x);
           mdct_butterfly_8(x+8);
}

/* 32 point butterfly (in place, 4 register) */
STIN void mdct_butterfly_32(DATA_TYPE *x){
  REG_TYPE r0     = x[30] - x[14];
  REG_TYPE r1     = x[31] - x[15];

           x[30] +=         x[14];
           x[31] +=         x[15];
           x[14]  =         r0;
           x[15]  =         r1;

           r0     = x[28] - x[12];
           r1     = x[29] - x[13];
           x[28] +=         x[12];
           x[29] +=         x[13];
           x[12]  = MULT_NORM( r0 * cPI1_8  -  r1 * cPI3_8 );
           x[13]  = MULT_NORM( r0 * cPI3_8  +  r1 * cPI1_8 );

           r0     = x[26] - x[10];
           r1     = x[27] - x[11];
           x[26] +=         x[10];
           x[27] +=         x[11];
           x[10]  = MULT_NORM(( r0  - r1 ) * cPI2_8);
           x[11]  = MULT_NORM(( r0  + r1 ) * cPI2_8);

           r0     = x[24] - x[8];
           r1     = x[25] - x[9];
           x[24] += x[8];
           x[25] += x[9];
           x[8]   = MULT_NORM( r0 * cPI3_8  -  r1 * cPI1_8 );
           x[9]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );

           r0     = x[22] - x[6];
           r1     = x[7]  - x[23];
           x[22] += x[6];
           x[23] += x[7];
           x[6]   = r1;
           x[7]   = r0;

           r0     = x[4]  - x[20];
           r1     = x[5]  - x[21];
           x[20] += x[4];
           x[21] += x[5];
           x[4]   = MULT_NORM( r1 * cPI1_8  +  r0 * cPI3_8 );
           x[5]   = MULT_NORM( r1 * cPI3_8  -  r0 * cPI1_8 );

           r0     = x[2]  - x[18];
           r1     = x[3]  - x[19];
           x[18] += x[2];
           x[19] += x[3];
           x[2]   = MULT_NORM(( r1  + r0 ) * cPI2_8);
           x[3]   = MULT_NORM(( r1  - r0 ) * cPI2_8);

           r0     = x[0]  - x[16];
           r1     = x[1]  - x[17];
           x[16] += x[0];
           x[17] += x[1];
           x[0]   = MULT_NORM( r1 * cPI3_8  +  r0 * cPI1_8 );
           x[1]   = MULT_NORM( r1 * cPI1_8  -  r0 * cPI3_8 );

           mdct_butterfly_16(x);
           mdct_butterfly_16(x+16);

}

/* N point first stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_first(DATA_TYPE *T,
                                        DATA_TYPE *x,
                                        int points){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;
  REG_TYPE   r0;
  REG_TYPE   r1;

  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[5]  +  r0 * T[4]);
               x2[5]   = MULT_NORM(r1 * T[4]  -  r0 * T[5]);

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[9]  +  r0 * T[8]);
               x2[3]   = MULT_NORM(r1 * T[8]  -  r0 * T[9]);

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[13] +  r0 * T[12]);
               x2[1]   = MULT_NORM(r1 * T[12] -  r0 * T[13]);

    x1-=8;
    x2-=8;
    T+=16;

  }while(x2>=x);
}

/* N/stage point generic N stage butterfly (in place, 2 register) */
STIN void mdct_butterfly_generic(DATA_TYPE *T,
                                          DATA_TYPE *x,
                                          int points,
                                          int trigint){

  DATA_TYPE *x1        = x          + points      - 8;
  DATA_TYPE *x2        = x          + (points>>1) - 8;
  REG_TYPE   r0;
  REG_TYPE   r1;

  do{

               r0      = x1[6]      -  x2[6];
               r1      = x1[7]      -  x2[7];
               x1[6]  += x2[6];
               x1[7]  += x2[7];
               x2[6]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[7]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[4]      -  x2[4];
               r1      = x1[5]      -  x2[5];
               x1[4]  += x2[4];
               x1[5]  += x2[5];
               x2[4]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[5]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[2]      -  x2[2];
               r1      = x1[3]      -  x2[3];
               x1[2]  += x2[2];
               x1[3]  += x2[3];
               x2[2]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[3]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;

               r0      = x1[0]      -  x2[0];
               r1      = x1[1]      -  x2[1];
               x1[0]  += x2[0];
               x1[1]  += x2[1];
               x2[0]   = MULT_NORM(r1 * T[1]  +  r0 * T[0]);
               x2[1]   = MULT_NORM(r1 * T[0]  -  r0 * T[1]);

               T+=trigint;
    x1-=8;
    x2-=8;

  }while(x2>=x);
}


#if defined(HV_MDCT_STAGE_VECTOR)
static inline __m128 hv_mdct_pair(const float* first,const float* second){
  __m128 pair=_mm_loadl_pi(_mm_setzero_ps(),(const __m64*)first);
  return _mm_loadh_pi(pair,(const __m64*)second);
}
static inline void hv_mdct_butterfly_stage(float* data,int points,const float* trig,int step){
  int half=points/2;
  for(int i=half-8;i>=0;i-=8,trig+=4*step){
    float* lower=data+i;
    float* upper=lower+half;
    __m128 lo0=_mm_loadu_ps(lower),lo1=_mm_loadu_ps(lower+4);
    __m128 hi0=_mm_loadu_ps(upper),hi1=_mm_loadu_ps(upper+4);
    __m128 d0=_mm_sub_ps(hi0,lo0),d1=_mm_sub_ps(hi1,lo1);
    __m128 even=_mm_shuffle_ps(d0,d1,_MM_SHUFFLE(2,0,2,0));
    __m128 odd=_mm_shuffle_ps(d0,d1,_MM_SHUFFLE(3,1,3,1));
    __m128 p0=hv_mdct_pair(trig+3*step,trig+2*step),p1=hv_mdct_pair(trig+step,trig);
    __m128 cosine=_mm_shuffle_ps(p0,p1,_MM_SHUFFLE(2,0,2,0));
    __m128 sine=_mm_shuffle_ps(p0,p1,_MM_SHUFFLE(3,1,3,1));
    __m128 real=_mm_add_ps(_mm_mul_ps(odd,sine),_mm_mul_ps(even,cosine));
    __m128 imag=_mm_sub_ps(_mm_mul_ps(odd,cosine),_mm_mul_ps(even,sine));
    _mm_storeu_ps(upper,_mm_add_ps(hi0,lo0));
    _mm_storeu_ps(upper+4,_mm_add_ps(hi1,lo1));
    _mm_storeu_ps(lower,_mm_unpacklo_ps(real,imag));
    _mm_storeu_ps(lower+4,_mm_unpackhi_ps(real,imag));
  }
}
#endif
STIN void mdct_butterflies(mdct_lookup *init,
                             DATA_TYPE *x,
                             int points){

  DATA_TYPE *T=init->trig;
  int stages=init->log2n-5;
  int i,j;

  if(--stages>0){
#ifdef HV_MDCT_STAGE_VECTOR
    hv_mdct_butterfly_stage(x,points,T,4);
#else
    mdct_butterfly_first(T,x,points);
#endif
  }

  for(i=1;--stages>0;i++){
    for(j=0;j<(1<<i);j++)
#if defined(HV_MDCT_STAGE_VECTOR)
      hv_mdct_butterfly_stage(x+(points>>i)*j,points>>i,T,4<<i);
#else
      mdct_butterfly_generic(T,x+(points>>i)*j,points>>i,4<<i);
#endif
  }

  for(j=0;j<points;j+=32)
    mdct_butterfly_32(x+j);

}

void mdct_clear(HvAllocator *allocator, mdct_lookup *l){
  if(l){
    if(l->trig)hv_free(allocator, l->trig);
    if(l->bitrev)hv_free(allocator, l->bitrev);
    memset(l,0,sizeof(*l));
  }
}

STIN void mdct_bitreverse(mdct_lookup *init,
                            DATA_TYPE *x){
  int        n       = init->n;
  int       *bit     = init->bitrev;
  DATA_TYPE *w0      = x;
  DATA_TYPE *w1      = x = w0+(n>>1);
  DATA_TYPE *T       = init->trig+n;


#if defined(HV_MDCT_STAGE_VECTOR)
  const __m128 half=_mm_set1_ps(.5f);
  do {
    __m128 p0=hv_mdct_pair(x+bit[0],x+bit[2]),p1=hv_mdct_pair(x+bit[4],x+bit[6]);
    __m128 q0=hv_mdct_pair(x+bit[1],x+bit[3]),q1=hv_mdct_pair(x+bit[5],x+bit[7]);
    __m128 ta=_mm_loadu_ps(T),tb=_mm_loadu_ps(T+4);
    __m128 re0=_mm_shuffle_ps(p0,p1,_MM_SHUFFLE(2,0,2,0)), im0=_mm_shuffle_ps(p0,p1,_MM_SHUFFLE(3,1,3,1));
    __m128 re1=_mm_shuffle_ps(q0,q1,_MM_SHUFFLE(2,0,2,0)), im1=_mm_shuffle_ps(q0,q1,_MM_SHUFFLE(3,1,3,1));
    __m128 tc=_mm_shuffle_ps(ta,tb,_MM_SHUFFLE(2,0,2,0)), ts=_mm_shuffle_ps(ta,tb,_MM_SHUFFLE(3,1,3,1));
    __m128 r0=_mm_sub_ps(im0,im1), r1=_mm_add_ps(re0,re1);
    __m128 r2=_mm_add_ps(_mm_mul_ps(r1,tc),_mm_mul_ps(r0,ts));
    __m128 r3=_mm_sub_ps(_mm_mul_ps(r1,ts),_mm_mul_ps(r0,tc));
    r0=_mm_mul_ps(_mm_add_ps(im0,im1),half);
    r1=_mm_mul_ps(_mm_sub_ps(re0,re1),half);
    __m128 a=_mm_add_ps(r0,r2), b=_mm_add_ps(r1,r3);
    __m128 c=_mm_sub_ps(r0,r2), d=_mm_sub_ps(r3,r1);
    c=_mm_shuffle_ps(c,c,_MM_SHUFFLE(0,1,2,3));
    d=_mm_shuffle_ps(d,d,_MM_SHUFFLE(0,1,2,3));
    w1-=8;
    _mm_storeu_ps(w0,_mm_unpacklo_ps(a,b)); _mm_storeu_ps(w0+4,_mm_unpackhi_ps(a,b));
    _mm_storeu_ps(w1,_mm_unpacklo_ps(c,d)); _mm_storeu_ps(w1+4,_mm_unpackhi_ps(c,d));
    bit+=8;T+=8;w0+=8;
  }while(w0<w1);
#else
  do{
    DATA_TYPE *x0    = x+bit[0];
    DATA_TYPE *x1    = x+bit[1];

    REG_TYPE  r0     = x0[1]  - x1[1];
    REG_TYPE  r1     = x0[0]  + x1[0];
    REG_TYPE  r2     = MULT_NORM(r1     * T[0]   + r0 * T[1]);
    REG_TYPE  r3     = MULT_NORM(r1     * T[1]   - r0 * T[0]);

              w1    -= 4;

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[0]  = r0     + r2;
              w1[2]  = r0     - r2;
              w0[1]  = r1     + r3;
              w1[3]  = r3     - r1;

              x0     = x+bit[2];
              x1     = x+bit[3];

              r0     = x0[1]  - x1[1];
              r1     = x0[0]  + x1[0];
              r2     = MULT_NORM(r1     * T[2]   + r0 * T[3]);
              r3     = MULT_NORM(r1     * T[3]   - r0 * T[2]);

              r0     = HALVE(x0[1] + x1[1]);
              r1     = HALVE(x0[0] - x1[0]);

              w0[2]  = r0     + r2;
              w1[0]  = r0     - r2;
              w0[3]  = r1     + r3;
              w1[1]  = r3     - r1;

              T     += 4;
              bit   += 4;
              w0    += 4;

  }while(w0<w1);
#endif
}

void mdct_backward(mdct_lookup *init, DATA_TYPE *in, DATA_TYPE *out){
  int n=init->n;
  int n2=n>>1;
  int n4=n>>2;

  /* rotate */

  DATA_TYPE *iX = in+n2-7;
  DATA_TYPE *oX = out+n2+n4;
  DATA_TYPE *T  = init->trig+n4;

  do{
    oX         -= 4;
    oX[0]       = MULT_NORM(-iX[2] * T[3] - iX[0]  * T[2]);
    oX[1]       = MULT_NORM (iX[0] * T[3] - iX[2]  * T[2]);
    oX[2]       = MULT_NORM(-iX[6] * T[1] - iX[4]  * T[0]);
    oX[3]       = MULT_NORM (iX[4] * T[1] - iX[6]  * T[0]);
    iX         -= 8;
    T          += 4;
  }while(iX>=in);

  iX            = in+n2-8;
  oX            = out+n2+n4;
  T             = init->trig+n4;

  do{
    T          -= 4;
    oX[0]       =  MULT_NORM (iX[4] * T[3] + iX[6] * T[2]);
    oX[1]       =  MULT_NORM (iX[4] * T[2] - iX[6] * T[3]);
    oX[2]       =  MULT_NORM (iX[0] * T[1] + iX[2] * T[0]);
    oX[3]       =  MULT_NORM (iX[0] * T[0] - iX[2] * T[1]);
    iX         -= 8;
    oX         += 4;
  }while(iX>=in);

  mdct_butterflies(init,out+n2,n2);
  mdct_bitreverse(init,out);

  /* roatate + window */

  {
    DATA_TYPE *oX1=out+n2+n4;
    DATA_TYPE *oX2=out+n2+n4;
    DATA_TYPE *iX =out;
    T             =init->trig+n2;

    do{
      oX1-=4;

      oX1[3]  =  MULT_NORM (iX[0] * T[1] - iX[1] * T[0]);
      oX2[0]  = -MULT_NORM (iX[0] * T[0] + iX[1] * T[1]);

      oX1[2]  =  MULT_NORM (iX[2] * T[3] - iX[3] * T[2]);
      oX2[1]  = -MULT_NORM (iX[2] * T[2] + iX[3] * T[3]);

      oX1[1]  =  MULT_NORM (iX[4] * T[5] - iX[5] * T[4]);
      oX2[2]  = -MULT_NORM (iX[4] * T[4] + iX[5] * T[5]);

      oX1[0]  =  MULT_NORM (iX[6] * T[7] - iX[7] * T[6]);
      oX2[3]  = -MULT_NORM (iX[6] * T[6] + iX[7] * T[7]);

      oX2+=4;
      iX    +=   8;
      T     +=   8;
    }while(iX<oX1);

    iX=out+n2+n4;
    oX1=out+n4;
    oX2=oX1;

    do{
      oX1-=4;
      iX-=4;

      oX2[0] = -(oX1[3] = iX[3]);
      oX2[1] = -(oX1[2] = iX[2]);
      oX2[2] = -(oX1[1] = iX[1]);
      oX2[3] = -(oX1[0] = iX[0]);

      oX2+=4;
    }while(oX2<iX);

    iX=out+n2+n4;
    oX1=out+n2+n4;
    oX2=out+n2;
    do{
      oX1-=4;
      oX1[0]= iX[3];
      oX1[1]= iX[2];
      oX1[2]= iX[1];
      oX1[3]= iX[0];
      iX+=4;
    }while(oX1>oX2);
  }
}


