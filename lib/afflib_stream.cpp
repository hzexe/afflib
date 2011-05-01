/*
 * The AFFLIB data stream interface.
 * Supports the page->segment name translation, and the actual file pointer.
 */

/*
 * Copyright (c) 2005, 2006
 *	Simson L. Garfinkel and Basis Technology, Inc. 
 *      All rights reserved.
 *
 * This code is derrived from software contributed by
 * Simson L. Garfinkel
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Simson L. Garfinkel
 *    and Basis Technology Corp.
 * 4. Neither the name of Simson Garfinkel, Basis Technology, or other
 *    contributors to this program may be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY SIMSON GARFINKEL, BASIS TECHNOLOGY,
 * AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL SIMSON GARFINKEL, BAIS TECHNOLOGy,
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.  
 */


#include "affconfig.h"
#include "afflib.h"
#include "afflib_i.h"


/****************************************************************
 *** Internal Functions.
 ****************************************************************/

/*
 * af_set_maxsize
 * Sets the maxsize.
 Fails with -1 if imagesize >= 0 unless this is a raw or split_raw file
 */
int af_set_maxsize(AFFILE *af,int64_t maxsize)
{
    AF_WRLOCK(af);
    if(af->image_size>0){
	(*af->error_reporter)("Cannot set maxsize as imagesize is already set (%"I64d")",af->image_size);
	AF_UNLOCK(af);
	return -1;	// now allowed to set if imagesize is bigger than 0
    }
    if((af->image_pagesize!=0)
       && (af->v->type & AF_VNODE_MAXSIZE_MULTIPLE)
       && (maxsize % af->image_pagesize != 0)){
	(*af->error_reporter)("Cannot set maxsize to %"I64d" --- not multiple of pagesize=%d\n",
			      maxsize,af->image_pagesize);
	AF_UNLOCK(af);
	return -1;
    }
    af->maxsize = maxsize;
    AF_UNLOCK(af);
    return 0;
}

const unsigned char *af_badflag(AFFILE *af)
{
    return af->badflag;
}


/****************************************************************
 *** Stream-level interface
 ****************************************************************/


/* Throw out the current segment */
int af_purge(AFFILE *af)
{
    AF_WRLOCK(af);
    if (af_trace) fprintf(af_trace,"af_purge(%p)\n",af);
    int ret = af_cache_flush(af);	// flush the cache
    af->pb = 0;				// no longer have a current page
    AF_UNLOCK(af);
    return ret;
}

ssize_t af_read(AFFILE *af,unsigned char *buf,ssize_t count)
{
    int total = 0;

    AF_WRLOCK(af);			// wrlock because cache may change
    if (af_trace) fprintf(af_trace,"af_read(%p,%p,%zd) (pos=%"I64d")\n",af,buf,count,af->pos);
    if (af->v->read){			// check for bypass
	int r = (af->v->read)(af, buf, af->pos, count);
	if(r>0) af->pos += r;
	AF_UNLOCK(af);
	return r;
    }

    /* performance improvement: use af->image_size if it is set */
    uint64_t offset = af->pos;		/* where to start */

    if(af->image_size<0)  {total=-1;goto done;}	// error
    if(af->image_size==0) {goto done;}		// no data in file
    if(af->pos > af->image_size) {goto done;}	// seeked beyond end of file
    if(af->pos+count > af->image_size) count = af->image_size - af->pos; // only this much left in file


    /* Make sure we have a pagebuf if none was defined */
    if(af->image_pagesize==0){		// page size not defined
	errno = EFAULT;
	total=-1;
	goto done;
    }

    while(count>0){
	/* If the correct segment is not loaded, purge the segment */
	int64_t new_page = offset / af->image_pagesize;

	if(af->pb==0 || new_page != af->pb->pagenum){
	    af_cache_flush(af);
	    af->pb = 0;
	}

	/* If no segment is loaded in cache, load the current segment */
	if(af->pb==0){
	    int64_t pagenum = offset / af->image_pagesize;
	    af->pb = af_cache_alloc(af,pagenum);
	    if(af->pb->pagebuf_valid==0){ 
		/* page buffer isn't valid; need to get it */
		af->pb->pagebuf_bytes = af->image_pagesize;		// we can hold this much
		if(af_get_page(af,af->pb->pagenum,af->pb->pagebuf, &af->pb->pagebuf_bytes)){
		    /* Page doesn't exist; fill with NULs */
		    memset(af->pb->pagebuf,0,af->pb->pagebuf_bytes);
		    /* TK: Should fill with BADBLOCK here if desired */
		    /* previously had BREAK here */
		}
		af->pb->pagebuf_valid = 1;	// contents of the page buffer are valid
	    }
	}
	// Compute how many bytes can be copied...
	// where we were reading from
	u_int page_offset   = (u_int)(offset - af->pb->pagenum * af->image_pagesize); 

	if(page_offset > af->pb->pagebuf_bytes){
	    /* Page is short. */
	    /* Question - should we advance af->pos to the next page? */
	    break;
	}

	u_int page_left     = af->pb->pagebuf_bytes - page_offset; // number we can get out
	u_int bytes_to_read = count;

	if(bytes_to_read > page_left)               bytes_to_read = page_left;
	if(bytes_to_read > af->image_size - offset) bytes_to_read = (u_int)(af->image_size - offset);

	assert(bytes_to_read >= 0);	// 
	if(bytes_to_read==0) break; // that's all we could get

	/* Copy out the bytes for the user */
	memcpy(buf,af->pb->pagebuf+page_offset,bytes_to_read); // copy out
	af->bytes_memcpy += bytes_to_read;
	buf     += bytes_to_read;
	offset  += bytes_to_read;
	count   -= bytes_to_read;
	total   += bytes_to_read;
	af->pos += bytes_to_read;
    }
    /* We have copied all of the user's requested data, so return */
 done:
    AF_UNLOCK(af);
    return total;
}


/*
 * Handle writing to the file...
 * af_write() --- returns the number of bytes written
 *
 */

int af_write(AFFILE *af,unsigned char *buf,size_t count)
{
    AF_WRLOCK(af);
    if (af_trace){
	fprintf(af_trace,"af_write(af=%p,buf=%p,count=%zd) pos=%"I64d"\n", af,buf,count,af->pos);
    }
    /* Invalidate caches */
    af_invalidate_vni_cache(af);

    /* vnode write bypass:
     * If a write function is defined, use it and avoid the page and cache business. 
     */
    if (af->v->write){		
	int r = (af->v->write)(af, buf, af->pos, count);
	if(r>0){
	    af->pos += r;
	    af->bytes_written += r;
	}
	if(af->pos >= af->image_size) af->image_size = af->pos;
	AF_UNLOCK(af);
	return r;
    }

    /* If no pagesize has been set, go with the default pagesize */
    if(af->image_pagesize==0){
	if(af_set_pagesize(af,AFF_DEFAULT_PAGESIZE)){
	    AF_UNLOCK(af);
	    return -1;
	}
    }

    int64_t offset = af->pos;		// where to start

    /* If the correct segment is not loaded, purge the current segment */
    int64_t write_page = offset / af->image_pagesize;
    if(af->pb && af->pb->pagenum!=write_page){
	af_cache_flush(af);
	af->pb = 0;
    }

    int write_page_offset = (int)(offset % af->image_pagesize);

    /* Page Write Bypass:
     * If no data has been written into the current page buffer,
     * and if the position of the stream is byte-aligned on the page buffer,
     * and if an entire page is being written,
     * just write it out and update the pointers, then return.
     */
    if(af->pb==0 && af->image_pagesize==(unsigned)count && write_page_offset == 0){
	// copy into cache if we have this page anywhere in our cache
	af_cache_writethrough(af,write_page,buf,count);
	int ret = af_update_page(af,write_page,buf,count);
	if(ret==0){			// no error
	    af->pos += count;
	    if(af->pos > af->image_size) af->image_size = af->pos;
	    AF_UNLOCK(af);
	    return count;
	}
	AF_UNLOCK(af);
	return -1;			// error
    }
       

    /* Can't use high-speed optimization; write through the cache */
    int total = 0;
    while(count>0){
	/* If no page is loaded, or the wrong page is loaded, load the correct page */
	int64_t pagenum = offset / af->image_pagesize;	// will be the segment we want
	if(af->pb==0 || af->pb->pagenum != pagenum){
	    af->pb = af_cache_alloc(af,pagenum);
	    af->pb->pagebuf_bytes = af->image_pagesize;
	    assert(af->pb->pagenum == pagenum);

	    /* Now try to load the page.
	     * If we can't load it, then we are creating a new page.
	     */
	    if(af_get_page(af,af->pb->pagenum,af->pb->pagebuf, &af->pb->pagebuf_bytes)){
		/* Creating a new page; note that we have no bytes in this page */
		af->pb->pagebuf_bytes = 0;
	    }
	}
	// where writing to
	u_int seg_offset = (u_int)(offset - af->pb->pagenum * af->image_pagesize); 

	// number we can write into
	u_int seg_left   = af->image_pagesize - seg_offset; 
	u_int bytes_to_write = count;

	if(bytes_to_write > seg_left) bytes_to_write = seg_left;

	assert(bytes_to_write >= 0);	// 
	if(bytes_to_write==0) break; // that's all we could get

	/* Copy out the bytes for the user */
	memcpy(af->pb->pagebuf+seg_offset,buf,bytes_to_write); // copy into the page cache
	af->bytes_memcpy += bytes_to_write;

	if(af->pb->pagebuf_bytes < seg_offset+bytes_to_write){
	    af->pb->pagebuf_bytes = seg_offset+bytes_to_write; // it has been extended.
	}

	buf     += bytes_to_write;
	offset  += bytes_to_write;
	count   -= bytes_to_write;
	total   += bytes_to_write;
	af->pos += bytes_to_write;
	af->pb->pagebuf_valid = 1;
	af->pb->pagebuf_dirty = 1;

	/* If we wrote out all of the bytes that were left in the segment,
	 * then we are at the end of the segment, write it back...
	 */
	if(seg_left == bytes_to_write){	
	    if(af_cache_flush(af)){
		AF_UNLOCK(af);
		return -1;
	    }
	}

	/* If we have written more than the image size, update the image size */
	if((uint64_t)offset > af->image_size) af->image_size = offset;
    }
    /* We have copied all of the user's requested data, so return */
    AF_UNLOCK(af);
    return total;
}

/* No lock needed? */
int af_is_badsector(AFFILE *af,const unsigned char *buf)
{
    if(af->badflag_set==0) return 0;
    if(af->badflag==0) return 0;
    return memcmp(af->badflag,buf,af->image_sectorsize)==0;
}
