// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>

#include "llite_internal.h"

void ll_page_pool_init(struct ll_page_pool *pool)
{
	printk("[%s] start! from %ps\n",
	       __func__, __builtin_return_address(0));

	spin_lock_init(&pool->lpp_lock);
	INIT_LIST_HEAD(&pool->lpp_pages);

	pool->lpp_nr_pages = 0;

	/* Set the limit when pool_put() is implemented. */
	pool->lpp_max_pages = 0;
}

void ll_page_pool_fini(struct ll_page_pool *pool)
{
	struct page *page;
	struct page *next;
	LIST_HEAD(release_pages);

	printk("[%s] start! from %ps\n",
	       __func__, __builtin_return_address(0));

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
