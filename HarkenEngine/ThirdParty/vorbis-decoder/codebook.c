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

 function: basic codebook pack/unpack/code/decode operations

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codebook.h"
#include "scales.h"
#include "misc.h"
#include "os.h"

/* packs the given codebook into the bitstream **************************/



/* unpacks a codebook from the packet buffer into the codebook struct,
   readies the codebook auxiliary structures for decode *************/
static_codebook *vorbis_staticbook_unpack(HvAllocator *allocator, oggpack_buffer *opb){
  long i,j;
  static_codebook *s=hv_callocate(allocator, 1,sizeof(*s));
  s->allocedp=1;

  /* make sure alignment is correct */
  if(oggpack_read(opb,24)!=0x564342)goto _eofout;

  /* first the basic parameters */
  s->dim=oggpack_read(opb,16);
  s->entries=oggpack_read(opb,24);
  if(s->entries==-1)goto _eofout;

  if(ov_ilog(s->dim)+ov_ilog(s->entries)>24)goto _eofout;

  /* codeword ordering.... length ordered or unordered? */
  switch((int)oggpack_read(opb,1)){
  case 0:{
    long unused;
    /* allocated but unused entries? */
    unused=oggpack_read(opb,1);
    if((s->entries*(unused?1:5)+7)>>3>opb->storage-oggpack_bytes(opb))
      goto _eofout;
    /* unordered */
    s->lengthlist=hv_allocate(allocator, sizeof(*s->lengthlist)*s->entries);

    /* allocated but unused entries? */
    if(unused){
      /* yes, unused entries */

      for(i=0;i<s->entries;i++){
        if(oggpack_read(opb,1)){
          long num=oggpack_read(opb,5);
          if(num==-1)goto _eofout;
          s->lengthlist[i]=num+1;
        }else
          s->lengthlist[i]=0;
      }
    }else{
      /* all entries used; no tagging */
      for(i=0;i<s->entries;i++){
        long num=oggpack_read(opb,5);
        if(num==-1)goto _eofout;
        s->lengthlist[i]=num+1;
      }
    }

    break;
  }
  case 1:
    /* ordered */
    {
      long length=oggpack_read(opb,5)+1;
      if(length==0)goto _eofout;
      s->lengthlist=hv_allocate(allocator, sizeof(*s->lengthlist)*s->entries);

      for(i=0;i<s->entries;){
        long num=oggpack_read(opb,ov_ilog(s->entries-i));
        if(num==-1)goto _eofout;
        if(length>32 || num>s->entries-i ||
           (num>0 && (num-1)>>(length-1)>1)){
          goto _errout;
        }
        if(length>32)goto _errout;
        for(j=0;j<num;j++,i++)
          s->lengthlist[i]=length;
        length++;
      }
    }
    break;
  default:
    /* EOF */
    goto _eofout;
  }

  /* Do we have a mapping to unpack? */
  switch((s->maptype=oggpack_read(opb,4))){
  case 0:
    /* no mapping */
    break;
  case 1: case 2:
    /* A mapped vector must have a dimension before its lookup is built. */
    if(s->dim==0)goto _eofout;
    /* implicitly populated value mapping */
    /* explicitly populated value mapping */

    s->q_min=oggpack_read(opb,32);
    s->q_delta=oggpack_read(opb,32);
    s->q_quant=oggpack_read(opb,4)+1;
    s->q_sequencep=oggpack_read(opb,1);
    if(s->q_sequencep==-1)goto _eofout;

    {
      int quantvals=0;
      switch(s->maptype){
      case 1:
        quantvals=(s->dim==0?0:_book_maptype1_quantvals(s));
        break;
      case 2:
        quantvals=s->entries*s->dim;
        break;
      }

      /* quantized values */
      if(((quantvals*s->q_quant+7)>>3)>opb->storage-oggpack_bytes(opb))
        goto _eofout;
      s->quantlist=hv_allocate(allocator, sizeof(*s->quantlist)*quantvals);
      for(i=0;i<quantvals;i++)
        s->quantlist[i]=oggpack_read(opb,s->q_quant);

      if(quantvals&&s->quantlist[quantvals-1]==-1)goto _eofout;
    }
    break;
  default:
    goto _errout;
  }

  /* all set */
  return(s);

 _errout:
 _eofout:
  vorbis_staticbook_destroy(allocator, s);
  return(NULL);
}

/* returns the number of bits ************************************************/


/* the 'eliminate the decode tree' optimization actually requires the
   codewords to be MSb first, not LSb.  This is an annoying inelegancy
   (and one of the first places where carefully thought out design
   turned out to be wrong; Vorbis II and future Ogg codecs should go
   to an MSb bitpacker), but not actually the huge hit it appears to
   be.  The first-stage decode table catches most words so that
   bitreverse is not in the main execution path. */

static ogg_uint32_t bitreverse(ogg_uint32_t x){
  x=    ((x>>16)&0x0000ffff) | ((x<<16)&0xffff0000);
  x=    ((x>> 8)&0x00ff00ff) | ((x<< 8)&0xff00ff00);
  x=    ((x>> 4)&0x0f0f0f0f) | ((x<< 4)&0xf0f0f0f0);
  x=    ((x>> 2)&0x33333333) | ((x<< 2)&0xcccccccc);
  return((x>> 1)&0x55555555) | ((x<< 1)&0xaaaaaaaa);
}

STIN long decode_packed_entry_number(codebook *book, oggpack_buffer *b){
  int  read=book->dec_maxlength;
  long lo,hi;
  long lok;
  int have_window=b->endbyte<=b->storage-4;
  ogg_uint32_t window=0;
  if(have_window){
    const unsigned char *p=b->ptr;
    window=((ogg_uint32_t)p[0] | ((ogg_uint32_t)p[1]<<8) |
            ((ogg_uint32_t)p[2]<<16) | ((ogg_uint32_t)p[3]<<24)) >> b->endbit;
    lok=window & ((1u<<book->dec_firsttablen)-1u);
  }else{
    lok=oggpack_look(b,book->dec_firsttablen);
  }

  if(lok>=0){
    ogg_uint32_t entry=book->dec_firsttable[lok];
    if(!(entry & 0x80000000u)){
      /* A direct entry includes its bit length. The successful peek already
         established that this many bits are available, including at the tail. */
      unsigned cursor=b->endbit+(entry&15u);
      b->ptr+=cursor>>3;
      b->endbyte+=cursor>>3;
      b->endbit=cursor&7u;
      return entry>>4;
    }
    lo=(entry>>15)&0x7fff;
    hi=book->used_entries-(entry&0x7fff);
  }else{
    lo=0;
    hi=book->used_entries;
  }
  lok = have_window && read<=24 ? (long)(window & ((1u<<read)-1u)) : oggpack_look(b,read);

  while(lok<0 && read>1)
    lok = oggpack_look(b, --read);
  if(lok<0)return -1;

  /* bisect search for the codeword in the ordered list */
  {
    ogg_uint32_t testword=bitreverse((ogg_uint32_t)lok);

    while(hi-lo>1){
      long p=(hi-lo)>>1;
      long test=book->codelist[lo+p]>testword;
      lo+=p&(test-1);
      hi-=p&(-test);
      }

    if(book->dec_codelengths[lo]<=read){
      oggpack_adv(b, book->dec_codelengths[lo]);
      return(lo);
    }
  }

  oggpack_adv(b, read);

  return(-1);
}

/* Decode side is specced and easier, because we don't need to find
   matches using different criteria; we simply read and map.  There are
   two things we need to do 'depending':

   We may need to support interleave.  We don't really, but it's
   convenient to do it here rather than rebuild the vector later.

   Cascades may be additive or multiplicitive; this is not inherent in
   the codebook, but set in the code using the codebook.  Like
   interleaving, it's easiest to do it here.
   addmul==0 -> declarative (set the value)
   addmul==1 -> additive
   addmul==2 -> multiplicitive */

/* returns the [original, not compacted] entry number or -1 on eof *********/
long vorbis_book_decode(codebook *book, oggpack_buffer *b){
  if(book->used_entries>0){
    long packed_entry=decode_packed_entry_number(book,b);
    if(packed_entry>=0)
      return(book->dec_index[packed_entry]);
  }

  /* if there's no dec_index, the codebook unpacking isn't collapsed */
  return(-1);
}

/* returns 0 on OK or -1 on eof *************************************/
/* decode vector / dim granularity gaurding is done in the upper layer */
long vorbis_book_decodevs_add(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int step=n/book->dim;
    size_t mark=b->workspace->used;
    long *entry = hv_scratch_allocate(b->workspace,sizeof(*entry)*step);
    float **t = hv_scratch_allocate(b->workspace,sizeof(*t)*step);
    int i,j,o;

    for (i = 0; i < step; i++) {
      entry[i]=decode_packed_entry_number(book,b);
      if(entry[i]==-1){ b->workspace->used=mark; return(-1); }
      t[i] = book->valuelist+entry[i]*book->dim;
    }
    for(i=0,o=0;i<book->dim;i++,o+=step)
      for (j=0;o+j<n && j<step;j++)
        a[o+j]+=t[j][i];
    b->workspace->used=mark;
  }
  return(0);
}

/* decode vector / dim granularity gaurding is done in the upper layer */
long vorbis_book_decodev_add(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int i,j,entry;
    float *t;

    if(book->dim==2 || book->dim==4){
      const int dim=book->dim;

      i=0;
      while(i<=n-2*dim && b->endbyte<=b->storage-8) {
        unsigned long long window=0;
        unsigned used=0;
        const unsigned mask=(1u<<book->dec_firsttablen)-1u;
        const int begin=i;
        const unsigned char *p=b->ptr;
        for(unsigned k=0;k<8;k++) window|=((unsigned long long)p[k])<<(8*k);
        window>>=b->endbit;
        while(i<=n-dim && used+book->dec_firsttablen<=56) {
          const ogg_uint32_t word=book->dec_firsttable[(unsigned)window & mask];
          if(word&0x80000000u)break;
          const unsigned length=word&15;
          window>>=length;
          used+=length;
          t=book->valuelist+(word>>4)*dim;
          a[i]+=t[0];a[i+1]+=t[1];
          if(dim==4){a[i+2]+=t[2];a[i+3]+=t[3];}
          i+=dim;
        }
        if(i==begin) {
          entry=decode_packed_entry_number(book,b);
          if(entry==-1)return -1;
          t=book->valuelist+entry*dim;
          a[i]+=t[0];a[i+1]+=t[1];
          if(dim==4){a[i+2]+=t[2];a[i+3]+=t[3];}
          i+=dim;
        }else{
          const unsigned next=b->endbit+used;
          b->ptr+=next>>3;b->endbyte+=next>>3;b->endbit=next&7;
        }
      }
      for(;i<=n-dim;i+=dim){
        entry=decode_packed_entry_number(book,b);
        if(entry==-1)return -1;
        t=book->valuelist+entry*dim;
        a[i]+=t[0];
        a[i+1]+=t[1];
        if(dim==4){ a[i+2]+=t[2]; a[i+3]+=t[3]; }
      }
    }else i=0;
    for(;i<n;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      t     = book->valuelist+entry*book->dim;
      for(j=0;i<n && j<book->dim;)
        a[i++]+=t[j++];
    }
  }
  return(0);
}

/* unlike the others, we guard against n not being an integer number
   of <dim> internally rather than in the upper layer (called only by
   floor0) */
long vorbis_book_decodev_set(codebook *book,float *a,oggpack_buffer *b,int n){
  if(book->used_entries>0){
    int i,j,entry;
    float *t;

    for(i=0;i<n;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      t     = book->valuelist+entry*book->dim;
      for (j=0;i<n && j<book->dim;){
        a[i++]=t[j++];
      }
    }
  }else{
    int i;

    for(i=0;i<n;){
      a[i++]=0.f;
    }
  }
  return(0);
}


/* A complete stereo vector alternates left and right values. */
STIN void hv_add_stereo(float* left,float* right,const float* values,int frames){
  for(int frame=0;frame<frames;frame++){
    left[frame]+=values[frame*2];
    right[frame]+=values[frame*2+1];
  }
}
STIN long hv_decode_stereo(codebook* book,float** planes,long start,long end,
                           oggpack_buffer* reader,int framesPerEntry){
  const unsigned mask=(1u<<book->dec_firsttablen)-1u;
  long frame=start;
  while(frame<=end-framesPerEntry && reader->endbyte<=reader->storage-8){
    const unsigned char* source=reader->ptr;
    unsigned long long window=0;
    for(unsigned k=0;k<8;k++)window|=((unsigned long long)source[k])<<(8*k);
    window>>=reader->endbit;
    unsigned consumed=0;
    while(frame<=end-framesPerEntry && consumed+book->dec_firsttablen<=56){
      const ogg_uint32_t word=book->dec_firsttable[(unsigned)window&mask];
      if(word&0x80000000u)break;
      const unsigned length=word&15u;
      hv_add_stereo(planes[0]+frame,planes[1]+frame,
                    book->valuelist+(word>>4)*2u*framesPerEntry,framesPerEntry);
      window>>=length;
      consumed+=length;
      frame+=framesPerEntry;
    }
    if(consumed){
      const unsigned next=reader->endbit+consumed;
      reader->ptr+=next>>3;
      reader->endbyte+=next>>3;
      reader->endbit=next&7;
      continue;
    }
    const long entry=decode_packed_entry_number(book,reader);
    if(entry<0)return -1;
    hv_add_stereo(planes[0]+frame,planes[1]+frame,
                  book->valuelist+entry*2*framesPerEntry,framesPerEntry);
    frame+=framesPerEntry;
  }
  while(frame<end){
    const long entry=decode_packed_entry_number(book,reader);
    if(entry<0)return -1;
    const int frames=end-frame<framesPerEntry?(int)(end-frame):framesPerEntry;
    hv_add_stereo(planes[0]+frame,planes[1]+frame,
                  book->valuelist+entry*2*framesPerEntry,frames);
    frame+=frames;
  }
  return 0;
}
long vorbis_book_decodevv_add(codebook *book,float **a,long offset,int ch,
                              oggpack_buffer *b,int n){

  long i,j,entry;
  int chptr=0;
  if(book->used_entries>0){
    int m=(offset+n)/ch;
    if(ch==2){
      switch(book->dim){
        case 2: return hv_decode_stereo(book,a,offset/2,m,b,1);
        case 4: return hv_decode_stereo(book,a,offset/2,m,b,2);
        case 8: return hv_decode_stereo(book,a,offset/2,m,b,4);
        case 16: return hv_decode_stereo(book,a,offset/2,m,b,8);
      }
    }
    for(i=offset/ch;i<m;){
      entry = decode_packed_entry_number(book,b);
      if(entry==-1)return(-1);
      {
        const float *t = book->valuelist+entry*book->dim;
        for (j=0;i<m && j<book->dim;j++){
          a[chptr++][i]+=t[j];
          if(chptr==ch){
            chptr=0;
            i++;
          }
        }
      }
    }
  }
  return(0);
}
