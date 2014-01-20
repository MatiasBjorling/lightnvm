/*
 * Copyright (C) 2012 Matias Bj�rling.
 *
 * This file is released under the GPL.
 *
 * Todo
 *
 * - Implement translation mapping from logical to physical flash pages
 * - Implement garbage collection
 * - Implement fetching of bad pages from flash
 * 
 * Hints
 * - configurable sector size
 * - handle case of in-page bv_offset (currently hidden assumption of offset=0, and bv_len spans entire page)
 */

#include "dm-openssd.h"
#include "dm-openssd-pool.h"
#include "dm-openssd-hint.h"

#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/dm-kcopyd.h>
#include <linux/blkdev.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>

#define DM_MSG_PREFIX "openssd hint mapper"
#define APS_PER_POOL 1 /* Number of append points per pool. We assume that accesses within 
						  a pool is serial (NAND flash / PCM / etc.) */
#define SERIALIZE_AP_ACCESS 1 /* If enabled, we delay bios on each ap to run serialized. */

/* Sleep timings before simulating device specific storage (in us)*/
#define TIMING_READ 25
#define TIMING_WRITE 500
#define TIMING_ERASE 1500

/* Run GC every X seconds */
#define GC_TIME 10
/* Run only GC is less than 1/X blocks are free */
#define GC_LIMIT_INVERSE 2
/*
	 * For now we hardcode the configuration for the OpenSSD unit that we own. In
	 * the future this should be made configurable.
	 *
	 * Configuration:
	 *
	 * Physical address space is divided into 8 chips. I.e. we create 8 pools for the
	 * addressing. We also omit the first block of each chip as they contain
	 * either the drive firmware or recordings of bad blocks.
	 *
	 */
#define DEBUG 1
#ifdef DEBUG
#define POOL_COUNT 8
#define POOL_BLOCK_COUNT 32
#define BLOCK_PAGE_COUNT 64
#else
#define POOL_COUNT 8
#define POOL_BLOCK_COUNT 4
#define BLOCK_PAGE_COUNT 64
#endif

struct openssd_dev_conf {
	unsigned short int flash_block_size; /* the number of flash pages per block */
	unsigned short int flash_page_size;  /* the flash page size in bytes */
	unsigned int num_blocks;	   /* the number of blocks addressable by the mapped SSD. */
};

/* Pool descriptions */
struct openssd_pool_block {
	struct {
		atomic_t next_page; /* points to the next writable page within the block */
		struct openssd_pool *parent;
		unsigned int id;
	} ____cacheline_aligned_in_smp;

	unsigned int nr_invalid_pages;

	struct list_head list;
	struct list_head prio;
};

struct openssd_addr {
	sector_t addr;
	struct openssd_pool_block *block;
};

struct openssd_pool {
	/* Pool block lists */
	struct {
		spinlock_t lock;
		struct list_head used_list;
		struct list_head free_list;
		struct list_head prio_list;
	} ____cacheline_aligned_in_smp;
	unsigned long phy_addr_start;	/* References the physical start block */
	unsigned int phy_addr_end;		/* References the physical end block */

	unsigned int nr_blocks;			/* Derived value from end_block - start_block. */
	unsigned int nr_free_blocks;	/* Number of unused blocks */

	struct openssd_pool_block *blocks;
};

/*
 * openssd_ap. ap is an append point. A pool can have 1..X append points attached.
 * An append point has a current block, that it writes to, and when its full, it requests
 * a new block, of which it continues its writes.
 */
struct openssd_ap {
	spinlock_t lock;
	struct openssd *parent;
	struct openssd_pool *pool;
	struct openssd_pool_block *cur;

	/* Timings used for end_io waiting */
	unsigned long t_read;
	unsigned long t_write;
	unsigned long t_erase;

	/* Postpone issuing I/O if append point is active */
	atomic_t is_active;
	struct work_struct waiting_ws;
	spinlock_t waiting_lock;
	struct bio_list waiting_bios;

	unsigned long io_delayed;
	unsigned long io_accesses[2];
};

struct openssd;

typedef sector_t (map_ltop_fn)(struct openssd *, sector_t, struct openssd_ap **);
typedef sector_t (lookup_ltop_fn)(struct openssd *, sector_t);

/* Main structure */
struct openssd {
	struct dm_dev *dev;
	struct dm_target *ti;
	uint32_t sector_size;

	/* Simple translation map of logical addresses to physical addresses. The 
	 * logical addresses is known by the host system, while the physical
	 * addresses are used when writing to the disk block device. */
	struct openssd_addr *trans_map;
	/* also store a reverse map for garbage collection */
	sector_t *rev_trans_map;

	struct openssd_dev_conf dev_conf;

	/* Usually instantiated to the number of available parallel channels
	 * within the hardware device. i.e. a controller with 4 flash channels,
	 * would have 4 pools.
	 *
	 * We assume that the device exposes its channels as a linear address
	 * space. A pool therefore have a phy_addr_start and phy_addr_end that
	 * denotes the start and end. This abstraction is used to let the openssd
	 * (or any other device) expose its read/write/erase interface and be 
	 * administrated by the host system.
	 */
	struct openssd_pool *pools;

	/* Append points */
	struct openssd_ap *aps;

	int nr_pools;
	int nr_aps;
	int nr_aps_per_pool;

	unsigned long nr_pages;

	unsigned int next_collect_pool;

	map_ltop_fn *map_ltop;
	lookup_ltop_fn *lookup_ltop;
	/* Write strategy variables. Move these into each for structure for each 
	 * strategy */
	atomic_t next_write_ap; /* Whenever a page is written, this is updated to point
							   to the next write append point */

	bool serialize_ap_access;		/* Control accesses to append points in the host.
							 * Enable this for devices that doesn't have an
							 * internal queue that only lets one command run
							 * at a time within an append point 
							*/
	struct workqueue_struct *kbiod_wq;

	struct task_struct *kt_openssd; /* handles gc and any other async work */
};

struct per_bio_data {
	struct openssd_ap *ap;
	struct timeval start_tv;
};

static struct per_bio_data *get_per_bio_data(struct bio *bio) 
{
	struct per_bio_data *pb = dm_per_bio_data(bio, sizeof(struct per_bio_data));
	BUG_ON(!pb);
	return pb;
}

static void openssd_delayed_bio_submit(struct work_struct *work)
{
	struct openssd_ap *ap = container_of(work, struct openssd_ap, waiting_ws);
	struct bio *bio;

	spin_lock(&ap->waiting_lock);
	bio = bio_list_pop(&ap->waiting_bios);
	spin_unlock(&ap->waiting_lock);

	generic_make_request(bio);
}

static void openssd_update_mapping(struct openssd *os,  sector_t l_addr,
						   sector_t p_addr, struct openssd_pool_block *p_block)
{
	struct openssd_addr *l;

	BUG_ON(l_addr >= os->nr_pages);
	BUG_ON(p_addr >= os->nr_pages);

	l = &os->trans_map[l_addr];
	if (l->block) {
		BUG_ON(l->block->nr_invalid_pages == BLOCK_PAGE_COUNT);
		l->block->nr_invalid_pages++;
	}

	l->addr = p_addr;
	l->block = p_block;

	/* Physical -> Logical address */
	os->rev_trans_map[p_addr] = l_addr;
}

/* use pool_[get/put]_block to administer the blocks in use for each pool.
 * Whenever a block is in used by an append poing, we store it within the used_list.
 * We then move itback when its free to be used by another append point.
 *
 * The newly acclaimed block is always added to the back of user_list. As we assume
 * that the start of used list is the oldest block, and therefore higher probability
 * of invalidated pages.
 */
static struct openssd_pool_block *openssd_pool_get_block(struct openssd_pool *pool)
{
	struct openssd_pool_block *block;

	spin_lock(&pool->lock);
	if (list_empty(&pool->free_list))
		return NULL;

	block = list_first_entry(&pool->free_list, struct openssd_pool_block, list);
	list_move_tail(&block->list, &pool->used_list);

	pool->nr_free_blocks--;

	spin_unlock(&pool->lock);
	return block;
}

/* requires pool->lock taken */
static inline void openssd_reset_block(struct openssd_pool_block *block)
{
	BUG_ON(!block);
	BUG_ON(spin_is_locked(&block->parent->lock));

	atomic_set(&block->next_page, -1);
}

/* We assume that all valid pages have already been moved when added back to the
 * free list. We add it last to allow round-robin use of all pages. Thereby provide
 * simple (naive) wear-leveling.
 */
static void openssd_pool_put_block(struct openssd_pool_block *block)
{
	struct openssd_pool *pool = block->parent;

	spin_lock(&pool->lock);

	openssd_reset_block(block);
	list_move_tail(&block->list, &pool->free_list);

	pool->nr_free_blocks++;
	spin_unlock(&pool->lock);
}

static int openssd_get_page_id(struct openssd_pool_block *block)
{
	int page_id = atomic_inc_return(&block->next_page);

	if (page_id >= BLOCK_PAGE_COUNT)
		return -1;

	return page_id;
}

static sector_t block_to_addr(struct openssd_pool_block *block)
{
	return (block->id * BLOCK_PAGE_COUNT);
}

static void openssd_set_ap_cur(struct openssd_ap *ap, struct openssd_pool_block *block)
{
	spin_lock(&ap->lock);
	ap->cur = block;
	spin_unlock(&ap->lock);
}

/* the block with highest number of invalid pages, will be in the beginning of the list */
static int block_prio_sort_cmp(void *priv, struct list_head *lh_a, struct list_head *lh_b)
{
	struct openssd_pool_block *a = list_entry(lh_a, struct openssd_pool_block, prio);
	struct openssd_pool_block *b = list_entry(lh_b, struct openssd_pool_block, prio);

	if (a->nr_invalid_pages == b->nr_invalid_pages)
		return 0;

	return a->nr_invalid_pages < b->nr_invalid_pages;
}

static void erase_block(struct openssd_pool_block *block)
{
	/* Send erase command to device. */
}

static void openssd_move_valid_pages(struct openssd *os, struct openssd_pool_block *target_block)
{
	//sector_t saddr = block_to_addr(target_block);
}

static void openssd_print_total_blocks(struct openssd *os)
{
	struct openssd_pool *pool;
	unsigned int total = 0;
	int i;

	ssd_for_each_pool(os, pool, i)
		total += pool->nr_free_blocks;

	DMINFO("Total free blocks: %u", total);
}

static void openssd_gc_collect(struct openssd *os)
{
	struct openssd_pool *pool;
	struct openssd_pool_block *block;
	unsigned int nr_blocks_need;
	int pid, pid_start;
	int max_collect = os->nr_pools / 2;

	openssd_print_total_blocks(os);

	while (max_collect) {
		block = NULL;
		/* Iterate the pools once to look for pool that has a block to be freed. */
		pid = os->next_collect_pool % os->nr_pools;
		pid_start = pid;
		do {
			pool = &os->pools[pid];

			nr_blocks_need = pool->nr_blocks;
			do_div(nr_blocks_need, GC_LIMIT_INVERSE);

			if (nr_blocks_need >= pool->nr_free_blocks) {
				list_sort(NULL, &pool->prio_list, block_prio_sort_cmp);
				block = list_first_entry(&pool->prio_list, struct openssd_pool_block, prio);

				erase_block(block);
				openssd_pool_put_block(block);

				break;
			}

			pid++;
			pid %= os->nr_pools;
		} while (pid_start != pid);

		os->next_collect_pool++;
		max_collect--;
	}
}

static int openssd_kthread(void *data)
{
	struct openssd *os = (struct openssd *)data;
	BUG_ON(!os);

	while (!kthread_should_stop()) {

		openssd_gc_collect(os);

		schedule_timeout_uninterruptible(GC_TIME * HZ); 
	}

	return 0;
}

/* Simple round-robin Logical to physical address translation.
 *
 * Retrieve the mapping using the active append point. Then update the ap for the
 * next write to the disk.
 */
static sector_t openssd_map_ltop_rr(struct openssd *os, sector_t logical_addr, struct openssd_ap **ret_active_ap)
{
	int ap_id = atomic_inc_return(&os->next_write_ap) % os->nr_aps;
	struct openssd_pool_block *block;
	int page_id;
	sector_t physical_addr;

	*ret_active_ap = &os->aps[ap_id];
	block = (*ret_active_ap)->cur;

	page_id = openssd_get_page_id(block);
	while (page_id < 0) {
		block = openssd_pool_get_block(block->parent);
		if (!block)
			return -1;

		openssd_set_ap_cur(*ret_active_ap, block);
		page_id = openssd_get_page_id(block);
	}

	physical_addr = block_to_addr(block) + page_id;

	openssd_update_mapping(os, logical_addr, physical_addr, block);

	return physical_addr;
}

static int openssd_pool_init(struct openssd *os, struct dm_target *ti)
{
	struct openssd_pool *pool;
	struct openssd_pool_block *block;
	struct openssd_ap *ap;
	int i, j;

	os->nr_aps_per_pool = APS_PER_POOL;

	os->serialize_ap_access = SERIALIZE_AP_ACCESS;

	// Simple round-robin strategy
	atomic_set(&os->next_write_ap, -1);
	os->map_ltop = openssd_map_ltop_rr;

	os->nr_pools = POOL_COUNT;
	os->pools = kzalloc(sizeof(struct openssd_pool) * os->nr_pools, GFP_KERNEL);
	if (!os->pools)
		goto err_pool;

	ssd_for_each_pool(os, pool, i) {
		spin_lock_init(&pool->lock);

		INIT_LIST_HEAD(&pool->free_list);
		INIT_LIST_HEAD(&pool->used_list);
		INIT_LIST_HEAD(&pool->prio_list);

		pool->phy_addr_start = i * POOL_BLOCK_COUNT;
		pool->phy_addr_end = (i + 1) * POOL_BLOCK_COUNT - 1;

		pool->nr_free_blocks = pool->nr_blocks = pool->phy_addr_end - pool->phy_addr_start + 1;
		pool->blocks = kzalloc(sizeof(struct openssd_pool_block) * pool->nr_blocks, GFP_KERNEL);
		if (!pool->blocks)
			goto err_blocks;

		pool_for_each_block(pool, block, j) {
			block->parent = pool;
			atomic_set(&block->next_page, -1);
			block->id = (i * POOL_BLOCK_COUNT) + j;
			list_add_tail(&(block->list), &pool->free_list);
			list_add_tail(&block->prio, &pool->prio_list);
		}
	}

	os->nr_aps = os->nr_aps_per_pool * os->nr_pools;;
	os->aps = kmalloc(sizeof(struct openssd_ap) * os->nr_pools * os->nr_aps, GFP_KERNEL);
	if (!os->aps)
		goto err_blocks;

	ssd_for_each_pool(os, pool, i) {
		for (j = 0; j < os->nr_aps_per_pool; j++) {
			ap = &os->aps[(i * os->nr_aps_per_pool) + j];

			spin_lock_init(&ap->lock);
			spin_lock_init(&ap->waiting_lock);
			bio_list_init(&ap->waiting_bios);
			INIT_WORK(&ap->waiting_ws, openssd_delayed_bio_submit);
			atomic_set(&ap->is_active, 0);

			ap->parent = os;
			ap->pool = pool;
			ap->cur = openssd_pool_get_block(pool); // No need to lock ap->cur.

			ap->t_read = TIMING_READ;
			ap->t_write = TIMING_WRITE;
			ap->t_erase = TIMING_ERASE;
		}
	}

	os->kbiod_wq = alloc_workqueue("kopenssd-work", WQ_MEM_RECLAIM, 0);
	if (!os->kbiod_wq) {
		DMERR("Couldn't start kopenssd-worker");
		goto err_blocks;
	}

	return 0;

err_blocks:
	ssd_for_each_pool(os, pool, i) {
		if (!pool->blocks)
			break;
		kfree(pool->blocks);
	}
	kfree(os->pools);
err_pool:
	ti->error = "dm-openssd: Cannot allocate openssd data structures";
	return -ENOMEM;
}

fclass file_classify(struct bio_vec* bvec) {
	fclass fc = FC_UNKNOWN;
	char *sec_in_mem;
	char byte[4];

	if(!bvec || !bvec->bv_page){
		DMINFO("can't kmap empty bvec->bv_page. kmap failed");
		return fc;
	}

	byte[0] = 0x66;
	byte[1] = 0x74;
	byte[2] = 0x79;
	byte[3] = 0x70;

	sec_in_mem = kmap_atomic((bvec->bv_page) + bvec->bv_offset);

	if(!sec_in_mem) {
		DMERR("bvec->bv_page kmap failed");
		return fc;
	}
	if(!memcmp(sec_in_mem+4, byte,4)) {
		//hint_log("VIDEO classified");
		DMINFO("VIDEO classified");
		fc = FC_VIDEO_SLOW;
	}

	kunmap_atomic(sec_in_mem);
	return fc;
}

static int openssd_send_hint(struct dm_target *ti, hint_data_t *hint_data)
{
	// TODO: call special ioctl on target?
	// for now just print and free
	DMINFO("first %s hint fc=%d", CAST_TO_PAYLOAD(hint_data)->is_write?"WRITE":"READ",
				      INO_HINT_FROM_DATA(hint_data, 0).fc);
	DMINFO("send nothing free hint_data and simply return");
	kfree(hint_data);    

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
static void openssd_bio_hints(struct dm_target *ti, struct bio *bio)
{
	hint_data_t *hint_data = NULL;
	uint32_t lba = 0, bio_len = 0, hint_idx;
	uint32_t sectors_count = 0;
	struct page *bv_page = NULL;
	struct address_space *mapping;
	struct inode *host;
	unsigned long prev_ino = -1, first_sector = -1;
	unsigned ino = -1;
	struct bio_vec *bvec;
	fclass fc = FC_EMPTY;
	int i, ret;
	bool is_write = 0;
	uint32_t sector_size = ((struct openssd *) ti->private)->sector_size;

    /* can classify only writes*/
	switch(bio_rw(bio)) {
		case READ:
		case READA:
			/* read/readahead*/
			break;
		case WRITE:
			is_write = 1;
			break;
		default:
			/* ? */
			return;
	}

	// get lba and sector count
	lba = bio->bi_sector;
	sectors_count = bio->bi_size / sector_size;

	/* allocate hint_data */
	hint_data = kmalloc(sizeof(hint_data_t), GFP_ATOMIC);
	if (hint_data == NULL) {
		DMERR("hint_data_t kmalloc failed");  
		return;
	}

	memset(hint_data, 0, sizeof(hint_data_t));
	CAST_TO_PAYLOAD(hint_data)->lba = lba;
	CAST_TO_PAYLOAD(hint_data)->sectors_count = sectors_count;
	CAST_TO_PAYLOAD(hint_data)->is_write = is_write;
	ino = -1;            
	DMINFO("%s lba=%d sectors_count=%d", is_write?"WRITE":"READ", lba, sectors_count);
#if 0
	hint_log("free hint_data dont look in bvec. simply return");
	kfree(hint_data);
	return;
#endif

	bio_for_each_segment(bvec, bio, i) {
		bv_page = bvec[0].bv_page;

		if (bv_page && !PageSlab(bv_page)) {
			// swap hint
			if(PageSwapCache(bv_page)) {
				DMINFO("swap bio");
				// TODO - not tested
				CAST_TO_PAYLOAD(hint_data)->is_swap = 1;

				// for compatibility add one hint
				INO_HINT_SET(hint_data, CAST_TO_PAYLOAD(hint_data)->count,
								0, lba, sectors_count, fc);
				CAST_TO_PAYLOAD(hint_data)->count++;
              break;
		}

			mapping = bv_page->mapping;
//				continue;
		if (mapping && ((unsigned long)mapping & PAGE_MAPPING_ANON) == 0) {
			host = mapping->host;
			if (!host) {
				DMCRIT("page without mapping->host. shouldn't happen\n");
				bio_len+= bvec[0].bv_len;
				continue; // no host
			}

			prev_ino = ino;
			ino = host->i_ino;

			if(!host->i_sb || !host->i_sb->s_type || !host->i_sb->s_type->name){
				DMINFO("not related to file system");
				bio_len+= bvec[0].bv_len;
				continue;
			}

			if(!ino) {
				DMINFO("not inode related");
				bio_len+= bvec[0].bv_len;
				continue;
			}
			//if(bvec[0].bv_offset) 
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
			if(prev_ino == ino) {
				hint_idx = CAST_TO_PAYLOAD(hint_data)->count-1;
				if(INO_HINT_FROM_DATA(hint_data, hint_idx).ino != ino) {
					DMERR("updating hint of wrong ino (ino=%u expected=%lu)", ino,
						INO_HINT_FROM_DATA(hint_data, hint_idx).ino);            
					bio_len+= bvec[0].bv_len;
					continue;
				}

					INO_HINT_FROM_DATA(hint_data, hint_idx).count += bvec[0].bv_len / sector_size;
					DMINFO("increase count for hint %u. new count=%u", 
					hint_idx, INO_HINT_FROM_DATA(hint_data, hint_idx).count);
					bio_len+= bvec[0].bv_len;
					continue;
				}

				if(HINT_DATA_MAX_INOS == CAST_TO_PAYLOAD(hint_data)->count){
					DMERR("too many inos in hint");
					bio_len+= bvec[0].bv_len;
					continue;
				}

				DMINFO("add %s hint here - ino=%u lba=%u fc=%s count=%d hint_count=%u", 
					is_write?"WRITE":"READ", ino, lba + (bio_len / sector_size), 
					 (fc==FC_VIDEO_SLOW)?"VIDEO":(fc==FC_EMPTY)?"EMPTY":"UNKNOWN", 
					 bvec[0].bv_len / sector_size, CAST_TO_PAYLOAD(hint_data)->count+1);

				// add new hint to hint_data. lba count=bvec[0].bv_len / sector_size, will add more later on
				INO_HINT_SET(hint_data, CAST_TO_PAYLOAD(hint_data)->count, 
					ino, lba + (bio_len / sector_size), bvec[0].bv_len / sector_size, fc);
				CAST_TO_PAYLOAD(hint_data)->count++;
			}
		}

		// increment len
		bio_len+= bvec[0].bv_len;
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
	if(CAST_TO_PAYLOAD(hint_data)->count==0){
		//hint_log("request with no file data");
		kfree(hint_data);
		return;
	}

	/* non-empty hint_data, send to device */
	//hint_log("hint count=%u. send to hint device", CAST_TO_PAYLOAD(hint_data)->count);
	ret = openssd_send_hint(ti, hint_data);

	if (ret != 0) {
		DMINFO("openssd_send_hint error %d", ret);
		return;
	}
}

/*----------------------------------------------------------------
 * OpenSSD target methods
 *
 * ctr - Constructor
 * dtr - Destructor
 * map - Maps and execute a given IO.
 *--------------------------------------------------------------*/

/*
 * Accepts an OpenSSD-backed block-device. The OpenSSD device should run the
 * corresponding physical firmware that exports the flash as physical without any
 * mapping and garbage collection as it will be taken care of.
 */
static int openssd_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	struct openssd *os;

	// Which device it should map onto?
	if (argc != 1) {
		ti->error = "The target takes a single block device path as argument.";
		return -EINVAL;
	}

	os = kmalloc(sizeof(*os), GFP_KERNEL);
	if (os == NULL) {
		return -ENOMEM;
	}

	os->nr_pages = POOL_COUNT * POOL_BLOCK_COUNT * BLOCK_PAGE_COUNT;

	os->trans_map = vmalloc(sizeof(struct openssd_addr) * os->nr_pages);
	if (!os->trans_map)
		goto err_trans_map;
	memset(os->trans_map, 0, sizeof(struct openssd_addr) * os->nr_pages);

	os->rev_trans_map = vmalloc(sizeof(sector_t) * os->nr_pages);
	if (!os->rev_trans_map)
		goto err_rev_trans_map;

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &os->dev))
		goto err_dev_lookup;

	os->sector_size = bdev_physical_block_size(os->dev->bdev);
	if(os->sector_size <=0 || os->sector_size % 512 != 0){
		ti->error = "dm-openssd: Got bad sector size";
		goto err_dev_lookup;
	}
	DMINFO("os->sector_size=%d", os->sector_size);

	os->ti = ti;
	ti->private = os;

	ti->per_bio_data_size = sizeof(struct per_bio_data);

	/* Initialize pools. */
	openssd_pool_init(os, ti);

	// FIXME: Clean up pool init on failure.
	os->kt_openssd = kthread_run(openssd_kthread, os, "kopenssd");
	if (!os->kt_openssd)
		goto err_dev_lookup;

	DMINFO("allocated %lu physical pages (%lu KB)", os->nr_pages, os->nr_pages * os->sector_size / 1024);
	DMINFO("successful loaded");

	return 0;

err_dev_lookup:
	vfree(os->rev_trans_map);
err_rev_trans_map:
	vfree(os->trans_map);
err_trans_map:
	kfree(os);
	ti->error = "dm-openssd: Cannot allocate openssd mapping context";
	return -ENOMEM;
}

static void openssd_dtr(struct dm_target *ti)
{
	struct openssd *os = (struct openssd *) ti->private;
	struct openssd_pool *pool;
	struct openssd_ap *ap;
	int i;

	dm_put_device(ti, os->dev);

	ssd_for_each_ap(os, ap, i) {
		while (bio_list_peek(&ap->waiting_bios))
			flush_scheduled_work();
	}

	kthread_stop(os->kt_openssd);

	ssd_for_each_pool(os, pool, i)
		kfree(pool->blocks);

	kfree(os->pools);
	kfree(os->aps);

	vfree(os->trans_map);
	vfree(os->rev_trans_map);

	destroy_workqueue(os->kbiod_wq);

	kfree(os);

	DMINFO("dm-openssd successful unload");
}

static int openssd_map(struct dm_target *ti, struct bio *bio)
{
	struct openssd *os = ti->private;
	struct openssd_ap *active_ap = NULL;
	struct per_bio_data *pb;
	sector_t logical_addr, physical_addr;

	logical_addr = bio->bi_sector;
	physical_addr = os->map_ltop(os, logical_addr, &active_ap);

	if (physical_addr == -1) {
		DMERR("No more physical addresses");
		return DM_MAPIO_REQUEUE;
	}

	pb = get_per_bio_data(bio);
	pb->ap = active_ap;

	//DMINFO("Logical: %lu Physical: %lu Size: %u %u", logical_addr, physical_addr, bio->bi_size, active_ap->cur->id);

	/* do hint */
	openssd_bio_hints(ti, bio);

	/* setup timings - remember overhead. */
	do_gettimeofday(&pb->start_tv);

	/* accepted bio, don't make new request */
	bio->bi_bdev = os->dev->bdev;
	if (os->serialize_ap_access && atomic_read(&active_ap->is_active)) {
		spin_lock(&active_ap->waiting_lock);
		active_ap->io_delayed++;
		bio_list_add(&active_ap->waiting_bios, bio);
		spin_unlock(&active_ap->waiting_lock);
	} else {
		atomic_inc(&active_ap->is_active);
		generic_make_request(bio);
	}

	// We allow counting to be semi-accurate as theres no locking for accounting.
	active_ap->io_accesses[bio_data_dir(bio)]++;

	return DM_MAPIO_SUBMITTED;
}

static int openssd_user_hint_cmd(struct openssd *os, hint_data_t __user *uhint)
{
	hint_data_t* hint_data;
	DMINFO("send user hint");

	/* allocate hint_data */
	hint_data = kmalloc(sizeof(hint_data_t), GFP_ATOMIC);
	if (hint_data == NULL) {
		DMERR("hint_data_t kmalloc failed");  
		return;
	}

    // copy hint data from user space
	if (copy_from_user(hint_data, uhint, sizeof(uhint)))
		return -EFAULT;

	// send hint to device
	return openssd_send_hint(os->ti, hint_data);
}

static int openssd_kernel_hint_cmd(struct openssd *os, hint_data_t *khint)
{

    // send hint to device
    // TODO: do we need to free khint here? or is it freed by block layer?
    return openssd_send_hint(os->ti, khint);
}

static int openssd_ioctl(struct dm_target *ti, unsigned int cmd,
			             unsigned long arg)
{
    struct openssd *os;
    struct dm_dev *dev;

    os = (struct openssd *) ti->private;
    dev = os->dev;

    DMDEBUG("got ioctl %x\n", cmd);
	switch (cmd) {
	    case OPENSSD_IOCTL_ID:
		    return 12345678; // TODO: anything else?
	    case OPENSSD_IOCTL_SUBMIT_HINT:
		    return openssd_user_hint_cmd(os, (hint_data_t __user *)arg);
	    case OPENSSD_IOCTL_KERNEL_HINT:
		    return openssd_kernel_hint_cmd(os, (hint_data_t*)arg);
	    default:
            // general ioctl to device
            printk("generic ioctl. forward to device\n");
	        return __blkdev_driver_ioctl(dev->bdev, dev->mode, cmd, arg);
	}
}

static int openssd_endio(struct dm_target *ti,
		      struct bio *bio, int err)
{
	struct per_bio_data *pb = get_per_bio_data(bio);
	struct openssd_ap *ap = pb->ap;
	struct openssd *os = ap->parent;
	struct timeval end_tv;
	unsigned long diff, dev_wait, total_wait = 0;

	if (bio_data_dir(bio) == WRITE)
		dev_wait = ap->t_write;
	else
		dev_wait = ap->t_read;

	if (dev_wait) {
		do_gettimeofday(&end_tv);
		diff = end_tv.tv_usec - pb->start_tv.tv_usec;
		if (dev_wait > diff)
			total_wait = dev_wait - diff;

		if (total_wait > 50) {
			udelay(total_wait);
		}
	}

	/* Remember that the IO is first officially finished from here */
	if (bio_list_peek(&ap->waiting_bios))
		queue_work(os->kbiod_wq, &ap->waiting_ws);
	else
		atomic_set(&ap->is_active, 0);

	return 0;
}

static void openssd_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen) 
{
	struct openssd *os = ti->private;
	struct openssd_ap *ap;
	int i, sz = 0;

	switch(type) {
	case STATUSTYPE_INFO:
		DMEMIT("Use table information");
		break;
	case STATUSTYPE_TABLE:
		ssd_for_each_ap(os, ap, i) {
			DMEMIT("Reads: %lu Writes: %lu Delayed: %lu",
					ap->io_accesses[0], ap->io_accesses[1],
					ap->io_delayed);
		}
		break;
	}
}

static void openssd_postsuspend(struct dm_target *ti)
{
}

static int openssd_iterate_devices(struct dm_target *ti,
				iterate_devices_callout_fn fn, void *data)
{
	return 0;
}

static void openssd_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
}

static struct target_type openssd_target = {
	.name = "openssd",
	.version = {0, 0, 1},
	.module	= THIS_MODULE,
	.ctr = openssd_ctr,
	.dtr = openssd_dtr,
	.map = openssd_map,
	.ioctl = openssd_ioctl,
	.end_io = openssd_endio,
	.status = openssd_status,
	//.postsuspend = openssd_postsuspend,
	//.status = openssd_status,
	//.iterate_devices = openssd_iterate_devices,
	//.io_hints = openssd_io_hints,
};

static int __init dm_openssd_init(void)
{
	int r;

	r = dm_register_target(&openssd_target);

	return r;
}

static void dm_openssd_exit(void)
{
	dm_unregister_target(&openssd_target);
}

module_init(dm_openssd_init);
module_exit(dm_openssd_exit);

MODULE_DESCRIPTION(DM_NAME "device-mapper openssd target");
MODULE_AUTHOR("Matias Bj�rling <mb@silverwolf.dk>");
MODULE_LICENSE("GPL");
