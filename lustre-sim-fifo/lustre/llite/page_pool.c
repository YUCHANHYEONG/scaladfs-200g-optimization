// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "llite_internal.h"

/*
 * 65,536 pages = 256 MiB with 4 KiB pages.
 * This is a temporary limit for the initial prototype.
 */
#define LL_PAGE_POOL_MAX_PAGES	65536UL

void ll_page_pool_init(struct ll_page_pool *pool)
{
	printk("[%s] start! from %ps\n",
	       __func__, __builtin_return_address(0));

	spin_lock_init(&pool->lpp_lock);
	INIT_LIST_HEAD(&pool->lpp_pages);

	pool->lpp_nr_pages = 0;
	pool->lpp_max_pages = LL_PAGE_POOL_MAX_PAGES;
}

void ll_page_pool_fini(struct ll_page_pool *pool)
{
	struct page *page;
	struct page *next;
	LIST_HEAD(release_pages);

	printk("[%s] start! from %ps, nr_pages=%lu\n",
	       __func__, __builtin_return_address(0),
	       pool->lpp_nr_pages);

	/* Move remaining pages to a temporary list. */
	spin_lock(&pool->lpp_lock);

	list_splice_init(&pool->lpp_pages, &release_pages);
	pool->lpp_nr_pages = 0;

	spin_unlock(&pool->lpp_lock);

	/* Return remaining pages to the buddy allocator. */
	list_for_each_entry_safe(page, next, &release_pages, lru) {
		list_del_init(&page->lru);
		__free_page(page);
	}
}

struct page *ll_page_pool_get(struct ll_page_pool *pool)
{
	struct page *page = NULL;
	printk_once("[%s] start! from %ps\n",
			__func__, __builtin_return_address(0));

	spin_lock(&pool->lpp_lock);

	if (!list_empty(&pool->lpp_pages)) {
		page = list_first_entry(&pool->lpp_pages,
					struct page,
					lru);

		list_del_init(&page->lru);
		pool->lpp_nr_pages--;
	}

	spin_unlock(&pool->lpp_lock);

	return page;
}

bool ll_page_pool_put(struct ll_page_pool *pool, struct page *page)
{
	bool inserted = false;

	spin_lock(&pool->lpp_lock);

	if (pool->lpp_nr_pages < pool->lpp_max_pages) {
		/*
		 * Keep the existing page reference while the page
		 * is stored in the pool.
		 */
		list_add(&page->lru, &pool->lpp_pages);
		pool->lpp_nr_pages++;
		inserted = true;
	}

	spin_unlock(&pool->lpp_lock);

	return inserted;
}
