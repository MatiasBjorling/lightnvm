#include "dm-openssd.h"
#include "dm-openssd-hint.h"

/* Run only GC if less than 1/X blocks are free */
#define GC_LIMIT_INVERSE 2

static void __erase_block(struct nvm_block *block)
{
	// Perform device erase
}

/* the block with highest number of invalid pages, will be in the beginning of the list */
static int block_prio_sort_cmp(void *priv, struct list_head *lh_a, struct list_head *lh_b)
{
	struct nvm_block *a = list_entry(lh_a, struct nvm_block, prio);
	struct nvm_block *b = list_entry(lh_b, struct nvm_block, prio);

	if (a->nr_invalid_pages == b->nr_invalid_pages)
		return 0;

	return a->nr_invalid_pages < b->nr_invalid_pages;
}

/* linearly find the block with highest number of invalid pages */
static struct nvm_block * block_prio_find_max(struct list_head *pool_block_list)
{
	struct list_head *lh, *lh_max = NULL;

	lh_max = pool_block_list->next;
	list_for_each(lh, pool_block_list) {
		if (block_prio_sort_cmp(NULL, lh_max, lh)) {
			lh_max = lh;
		}
	}
	//DMINFO("GC max: return block with max invalid %d", list_entry(lh_max, struct nvm_block, prio)->nr_invalid_pages);

	return list_entry(lh_max, struct nvm_block, prio);
}

/* Move data away from flash block to be erased. Additionally update the l to p and p to l
 * mappings.
 */
static void openssd_move_valid_pages(struct openssd *os, struct nvm_block *block)
{
	struct bio *src_bio;
	struct page *page;
	struct nvm_block* victim_block;
	int slot = -1;
	sector_t p_addr, l_addr, dst_addr;
	int i;
	struct bio_vec *bv;
	void *gc_private = NULL;

	if (bitmap_full(block->invalid_pages, os->nr_host_pages_in_blk))
		return;

	page = alloc_page(GFP_NOIO);
	while ((slot = find_next_zero_bit(block->invalid_pages, os->nr_host_pages_in_blk, slot + 1)) < os->nr_host_pages_in_blk) {
		/* Perform read */
		p_addr = block_to_addr(block) + slot;

		/* TODO: check for memory failure */
		src_bio = bio_alloc(GFP_NOIO, 1);

		src_bio->bi_bdev = os->dev->bdev;
		src_bio->bi_sector = p_addr * NR_PHY_IN_LOG;
		bio_add_page(src_bio, page, EXPOSED_PAGE_SIZE, 0);

		openssd_submit_bio(os, block, READ, src_bio, 1);

		/* Perform write */
		/* We use the physical address to go to the logical page addr,
		 * and then update its mapping to its new place. */
		l_addr = os->lookup_ptol(os, p_addr);
		/* DMDEBUG("move page p_addr=%ld l_addr=%ld (map[%ld]=%ld)", p_addr, l_addr, l_addr, os->trans_map[l_addr].addr);*/

		if (os->begin_gc_private)
			gc_private = os->begin_gc_private(l_addr, p_addr, block);

		dst_addr = os->map_ltop(os, l_addr, &victim_block, gc_private);

		if (os->end_gc_private)
			os->end_gc_private(gc_private);

		/* Write using regular write machanism */
		bio_for_each_segment(bv, src_bio, i) {
			unsigned int size = openssd_handle_buffered_write(dst_addr, victim_block, bv);
			if (size % NR_HOST_PAGES_IN_FLASH_PAGE == 0)
				openssd_submit_write(os, dst_addr, victim_block, size);
		}

		bio_put(src_bio);
	}
	__free_page(page);
	BUG_ON(!bitmap_full(block->invalid_pages, os->nr_host_pages_in_blk));
}

/* Push erase condition to automatically be executed when block goes to zero.
 * Only GC should do this */
void openssd_block_release(struct kref *ref)
{
	struct nvm_block *block = container_of(ref, struct nvm_block, ref_count);

	__erase_block(block);

	spin_unlock(&block->gc_lock);
	nvm_pool_put_block(block);
}

int openssd_gc_collect(struct openssd *os)
{
	struct nvm_pool *pool;
	struct nvm_block *block;
	unsigned int nr_blocks_need;
	int pid, pid_start;

	if (!spin_trylock(&os->gc_lock)) 
		return 1;

	block = NULL;
	/* Iterate the pools once to look for pool that has a block to be freed. */
	pid = os->next_collect_pool % os->nr_pools;
	pid_start = pid;
	do {
		pool = &os->pools[pid];

		nr_blocks_need = pool->nr_blocks;
		do_div(nr_blocks_need, GC_LIMIT_INVERSE);

		//DMINFO("pool_id=%d nr_blocks_need %d pool->nr_free_blocks %d", pid, nr_blocks_need, pool->nr_free_blocks);
		if (nr_blocks_need < pool->nr_free_blocks)
			goto finished;

		spin_lock(&pool->lock);
		block = block_prio_find_max(&pool->prio_list);
		list_del(&block->prio);

		/* this should never happen. Its just here for an extra check */
		if (!block->nr_invalid_pages) {
			list_add(&block->prio, &pool->prio_list);
			spin_unlock(&pool->lock);
			goto finished;
		}

		spin_unlock(&pool->lock);

		/* this should never happen. Anyway, lets check for it.*/
		BUG_ON(!block_is_full(block));

		/* take the lock. But also make sure that we haven't messed up the 
		 * gc routine, by removing the global gc lock. */
		BUG_ON(!spin_trylock(&block->gc_lock));

		/* rewrite to have moves outside lock. i.e. so we can
		 * prepare multiple pages in parallel on the attached
		 * device. */
		openssd_move_valid_pages(os, block);

		kref_put(&block->ref_count, openssd_block_release);
finished:
		pid++;
		pid %= os->nr_pools;
	} while (pid_start != pid);

	os->next_collect_pool++;

	spin_unlock(&os->gc_lock);

	complete_all(&os->gc_finished);

	return 0;
}
