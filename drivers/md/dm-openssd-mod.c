/*
 * Copyright (C) 2012 Matias Bjørling.
 *
 * This file is released under GPL.
 *
 * Todo
 *
 * - Implement fetching of bad pages from flash
 *
 * Hints
 * - configurable sector size
 * - handle case of in-page bv_offset (currently hidden assumption of offset=0,
 *   and bv_len spans entire page)
 *
 * Optimization possibilities
 * - Move ap_next_write into a conconcurrency friendly data structure. Could be
 *   handled by more intelligent map_ltop function.
 * - Implement per-cpu nvm_block data structure ownership. Removes need
 *   for taking lock on block next_write_id function. I.e. page allocation
 *   becomes nearly lockless, with occasionally movement of blocks on
 *   nvm_block lists.
 */

#include "dm-openssd.h"
#include "dm-openssd-hint.h"

/* Defaults */
/* Number of append points per pool. We assume that accesses within a pool is
 * serial (NAND flash/PCM/etc.) */
#define APS_PER_POOL 1

/* If enabled, we delay bios on each ap to run serialized. */
#define SERIALIZE_POOL_ACCESS 0

/* Sleep timings before simulating device specific storage (in us)*/
#define TIMING_READ 25
#define TIMING_WRITE 500
#define TIMING_ERASE 1500

/* Run GC every X seconds */
#define GC_TIME 10

#define MIN_POOL_PAGES 16

static struct kmem_cache *_per_bio_cache;

static int nvm_ioctl(struct dm_target *ti, unsigned int cmd,
                         unsigned long arg)
{
	struct nvmd *nvmd = ti->private;

	DMDEBUG("got ioctl %x\n", cmd);

	switch (cmd) {
	case LIGHTNVM_IOCTL_ID:
		return 12345678; // TODO: anything else?
		break;
	}

	return nvm_ioctl_hint(nvmd, cmd, arg);
}

static int nvm_map(struct dm_target *ti, struct bio *bio)
{
	struct nvmd *nvmd = ti->private;
	int ret;

	if(bio->bi_sector < 0 || bio->bi_sector / NR_PHY_IN_LOG >= nvmd->nr_pages){
		DMERR("ERROR - %s illegal address %ld", (bio_data_dir(bio) == WRITE) ? "WRITE" : "READ", bio->bi_sector / NR_PHY_IN_LOG );
		bio_io_error(bio);
		return DM_MAPIO_SUBMITTED;
	};

	bio->bi_bdev = nvmd->dev->bdev;

	/*DMINFO("map: %s l: %llu size %d",
			(bio_data_dir(bio) == WRITE) ? "WRITE" : "READ",
			(unsigned long long) bio->bi_sector / NR_PHY_IN_LOG,
			bio->bi_size);*/

	if (bio_data_dir(bio) == WRITE) {
		if (bio_sectors(bio) != NR_PHY_IN_LOG) {
			DMERR("Write: num of sectors not supported (%u)", bio_sectors(bio));
			return DM_MAPIO_REQUEUE;
		}
		ret = nvmd->write_bio(nvmd, bio);
	} else {
		ret = nvmd->read_bio(nvmd, bio);
	}

	return ret;
}

static void nvm_status(struct dm_target *ti, status_type_t type,
                           unsigned status_flags, char *result, unsigned maxlen)
{
	struct nvmd *nvmd = ti->private;
	struct nvm_ap *ap;
	int i, sz = 0;

	switch(type) {
	case STATUSTYPE_INFO:
		DMEMIT("Use table information");
		break;
	case STATUSTYPE_TABLE:
		ssd_for_each_ap(nvmd, ap, i) {
			DMEMIT("Reads: %lu Writes: %lu Delayed: %lu",
			       ap->io_accesses[0], ap->io_accesses[1],
			       ap->io_delayed);
		}
		break;
	}
}

static int nvm_pool_init(struct nvmd *nvmd, struct dm_target *ti)
{
	struct nvm_pool *pool;
	struct nvm_block *block;
	struct nvm_ap *ap;
	int i, j;

	spin_lock_init(&nvmd->trans_lock);
	spin_lock_init(&nvmd->deferred_lock);
	INIT_WORK(&nvmd->deferred_ws, nvm_deferred_bio_submit);
	bio_list_init(&nvmd->deferred_bios);

	nvmd->pools = kzalloc(sizeof(struct nvm_pool) * nvmd->nr_pools, GFP_KERNEL);
	if (!nvmd->pools)
		goto err_pool;

	ssd_for_each_pool(nvmd, pool, i) {
		spin_lock_init(&pool->lock);
		spin_lock_init(&pool->gc_lock);
		spin_lock_init(&pool->waiting_lock);

		init_completion(&pool->gc_finished);

		INIT_WORK(&pool->gc_ws, nvm_gc_collect);
		INIT_WORK(&pool->waiting_ws, nvm_delayed_bio_submit);

		INIT_LIST_HEAD(&pool->free_list);
		INIT_LIST_HEAD(&pool->used_list);
		INIT_LIST_HEAD(&pool->prio_list);

		pool->nvmd = nvmd;
		pool->phy_addr_start = i * nvmd->nr_blks_per_pool;
		pool->phy_addr_end = (i + 1) * nvmd->nr_blks_per_pool - 1;
		pool->nr_free_blocks = pool->nr_blocks = pool->phy_addr_end - pool->phy_addr_start + 1;

		bio_list_init(&pool->waiting_bios);
		atomic_set(&pool->is_active, 0);

		pool->blocks = kzalloc(sizeof(struct nvm_block) * pool->nr_blocks, GFP_KERNEL);
		if (!pool->blocks)
			goto err_blocks;

		pool_for_each_block(pool, block, j) {
			spin_lock_init(&block->lock);
			atomic_set(&block->gc_running, 0);

			block->pool = pool;
			block->id = (i * nvmd->nr_blks_per_pool) + j;

			list_add_tail(&block->list, &pool->free_list);
		}
	}

	nvmd->nr_aps = nvmd->nr_aps_per_pool * nvmd->nr_pools;
	nvmd->aps = kzalloc(sizeof(struct nvm_ap) * nvmd->nr_pools *nvmd->nr_aps, GFP_KERNEL);
	if (!nvmd->aps)
		goto err_blocks;

	ssd_for_each_ap(nvmd, ap, i) {
		spin_lock_init(&ap->lock);
		ap->parent = nvmd;
		ap->pool = &nvmd->pools[i / nvmd->nr_aps_per_pool];

		block = nvm_pool_get_block(ap->pool, 0);
		nvm_set_ap_cur(ap, block);
		/* Emergency gc block */
		block = nvm_pool_get_block(ap->pool, 1);
		ap->gc_cur = block;

		ap->t_read = nvmd->config.t_read;
		ap->t_write = nvmd->config.t_write;
		ap->t_erase = nvmd->config.t_erase;
	}

	nvmd->kbiod_wq = alloc_workqueue("knvm-work", WQ_MEM_RECLAIM, 0);
	if (!nvmd->kbiod_wq) {
		DMERR("Couldn't start knvm-worker");
		goto err_blocks;
	}

	nvmd->kgc_wq = alloc_workqueue("knvm-gc", WQ_MEM_RECLAIM, 0);
	if (!nvmd->kgc_wq) {
		DMERR("Couldn't start knvm-gc");
		destroy_workqueue(nvmd->kgc_wq);
		goto err_blocks;
	}

	return 0;
err_blocks:
	ssd_for_each_pool(nvmd, pool, i) {
		if (!pool->blocks)
			break;
		kfree(pool->blocks);
	}
	kfree(nvmd->pools);
err_pool:
	ti->error = "dm-lightnvm: Cannot allocate lightnvm data structures";
	return -ENOMEM;
}

static int nvm_init(struct dm_target *ti, struct nvmd *nvmd)
{
	int i;
	unsigned int order;

	nvmd->nr_host_pages_in_blk = NR_HOST_PAGES_IN_FLASH_PAGE * nvmd->nr_pages_per_blk;
	nvmd->nr_pages = nvmd->nr_pools * nvmd->nr_blks_per_pool * nvmd->nr_host_pages_in_blk;
	order = ffs(nvmd->nr_host_pages_in_blk) - 1;

	/* Invalid pages in block bitmap is preallocated. */
	if (nvmd->nr_host_pages_in_blk > MAX_INVALID_PAGES_STORAGE * BITS_PER_LONG)
		return -EINVAL;

	nvmd->trans_map = vmalloc(sizeof(struct nvm_addr) * nvmd->nr_pages);
	if (!nvmd->trans_map)
		return -ENOMEM;
	memset(nvmd->trans_map, 0, sizeof(struct nvm_addr) * nvmd->nr_pages);

	// initial l2p is LTOP_EMPTY
	for (i = 0; i < nvmd->nr_pages; i++) {
		struct nvm_addr *p = &nvmd->trans_map[i];
		p->addr = LTOP_EMPTY;
		atomic_set(&p->inflight, 0);
	}

	nvmd->rev_trans_map = vmalloc(sizeof(sector_t) * nvmd->nr_pages);
	if (!nvmd->rev_trans_map)
		goto err_rev_trans_map;

	nvmd->per_bio_pool = mempool_create_slab_pool(16, _per_bio_cache);
	if (!nvmd->per_bio_pool)
		goto err_dev_lookup;

	nvmd->page_pool = mempool_create_page_pool(MIN_POOL_PAGES, 0);
	if (!nvmd->page_pool)
		goto err_per_bio_pool;

	nvmd->block_page_pool = mempool_create_page_pool(nvmd->nr_aps, order);
	if (!nvmd->block_page_pool)
		goto err_page_pool;

	if (bdev_physical_block_size(nvmd->dev->bdev) > EXPOSED_PAGE_SIZE) {
		ti->error = "dm-lightnvm: Got bad sector size. Device sector size \
			is larger than exposed";
		goto err_block_page_pool;
	}
	nvmd->sector_size = EXPOSED_PAGE_SIZE;

	// Simple round-robin strategy
	atomic_set(&nvmd->next_write_ap, -1);

	nvmd->lookup_ltop = nvm_lookup_ltop;
	nvmd->lookup_ptol = nvm_lookup_ptol;
	nvmd->map_ltop = nvm_alloc_map_ltop_rr;
	nvmd->write_bio = nvm_write_bio;
	nvmd->read_bio = nvm_read_bio;

	nvmd->ti = ti;
	ti->private = nvmd;

	/* Initialize pools. */
	nvm_pool_init(nvmd, ti);

	if (nvm_alloc_hint(nvmd))
		goto err_block_page_pool;

	// FIXME: Clean up pool init on failure.
	setup_timer(&nvmd->gc_timer, nvm_gc_cb, (unsigned long)nvmd);
	mod_timer(&nvmd->gc_timer, jiffies + msecs_to_jiffies(1000));

	return 0;
err_block_page_pool:
	mempool_destroy(nvmd->block_page_pool);
err_page_pool:
	mempool_destroy(nvmd->page_pool);
err_per_bio_pool:
	mempool_destroy(nvmd->per_bio_pool);
err_dev_lookup:
	vfree(nvmd->rev_trans_map);
err_rev_trans_map:
	vfree(nvmd->trans_map);
	return -ENOMEM;
}

/*
 * Accepts an LightNVM-backed block-device. The LightNVM device should run the
 * corresponding physical firmware that exports the flash as physical without any
 * mapping and garbage collection as it will be taken care of.
 */
static int nvm_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	struct nvmd *nvmd;
	unsigned int tmp;
	char dummy;

	// Which device it should map onto?
	if (argc < 5) {
		ti->error = "Insufficient arguments";
		return -EINVAL;
	}

	nvmd = kzalloc(sizeof(*nvmd), GFP_KERNEL);
	if (!nvmd) {
		ti->error = "Cannot allocate data structures";
		return -ENOMEM;
	}

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &nvmd->dev))
		goto err_map;

	dm_set_target_max_io_len(ti, NR_PHY_IN_LOG);

	if (!strcmp(argv[1], "swap"))
		nvmd->config.flags |= NVM_OPT_ENGINE_SWAP;
	else if (!strcmp(argv[1], "latency"))
		nvmd->config.flags |=
		        (NVM_OPT_ENGINE_LATENCY | NVM_OPT_ENGINE_IOCTL);
	else if (!strcmp(argv[1], "pack"))
		nvmd->config.flags |=
		        (NVM_OPT_ENGINE_PACK | NVM_OPT_ENGINE_IOCTL);

	if (sscanf(argv[2], "%u%c", &tmp, &dummy) != 1) {
		ti->error = "Cannot read number of pools";
		goto err_map;
	}
	nvmd->nr_pools = tmp;

	if (sscanf(argv[3], "%u%c", &tmp, &dummy) != 1) {
		ti->error = "Cannot read number of blocks within a pool";
		goto err_map;
	}
	nvmd->nr_blks_per_pool = tmp;

	if (sscanf(argv[4], "%u%c", &tmp, &dummy) != 1) {
		ti->error = "Cannot read number of pages within a block";
		goto err_map;
	}
	nvmd->nr_pages_per_blk = tmp;

	/* Optional */
	nvmd->nr_aps_per_pool = APS_PER_POOL;
	if (argc > 5) {
		if (sscanf(argv[5], "%u%c", &tmp, &dummy) == 1) {
			if (!tmp) {
				DMERR("Number of aps set to 1.");
				tmp = APS_PER_POOL;
			}
			nvmd->nr_aps_per_pool = tmp;
		} else {
			ti->error = "Cannot read number of append points";
			goto err_map;
		}
	}

	if (argc > 6) {
		if (sscanf(argv[6], "%u%c", &tmp, &dummy) == 1) {
			nvmd->config.flags |= (tmp << NVM_OPT_MISC_OFFSET);
		} else {
			ti->error = "Cannot read flags";
			goto err_map;
		}
	}

	nvmd->config.gc_time = GC_TIME;
	if (argc > 7) {
		if (sscanf(argv[7], "%u%c", &tmp, &dummy) == 1) {
			nvmd->config.gc_time = tmp;
			if (nvmd->config.gc_time <= 0)
				nvmd->config.gc_time = 1000;
		} else {
			ti->error = "Cannot read gc timing";
			goto err_map;
		}
	}

	nvmd->config.t_read = TIMING_READ;
	if (argc > 8) {
		if (sscanf(argv[8], "%u%c", &tmp, &dummy) == 1) {
			nvmd->config.t_read = tmp;
		} else {
			ti->error = "Cannot read read access timing";
			goto err_map;
		}
	}

	nvmd->config.t_write = TIMING_WRITE;
	if (argc > 9) {
		if (sscanf(argv[9], "%u%c", &tmp, &dummy) == 1) {
			nvmd->config.t_write = tmp;
		} else {
			ti->error = "Cannot read write access timing";
			goto err_map;
		}
	}

	nvmd->config.t_erase = TIMING_ERASE;
	if (argc > 10) {
		if (sscanf(argv[10], "%u%c", &tmp, &dummy) == 1) {
			nvmd->config.t_erase = tmp;
		} else {
			ti->error = "Cannot read erase access timing";
			goto err_map;
		}
	}

	if (nvm_init(ti, nvmd) < 0) {
		ti->error = "Cannot initialize lightnvm structure";
		goto err_map;
	}

	DMINFO("Configured with");
	DMINFO("Pools: %u Blocks: %u Pages: %u Host Pages: %u \
			Aps: %u Aps Pool: %u",
	       nvmd->nr_pools,
	       nvmd->nr_blks_per_pool,
	       nvmd->nr_pages_per_blk,
	       nvmd->nr_host_pages_in_blk,
	       nvmd->nr_aps,
	       nvmd->nr_aps_per_pool);
	DMINFO("Timings: %u/%u/%u", nvmd->config.t_read, nvmd->config.t_write,
			nvmd->config.t_erase);
	DMINFO("Target sector size=%d", nvmd->sector_size);
	DMINFO("Disk logical sector size=%d",
	       bdev_logical_block_size(nvmd->dev->bdev));
	DMINFO("Disk physical sector size=%d",
	       bdev_physical_block_size(nvmd->dev->bdev));
	DMINFO("Disk flash page size=%d", FLASH_PAGE_SIZE);
	DMINFO("Allocated %lu physical pages (%lu KB)",
	       nvmd->nr_pages, nvmd->nr_pages * nvmd->sector_size / 1024);

	return 0;
err_map:
	kfree(nvmd);
	return -ENOMEM;
}

static void nvm_dtr(struct dm_target *ti)
{
	struct nvmd *nvmd = ti->private;
	struct nvm_pool *pool;
	int i;

	nvm_free_hint(nvmd);

	ssd_for_each_pool(nvmd, pool, i) {
		while (bio_list_peek(&pool->waiting_bios))
			flush_scheduled_work();
	}

	/* TODO: remember outstanding block refs, waiting to be erased... */
	ssd_for_each_pool(nvmd, pool, i)
		kfree(pool->blocks);

	kfree(nvmd->pools);
	kfree(nvmd->aps);

	vfree(nvmd->trans_map);
	vfree(nvmd->rev_trans_map);

	del_timer(&nvmd->gc_timer);

	destroy_workqueue(nvmd->kbiod_wq);
	destroy_workqueue(nvmd->kgc_wq);
	mempool_destroy(nvmd->per_bio_pool);
	mempool_destroy(nvmd->page_pool);

	dm_put_device(ti, nvmd->dev);

	kfree(nvmd);

	DMINFO("dm-lightnvm successfully unloaded");
}

static struct target_type lightnvm_target = {
	.name		= "lightnvm",
	.version	= {1, 0, 0},
	.module		= THIS_MODULE,
	.ctr		= nvm_ctr,
	.dtr		= nvm_dtr,
	.map		= nvm_map,
	.ioctl		= nvm_ioctl,
	.status		= nvm_status,
};

static int __init dm_lightnvm_init(void)
{
	int ret = -ENOMEM;

	_per_bio_cache = kmem_cache_create("lightnvm_per_bio_cache",
	                                   sizeof(struct per_bio_data), 0, 0, NULL);
	if (!_per_bio_cache)
		return ret;

	ret = dm_register_target(&lightnvm_target);
	if (ret < 0)
		DMERR("register failed %d", ret);

	return ret;
}

static void __exit dm_lightnvm_exit(void)
{
	kmem_cache_destroy(_per_bio_cache);
	dm_unregister_target(&lightnvm_target);
}

module_init(dm_lightnvm_init);
module_exit(dm_lightnvm_exit);

MODULE_DESCRIPTION(DM_NAME " target");
MODULE_AUTHOR("Matias Bjorling <m@bjorling.me>");
MODULE_LICENSE("GPL");
