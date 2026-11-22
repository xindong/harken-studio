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

 function: maintain the info structure, info <-> header packets

 ********************************************************************/

/* general handling of the header and the vorbis_info structure (and
   substructures) */

#include <stdlib.h>
#include <string.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codec_internal.h"
#include "codebook.h"
#include "registry.h"
#include "window.h"
#include "psy.h"
#include "misc.h"
#include "os.h"

#define GENERAL_VENDOR_STRING "Xiph.Org libVorbis 1.3.7"
#define ENCODE_VENDOR_STRING "Xiph.Org libVorbis I 20200704 (Reducing Environment)"

/* helpers */


static void _v_readstring(oggpack_buffer *o,char *buf,int bytes){
  while(bytes--){
    *buf++=oggpack_read(o,8);
  }
}



void vorbis_comment_init(vorbis_comment *vc){
  memset(vc,0,sizeof(*vc));
}





/* This is more or less the same as strncasecmp - but that doesn't exist
 * everywhere, and this is a fairly trivial function, so we include it */






void vorbis_comment_clear(HvAllocator *allocator, vorbis_comment *vc){
  if(vc){
    long i;
    if(vc->user_comments){
      for(i=0;i<vc->comments;i++)
        if(vc->user_comments[i])hv_free(allocator, vc->user_comments[i]);
      hv_free(allocator, vc->user_comments);
    }
    if(vc->comment_lengths)hv_free(allocator, vc->comment_lengths);
    if(vc->vendor)hv_free(allocator, vc->vendor);
    memset(vc,0,sizeof(*vc));
  }
}

/* blocksize 0 is guaranteed to be short, 1 is guaranteed to be long.
   They may be equal, but short will never ge greater than long */
int vorbis_info_blocksize(vorbis_info *vi,int zo){
  codec_setup_info *ci = vi->codec_setup;
  return ci ? ci->blocksizes[zo] : -1;
}

/* used by synthesis, which has a full, alloced vi */
void vorbis_info_init(HvAllocator *allocator, vorbis_info *vi){
  memset(vi,0,sizeof(*vi));
  vi->codec_setup=hv_callocate(allocator, 1,sizeof(codec_setup_info));
}

void vorbis_info_clear(HvAllocator *allocator, vorbis_info *vi){
  codec_setup_info     *ci=vi->codec_setup;
  int i;

  if(ci){

    for(i=0;i<ci->modes;i++)
      if(ci->mode_param[i])hv_free(allocator, ci->mode_param[i]);

    for(i=0;i<ci->maps;i++) /* unpack does the range checking */
      if(ci->map_param[i]) /* this may be cleaning up an aborted
                              unpack, in which case the below type
                              cannot be trusted */
        _mapping_P[ci->map_type[i]]->free_info(allocator, ci->map_param[i]);

    for(i=0;i<ci->floors;i++) /* unpack does the range checking */
      if(ci->floor_param[i]) /* this may be cleaning up an aborted
                                unpack, in which case the below type
                                cannot be trusted */
        _floor_P[ci->floor_type[i]]->free_info(allocator, ci->floor_param[i]);

    for(i=0;i<ci->residues;i++) /* unpack does the range checking */
      if(ci->residue_param[i]) /* this may be cleaning up an aborted
                                  unpack, in which case the below type
                                  cannot be trusted */
        _residue_P[ci->residue_type[i]]->free_info(allocator, ci->residue_param[i]);

    for(i=0;i<ci->books;i++){
      if(ci->book_param[i]){
        /* knows if the book was not alloced */
        vorbis_staticbook_destroy(allocator, ci->book_param[i]);
      }
      if(ci->fullbooks)
        vorbis_book_clear(allocator, ci->fullbooks+i);
    }
    if(ci->fullbooks)
        hv_free(allocator, ci->fullbooks);



    hv_free(allocator, ci);
  }

  memset(vi,0,sizeof(*vi));
}

/* Header packing/unpacking ********************************************/

static int _vorbis_unpack_info(HvAllocator *allocator, vorbis_info *vi,oggpack_buffer *opb){
  codec_setup_info     *ci=vi->codec_setup;
  int bs;
  if(!ci)return(OV_EFAULT);

  vi->version=oggpack_read(opb,32);
  if(vi->version!=0)return(OV_EVERSION);

  vi->channels=oggpack_read(opb,8);
  vi->rate=oggpack_read(opb,32);

  vi->bitrate_upper=(ogg_int32_t)oggpack_read(opb,32);
  vi->bitrate_nominal=(ogg_int32_t)oggpack_read(opb,32);
  vi->bitrate_lower=(ogg_int32_t)oggpack_read(opb,32);

  bs = oggpack_read(opb,4);
  if(bs<0)goto err_out;
  ci->blocksizes[0]=1<<bs;
  bs = oggpack_read(opb,4);
  if(bs<0)goto err_out;
  ci->blocksizes[1]=1<<bs;

  if(vi->rate<1)goto err_out;
  if(vi->channels<1)goto err_out;
  if(ci->blocksizes[0]<64)goto err_out;
  if(ci->blocksizes[1]<ci->blocksizes[0])goto err_out;
  if(ci->blocksizes[1]>8192)goto err_out;

  if(oggpack_read(opb,1)!=1)goto err_out; /* EOP check */

  return(0);
 err_out:
  vorbis_info_clear(allocator, vi);
  return(OV_EBADHEADER);
}

static int _vorbis_unpack_comment(HvAllocator *allocator, vorbis_comment *vc,oggpack_buffer *opb){
  int i;
  int vendorlen=oggpack_read(opb,32);
  if(vendorlen<0)goto err_out;
  if(vendorlen>opb->storage-8)goto err_out;
  vc->vendor=hv_callocate(allocator, vendorlen+1,1);
  _v_readstring(opb,vc->vendor,vendorlen);
  i=oggpack_read(opb,32);
  if(i<0)goto err_out;
  if(i>((opb->storage-oggpack_bytes(opb))>>2))goto err_out;
  vc->comments=i;
  vc->user_comments=hv_callocate(allocator, vc->comments+1,sizeof(*vc->user_comments));
  vc->comment_lengths=hv_callocate(allocator, vc->comments+1, sizeof(*vc->comment_lengths));

  for(i=0;i<vc->comments;i++){
    int len=oggpack_read(opb,32);
    if(len<0)goto err_out;
    if(len>opb->storage-oggpack_bytes(opb))goto err_out;
    vc->comment_lengths[i]=len;
    vc->user_comments[i]=hv_callocate(allocator, len+1,1);
    _v_readstring(opb,vc->user_comments[i],len);
  }
  if(oggpack_read(opb,1)!=1)goto err_out; /* EOP check */

  return(0);
 err_out:
  vorbis_comment_clear(allocator, vc);
  return(OV_EBADHEADER);
}

/* all of the real encoding details are here.  The modes, books,
   everything */
static int _vorbis_unpack_books(HvAllocator *allocator, vorbis_info *vi,oggpack_buffer *opb){
  codec_setup_info     *ci=vi->codec_setup;
  int i;

  /* codebooks */
  ci->books=oggpack_read(opb,8)+1;
  if(ci->books<=0)goto err_out;
  for(i=0;i<ci->books;i++){
    ci->book_param[i]=vorbis_staticbook_unpack(allocator, opb);
    if(!ci->book_param[i])goto err_out;
  }

  /* time backend settings; hooks are unused */
  {
    int times=oggpack_read(opb,6)+1;
    if(times<=0)goto err_out;
    for(i=0;i<times;i++){
      int test=oggpack_read(opb,16);
      if(test<0 || test>=VI_TIMEB)goto err_out;
    }
  }

  /* floor backend settings */
  ci->floors=oggpack_read(opb,6)+1;
  if(ci->floors<=0)goto err_out;
  for(i=0;i<ci->floors;i++){
    ci->floor_type[i]=oggpack_read(opb,16);
    if(ci->floor_type[i]!=1){ allocator->unsupported=1; goto err_out; }
    ci->floor_param[i]=_floor_P[ci->floor_type[i]]->unpack(allocator, vi,opb);
    if(!ci->floor_param[i])goto err_out;
  }

  /* residue backend settings */
  ci->residues=oggpack_read(opb,6)+1;
  if(ci->residues<=0)goto err_out;
  for(i=0;i<ci->residues;i++){
    ci->residue_type[i]=oggpack_read(opb,16);
    if(ci->residue_type[i]<0 || ci->residue_type[i]>=VI_RESB)goto err_out;
    ci->residue_param[i]=_residue_P[ci->residue_type[i]]->unpack(allocator, vi,opb);
    if(!ci->residue_param[i])goto err_out;
  }

  /* map backend settings */
  ci->maps=oggpack_read(opb,6)+1;
  if(ci->maps<=0)goto err_out;
  for(i=0;i<ci->maps;i++){
    ci->map_type[i]=oggpack_read(opb,16);
    if(ci->map_type[i]<0 || ci->map_type[i]>=VI_MAPB)goto err_out;
    ci->map_param[i]=_mapping_P[ci->map_type[i]]->unpack(allocator, vi,opb);
    if(!ci->map_param[i])goto err_out;
  }

  /* mode settings */
  ci->modes=oggpack_read(opb,6)+1;
  if(ci->modes<=0)goto err_out;
  for(i=0;i<ci->modes;i++){
    ci->mode_param[i]=hv_callocate(allocator, 1,sizeof(*ci->mode_param[i]));
    ci->mode_param[i]->blockflag=oggpack_read(opb,1);
    ci->mode_param[i]->windowtype=oggpack_read(opb,16);
    ci->mode_param[i]->transformtype=oggpack_read(opb,16);
    ci->mode_param[i]->mapping=oggpack_read(opb,8);

    if(ci->mode_param[i]->windowtype>=VI_WINDOWB)goto err_out;
    if(ci->mode_param[i]->transformtype>=VI_WINDOWB)goto err_out;
    if(ci->mode_param[i]->mapping>=ci->maps)goto err_out;
    if(ci->mode_param[i]->mapping<0)goto err_out;
  }

  if(oggpack_read(opb,1)!=1)goto err_out; /* top level EOP check */

  return(0);
 err_out:
  vorbis_info_clear(allocator, vi);
  return(OV_EBADHEADER);
}

/* Is this packet a vorbis ID header? */
int vorbis_synthesis_idheader(ogg_packet *op){
  oggpack_buffer opb;
  char buffer[6];

  if(op){
    oggpack_readinit(&opb,op->packet,op->bytes);

    if(!op->b_o_s)
      return(0); /* Not the initial packet */

    if(oggpack_read(&opb,8) != 1)
      return 0; /* not an ID header */

    memset(buffer,0,6);
    _v_readstring(&opb,buffer,6);
    if(memcmp(buffer,"vorbis",6))
      return 0; /* not vorbis */

    return 1;
  }

  return 0;
}

/* The Vorbis header is in three packets; the initial small packet in
   the first page that identifies basic parameters, a second packet
   with bitstream comments and a third packet that holds the
   codebook. */

int vorbis_synthesis_headerin(HvAllocator *allocator, vorbis_info *vi,vorbis_comment *vc,ogg_packet *op){
  oggpack_buffer opb;

  if(op){
    oggpack_readinit(&opb,op->packet,op->bytes);

    /* Which of the three types of header is this? */
    /* Also verify header-ness, vorbis */
    {
      char buffer[6];
      int packtype=oggpack_read(&opb,8);
      memset(buffer,0,6);
      _v_readstring(&opb,buffer,6);
      if(memcmp(buffer,"vorbis",6)){
        /* not a vorbis header */
        return(OV_ENOTVORBIS);
      }
      switch(packtype){
      case 0x01: /* least significant *bit* is read first */
        if(!op->b_o_s){
          /* Not the initial packet */
          return(OV_EBADHEADER);
        }
        if(vi->rate!=0){
          /* previously initialized info header */
          return(OV_EBADHEADER);
        }

        return(_vorbis_unpack_info(allocator, vi,&opb));

      case 0x03: /* least significant *bit* is read first */
        if(vi->rate==0){
          /* um... we didn't get the initial header */
          return(OV_EBADHEADER);
        }
        if(vc->vendor!=NULL){
          /* previously initialized comment header */
          return(OV_EBADHEADER);
        }

        return(_vorbis_unpack_comment(allocator, vc,&opb));

      case 0x05: /* least significant *bit* is read first */
        if(vi->rate==0 || vc->vendor==NULL){
          /* um... we didn;t get the initial header or comments yet */
          return(OV_EBADHEADER);
        }
        if(vi->codec_setup==NULL){
          /* improperly initialized vorbis_info */
          return(OV_EFAULT);
        }
        if(((codec_setup_info *)vi->codec_setup)->books>0){
          /* previously initialized setup header */
          return(OV_EBADHEADER);
        }

        return(_vorbis_unpack_books(allocator, vi,&opb));

      default:
        /* Not a valid vorbis header type */
        return(OV_EBADHEADER);
        break;
      }
    }
  }
  return(OV_EBADHEADER);
}

/* pack side **********************************************************/

