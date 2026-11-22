/* Derived from libvorbis 1.3.7; modified for private packet decoding and explicit memory ownership. */
#include "private.h"
/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2015             *
 * by the Xiph.Org Foundation https://xiph.org/                     *
 *                                                                  *
 ********************************************************************

 function: basic shared codebook operations

 ********************************************************************/

#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <ogg/ogg.h>
#include "os.h"
#include "misc.h"
#include "vorbis/codec.h"
#include "codebook.h"
#include "scales.h"

/**** pack/unpack helpers ******************************************/

int ov_ilog(ogg_uint32_t v){
  int ret;
  for(ret=0;v;ret++)v>>=1;
  return ret;
}

/* 32 bit float (not IEEE; nonnormalized mantissa +
   biased exponent) : neeeeeee eeemmmmm mmmmmmmm mmmmmmmm
   Why not IEEE?  It's just not that important here. */

#define VQ_FEXP 10
#define VQ_FMAN 21
#define VQ_FEXP_BIAS 768 /* bias toward values smaller than 1. */

/* doesn't currently guard under/overflow */


float _float32_unpack(long val){
  double mant=val&0x1fffff;
  int    sign=val&0x80000000;
  long   exp =(val&0x7fe00000L)>>VQ_FMAN;
  if(sign)mant= -mant;
  exp=exp-(VQ_FMAN-1)-VQ_FEXP_BIAS;
  /* clamp excessive exponent values */
  if (exp>63){
    exp=63;
  }
  if (exp<-63){
    exp=-63;
  }
  return(ldexp(mant,exp));
}

/* Preserve the original 32-bit marker validation at the maximum wire length. */
static int validate_long_codewords(const char *l,long n){
  long i,j,count=0;
  ogg_uint32_t marker[33];
  memset(marker,0,sizeof(marker));

  for(i=0;i<n;i++){
    long length=l[i];
    if(length>0){
      ogg_uint32_t entry=marker[length];

      /* when we claim a node for an entry, we also claim the nodes
         below it (pruning off the imagined tree that may have dangled
         from it) as well as blocking the use of any nodes directly
         above for leaves */

      /* update ourself */
      if(length<32 && (entry>>length)){
        /* error condition; the lengths must specify an overpopulated tree */
        return -1;
      }
      count++;

      /* Look to see if the next shorter marker points to the node
         above. if so, update it and repeat.  */
      {
        for(j=length;j>0;j--){

          if(marker[j]&1){
            /* have to jump branches */
            if(j==1)
              marker[1]++;
            else
              marker[j]=marker[j-1]<<1;
            break; /* invariant says next upper marker would already
                      have been moved if it was on the same path */
          }
          marker[j]++;
        }
      }

      /* prune the tree; the implicit invariant says all the longer
         markers were dangling from our just-taken node.  Dangle them
         from our *new* node. */
      for(j=length+1;j<33;j++)
        if((marker[j]>>1) == entry){
          entry=marker[j];
          marker[j]=marker[j-1]<<1;
        }else
          break;
    }
  }

  /* any underpopulated tree must be rejected. */
  /* Single-entry codebooks are a retconned extension to the spec.
     They have a single codeword '0' of length 1 that results in an
     underpopulated tree.  Shield that case from the underformed tree check. */
  if(!(count==1 && marker[2]==2)){
    for(i=1;i<33;i++)
      if(marker[i] & (0xffffffffUL>>(32-i))){
        return -1;
      }
  }

  return 0;
}

int hv_book_validate(const static_codebook *book){
  unsigned long counts[33]={0};
  long i;
  uint64_t occupied=0;
  const uint64_t capacity=UINT64_C(1)<<32;
  /* Header unpacking bounds each length to 0..32 and the entry count to 24 bits.
     A word of length n occupies 2^(32-n) leaves at the deepest level. */
  for(i=0;i<book->entries;i++)
    counts[(unsigned char)book->lengthlist[i]]++;
  /* Use the decoder's marker arithmetic at its 32-bit boundary. */
  if(counts[32])
    return validate_long_codewords(book->lengthlist,book->entries);
  for(i=1;i<32;i++)
    occupied+=(uint64_t)counts[i]<<(32-i);
  /* Empty sparse books and a lone one-bit word are accepted by the decoder. */
  if(!occupied || (counts[1]==1 && occupied==capacity/2))return 0;
  return occupied==capacity ? 0 : -1;
}

/* there might be a straightforward one-line way to do the below
   that's portable and totally safe against roundoff, but I haven't
   thought of it.  Therefore, we opt on the side of caution */
long _book_maptype1_quantvals(const static_codebook *b){
  long vals;
  if(b->entries<1){
    return(0);
  }
  vals=floor(pow((float)b->entries,1.f/b->dim));

  /* the above *should* be reliable, but we'll not assume that FP is
     ever reliable when bitstream sync is at stake; verify via integer
     means that vals really is the greatest value of dim for which
     vals^b->bim <= b->entries */
  /* treat the above as an initial guess */
  if(vals<1){
    vals=1;
  }
  while(1){
    long acc=1;
    long acc1=1;
    int i;
    for(i=0;i<b->dim;i++){
      if(b->entries/vals<acc)break;
      acc*=vals;
      if(LONG_MAX/(vals+1)<acc1)acc1=LONG_MAX;
      else acc1*=vals+1;
    }
    if(i>=b->dim && acc<=b->entries && acc1>b->entries){
      return(vals);
    }else{
      if(i<b->dim || acc>b->entries){
        vals--;
      }else{
        vals++;
      }
    }
  }
}

/* unpack the quantized list of values for encode/decode ***********/
/* we need to deal with two map types: in map type 1, the values are
   generated algorithmically (each column of the vector counts through
   the values in the quant vector). in map type 2, all the values came
   in in an explicit list.  Both value lists must be unpacked */
float *_book_unquantize(HvAllocator *allocator, const static_codebook *b,int n,int *sparsemap){
  long j,k,count=0;
  if(b->maptype==1 || b->maptype==2){
    int quantvals;
    float mindel=_float32_unpack(b->q_min);
    float delta=_float32_unpack(b->q_delta);
    float *r=hv_callocate(allocator, n*b->dim,sizeof(*r));

    /* maptype 1 and 2 both use a quantized value vector, but
       different sizes */
    switch(b->maptype){
    case 1:
      /* most of the time, entries%dimensions == 0, but we need to be
         well defined.  We define that the possible vales at each
         scalar is values == entries/dim.  If entries%dim != 0, we'll
         have 'too few' values (values*dim<entries), which means that
         we'll have 'left over' entries; left over entries use zeroed
         values (and are wasted).  So don't generate codebooks like
         that */
      quantvals=_book_maptype1_quantvals(b);
      /* Advance the map-1 lattice as a mixed-radix counter. Only entries
         present in a sparse book need a vector. The wire limits entries to
         24 bits, so a base greater than one cannot use more than 24 digits. */
      {
        unsigned digits[24]={0};
        double levels[32];
        if(quantvals<=32){
          for(k=0;k<quantvals;k++)levels[k]=fabs((float)b->quantlist[k])*delta+mindel;
        }
        for(j=0;j<b->entries;j++){
          if(!sparsemap || b->lengthlist[j]){
            float *out=r+(sparsemap?sparsemap[count]:count)*b->dim;
            float last=0.f;
            for(k=0;k<b->dim;k++){
              unsigned digit=k<24?digits[k]:0;
              const double value=quantvals<=32 ? levels[digit] :
                  fabs((float)b->quantlist[digit])*delta+mindel;
              out[k]=(float)(value+last);
              if(b->q_sequencep)last=out[k];
            }
            count++;
          }
          if(quantvals>1){
            for(k=0;k<b->dim && k<24;k++){
              if(++digits[k]<(unsigned)quantvals)break;
              digits[k]=0;
            }
          }
        }
      }
      break;
    case 2:
      for(j=0;j<b->entries;j++){
        if((sparsemap && b->lengthlist[j]) || !sparsemap){
          float last=0.f;

          for(k=0;k<b->dim;k++){
            float val=b->quantlist[j*b->dim+k];
            val=fabs(val)*delta+mindel+last;
            if(b->q_sequencep)last=val;
            if(sparsemap)
              r[sparsemap[count]*b->dim+k]=val;
            else
              r[count*b->dim+k]=val;
          }
          count++;
        }
      }
      break;
    }

    return(r);
  }
  return(NULL);
}

void vorbis_staticbook_destroy(HvAllocator *allocator, static_codebook *b){
  if(b->allocedp){
    if(b->quantlist)hv_free(allocator, b->quantlist);
    if(b->lengthlist)hv_free(allocator, b->lengthlist);
    memset(b,0,sizeof(*b));
    hv_free(allocator, b);
  } /* otherwise, it is in static memory */
}

void vorbis_book_clear(HvAllocator *allocator, codebook *b){
  /* static book is not cleared; we're likely called on the lookup and
     the static codebook belongs to the info struct */
  if(b->valuelist)hv_free(allocator, b->valuelist);
  if(b->codelist)hv_free(allocator, b->codelist);

  if(b->dec_index)hv_free(allocator, b->dec_index);
  if(b->dec_codelengths)hv_free(allocator, b->dec_codelengths);
  if(b->dec_firsttable)hv_free(allocator, b->dec_firsttable);

  memset(b,0,sizeof(*b));
}



static ogg_uint32_t bitreverse(ogg_uint32_t x){
  x=    ((x>>16)&0x0000ffffUL) | ((x<<16)&0xffff0000UL);
  x=    ((x>> 8)&0x00ff00ffUL) | ((x<< 8)&0xff00ff00UL);
  x=    ((x>> 4)&0x0f0f0f0fUL) | ((x<< 4)&0xf0f0f0f0UL);
  x=    ((x>> 2)&0x33333333UL) | ((x<< 2)&0xccccccccUL);
  return((x>> 1)&0x55555555UL) | ((x<< 1)&0xaaaaaaaaUL);
}

/* A Vorbis code is the first available prefix of the requested length.
   The free intervals are ordered from deepest to shallowest, with at most
   one per depth. Taking the deepest prefix that fits, then splitting its
   left edge, gives the same canonical assignment as the marker algorithm.
   Keep free intervals and assigned leaves in one ordered list: each split
   creates one interval and each leaf is visited once to emit the lookup. */
static int make_decode_order(HvAllocator *allocator,const static_codebook *book,
                             int used,int *ranks,ogg_uint32_t *words){
  struct FreePrefix { int previous,next; } freeAt[33]={{-1,-1}};
  int *nextLeaf=hv_allocate(allocator,(size_t)used*sizeof(*nextLeaf));
  uint64_t available=1;
  int head=used,leaf=0;
  for(long i=0;i<book->entries;++i){
    int length=book->lengthlist[i];
    if(!length)continue;
    uint64_t fits=available&((UINT64_C(1)<<(length+1))-1);
    if(!fits)goto invalid;
    unsigned depth;
#if defined(__clang__) || defined(__GNUC__)
    depth=63u-(unsigned)__builtin_clzll(fits);
#else
    for(depth=0;fits>>=1;)++depth;
#endif
    int before=freeAt[depth].previous,after=freeAt[depth].next;
    if(before<0)head=leaf;
    else if(before<used)nextLeaf[before]=leaf;
    else freeAt[before-used].next=leaf;
    int last=leaf;
    for(int d=length;d>(int)depth;--d){
      const int slot=used+d;
      freeAt[d].previous=last;
      if(last<used)nextLeaf[last]=slot;
      else freeAt[last-used].next=slot;
      last=slot;
    }
    if(last<used)nextLeaf[last]=after;
    else freeAt[last-used].next=after;
    if(after>=used)freeAt[after-used].previous=last;
    available &= ~(UINT64_C(1) << depth);
    available |= (UINT64_C(1) << (length + 1)) - (UINT64_C(1) << (depth + 1));
    ranks[leaf++]=length;
  }
  if(available && !(used==1 && ranks[0]==1))goto invalid;
  uint64_t word=0;
  for(int rank=0;rank<used;++rank){
    const int length=ranks[head];
    words[rank]=(ogg_uint32_t)word;
    word+=UINT64_C(1)<<(32-length);
    ranks[head]=rank;
    head=nextLeaf[head];
  }
  hv_free(allocator,nextLeaf);
  return 0;
invalid:
  hv_free(allocator,nextLeaf);
  return -1;
}

/* decode codebook arrangement is more heavily optimized than encode */
int vorbis_book_init_decode(HvAllocator *allocator, codebook *c,const static_codebook *s){
  int i,j,n=0,tabn;
  int *sortindex;

  memset(c,0,sizeof(*c));

  /* count actually used entries and find max length */
  for(i=0;i<s->entries;i++){
    int length=s->lengthlist[i];
    n+=length>0;
    if(length>c->dec_maxlength)c->dec_maxlength=length;
  }

  c->entries=s->entries;
  c->used_entries=n;
  c->dim=s->dim;

  if(n>0){
    /* two different remappings go on here.

    First, we collapse the likely sparse codebook down only to
    actually represented values/words.  This collapsing needs to be
    indexed as map-valueless books are used to encode original entry
    positions as integers.

    Second, we reorder all vectors, including the entry index above,
    by sorted bitreversed codeword to allow treeless decode. */

    sortindex=hv_allocate(allocator, n*sizeof(*sortindex));
    c->codelist=hv_allocate(allocator, n*sizeof(*c->codelist));
    if(make_decode_order(allocator,s,n,sortindex,c->codelist)){
      hv_free(allocator,sortindex);
      goto err_out;
    }

    c->valuelist=_book_unquantize(allocator, s,n,sortindex);
    c->dec_index=hv_allocate(allocator, n*sizeof(*c->dec_index));

    c->dec_codelengths=hv_allocate(allocator, n*sizeof(*c->dec_codelengths));
    for(n=0,i=0;i<s->entries;i++){
      if(s->lengthlist[i]>0){
        const int index=sortindex[n++];
        c->dec_index[index]=i;
        c->dec_codelengths[index]=s->lengthlist[i];
      }
    }

    if(n==1 && c->dec_maxlength==1){
      /* special case the 'single entry codebook' with a single bit
       fastpath table (that always returns entry 0 )in order to use
       unmodified decode paths. */
      c->dec_firsttablen=1;
      c->dec_firsttable=hv_callocate(allocator, 2,sizeof(*c->dec_firsttable));
      c->dec_firsttable[0]=c->dec_firsttable[1]=1;

    }else{
      c->dec_firsttablen=ov_ilog(c->used_entries)-1; /* this is magic */
      if(c->dec_firsttablen<8)c->dec_firsttablen=8;
      if(c->dec_firsttablen>11)c->dec_firsttablen=11;

      if(allocator->compactCodebook){
        c->dec_firsttablen=ov_ilog(c->used_entries)-4;
        if(c->dec_firsttablen<5)c->dec_firsttablen=5;
        if(c->dec_firsttablen>8)c->dec_firsttablen=8;
      }
      tabn=1<<c->dec_firsttablen;
      c->dec_firsttable=hv_callocate(allocator, tabn,sizeof(*c->dec_firsttable));

      for(i=0;i<n;i++){
        if(c->dec_codelengths[i]<=c->dec_firsttablen){
          ogg_uint32_t orig=bitreverse(c->codelist[i]);
          for(j=0;j<(1<<(c->dec_firsttablen-c->dec_codelengths[i]));j++)
            c->dec_firsttable[orig|(j<<c->dec_codelengths[i])]=((ogg_uint32_t)i<<4)|c->dec_codelengths[i];
        }
      }

      /* now fill in 'unused' entries in the firsttable with hi/lo search
         hints for the non-direct-hits */
      {
        ogg_uint32_t mask=0xfffffffeUL<<(31-c->dec_firsttablen);
        long lo=0,hi=0;

        for(i=0;i<tabn;i++){
          ogg_uint32_t word=((ogg_uint32_t)i<<(32-c->dec_firsttablen));
          if(c->dec_firsttable[bitreverse(word)]==0){
            while((lo+1)<n && c->codelist[lo+1]<=word)lo++;
            while(    hi<n && word>=(c->codelist[hi]&mask))hi++;

            /* we only actually have 15 bits per hint to play with here.
               In order to overflow gracefully (nothing breaks, efficiency
               just drops), encode as the difference from the extremes. */
            {
              unsigned long loval=lo;
              unsigned long hival=n-hi;

              if(loval>0x7fff)loval=0x7fff;
              if(hival>0x7fff)hival=0x7fff;
              c->dec_firsttable[bitreverse(word)]=
                0x80000000UL | (loval<<15) | hival;
            }
          }
        }
      }
    }
    hv_free(allocator, sortindex);
  }

  return(0);
 err_out:
  vorbis_book_clear(allocator, c);
  return(-1);
}





