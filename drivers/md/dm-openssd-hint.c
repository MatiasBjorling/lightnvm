#include "dm-openssd.h"
#include "dm-openssd-hint.h"

// TODO make inline?
static unsigned long diff_tv(struct timeval *curr_tv, struct timeval *ap_tv)
{
	if(curr_tv->tv_sec == ap_tv->tv_sec)
		return curr_tv->tv_usec - ap_tv->tv_usec;

	return (curr_tv->tv_sec - ap_tv->tv_sec -1) * 1000000 + (1000000-ap_tv->tv_usec) + curr_tv->tv_usec;
}

void openssd_delay_endio_hint(struct openssd *os, struct bio *bio,
                              struct per_bio_data *pb, unsigned long *delay)
{
	int page_id;

	if (!(os->config.flags & NVM_OPT_ENGINE_SWAP))
		return;

	if (bio_data_dir(bio) != WRITE)
		return;

	page_id = (pb->physical_addr / NR_HOST_PAGES_IN_FLASH_PAGE)
	          % os->nr_pages_per_blk;

	/* different timings, roughly based on "Harey Tortoise" paper
	 * TODO: ratio is actually 4.8 on average
	 * TODO: consider dev_wait to be part of per_bio_data? */
	if (page_is_fast(page_id, os))
		(*delay) = os->config.t_write / 2;
	else
		(*delay) = os->config.t_write * 2;
}

void *openssd_begin_gc_hint(sector_t l_addr, sector_t p_addr, struct
                            nvm_block *block)
{
	struct openssd_hint_map_private *private =
	        kzalloc(sizeof(struct openssd_hint_map_private), GFP_NOIO);

	private->old_p_addr = p_addr;

	return private;
}

void openssd_end_gc_hint(void *private)
{
	kfree(private);
}

// iterate hints list, and check if lba of current req is covered by some hint
hint_info_t* openssd_find_hint(struct openssd *os, sector_t logical_addr, bool is_write)
{
	struct openssd_hint *hint = os->hint_private;
	hint_info_t *hint_info;
	struct list_head *node;

	//DMINFO("find hint for lba %ld is_write %d", logical_addr, is_write);
	spin_lock(&hint->hintlock);
	/*see if hint is already in list*/
	list_for_each(node, &hint->hintlist) {
		hint_info = list_entry(node, hint_info_t, list_member);
		//DMINFO("hint start_lba=%d count=%d", hint_info->hint.start_lba, hint_info->hint.count);
		//continue;
		/* verify lba covered by hint*/
		if (is_hint_relevant(logical_addr, hint_info, is_write, os->config.flags)) {
			DMDEBUG("found hint for lba %ld (ino %ld)",
				logical_addr, hint_info->hint.ino);
			hint_info->processed++;
			spin_unlock(&hint->hintlock);
			return hint_info;
		}
	}
	spin_unlock(&hint->hintlock);
	DMDEBUG("no hint found for %s lba %ld", (is_write)?"WRITE":"READ",logical_addr);

	return NULL;
}

fclass file_classify(struct bio_vec* bvec)
{
	fclass fc = FC_UNKNOWN;
	char *sec_in_mem;
	char byte[4];

	if (!bvec || !bvec->bv_page) {
		DMINFO("can't kmap empty bvec->bv_page. kmap failed");
		return fc;
	}

	byte[0] = 0x66;
	byte[1] = 0x74;
	byte[2] = 0x79;
	byte[3] = 0x70;

	sec_in_mem = kmap_atomic((bvec->bv_page) + bvec->bv_offset);

	if (!sec_in_mem) {
		DMERR("bvec->bv_page kmap failed");
		return fc;
	}

	if (!memcmp(sec_in_mem+4, byte,4)) {
		//hint_log("VIDEO classified");
		DMINFO("VIDEO classified");
		fc = FC_VIDEO_SLOW;
	}

	if(sec_in_mem[0]==0xfffffffe && sec_in_mem[1]==0xfffffffe && sec_in_mem[2]==0x07 && sec_in_mem[3]==0x01){
		DMINFO("identified DB_INDEX file");
		fc = FC_DB_INDEX;
	}

	kunmap_atomic(sec_in_mem);
	return fc;
}

int openssd_is_fc_latency(fclass fc)
{
	if(fc == FC_DB_INDEX)
		return 1;

	return 0;
}

int openssd_is_fc_packable(fclass fc)
{
	if(fc == FC_VIDEO_SLOW)
		return 1;

	return 0;
}

/* no real sending for now, in prototype just put it directly in FTL's hints list
   and update ino_hint map when necessary*/
static int openssd_send_hint(struct openssd *os, hint_data_t *hint_data)
{
	struct openssd_hint *hint = os->hint_private;
	int i;
	hint_info_t* hint_info;

	if (!(os->config.flags &
	      (NVM_OPT_ENGINE_LATENCY | NVM_OPT_ENGINE_SWAP | NVM_OPT_ENGINE_PACK))){
		DMERR("got unsupported hint");
		goto send_done;
	}

	DMDEBUG("first %s hint count=%d lba=%d fc=%d",
	       CAST_TO_PAYLOAD(hint_data)->is_write ? "WRITE" : "READ",
	       CAST_TO_PAYLOAD(hint_data)->count,
	       INO_HINT_FROM_DATA(hint_data, 0).start_lba,
	       INO_HINT_FROM_DATA(hint_data, 0).fc);

	// assert relevant hint support
	if ((CAST_TO_PAYLOAD(hint_data)->hint_flags & HINT_SWAP && !(os->config.flags & NVM_OPT_ENGINE_SWAP)) ||
	    (CAST_TO_PAYLOAD(hint_data)->hint_flags & HINT_LATENCY && !(os->config.flags & NVM_OPT_ENGINE_LATENCY)) ||
	    (CAST_TO_PAYLOAD(hint_data)->hint_flags & HINT_PACK && !(os->config.flags & NVM_OPT_ENGINE_PACK))) {
		DMERR("hint of types %x not supported (1st entry ino %lu lba %u count %u)",
		       CAST_TO_PAYLOAD(hint_data)->hint_flags,
		       INO_HINT_FROM_DATA(hint_data, 0).ino,
		       INO_HINT_FROM_DATA(hint_data, 0).start_lba,
		       INO_HINT_FROM_DATA(hint_data, 0).count);
		goto send_done;
	}

	// insert to hints list
	for(i = 0; i < CAST_TO_PAYLOAD(hint_data)->count; i++) {
		// handle file type  for
		// 1) identified latency writes
		// 2) identified pack writes
		if ((os->config.flags & NVM_OPT_ENGINE_LATENCY || os->config.flags & NVM_OPT_ENGINE_PACK)
		    && INO_HINT_FROM_DATA(hint_data, i).fc != FC_EMPTY) {
			DMINFO("ino %lu got new fc %d", INO_HINT_FROM_DATA(hint_data, i).ino,
			       INO_HINT_FROM_DATA(hint_data, i).fc);
			hint->ino2fc[INO_HINT_FROM_DATA(hint_data, i).ino] = INO_HINT_FROM_DATA(hint_data, 0).fc;
		}

		/* non-packable file. ignore hint*/
		if(os->config.flags & NVM_OPT_ENGINE_PACK && 
		   !openssd_is_fc_packable(hint->ino2fc[INO_HINT_FROM_DATA(hint_data, i).ino])){
			DMDEBUG("non-packable file. ignore hint");
			continue;
		}

		/* non-latency file. ignore hint*/
		if(os->config.flags & NVM_OPT_ENGINE_LATENCY && INO_HINT_FROM_DATA(hint_data, i).fc == FC_EMPTY &&
		   !openssd_is_fc_latency(hint->ino2fc[INO_HINT_FROM_DATA(hint_data, i).ino])){
			DMDEBUG("non-latency file. ignore hint");
			continue;
		}

		// insert to hints list
		hint_info = kmalloc(sizeof(hint_info_t), GFP_KERNEL);
		if (!hint_info) {
			DMERR("can't allocate hint info");
			return -ENOMEM;
		}
		memcpy(&hint_info->hint, &INO_HINT_FROM_DATA(hint_data, i), sizeof(ino_hint_t));
		hint_info->processed  = 0;
		hint_info->is_write   = CAST_TO_PAYLOAD(hint_data)->is_write;
		hint_info->hint_flags = CAST_TO_PAYLOAD(hint_data)->hint_flags;

		DMDEBUG("about to add hint_info to list. %s %s",
		       (CAST_TO_PAYLOAD(hint_data)->hint_flags & HINT_SWAP) ? "SWAP" :
		       (CAST_TO_PAYLOAD(hint_data)->hint_flags & HINT_LATENCY)?"LATENCY":"REGULAR",
		       (CAST_TO_PAYLOAD(hint_data)->is_write) ? "WRITE" : "READ");

		spin_lock(&hint->hintlock);
		list_add_tail(&hint_info->list_member, &hint->hintlist);
		spin_unlock(&hint->hintlock);
	}

send_done:
	return 0;
}


/**
 * automatically extract hint from a bio, and send to target.
 * iterate all pages, look into inode. There are several cases:
 * 1) swap - stop and send hint on entire bio (assuming swap LBAs are not mixed with regular LBAs in one bio)
 * 2) read - iterate all pages and send hint_data composed of multiple hints, one for each inode number and
 *           relevant range of LBAs covered by a page
 * 3) write - check if a page is the first sector of a file, classify it and set in hint. rest same as read
 */
void openssd_bio_hint(struct openssd *os, struct bio *bio)
{
	hint_data_t *hint_data;
	fclass fc = FC_EMPTY;
	unsigned ino = -1;
	struct page *bv_page;
	struct address_space *mapping;
	struct inode *host;
	struct bio_vec *bvec;
	uint32_t sector_size = os->sector_size;
	uint32_t sectors_count = 0;
	uint32_t lba = 0, bio_len = 0, hint_idx;
	unsigned long prev_ino = -1, first_sector = -1;
	int i, ret;
	bool is_write = 0;

	return;
	/* can classify only writes*/
	switch(bio_rw(bio)) {
	case READ:
	case READA:
		/* read/readahead*/
		break;
	case WRITE:
		is_write = 1;
		break;
	}

	// get lba and sector count
	lba = bio->bi_sector;
	sectors_count = bio->bi_size / sector_size;

	/* allocate hint_data */
	hint_data = kzalloc(sizeof(hint_data_t), GFP_NOIO);
	if (!hint_data) {
		DMERR("hint_data_t kmalloc failed");
		return;
	}

	CAST_TO_PAYLOAD(hint_data)->lba = lba;
	CAST_TO_PAYLOAD(hint_data)->sectors_count = sectors_count;
	CAST_TO_PAYLOAD(hint_data)->is_write = is_write;
	ino = -1;
	DMDEBUG("%s lba=%d sectors_count=%d",
	       is_write ? "WRITE" : "READ",
	       lba, sectors_count);
#if 0
	hint_log("free hint_data dont look in bvec. simply return");
	kfree(hint_data);
	return;
#endif

	bio_for_each_segment(bvec, bio, i) {
		bv_page = bvec[0].bv_page;

		if (bv_page && !PageSlab(bv_page)) {
			// swap hint
			if (PageSwapCache(bv_page)) {
				DMDEBUG("swap bio");
				// TODO - not tested
				CAST_TO_PAYLOAD(hint_data)->hint_flags |= HINT_SWAP;

				// for compatibility add one hint
				INO_HINT_SET(hint_data, CAST_TO_PAYLOAD(hint_data)->count,
				             0, lba, sectors_count, fc);
				CAST_TO_PAYLOAD(hint_data)->count++;
				break;
			}

			mapping = bv_page->mapping;

			if (mapping && ((unsigned long)mapping & PAGE_MAPPING_ANON) == 0) {
				host = mapping->host;
				if (!host) {
					DMCRIT("page without mapping->host. shouldn't happen");
					bio_len += bvec[0].bv_len;
					continue; // no host
				}

				prev_ino = ino;
				ino = host->i_ino;

				if (!host->i_sb || !host->i_sb->s_type || !host->i_sb->s_type->name) {
					DMDEBUG("not related to file system");
					bio_len += bvec[0].bv_len;
					continue;
				}

				if (!ino) {
					DMDEBUG("not inode related");
					bio_len += bvec[0].bv_len;
					continue;
				}
				//if (bvec[0].bv_offset)
				//   DMINFO("bv_page->index %d offset %d len %d", bv_page->index, bvec[0].bv_offset, bvec[0].bv_len);

				/* classify if we can.
				 * can only classify writes to file's first sector */
				fc = FC_EMPTY;
				if (is_write && bv_page->index == 0 && bvec[0].bv_offset ==0) {
					// should be first sector in file. classify
					first_sector = lba + (bio_len / sector_size);
					fc = file_classify(&bvec[0]);
				}

				/* change previous hint, unless this is a new inode
				   and then simply increment count in existing hint */
				if (prev_ino == ino) {
					hint_idx = CAST_TO_PAYLOAD(hint_data)->count - 1;
					if (INO_HINT_FROM_DATA(hint_data, hint_idx).ino != ino) {
						DMERR("updating hint of wrong ino (ino=%u expected=%lu)", ino,
						      INO_HINT_FROM_DATA(hint_data, hint_idx).ino);
						bio_len += bvec[0].bv_len;
						continue;
					}

					INO_HINT_FROM_DATA(hint_data, hint_idx).count +=
					        bvec[0].bv_len / sector_size;
					DMDEBUG("increase count for hint %u. new count=%u",
					       hint_idx, INO_HINT_FROM_DATA(hint_data, hint_idx).count);
					bio_len+= bvec[0].bv_len;
					continue;
				}

				if (HINT_DATA_MAX_INOS == CAST_TO_PAYLOAD(hint_data)->count) {
					DMERR("too many inos in hint");
					bio_len+= bvec[0].bv_len;
					continue;
				}

				DMDEBUG("add %s hint here - ino=%u lba=%u fc=%s count=%d hint_count=%u",
				       is_write ? "WRITE":"READ",
				       ino,
				       lba + (bio_len / sector_size),
				       (fc == FC_VIDEO_SLOW) ? "VIDEO" : (fc == FC_EMPTY) ? "EMPTY" : "UNKNOWN",
				       bvec[0].bv_len / sector_size,
				       CAST_TO_PAYLOAD(hint_data)->count+1);

				// add new hint to hint_data. lba count=bvec[0].bv_len / sector_size, will add more later on
				INO_HINT_SET(hint_data, CAST_TO_PAYLOAD(hint_data)->count,
				             ino, lba + (bio_len / sector_size), bvec[0].bv_len / sector_size, fc);
				CAST_TO_PAYLOAD(hint_data)->count++;
			}
		}

		// increment len
		bio_len += bvec[0].bv_len;
	}
#if 0
	// TESTING
	// dont send hints yet. just print whatever we got, and free
	hint_log("send nothing free hint_data and simply return.");
	kfree(hint_data);
	hint_log("return");
	return;
#endif
	// hint empty - return.
	// Note: not error, maybe we're not doing file-related/swap I/O
	if (CAST_TO_PAYLOAD(hint_data)->count == 0) {
		//hint_log("request with no file data");
		goto done;
	}

	/* non-empty hint_data, send to device */
	//hint_log("hint count=%u. send to hint device", CAST_TO_PAYLOAD(hint_data)->count);
	ret = openssd_send_hint(os, hint_data);

	if (ret != 0)
		DMERR("openssd_send_hint error %d", ret);

done:
	kfree(hint_data);
}

static int openssd_read_bio_hint(struct openssd *os, struct bio *bio)
{
	return openssd_read_bio_generic(os, bio);
}

static void openssd_trim_map_shadow(struct openssd *os, sector_t l_addr);

static int openssd_write_bio_hint(struct openssd *os, struct bio *bio)
{
	struct openssd_hint *hint = os->hint_private;
	sector_t l_addr;
	int i;
	unsigned int numCopies = 1;
	struct openssd_hint_map_private map_alloc_data;

	map_alloc_data.old_p_addr = LTOP_EMPTY;
	map_alloc_data.flags = MAP_PRIMARY;

	//openssd_bio_hint(os, bio);

	l_addr = bio->bi_sector / NR_PHY_IN_LOG;
	map_alloc_data.hint_info = openssd_find_hint(os, l_addr, 1);

	if (map_alloc_data.hint_info && map_alloc_data.hint_info->hint_flags & HINT_LATENCY)
		numCopies = 2;

	/* Submit bio for all physical addresses*/
	DMDEBUG("logical_addr %llu numCopies=%u", (unsigned long long)l_addr, numCopies);
	for(i = 0; i < numCopies; i++) {
		openssd_write_execute_bio(os, bio, 0, &map_alloc_data);

		/* primary updated. trim old shadow */
		if(os->config.flags & NVM_OPT_ENGINE_LATENCY && i == 0)
			openssd_trim_map_shadow(os, l_addr);

		map_alloc_data.flags = MAP_SHADOW;
	}

	/* Processed entire hint */
	if (map_alloc_data.hint_info) {
		spin_lock(&hint->hintlock);
		if (map_alloc_data.hint_info->processed == map_alloc_data.hint_info->hint.count) {
			list_del(&map_alloc_data.hint_info->list_member);
			kfree(map_alloc_data.hint_info);
		}
		spin_unlock(&hint->hintlock);
	}

	bio_endio(bio, 0);
	return DM_MAPIO_SUBMITTED;
}

void openssd_alloc_phys_addr_pack(struct openssd *os, struct nvm_block *block)
{
	/* pack ap's need an ap not related to any inode*/ 
	if (block_is_full(block)){
		DMDEBUG("__openssd_alloc_phys_addr - block is full. init ap_hint. block->parent_ap %p", block->ap);
		BUG_ON(!block->ap);
		if(block->ap->hint_private)
			init_ap_hint(block->ap);
		block->ap = NULL;
	}
}

sector_t openssd_alloc_phys_pack_addr(struct openssd *os, struct
		nvm_block **ret_victim_block, struct openssd_hint_map_private *map_alloc_data)
{
	struct nvm_ap *ap;
	sector_t addr = LTOP_EMPTY;
	struct openssd_ap_hint* ap_pack_data = NULL;
	struct timeval curr_tv;
	unsigned long diff;
	int i;

	/* find open ap for requested inode number*/
	for (i = 0, ap = &os->aps[0]; i < os->nr_pools; i++, ap = &os->aps[i]){
		/* not hint related */
		if(!ap->hint_private)
			continue;

		ap_pack_data = ap->hint_private;

		/* got it */
		if(ap_pack_data->ino == map_alloc_data->hint_info->hint.ino){
			DMDEBUG("ap with block_addr %ld associated to requested inode %d", block_to_addr(ap->cur), ap_pack_data->ino);
			addr = openssd_alloc_addr_from_ap(ap, ret_victim_block, 0);
			break;
		}
	}

	if (addr != LTOP_EMPTY){
		DMDEBUG("allocated addr %ld from PREVIOUS associated ap ", addr);
		goto pack_alloc_done;
	}

	/* no ap associated to requested inode.
	   find some empty pack ap, and use it*/
	DMDEBUG("no ap associated to inode %lu", map_alloc_data->hint_info->hint.ino);
	for (i = 0; addr == LTOP_EMPTY && i < os->nr_pools; i++) {
		ap = get_next_ap(os);

		/* not hint associated */
		if(!ap->hint_private)
			continue;

		ap_pack_data = (struct openssd_ap_hint*)ap->hint_private;

		/* associated to an other inode */
		if(ap_pack_data->ino != INODE_EMPTY && 
		   ap_pack_data->ino != map_alloc_data->hint_info->hint.ino){
			/* check threshold and decide whether to replace associated inode */
			do_gettimeofday(&curr_tv);
			diff = diff_tv(&curr_tv, &ap_pack_data->tv);
			if(AP_DISASSOCIATE_TIME > diff)
				continue;
			DMINFO("ap association timeout expired");
			/* proceed to associate with some other inode*/			
		}

		/* got it - empty ap not associated to any inode */
		ap_pack_data->ino = map_alloc_data->hint_info->hint.ino; // do this before alloc_addr
		addr = openssd_alloc_addr_from_ap(ap, ret_victim_block, 0);
		DMDEBUG("re-associated ap with block_addr %ld to new inode %d", block_to_addr(ap->cur), ap_pack_data->ino);

		break;
	}

	if (addr != LTOP_EMPTY){
		DMDEBUG("allocated addr %ld from NEW associated ap ", addr);
		goto pack_alloc_done;
	}

	DMDEBUG("no new/previous ap associated to inode. do regular allocation");
	/* haven't found any relevant/empty pack ap. resort to regular allocation
	   (from non-packed ap)*/
	/* TODO: overtake "regular" ap? return error? */
	do{
		ap = get_next_ap(os);
	}while(ap->hint_private);

	addr = openssd_alloc_addr_from_ap(ap, ret_victim_block, 0);

pack_alloc_done:
	if(ap_pack_data)
		do_gettimeofday(&ap_pack_data->tv);
	return addr;
}



/* Latency-proned Logical to physical address translation.
 *
 * If latency hinted write, write data to two locations, and save extra mapping
 * If non-hinted write - resort to normal allocation
 * if GC write - no hint, but we use regular map_ltop() with GC addr
 */
static struct nvm_addr *openssd_map_pack_hint_ltop_rr(struct openssd *os, sector_t l_addr, int is_gc, void *private)
{
	struct openssd_hint_map_private *map_alloc_data = private;
	struct nvm_block *block;
	struct nvm_addr *p = NULL;
	sector_t p_addr;


	/* If there is no hint, or this is a reclaimed ltop mapping,
	 * use regular (single-page) map_ltop */
	if (!map_alloc_data ||
	    map_alloc_data->old_p_addr != LTOP_EMPTY ||
	    !map_alloc_data->hint_info) {
		DMDEBUG("pack_rr: reclaimed or regular allocation");
		p = openssd_alloc_map_ltop_rr(os, l_addr, 0, NULL);

		return p;
	}

	DMDEBUG("pack_ltop: regular request. allocate page");

	/* 1) get addr.
	      openssd_alloc_addr_from_pack_ap, finds ap AND allocates addr*/
	p_addr = openssd_alloc_phys_pack_addr(os, &block, map_alloc_data);
	
	if (block)
		p = openssd_update_map(os, l_addr, p_addr, block);

	DMDEBUG("pack_rr: for l_addr=%ld allocated p_addr=%ld ", l_addr, p_addr);

	return p;
}

// do any shadow address updating required (real, none, or trim of old one)
static struct nvm_addr *openssd_update_map_shadow(struct openssd *os,
                                      sector_t l_addr, sector_t p_addr,
                                      struct nvm_block *p_block, unsigned long flags)
{
	struct openssd_hint *hint = os->hint_private;
	struct nvm_addr *p;

	BUG_ON(l_addr >= os->nr_pages);
	BUG_ON(p_addr >= os->nr_pages);

	DMDEBUG("openssd_update_map_shadow: flags=%lu", flags);
	/* Secondary mapping. update shadow */
	if(flags & MAP_SHADOW) {
		spin_lock(&os->trans_lock);
		p = &hint->shadow_map[l_addr];

		invalidate_block_page(os, p);

		p->addr = p_addr;
		p->block = p_block;

		os->rev_trans_map[p_addr] = l_addr;
		spin_unlock(&os->trans_lock);

		return p;
	} else if (flags & MAP_PRIMARY) {
		DMDEBUG("should update primary only");
		return NULL;
	}

	/* Remove old shadow mapping from shadow map */
	DMDEBUG("init shadow");
	p = &hint->shadow_map[l_addr];
	p->addr = LTOP_EMPTY;
	p->block = NULL;
	return NULL;
}



static unsigned long openssd_get_mapping_flag(struct openssd *os, sector_t logical_addr, sector_t old_p_addr);

/* Latency-proned Logical to physical address translation.
 *
 * If latency hinted write, write data to two locations, and save extra mapping
 * If non-hinted write - resort to normal allocation
 * if GC write - no hint, but we use regular map_ltop() with GC addr
 */
static struct nvm_addr *openssd_map_latency_hint_ltop_rr(struct openssd *os, sector_t l_addr, int is_gc, void *private)
{
	struct openssd_hint_map_private *map_alloc_data = private;
	struct nvm_block *block;
	struct nvm_addr *p = NULL;
	sector_t p_addr;

	/* reclaimed write. need to know if we're reclaiming primary/shaddow*/
	if (is_gc) {
		map_alloc_data->flags = openssd_get_mapping_flag(os, l_addr, map_alloc_data->old_p_addr);
		DMDEBUG("gc write. flags %x", map_alloc_data->flags);
	}
	DMDEBUG("latency_ltop: allocate primary and shaddow pages");

	/* primary -> allcoate and update generic mapping */
	if (map_alloc_data->flags & MAP_PRIMARY)
		return openssd_alloc_map_ltop_rr(os, l_addr, is_gc, NULL);

	/* shadow -> allocate and update shaddow mapping*/
	p_addr = openssd_alloc_ltop_rr(os, l_addr, &block, is_gc, map_alloc_data);
	
	if (block)
		p = openssd_update_map_shadow(os, l_addr, p_addr, block, map_alloc_data->flags);

	DMDEBUG("got address of shadow page");

	return p;
}

/* Swap-proned Logical to physical address translation.
 *
 * If swap write, use simple fast page allocation - find some append point whose next page is fast.
 * Then update the ap for the next write to the disk.
 * If no reelvant ap found, or non-swap write - resort to normal allocation
 */
static struct nvm_addr *openssd_map_swap_hint_ltop_rr(struct openssd *os,
                sector_t l_addr, int is_gc, void *private)
{
	struct openssd_hint_map_private *map_alloc_data = private;
	struct nvm_block *block;
	sector_t p_addr;

	/* Check if there is a hint for relevant sector
	 * if not, resort to openssd_map_ltop_rr */
	if (map_alloc_data) {
		if (map_alloc_data->old_p_addr == LTOP_EMPTY && !map_alloc_data->hint_info) {
			DMDEBUG("swap_map: non-GC non-hinted write");
			return openssd_alloc_map_ltop_rr(os, l_addr, 0, NULL);
		}

		/* GC write of a slow page */
		if (map_alloc_data->old_p_addr != LTOP_EMPTY && !page_is_fast(physical_to_slot(os, map_alloc_data->old_p_addr), os)) {
			DMDEBUG("swap_map: GC write of a SLOW page (old_p_addr %ld block offset %d)", map_alloc_data->old_p_addr, physical_to_slot(os, map_alloc_data->old_p_addr));
			return openssd_alloc_map_ltop_rr(os, l_addr, 0, NULL);
		}
	}

	/* hinted write, or GC of FAST page*/
	p_addr = openssd_alloc_phys_fastest_addr(os, &block);

	/* no FAST page found. restort to regular allocation */
	if (!block)
		return openssd_alloc_map_ltop_rr(os, l_addr, 0, NULL);

	//DMINFO("swap_rr: got physical_addr %d *ret_victim_block %p", physical_addr, *ret_victim_block);
	DMDEBUG("write lba %ld to page %ld", l_addr, p_addr);
	return openssd_update_map(os, l_addr, p_addr, block);
}

// TODO: actually finding a non-busy pool is not enough. read should be moved up the request queue.
//	 however, no queue maipulation impl. yet...
static struct nvm_addr *openssd_latency_lookup_ltop(struct openssd *os, sector_t logical_addr) {
	struct openssd_hint *hint = os->hint_private;
	int pool_idx;

	BUG_ON(!(logical_addr >= 0 && logical_addr < os->nr_pages));

	// shadow is empty
	if (hint->shadow_map[logical_addr].addr == LTOP_EMPTY) {
		DMDEBUG("no shadow. read primary");
		return openssd_lookup_ltop(os, logical_addr);
	}

	// check if primary is busy
	pool_idx = os->trans_map[logical_addr].addr / (os->nr_pages / os->nr_pools);
	if (atomic_read(&os->pools[pool_idx].is_active)) {
		DMDEBUG("primary busy. read shadow");
		return openssd_lookup_ltop_map(os, logical_addr, hint->shadow_map);
	}

	// primary not busy
	DMDEBUG("primary not busy");
	return openssd_lookup_ltop(os, logical_addr);
}

static unsigned long openssd_get_mapping_flag(struct openssd *os, sector_t logical_addr, sector_t old_p_addr)
{
	struct openssd_hint *hint = os->hint_private;
	unsigned long flag = MAP_PRIMARY;

	if(old_p_addr != LTOP_EMPTY) {
		DMDEBUG("get_flag old_p_addr %ld os->trans_map[%ld].addr %ld hint->shadow_map[%ld].addr %ld", old_p_addr, logical_addr, os->trans_map[logical_addr].addr, logical_addr, hint->shadow_map[logical_addr].addr);
		spin_lock(&os->trans_lock);
		if(os->trans_map[logical_addr].addr == old_p_addr)
			flag = MAP_PRIMARY;
		else if(hint->shadow_map[logical_addr].addr == old_p_addr)
			flag = MAP_SHADOW;
		else {
			DMERR("Reclaiming a physical page %ld not mapped by any logical addr", old_p_addr);
			WARN_ON(true);
		}
		spin_unlock(&os->trans_lock);
	}

	return flag;
}

// if we ever support trim, this may be unified with some generic function
static void openssd_trim_map_shadow(struct openssd *os, sector_t l_addr)
{
	struct openssd_hint *hint = os->hint_private;
	struct nvm_addr *l;
	struct nvm_block *block;
	unsigned int page_offset;
	sector_t p_addr;

	BUG_ON(l_addr >= os->nr_pages);

	spin_lock(&os->trans_lock);
	l = &hint->shadow_map[l_addr];
	block = l->block;
	p_addr = l->addr;

	DMDEBUG("trim old shaddow");
	if (block) {
		BUG_ON(p_addr >= os->nr_pages);

		page_offset = l->addr % (os->nr_host_pages_in_blk);
		DMDEBUG("trim map shadow l_addr %ld p_addr %ld page_offset %ld ", l_addr, p_addr, page_offset);
		spin_lock(&block->lock);
		WARN_ON(test_and_set_bit(page_offset, block->invalid_pages));
		block->nr_invalid_pages++;
		spin_unlock(&block->lock);
	
		os->rev_trans_map[p_addr] = LTOP_EMPTY;
	}

	l->addr = 0;
	l->block = 0;
	spin_unlock(&os->trans_lock);
}

int openssd_ioctl_user_hint_cmd(struct openssd *os, unsigned long arg)
{
	hint_data_t __user *uhint = (hint_data_t __user *)arg;
	hint_data_t* hint_data;
	int ret;

	DMDEBUG("send user hint");

	/* allocate hint_data */
	hint_data = kmalloc(sizeof(hint_data_t), GFP_KERNEL);
	if (!hint_data) {
		DMERR("hint_data_t kmalloc failed");
		return -ENOMEM;
	}

	// copy hint data from user space
	if (copy_from_user(hint_data, uhint, sizeof(hint_data_t)))
		return -EFAULT;

	// send hint to device
	ret = openssd_send_hint(os, hint_data);
	kfree(hint_data);

	return ret;
}

int openssd_ioctl_kernel_hint_cmd(struct openssd *os, unsigned long arg)
{
	hint_data_t *hint = (hint_data_t *)arg;
	// send hint to device
	// TODO: do we need to free khint here? or is it freed by block layer?
	return openssd_send_hint(os, hint);
}

int openssd_ioctl_hint(struct openssd *os, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case OPENSSD_IOCTL_SUBMIT_HINT:
		return openssd_ioctl_user_hint_cmd(os, arg);
	case OPENSSD_IOCTL_KERNEL_HINT:
		return openssd_ioctl_kernel_hint_cmd(os, arg);
	default:
		return 0;
		//return __blkdev_driver_ioctl(os->dev->bdev, os->dev->mode, cmd, arg);
	}

	return 0;
}

int openssd_init_hint(struct openssd *os)
{
	return 0;
}

int openssd_alloc_hint(struct openssd *os)
{
	struct openssd_hint *hint;
	struct nvm_ap *ap;
	struct nvm_pool *pool;
	int i;

	hint = kmalloc(sizeof(struct openssd_hint), GFP_KERNEL);
	if (!hint)
		return -ENOMEM;

	hint->shadow_map = vmalloc(sizeof(struct nvm_addr) * os->nr_pages);
	if (!hint->shadow_map)
		goto err_shadow_map;
	memset(hint->shadow_map, 0, sizeof(struct nvm_addr) * os->nr_pages);

	for(i = 0; i < os->nr_pages; i++) {
		struct nvm_addr *p = &hint->shadow_map[i];
		p->addr = LTOP_EMPTY;
		atomic_set(&p->inflight, 0);
	}

	spin_lock_init(&hint->hintlock);
	INIT_LIST_HEAD(&hint->hintlist);

	hint->ino2fc = kzalloc(HINT_MAX_INOS, GFP_KERNEL);
	if (!hint->ino2fc)
		goto err_hints;

	/* mark all pack hint related ap's*/
	ssd_for_each_pool(os, pool, i) {
		unsigned int last_ap;
		/* choose the last ap in each pool */
		last_ap = (i * os->nr_aps_per_pool) + os->nr_aps_per_pool - 1;
		ap = &os->aps[last_ap];

		ap->hint_private = kmalloc(sizeof(struct openssd_ap_hint),
								GFP_KERNEL);
		if (!ap->hint_private) {
			DMERR("Couldn't allocate hint private for ap.");
			goto err_ap_hints;
		}
		init_ap_hint(ap);
	}

	if (os->config.flags & NVM_OPT_ENGINE_SWAP) {
		DMINFO("Swap hint support");
		os->map_ltop = openssd_map_swap_hint_ltop_rr;
		os->write_bio = openssd_write_bio_hint;
		os->read_bio = openssd_read_bio_hint;
		os->begin_gc_private = openssd_begin_gc_hint;
		os->end_gc_private = openssd_end_gc_hint;
	} else if (os->config.flags & NVM_OPT_ENGINE_LATENCY) {
		DMINFO("Latency hint support");
		os->map_ltop = openssd_map_latency_hint_ltop_rr;
		os->lookup_ltop = openssd_latency_lookup_ltop;
		os->write_bio = openssd_write_bio_hint;
		os->read_bio = openssd_read_bio_hint;
		os->begin_gc_private = openssd_begin_gc_hint;
		os->end_gc_private = openssd_end_gc_hint;
	} else if (os->config.flags & NVM_OPT_ENGINE_PACK) {
		DMINFO("Pack hint support");
		os->map_ltop = openssd_map_pack_hint_ltop_rr;
		os->alloc_phys_addr = openssd_alloc_phys_addr_pack;
		os->write_bio = openssd_write_bio_hint;
		os->read_bio = openssd_read_bio_hint;
		os->begin_gc_private = openssd_begin_gc_hint;
		os->end_gc_private = openssd_end_gc_hint;

		if (os->nr_aps_per_pool < 2 ) {
			DMERR("Need at least 2 aps for pack hints");
			goto err_hints;
		}
	}

	os->hint_private = hint;

	return 0;
err_ap_hints:
	ssd_for_each_ap(os, ap, i)
		kfree(ap->hint_private);
	kfree(hint->ino2fc);
err_hints:
	vfree(hint->shadow_map);
err_shadow_map:
	kfree(hint);
	return -ENOMEM;
}

void openssd_free_hint(struct openssd *os)
{
	struct openssd_hint *hint = os->hint_private;
	hint_info_t *hint_info, *next_hint_info;
	struct nvm_ap *ap;
	int i;

	spin_lock(&hint->hintlock);
	list_for_each_entry_safe(hint_info, next_hint_info, &hint->hintlist, list_member) {
		list_del(&hint_info->list_member);
		DMINFO("dtr: deleted hint");
		kfree(hint_info);
	}
	spin_unlock(&hint->hintlock);

	kfree(hint->ino2fc);
	vfree(hint->shadow_map);

	/* mark all pack hint related ap's*/
	DMINFO("deallocating hint private for pack ap's");
	ssd_for_each_ap(os, ap, i) {
		if(i % os->nr_aps_per_pool != os->nr_aps_per_pool -1){
			continue;
		}

		// only last ap in pool
		kfree(ap->hint_private);
	}

	kfree(os->hint_private);
}

void openssd_exit_hint(struct openssd *os)
{
	// release everything else needed
}
