/*
 * Copyright (C) 2012 Matias Bj�rling.
 *
 * This file is released under the GPL.
 */

#ifndef DM_OPENSSD_H_
#define DM_OPENSSD_H_

#define OPENSSD_IOC_MAGIC 'O'
#define OPENSSD_IOCTL_ID          _IO(OPENSSD_IOC_MAGIC, 0x40)

#ifdef __KERNEL__
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
#include <linux/mempool.h>
#include <linux/kref.h>
#include <linux/completion.h>
#include <linux/hashtable.h>

#define DM_MSG_PREFIX "openssd"
#define LTOP_EMPTY -1
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
 * BLOCK_PAGE_COUNT must be a power of two.
 */

#define EXPOSED_PAGE_SIZE 4096	/* The page size that we expose to the operating system */
#define FLASH_PAGE_SIZE 4096	/* The size of the physical flash page */

#define NR_HOST_PAGES_IN_FLASH_PAGE (FLASH_PAGE_SIZE / EXPOSED_PAGE_SIZE)
#define NR_PHY_IN_LOG (EXPOSED_PAGE_SIZE / 512)

enum ltop_flags {
	MAP_PRIMARY	= 1 << 0, /* Update primary mapping (and init secondary mapping as a result) */
	MAP_SHADOW	= 1 << 1, /* Update only shaddow mapping */
	MAP_SINGLE	= 1 << 2, /* Update only the relevant mapping (primary/shaddow) */
};

#define NVM_OPT_MISC_OFFSET 15

enum target_flags {
	/* No hints applied */
	NVM_OPT_ENGINE_NONE		= 0 <<  0,
	/* Swap aware hints. Detected from block request type */
	NVM_OPT_ENGINE_SWAP		= 1 <<  0,
	/* IOCTL aware hints. Applications may submit direct hints */
	NVM_OPT_ENGINE_IOCTL	= 1 <<  1,
	/* Latency aware hints. Detected from file type or directly from app */
	NVM_OPT_ENGINE_LATENCY	= 1 <<  2,
	/* Pack aware hints. Detected from file type or directly from app */
	NVM_OPT_ENGINE_PACK	= 1 <<  3,

	/* Control accesses to append points in the host. Enable this for
	 * devices that doesn't have an internal queue that only lets one
	 * command run at a time within an append point */
	NVM_OPT_POOL_SERIALIZE	= 1 << NVM_OPT_MISC_OFFSET,
	/* Use fast/slow page access pattern */
	NVM_OPT_FAST_SLOW_PAGES	= 1 << (NVM_OPT_MISC_OFFSET+1),
	/* Disable dev waits */
	NVM_OPT_NO_WAITS	= 1 << (NVM_OPT_MISC_OFFSET+2),
};

/* Pool descriptions */
struct nvm_block {
	struct {
		spinlock_t lock;
		/* points to the next writable flash page within a block */
		unsigned int next_page;
		/* if a flash page can have multiple host pages,
		   fill up the flash page before going to the next
		   writable flash page */
		unsigned char next_offset;
		/* number of pages that are invalid, with respect to host page size */
		unsigned int nr_invalid_pages;
#define MAX_INVALID_PAGES_STORAGE 8
		/* Bitmap for invalid page intries */
		unsigned long invalid_pages[MAX_INVALID_PAGES_STORAGE];
	} ____cacheline_aligned_in_smp;

	unsigned int id;
	struct nvm_pool *pool;
	struct nvm_ap *ap;

	// Management and GC structures
	struct list_head list;
	struct list_head prio;

	// Persistent data structures
	struct page *data;
	atomic_t data_size; /* data pages inserted into data variable */
	atomic_t data_cmnt_size; /* data pages committed to stable storage */

	/* Block state handling */
	atomic_t gc_running;
	struct kref ref_count; /* Outstanding IOs to be completed on block */
};

struct nvm_addr {
	sector_t addr;
	struct nvm_block *block;
	struct hlist_node *list;
	atomic_t inflight;
};

struct nvm_pool {
	/* Pool block lists */
	struct {
		spinlock_t lock;
	} ____cacheline_aligned_in_smp;
	struct {
		spinlock_t gc_lock;
	} ____cacheline_aligned_in_smp;

	struct list_head used_list;	/* In-use blocks */
	struct list_head free_list;	/* Not used blocks i.e. released and ready for use */
	struct list_head prio_list;	/* Blocks that may be GC'ed. */

	unsigned long phy_addr_start;	/* References the physical start block */
	unsigned int phy_addr_end;		/* References the physical end block */

	unsigned int nr_blocks;			/* Derived value from end_block - start_block. */
	unsigned int nr_free_blocks;	/* Number of unused blocks */

	struct nvm_block *blocks;
	struct openssd *os;

	/* Postpone issuing I/O if append point is active */
	atomic_t is_active;

	spinlock_t waiting_lock;
	struct work_struct waiting_ws;
	struct bio_list waiting_bios;

	unsigned int gc_running;
	struct completion gc_finished;
	struct work_struct gc_ws;
};

/*
 * nvm_ap. ap is an append point. A pool can have 1..X append points attached.
 * An append point has a current block, that it writes to, and when its full, it requests
 * a new block, of which it continues its writes.
 *
 * one ap per pool may be reserved for pack-hints related writes. 
 * In those that are not not, hint_private is NULL.
 */
struct nvm_ap {
	spinlock_t lock;
	struct openssd *parent;
	struct nvm_pool *pool;
	struct nvm_block *cur;
	struct nvm_block *gc_cur;

	/* Timings used for end_io waiting */
	unsigned long t_read;
	unsigned long t_write;
	unsigned long t_erase;

	unsigned long io_delayed;
	unsigned long io_accesses[2];

	/* Hint related*/
	void *hint_private;
};

struct nvm_config {
	unsigned long flags;

	unsigned int gc_time;		/* GC every X microseconds */

	unsigned int t_read;
	unsigned int t_write;
	unsigned int t_erase;
};

struct openssd;

typedef struct nvm_addr *(map_ltop_fn)(struct openssd *, sector_t, int, void *);
typedef struct nvm_addr *(lookup_ltop_fn)(struct openssd *, sector_t);
typedef sector_t (lookup_ptol_fn)(struct openssd *, sector_t);
typedef int (write_bio_fn)(struct openssd *, struct bio *);
typedef int (read_bio_fn)(struct openssd *, struct bio *);
typedef void (alloc_phys_addr_fn)(struct openssd *, struct nvm_block *);
typedef void *(begin_gc_private_fn)(sector_t, sector_t, struct nvm_block *);
typedef void (end_gc_private_fn)(void *);

/* Main structure */
struct openssd {
	struct dm_dev *dev;
	struct dm_target *ti;
	uint32_t sector_size;

	/* Simple translation map of logical addresses to physical addresses. The
	 * logical addresses is known by the host system, while the physical
	 * addresses are used when writing to the disk block device. */
	struct nvm_addr *trans_map;
	/* also store a reverse map for garbage collection */
	sector_t *rev_trans_map;

	spinlock_t trans_lock;
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
	struct nvm_pool *pools;

	/* Append points */
	struct nvm_ap *aps;

	mempool_t *per_bio_pool;
	mempool_t *page_pool;
	mempool_t *block_page_pool;

	/* Frequently used config variables */
	int nr_pools;
	int nr_blks_per_pool;
	int nr_pages_per_blk;
	int nr_aps;
	int nr_aps_per_pool;

	/* Calculated values */
	unsigned int nr_host_pages_in_blk;
	unsigned long nr_pages;

	unsigned int next_collect_pool;

	/* Engine interface */
	map_ltop_fn *map_ltop;
	lookup_ltop_fn *lookup_ltop;
	lookup_ptol_fn *lookup_ptol;
	write_bio_fn *write_bio;
	read_bio_fn *read_bio;
	alloc_phys_addr_fn *alloc_phys_addr;
	begin_gc_private_fn *begin_gc_private;
	end_gc_private_fn *end_gc_private;

	/* Write strategy variables. Move these into each for structure for each
	 * strategy */
	atomic_t next_write_ap; /* Whenever a page is written, this is updated to point
							   to the next write append point */

	struct workqueue_struct *kbiod_wq;
	struct workqueue_struct *kgc_wq;

	spinlock_t deferred_lock;
	struct work_struct deferred_ws;
	struct bio_list deferred_bios;

	struct timer_list gc_timer;

	/* in-flight data lookup, lookup by logical address */
	struct hlist_head *inflight;

	/* Hint related*/
	void *hint_private;

	/* Configuration */
	struct nvm_config config;
};

struct per_bio_data {
	struct nvm_ap *ap;
	struct nvm_addr *addr;
	struct timeval start_tv;
	sector_t physical_addr;

	// Hook up for our overwritten bio fields
	bio_end_io_t *bi_end_io;
	void *bi_private;
	struct completion event;
	unsigned int sync;
};

/* dm-openssd-c */

/*   Helpers */
void invalidate_block_page(struct openssd *os, struct nvm_addr *p);
void openssd_set_ap_cur(struct nvm_ap *ap, struct nvm_block *block);
struct nvm_block *nvm_pool_get_block(struct nvm_pool *pool, int is_gc);
sector_t openssd_alloc_phys_addr(struct nvm_block *block);
sector_t openssd_alloc_phys_fastest_addr(struct openssd *os, struct nvm_block **ret_victim_block);

/*   Naive implementations */
void openssd_delayed_bio_submit(struct work_struct *work);
void openssd_deferred_bio_submit(struct work_struct *work);

/* Allocation of physical addresses from block when increasing responsibility. */
sector_t openssd_alloc_addr_from_ap(struct nvm_ap *ap, struct nvm_block **ret_victim_block, int is_gc);
struct nvm_addr *openssd_alloc_map_ltop_rr(struct openssd *os, sector_t logical_addr, int is_gc, void *private);
sector_t openssd_alloc_ltop_rr(struct openssd *os, sector_t l_addr, struct nvm_block **ret_victim_block, int is_gc, void *private);

/* Calls map_ltop_rr. Cannot fail (FIXME: unless out of memory) */
struct nvm_addr *openssd_alloc_addr(struct openssd *os, sector_t logical_addr, int is_gc, void *private);

/* Gets an address from os->trans_map and take a ref count on the blocks usage. Remember to put later */
struct nvm_addr *openssd_lookup_ltop_map(struct openssd *os, sector_t l_addr, struct nvm_addr *l2p_map);
struct nvm_addr *openssd_lookup_ltop(struct openssd *os, sector_t logical_addr);
sector_t openssd_lookup_ptol(struct openssd *os, sector_t physical_addr);

/*   I/O bio related */
void openssd_submit_bio(struct openssd *os, struct nvm_addr *p, int rw, struct bio *bio, int sync);
struct bio *openssd_write_init_bio(struct openssd *os, struct bio *bio, struct nvm_addr *p);
int openssd_bv_copy(struct nvm_addr *p, struct bio_vec *bv);
int openssd_write_bio_generic(struct openssd *os, struct bio *bio);
int openssd_write_execute_bio(struct openssd *os, struct bio *bio, int is_gc, void *private);
int openssd_read_bio_generic(struct openssd *os, struct bio *bio);
struct nvm_addr *openssd_update_map(struct openssd *os,  sector_t l_addr,
				    sector_t p_addr, struct nvm_block *p_block);

/*   NVM device related */
void openssd_block_release(struct kref *);

/*   Block maintanence */

void nvm_pool_put_block(struct nvm_block *block);
void openssd_reset_block(struct nvm_block *block);

/* dm-openssd-gc.c */
void openssd_block_erase(struct kref *);
void openssd_gc_cb(unsigned long data);
void openssd_gc_collect(struct work_struct *work);
void openssd_gc_kick(struct nvm_pool *pool);


/* dm-openssd-hint.c */
int openssd_alloc_hint(struct openssd *);
int openssd_init_hint(struct openssd *);
void openssd_exit_hint(struct openssd *);
void openssd_free_hint(struct openssd *);

/*   Hint core */
int openssd_ioctl_hint(struct openssd *os, unsigned int cmd, unsigned long arg);

/*   Callbacks */
void openssd_delay_endio_hint(struct openssd *os, struct bio *bio, struct per_bio_data *pb, unsigned long *delay);
void openssd_bio_hint(struct openssd *os, struct bio *bio);

#define ssd_for_each_pool(openssd, pool, i)									\
		for ((i) = 0, pool = &(openssd)->pools[0];							\
			 (i) < (openssd)->nr_pools; (i)++, pool = &(openssd)->pools[(i)])

#define ssd_for_each_ap(openssd, ap, i)										\
		for ((i) = 0, ap = &(openssd)->aps[0];								\
			 (i) < (openssd)->nr_aps; (i)++, ap = &(openssd)->aps[(i)])

#define pool_for_each_block(pool, block, i)									\
		for ((i) = 0, block = &(pool)->blocks[0];							\
			 (i) < (pool)->nr_blocks; (i)++, block = &(pool)->blocks[(i)])

static inline struct nvm_ap *get_next_ap(struct openssd *os) {
	return &os->aps[atomic_inc_return(&os->next_write_ap) % os->nr_aps];
}

static inline int block_is_full(struct nvm_block *block)
{
	struct openssd *os = block->pool->os;
	return ((block->next_page * NR_HOST_PAGES_IN_FLASH_PAGE) + block->next_offset == os->nr_host_pages_in_blk);
}

static inline sector_t block_to_addr(struct nvm_block *block)
{
	struct openssd *os;
	BUG_ON(!block);
	os = block->pool->os;
	return block->id * os->nr_host_pages_in_blk;
}

static inline int page_is_fast(unsigned int pagenr, struct openssd *os)
{
	/* pages: F F F F | SSFFSS | SSFFSS | ... | S S S S . S Slow F Fast */
	if (pagenr < 4)
		return 1;

	if (pagenr >= os->nr_pages_per_blk - 4)
		return 0;

	pagenr -= 4;
	pagenr %= 4;

	if (pagenr == 2 || pagenr == 3) 
		return 1;
	
	return 0;
}

static inline struct nvm_ap *block_to_ap(struct openssd *os, struct nvm_block *block) {
	unsigned int ap_idx, div, mod;

	div = block->id / os->nr_blks_per_pool;
	mod = block->id % os->nr_blks_per_pool;
	ap_idx = div + (mod / (os->nr_blks_per_pool / os->nr_aps_per_pool));

	return &os->aps[ap_idx];
}

static inline int physical_to_slot(struct openssd *os, sector_t phys)
{
	return (phys % (os->nr_pages_per_blk * NR_HOST_PAGES_IN_FLASH_PAGE)) / NR_HOST_PAGES_IN_FLASH_PAGE;
}

#endif

#endif /* DM_OPENSSD_H_ */

