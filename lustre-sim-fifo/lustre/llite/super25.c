/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#define DEBUG_SUBSYSTEM S_LLITE

#define D_MOUNT (D_SUPER | D_CONFIG/*|D_WARNING */)

#include <linux/module.h>
#include <linux/types.h>
#include <linux/version.h>
#include <lustre_ha.h>
#include <lustre_dlm.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/random.h>
#include <lprocfs_status.h>
#include "llite_internal.h"
#include "vvp_internal.h"

#include "../include/calclock.h"
#include "../include/cl_object.h"
#include "../osc/osc_internal.h"
#include "../mdc/mdc_internal.h"
#include <linux/lflist.h>

#include "lustre_flusher.h"

static struct kmem_cache *ll_inode_cachep;
struct LNode *AllocInodeNode(struct list_head *inode);

static struct inode *ll_alloc_inode(struct super_block *sb)
{
	struct ll_inode_info *lli;
	struct inode *vfs_inode;
#ifdef HAVE_ALLOC_INODE_SB
	lli = alloc_inode_sb(sb, ll_inode_cachep, GFP_NOFS);
	if (!lli)
		return NULL;
	OBD_ALLOC_POST(lli, sizeof(*lli), "slab-alloced");
	memset(lli, 0, sizeof(*lli));
#else
	OBD_SLAB_ALLOC_PTR_GFP(lli, ll_inode_cachep, GFP_NOFS);
	if (!lli)
		return NULL;
#endif
	inode_init_once(&lli->lli_vfs_inode);
	lli->lli_open_thrsh_count = UINT_MAX;

	vfs_inode = &lli->lli_vfs_inode;
	lli->lli_lnode = AllocInodeNode(&vfs_inode->i_io_list);
	if (unlikely(!lli->lli_lnode))
		printk(KERN_ERR "[%s]: error!\n", __func__);

	return &lli->lli_vfs_inode;
}

static void ll_inode_destroy_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	struct ll_inode_info *ptr = ll_i2info(inode);
	llcrypt_free_inode(inode);

	kfree(ptr->lli_lnode);
	ptr->lli_lnode = NULL;

	OBD_SLAB_FREE_PTR(ptr, ll_inode_cachep);
}

static void ll_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, ll_inode_destroy_callback);
}

static int ll_drop_inode(struct inode *inode)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	int drop;

	if (!sbi->ll_inode_cache_enabled)
		return 1;

	drop = generic_drop_inode(inode);
	if (!drop)
		drop = llcrypt_drop_inode(inode);

	return drop;
}

/* exported operations */
const struct super_operations lustre_super_operations =
{
	.alloc_inode   = ll_alloc_inode,
	.destroy_inode = ll_destroy_inode,
	.drop_inode    = ll_drop_inode,
	.evict_inode   = ll_delete_inode,
	.put_super     = ll_put_super,
	.statfs        = ll_statfs,
	.umount_begin  = ll_umount_begin,
	.remount_fs    = ll_remount_fs,
	.show_options  = ll_show_options,
};

/* YCH	*/
extern int lustre_flusher_init(struct lustre_sb_info *lsi);
extern void lustre_flusher_exit(struct lustre_sb_info *lsi);
/* YCH	*/

/**
 * This is the entry point for the mount call into Lustre.
 * This is called when a client is mounted, and this is
 * where we start setting things up.
 *
 * @lmd2data data Mount options (e.g. -o flock,abort_recov)
 */
static int lustre_fill_super(struct super_block *sb, void *lmd2_data,
			     int silent)
{
	struct lustre_mount_data *lmd;
	struct lustre_sb_info *lsi;
	int rc;

	ENTRY;

	CDEBUG(D_MOUNT|D_VFSTRACE, "VFS Op: sb %p\n", sb);

	lsi = lustre_init_lsi(sb);
	pr_info("lustre_fill_super\n");
	if (!lsi)
		RETURN(-ENOMEM);
	lmd = lsi->lsi_lmd;

	/*
	 * Disable lockdep during mount, because mount locking patterns are
	 * 'special'.
	 */
	lockdep_off();

	/*
	 * LU-639: the OBD cleanup of last mount may not finish yet, wait here.
	 */
	obd_zombie_barrier();

	/* Figure out the lmd from the mount options */
	if (lmd_parse(lmd2_data, lmd)) {
		lustre_put_lsi(sb);
		GOTO(out, rc = -EINVAL);
	}

	if (!lmd_is_client(lmd)) {
#ifdef HAVE_SERVER_SUPPORT
		static bool printed;

		if (!printed) {
			LCONSOLE_WARN("%s: mounting server target with '-t lustre' deprecated, use '-t lustre_tgt'\n",
				      lmd->lmd_profile);
			printed = true;
		}
		rc = server_fill_super(sb);
#else
		rc = -ENODEV;
		CERROR("%s: This is client-side-only module, cannot handle server mount: rc = %d\n",
		       lmd->lmd_profile, rc);
		lustre_put_lsi(sb);
#endif
		GOTO(out, rc);
	}

	CDEBUG(D_MOUNT, "Mounting client %s\n", lmd->lmd_profile);
	rc = lustre_start_mgc(sb);
	if (rc) {
		lustre_common_put_super(sb);
		GOTO(out, rc);
	}
	/* Connect and start */
	rc = ll_fill_super(sb);

	/* YCH	*/
	rc = lustre_flusher_init(s2lsi(sb));
	if (rc) {
		CERROR("lustre flusher init failed: %d\n", rc);
		goto out;
	}
	/* YCH	*/

	/* ll_file_super will call lustre_common_put_super on failure,
	 * which takes care of the module reference.
	 *
	 * If error happens in fill_super() call, @lsi will be killed there.
	 * This is why we do not put it here.
	 */
out:
	if (rc) {
		CERROR("llite: Unable to mount %s: rc = %d\n",
		       s2lsi(sb) ? lmd->lmd_dev : "<unknown>", rc);
	} else {
		CDEBUG(D_SUPER, "%s: Mount complete\n",
		       lmd->lmd_dev);
	}
	lockdep_on();
	return rc;
}

/***************** FS registration ******************/
static struct dentry *lustre_mount(struct file_system_type *fs_type, int flags,
				   const char *devname, void *data)
{
	return mount_nodev(fs_type, flags, data, lustre_fill_super);
}

static void lustre_kill_super(struct super_block *sb)
{
	struct lustre_sb_info *lsi = s2lsi(sb);

	/* YCH	*/
	if (lsi && lsi->flusher_ctx)
		lustre_flusher_exit(lsi);
	/* YCH	*/

	if (lsi && !IS_SERVER(lsi))
		ll_kill_super(sb);

	kill_anon_super(sb);
}

/** Register the "lustre" fs type
 */
static struct file_system_type lustre_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "lustre",
	.mount		= lustre_mount,
	.kill_sb	= lustre_kill_super,
	.fs_flags	= FS_RENAME_DOES_D_MOVE,
};
MODULE_ALIAS_FS("lustre");

// extern int (*io_submit_one_in_lustre)(struct kioctx *ctx, struct iocb __user *user_iocb,
// 			 bool compat);
// extern int lustre_io_submit_one(struct kioctx *ctx, struct iocb __user *user_iocb,
// 			 bool compat);

extern struct lflist_head lf_b_more_io;

extern void lustre_iput(struct inode *inode);
extern void (*iput_in_lustre)(struct inode *inode);

// mempool_t *lustre_mempool;
#define POOL_SIZE 10000

static int __init lustre_init(void)
{
	int rc;
	unsigned long lustre_inode_cache_flags;

	printk("[%s]: 1\n", __func__);
	pr_info("Insert module \n");

	init_lflist_head(&lf_b_more_io);

	// io_submit_one_in_lustre = &lustre_io_submit_one;
	iput_in_lustre = NULL;

	BUILD_BUG_ON(sizeof(LUSTRE_VOLATILE_HDR) !=
		     LUSTRE_VOLATILE_HDR_LEN + 1);

	/* print an address of _any_ initialized kernel symbol from this
	 * module, to allow debugging with gdb that doesn't support data
	 * symbols from modules.*/
	CDEBUG(D_INFO, "Lustre client module (%p).\n",
	       &lustre_super_operations);

	lustre_inode_cache_flags = SLAB_HWCACHE_ALIGN | SLAB_RECLAIM_ACCOUNT |
				   SLAB_MEM_SPREAD;
#ifdef SLAB_ACCOUNT
	lustre_inode_cache_flags |= SLAB_ACCOUNT;
#endif

	ll_inode_cachep = kmem_cache_create("lustre_inode_cache",
					    sizeof(struct ll_inode_info),
					    0, lustre_inode_cache_flags, NULL);
	if (ll_inode_cachep == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	ll_file_data_slab = kmem_cache_create("ll_file_data",
						 sizeof(struct ll_file_data), 0,
						 SLAB_HWCACHE_ALIGN, NULL);
	if (ll_file_data_slab == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	pcc_inode_slab = kmem_cache_create("ll_pcc_inode",
					   sizeof(struct pcc_inode), 0,
					   SLAB_HWCACHE_ALIGN, NULL);
	if (pcc_inode_slab == NULL)
		GOTO(out_cache, rc = -ENOMEM);

	rc = llite_tunables_register();
	if (rc)
		GOTO(out_cache, rc);

	rc = vvp_global_init();
	if (rc != 0)
		GOTO(out_tunables, rc);

	cl_inode_fini_env = cl_env_alloc(&cl_inode_fini_refcheck,
					 LCT_REMEMBER | LCT_NOREF);
	if (IS_ERR(cl_inode_fini_env))
		GOTO(out_vvp, rc = PTR_ERR(cl_inode_fini_env));

	cl_inode_fini_env->le_ctx.lc_cookie = 0x4;

	rc = ll_xattr_init();
	if (rc != 0)
		GOTO(out_inode_fini_env, rc);

	rc = register_filesystem(&lustre_fs_type);
	if (rc)
		GOTO(out_xattr, rc);

	// lustre_mempool = mempool_create(POOL_SIZE, mempool_alloc_pages, mempool_free_pages, NULL);
    // if (!lustre_mempool) {
    //     printk(KERN_ERR "Failed to create page mempool\n");
    //     GOTO(out_xattr, rc);
    // }

	RETURN(0);

out_xattr:
	ll_xattr_fini();
out_inode_fini_env:
	cl_env_put(cl_inode_fini_env, &cl_inode_fini_refcheck);
out_vvp:
	vvp_global_fini();
out_tunables:
	llite_tunables_unregister();
out_cache:
	kmem_cache_destroy(ll_inode_cachep);
	kmem_cache_destroy(ll_file_data_slab);
	kmem_cache_destroy(pcc_inode_slab);
	return rc;
}

KTDEC(ll_file_write_iter);
KTDEC(cl_io_loop);
KTDEC(ll_file_io_generic);
KTDEC(cl_io_loop_internal);
KTDEC(cl_io_lock);
KTDEC(lov_io_lock);
KTDEC(lov_cls_todo_scan);
KTDEC(lov_lis_active_scan);
KTDEC(fast_ldlm_lock_addref_try);
KTDEC(addref_lock_res_and_lock);
KTDEC(ldlm_lock_addref_internal_nolock);
KTDEC(fast_lock_res_and_lock);
KTDEC(fast_ldlm_lock_addref_internal_nolock);
KTDEC(ldlm_lock_remove_from_lru);
KTDEC(lov_io_call);
KTDEC(cl_io_iter_init);
KTDEC(cl_io_start);
KTDEC(cl_io_end);
KTDEC(cl_io_unlock);
KTDEC(cl_io_iter_fini);
KTDEC(cl_io_rw_advance);

KTDEC(vvp_io_write_start);
KTDEC(vvp_io_lseek_start);
KTDEC(vvp_io_fault_start);
KTDEC(vvp_io_setattr_start);
KTDEC(vvp_io_read_start);
KTDEC(cis_op);
KTDEC(generic_file_read_iter);
KTDEC(__generic_file_write_iter);
KTDEC(generic_write_sync);
KTDEC(vvp_io_write_commit);

KTDEC(vvp_io_write_lock);
KTDEC(vvp_mmap_locks);
KTDEC(mmap_read_lock);
KTDEC(our_vma);
KTDEC(vvp_io_one_lock);
KTDEC(vvp_io_read_lock);
KTDEC(cl_lockset_lock);

KTDEC(cl_lock_enqueue);
KTDEC(osc_lock_enqueue);
KTDEC(osc_lock_enqueue_wait);
KTDEC(osc_enqueue_base);
KTDEC(ldlm_handle2lock);
KTDEC(ldlm_resource_get);
KTDEC(cfs_hash_bd_get_and_lock);
KTDEC(cfs_hash_bd_lookup_locked);
KTDEC(cfs_hash_bd_unlock);
KTDEC(hlist_entry);

KTDEC(LDLM_RESOURCE_ADDREF);
KTDEC(search_queue);
KTDEC(LDLM_RESOURCE_DELREF);
KTDEC(l_wait_event_abortable);
KTDEC(ldlm_resource_putref);
KTDEC(cfs_hash_bd_get);
KTDEC(cfs_hash_bd_dec_and_lock);
KTDEC(__ldlm_resource_putref_final);
KTDEC(lvbo_free);
KTDEC(ldlm_resource_free);

KTDEC(ldlm_lock2handle);
KTDEC(l_completion_ast);
KTDEC(wait_event_idle_timeout);

KTDEC(mdc_lock_enqueue);
KTDEC(cl_lock_init);

KTDEC(ldlm_lock_match_with_skip);
KTDEC(ldlm_cli_enqueue);
KTDEC(ll_direct_IO_impl);
KTDEC(cl_sync_io_wait_recycle);
KTDEC(cl_sync_io_wait);
KTDEC(cl_sync_io_wait_event_idle);
KTDEC(osc_io_end);
KTDEC(osc_io_fsync_end);
KTDEC(osc_io_fsync_end_wait_for_completion);
KTDEC(osc_cache_wait_range);

KTDEC(osc_lock_upcall);
KTDEC(osc_lock_granted);
KTDEC(ldlm_handle2lock_long);
KTDEC(lu_ref_add_atomic);
KTDEC(ych_lock_res_and_lock);
KTDEC(rcu_read_lock);
KTDEC(ych_lock_res);
KTDEC(osc_lock_lvb_update);
KTDEC(ych_unlock_res_and_lock);
KTDEC(osc_set_lock_data_wrap);
KTDEC(directIO);

KTDEC(ll_direct_rw_pages);
// KTDEC(lustre_io_submit_one);
KTDEC(osc_io_submit);
KTDEC(search_itree);
KTDEC(ll_atomic_open);

KTDEC(mdc_enqueue_send);
KTDEC(ptlrpc_queue_wait);
KTDEC(ptlrpc_set_wait);

KTDEC(ptlrpc_wait_event_abortable);
KTDEC(ptlrpc_wait_event_idle);
KTDEC(ptlrpc_send_new_req);
KTDEC(mdc_enqueue_base);
KTDEC(ll_intent_file_open);
KTDEC(ptl_send_buf);

KTDEC(ll_lookup_it);
KTDEC(md_intent_lock);
KTDEC(lmv_intent_lock);
KTDEC(lmv_intent_open);
KTDEC(md_intent_lock_lmv);
KTDEC(mdc_enqueue_base_mdt);
KTDEC(ldlm_cli_enqueue_mdt);
KTDEC(ptlrpc_queue_wait_mdt);
KTDEC(ptlrpc_get_mod_rpc_slot);
KTDEC(obd_get_mod_rpc_slot);
KTDEC(obd_get_mod_rpc_slot_wait);
KTDEC(ptlrpc_main);
KTDEC(claim_mod_rpc_function);
KTDEC(obd_get_request_slot);
KTDEC(obd_get_request_slot_wait);
KTDEC(ptlrpc_wait_event_abortable_mdt);
KTDEC(ptlrpc_wait_event_idle_mdt);
KTDEC(rq_lock);
KTDEC(cl_sync_io_wait_submit_sync);
KTDEC(cl_io_commit_async);
KTDEC(osc_page_cache_add);
KTDEC(osc_queue_async_io);
KTDEC(ll_merge_attr);
KTDEC(ll_inode_size_lock);
KTDEC(lli_size_mutex);
KTDEC(cl_object_attr_lock);
KTDEC(cl_loi_list_lock);
KTDEC(osc_enter_cache);
KTDEC(ptlrpc_prep_set);
KTDEC(ych_cl_loi_list_lock);
KTDEC(ych_osc_enter_cache_try_1);
KTDEC(application_unlock_and_unplug);
KTDEC(cli_lock_after_unplug);
KTDEC(ych_osc_enter_cache_try_2);
//KTDEC(osc_enter_cache_wait);
KTDEC(ptlrpc_set_destroy);

KTDEC(md_intent_lock_ll_open);
KTDEC(osc_extent_release);
KTDEC(ptlrpc_check_set);

KTDEC(vvp_io_commit_sync);

KTDEC(ll_file_release);

KTDEC(ll_getattr_dentry);
KTDEC(osc_io_unplug0_sync);
KTDEC(osc_enter_cache_try);
KTDEC(osc_enter_cache_atomic);
KTDEC(application_check_req);

KTDEC(balance_dirty_pages_ratelimited);
KTDEC(lustre_wb_do_writeback);
KTDEC(lustre_wb_writeback);
KTDEC(iov_iter_copy_from_user_atomic);
KTDEC(write_begin);
KTDEC(write_end);
KTDEC(grab_cache_page_nowait);
KTDEC(find_get_entry);
KTDEC(page_cache_alloc);
KTDEC(cl_page_find);
//KTDEC(sptlrpc_import_check_ctx);

KTDEC(ll_writepages);
KTDEC(wb_check_background_flush);
KTDEC(__writeback_inodes_wb);
KTDEC(__writeback_single_inode);
KTDEC(inode_to_wb_and_lock_list);
KTDEC(wb_do_writeback);
KTDEC(io_throttling);

KTDEC(add_to_page_cache_lru);
KTDEC(__add_to_page_cache_locked);
KTDEC(lru_cache_add);
KTDEC(__page_cache_alloc);
KTDEC(__alloc_pages_slowpath);
KTDEC(__alloc_pages_nodemask);
KTDEC(get_page_from_freelist_1);
KTDEC(__alloc_pages_direct_compact);
KTDEC(get_page_from_freelist_2);
KTDEC(__alloc_pages_direct_reclaim);
KTDEC(__perform_reclaim);
KTDEC(try_to_free_pages);
KTDEC(throttle_direct_reclaim);
KTDEC(lustre_do_try_to_free_pages);
KTDEC(shrink_zones);
KTDEC(shrink_node);
KTDEC(target_lruvec_lock_wait);
KTDEC(shrink_node_memcgs);
KTDEC(reclaim_throttle);
KTDEC(shrink_lruvec);
KTDEC(get_scan_count);
KTDEC(shrink_list);
KTDEC(shrink_lruvec_cond_resched);
KTDEC(blk_finish_plug);
KTDEC(shrink_active_list);
KTDEC(shrink_inactive_list);
KTDEC(lruvec_spinlock);
KTDEC(shrink_page_list);
KTDEC(shrink_page_list_cond_resched);
KTDEC(shrink_page_list_loop_cond_resched);
KTDEC(page_check_dirty_writeback);
KTDEC(wait_on_page_writeback);
KTDEC(page_check_references);
KTDEC(try_to_unmap);
KTDEC(pageout);
KTDEC(__remove_mapping);
KTDEC(mem_cgroup_uncharge_list);
KTDEC(try_to_unmap_flush);
KTDEC(free_unref_page_list);
KTDEC(free_unref_page_prepare);
KTDEC(free_unref_page_commit);
KTDEC(free_pcppages_bulk);
KTDEC(zone_spinlock);
KTDEC(__free_one_page);
KTDEC(shrink_slab);
KTDEC(vmpressure);

KTDEC(get_page_from_freelist);
KTDEC(prepare_alloc_pages);
KTDEC(cl_sync_file_range);
KTDEC(cl_sync_loop);
KTDEC(set_bit_wb);
KTDEC(lustre_wb_writeback_spinlock);
KTDEC(wb_over_bg_thresh);
KTDEC(lustre_requeue_inode);
KTDEC(InsertInode);
KTDEC(application_io_unplug);
KTDEC(application_check_set);
KTDEC(kiet_spin_lock);

KTDEC(sync_cl_io_lock);
KTDEC(sync_cl_io_iter_init);
KTDEC(sync_cl_io_start);
KTDEC(sync_cl_io_end);
KTDEC(sync_cl_io_unlock);
KTDEC(sync_cl_io_iter_fini);
KTDEC(sync_cl_io_rw_advance);

KTDEC(wait_woken);
// KTDEC(ksocknal_process_receive_wait);
// KTDEC(ksocknal_process_receive);
// KTDEC(ksocknal_process_transmit);

KTDEC(osc_extent_find);
KTDEC(osc_extent_alloc);
KTDEC(ldlm_lock_get);
KTDEC(osc_extent_search);

KTDEC(test);
KTDEC(lock_res);
KTDEC(lock_res_and_lock);

KTDEC(ldlm_lock_decref);
KTDEC(ldlm_lock_add_to_lru);

KTDEC(ptlrpcd_check);
KTDEC(cl_io_lru_reserve);
KTDEC(osc_lru_reserve_wait);
KTDEC(osc_lru_reserve);
KTDEC(sync_cio_start);
KTDEC(lov_io_start);
KTDEC(osc_io_fsync_start);
KTDEC(osc_cache_writeback_range);
KTDEC(osc_io_unplug);
KTDEC(osc_cache_while);
KTDEC(osc_extent_make_ready_top);
KTDEC(osc_extent_make_ready_bottom);

// osc_extent_finish
KTDEC(osc_cache_bottom);
KTDEC(osc_extent_finish_top);
KTDEC(osc_extent_finish_bottom);

KTDEC(application_check_set_INTERPRET);
KTDEC(application_check_set_RPC);
KTDEC(application_check_set_bot_1);
KTDEC(application_check_set_bot);
KTDEC(application_check_set_BULK);
KTDEC(application_check_set_middle);
KTDEC(application_check_set_middle_1);
KTDEC(application_check_set_atomic);
KTDEC(application_check_set_for);
KTDEC(osc_lru_reclaim);

KTDEC(ptlrpc_check_set_rq_lock);
KTDEC(ptlrpc_req_interpret);
KTDEC(brw_interpret);
KTDEC(osc_brw_fini_request);
KTDEC(brw_cl_loi_list_lock);
KTDEC(brw_osc_io_unplug);

KTDEC(application_cond_resched);

KTDEC(lustre_balance_dirty_pages);
KTDEC(lustre_wb_workfn);

KTDEC(wb_workfn);

/* Start profiling for cl_loi_list_lock	*/
// osc_cache.c
//KTDEC(cl_loi_list_lock);
//KTDEC(ych_cl_loi_list_lock);
//KTDEC(osc_io_unplug0_sync);

KTDEC(cl_loi_list_lock_osc_unreserve_grant);
KTDEC(cl_loi_list_lock_osc_free_grant);
KTDEC(cl_loi_list_lock_osc_exit_cache);
KTDEC(cl_loi_list_lock_cli_lock_after_unplug);
KTDEC(cl_loi_list_lock_osc_list_maint);
KTDEC(cl_loi_list_lock_osc_ap_completion);
KTDEC(cl_loi_list_lock_osc_check_rpcs);
KTDEC(cl_loi_list_lock_application_check_rpcs);
KTDEC(cl_loi_list_lock_application_io_unplug);
KTDEC(cl_loi_list_lock_osc_queue_async_io_1);
KTDEC(cl_loi_list_lock_osc_queue_sync_pages);

// osc_request.c
//KTDEC(brw_cl_loi_list_lock);

KTDEC(cl_loi_list_lock_osc_announce_cached);
KTDEC(cl_loi_list_lock___osc_update_grant);
KTDEC(cl_loi_list_lock_osc_shrink_grant_local);
KTDEC(cl_loi_list_lock_osc_shrink_grant);
KTDEC(cl_loi_list_lock_osc_shrink_grant_to_target_1);
KTDEC(cl_loi_list_lock_osc_shrink_grant_to_target_2);
KTDEC(cl_loi_list_lock_osc_init_grant);
KTDEC(cl_loi_list_lock_osc_build_rpc);
KTDEC(cl_loi_list_lock_application_build_rpc);
KTDEC(cl_loi_list_lock_osc_reconnect);
KTDEC(cl_loi_list_lock_osc_import_event);
/* End profiling for cl_loi_list_lock	*/

static void __exit lustre_exit(void)
{
	unregister_filesystem(&lustre_fs_type);

	llite_tunables_unregister();
	pr_info("llite_tunables_unregister \n");

	ll_xattr_fini();
	cl_env_put(cl_inode_fini_env, &cl_inode_fini_refcheck);
	pr_info("cl_env_put \n");
	vvp_global_fini();

	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();

	iput_in_lustre = NULL;

	kmem_cache_destroy(ll_inode_cachep);
	kmem_cache_destroy(ll_file_data_slab);
	kmem_cache_destroy(pcc_inode_slab);
	pr_info("Destroy cache done \n");
	// mempool_destroy(lustre_mempool);

	// ktprint(0, ll_atomic_open);
	// ktprint(1, mdc_enqueue_base);
	// ktprint(1, ldlm_cli_enqueue);
	// ktprint(1, mdc_enqueue_send);
	// ktprint(2, ptlrpc_queue_wait);
	// ktprint(3, ptlrpc_set_wait);
	// ktprint(3, ptl_send_buf);
	// ktprint(4, ptlrpc_send_new_req);
	// ktprint(4, ptlrpc_wait_event_abortable);
	// ktprint(4, ptlrpc_wait_event_idle);

	ktprint(0, ll_file_io_generic);
	ktprint(1, ll_file_write_iter);
	//ktprint(2, lustre_balance_dirty_pages);
	//ktprint(2, lustre_wb_workfn);
	//ktprint(3, cl_sync_io_wait_recycle);
	//ktprint(4, cl_sync_io_wait);
	//ktprint(5, cl_sync_io_wait_event_idle);
	//ktprint(5, ll_direct_IO_impl);
	ktprint(2, cl_io_loop);
	
	/* Profiling for cl_loi_list_lock	*/
	printk("===============================================");
	printk("		[[osc_cache.c]]		");
	ktprint(3, cl_loi_list_lock);
	ktprint(3, ych_cl_loi_list_lock);
	ktprint(3, osc_io_unplug0_sync);
	ktprint(3, cl_loi_list_lock_osc_unreserve_grant);
	ktprint(3, cl_loi_list_lock_osc_free_grant);
	ktprint(3, cl_loi_list_lock_osc_exit_cache);
	ktprint(3, cl_loi_list_lock_cli_lock_after_unplug);
	ktprint(3, cl_loi_list_lock_osc_list_maint);
	ktprint(3, cl_loi_list_lock_osc_ap_completion);
	ktprint(3, cl_loi_list_lock_osc_check_rpcs);
	ktprint(3, cl_loi_list_lock_application_check_rpcs);
	ktprint(3, cl_loi_list_lock_application_io_unplug);
	ktprint(3, cl_loi_list_lock_osc_queue_async_io_1);
	ktprint(3, cl_loi_list_lock_osc_queue_sync_pages);
	printk("		[[osc_request.c]]		");
	ktprint(3, brw_cl_loi_list_lock);
	ktprint(3, cl_loi_list_lock_osc_announce_cached);
	ktprint(3, cl_loi_list_lock___osc_update_grant);
	ktprint(3, cl_loi_list_lock_osc_shrink_grant_local);
	ktprint(3, cl_loi_list_lock_osc_shrink_grant);
	ktprint(3, cl_loi_list_lock_osc_shrink_grant_to_target_1);
	ktprint(3, cl_loi_list_lock_osc_shrink_grant_to_target_2);
	ktprint(3, cl_loi_list_lock_osc_init_grant);
	ktprint(3, cl_loi_list_lock_osc_build_rpc);
	ktprint(3, cl_loi_list_lock_application_build_rpc);
	ktprint(3, cl_loi_list_lock_osc_reconnect);
	ktprint(3, cl_loi_list_lock_osc_import_event);
	printk("===============================================");
	/* Profiling for cl_loi_list_lock	*/

	//ktprint(3, cl_io_loop_internal);
	ktprint(4, cl_io_lock);
	ktprint(5, vvp_io_write_lock);
	ktprint(6, vvp_mmap_locks);
	ktprint(7, mmap_read_lock);
	ktprint(7, our_vma);
	ktprint(6, vvp_io_one_lock);
	ktprint(5, lov_io_lock);
	ktprint(6, lov_cls_todo_scan);
	ktprint(6, lov_lis_active_scan);
	ktprint(6, fast_ldlm_lock_addref_try);
	ktprint(7, fast_lock_res_and_lock);
	ktprint(7, fast_ldlm_lock_addref_internal_nolock);
	ktprint(8, ldlm_lock_remove_from_lru);
	//ktprint(7, addref_lock_res_and_lock);
	//ktprint(7, ldlm_lock_addref_internal_nolock);
	ktprint(6, lov_io_call);
	ktprint(5, cl_lockset_lock);
	ktprint(6, cl_lock_init);
	ktprint(6, cl_lock_enqueue);
	ktprint(7, osc_lock_enqueue);
	ktprint(8, osc_lock_enqueue_wait);
	ktprint(8, osc_enqueue_base);
	ktprint(9, ldlm_lock_match_with_skip);
	//ktprint(10, sptlrpc_import_check_ctx);
	ktprint(10, ldlm_handle2lock);
	ktprint(10, ldlm_resource_get);
	ktprint(11, cfs_hash_bd_get_and_lock);
	ktprint(11, cfs_hash_bd_lookup_locked);
	ktprint(11, cfs_hash_bd_unlock);
	ktprint(11, hlist_entry);
	ktprint(10, LDLM_RESOURCE_ADDREF);
	ktprint(10, lock_res);
	ktprint(10, search_itree);
	ktprint(10, search_queue);
	ktprint(10, LDLM_RESOURCE_DELREF);
	ktprint(10, l_wait_event_abortable);
	ktprint(10, ldlm_resource_putref);
	ktprint(11, cfs_hash_bd_get);
	ktprint(11, cfs_hash_bd_dec_and_lock);
	ktprint(11, __ldlm_resource_putref_final);
	ktprint(11, lvbo_free);
	ktprint(11, ldlm_resource_free);
	ktprint(10, ldlm_lock2handle);
	ktprint(10, l_completion_ast);
	ktprint(10, wait_event_idle_timeout);
	ktprint(9, ldlm_cli_enqueue);
	ktprint(9, osc_lock_upcall);
	ktprint(10, osc_lock_granted);
	ktprint(11, ldlm_handle2lock_long);
	ktprint(11, lu_ref_add_atomic);
	ktprint(11, ych_lock_res_and_lock);
	ktprint(12, rcu_read_lock);
	ktprint(12, ych_lock_res);
	ktprint(11, osc_lock_lvb_update);
	ktprint(11, ych_unlock_res_and_lock);
	ktprint(9, ldlm_lock_decref);
	ktprint(10, ldlm_lock_add_to_lru);
	ktprint(10, lock_res_and_lock);
	ktprint(9, osc_set_lock_data_wrap);
	ktprint(7, mdc_lock_enqueue);
	// ktprint(5, vvp_io_read_lock);

	ktprint(4, cl_io_iter_init);
	ktprint(4, cl_io_start);
	// ktprint(5, cis_op);
	ktprint(6, vvp_io_write_start);
	//ktprint(7, cl_io_lru_reserve);
	//ktprint(8, osc_lru_reserve);
	//ktprint(9, osc_lru_reserve_wait);
	//ktprint(9, osc_lru_reclaim);
	ktprint(7, __generic_file_write_iter);
	ktprint(8, write_begin);
	ktprint(9, grab_cache_page_nowait);
	ktprint(10, find_get_entry);
	ktprint(10, page_cache_alloc);
	ktprint(11, __alloc_pages_nodemask);
	ktprint(12, prepare_alloc_pages);
	ktprint(12, __alloc_pages_slowpath);
	ktprint(13, get_page_from_freelist_1);
	ktprint(13, __alloc_pages_direct_compact);
	ktprint(13, get_page_from_freelist_2);
	ktprint(13, __alloc_pages_direct_reclaim);
	ktprint(14, __perform_reclaim);
	ktprint(15, try_to_free_pages);
	ktprint(16, throttle_direct_reclaim);
	ktprint(16, lustre_do_try_to_free_pages);
	ktprint(17, shrink_zones);
	ktprint(18, shrink_node);
	ktprint(19, target_lruvec_lock_wait);
	ktprint(19, shrink_node_memcgs);
	ktprint(20, shrink_lruvec);
	ktprint(21, get_scan_count);
	ktprint(21, shrink_list);
	ktprint(22, shrink_active_list);
	ktprint(22, shrink_inactive_list);
	ktprint(23, lruvec_spinlock);
	ktprint(23, shrink_page_list);
	ktprint(24, shrink_page_list_cond_resched);
	ktprint(24, shrink_page_list_loop_cond_resched);
	ktprint(24, page_check_dirty_writeback);
	ktprint(24, wait_on_page_writeback);
	ktprint(24, page_check_references);
	ktprint(24, try_to_unmap);
	ktprint(24, pageout);
	ktprint(24, __remove_mapping);
	ktprint(24, mem_cgroup_uncharge_list);
	ktprint(24, try_to_unmap_flush);
	ktprint(24, free_unref_page_list);
	ktprint(25, free_unref_page_prepare);
	ktprint(25, free_unref_page_commit);
	ktprint(26, free_pcppages_bulk);
	ktprint(27, zone_spinlock);
	ktprint(27, __free_one_page);
	ktprint(21, shrink_lruvec_cond_resched);
	ktprint(21, blk_finish_plug);
	ktprint(20, shrink_slab);
	ktprint(20, vmpressure);
	ktprint(19, reclaim_throttle);
	ktprint(12, get_page_from_freelist);
	ktprint(11, __page_cache_alloc);
	ktprint(10, add_to_page_cache_lru);
	ktprint(11, __add_to_page_cache_locked);
	ktprint(11, lru_cache_add);
	ktprint(9, cl_page_find);
	ktprint(8, iov_iter_copy_from_user_atomic);
	ktprint(8, balance_dirty_pages_ratelimited);
	ktprint(9, lustre_wb_do_writeback);
	ktprint(10, lustre_wb_writeback);
	//ktprint(9, wb_do_writeback);
	//ktprint(9, io_throttling);
	//ktprint(10, set_bit_wb);
	//ktprint(10, wb_check_background_flush);
	//ktprint(10, wb_over_bg_thresh);
	//ktprint(11, lustre_wb_writeback_spinlock);
	//ktprint(11, __writeback_inodes_wb);
	//ktprint(11, lustre_requeue_inode);
	//ktprint(12, InsertInode);
	//ktprint(12, __writeback_single_inode);
	//ktprint(12, inode_to_wb_and_lock_list);
	//ktprint(13, ll_writepages);
	//ktprint(14, cl_sync_file_range);
	//ktprint(15, cl_sync_loop);
	//ktprint(16, sync_cl_io_lock);
	//ktprint(16,sync_cl_io_iter_init);
	//ktprint(16,sync_cl_io_start);
	//ktprint(17, sync_cio_start);
	// ktprint(18, lov_io_start);
	//ktprint(18, osc_io_fsync_start);
	//ktprint(19, osc_cache_writeback_range);
	//ktprint(20, osc_io_unplug);
	//ktprint(20, osc_cache_while);
	//ktprint(21, osc_extent_make_ready_top);
	//ktprint(21, osc_extent_make_ready_bottom);

	//ktprint(20, osc_cache_bottom);
	//ktprint(21, osc_extent_finish_top);
	//ktprint(21, osc_extent_finish_bottom);
	
	//ktprint(16,sync_cl_io_end);
	//ktprint(16,sync_cl_io_unlock);
	//ktprint(16,sync_cl_io_iter_fini);
	//ktprint(16,sync_cl_io_rw_advance);
	//ktprint(8, directIO);
	//ktprint(9, ll_direct_IO_impl);
	//ktprint(10, ll_direct_rw_pages);
	//ktprint(11, test);
	//ktprint(7, generic_write_sync);
	ktprint(7, vvp_io_write_commit);
	ktprint(8, ll_merge_attr);
	ktprint(9, ll_inode_size_lock);
	ktprint(10, lli_size_mutex);
	ktprint(10, cl_object_attr_lock);
	ktprint(8, cl_io_commit_async);
	ktprint(9, osc_page_cache_add);
	ktprint(10, osc_queue_async_io);
	ktprint(11, osc_extent_find);
	ktprint(12, osc_extent_alloc);
	ktprint(12, ldlm_lock_get);
	ktprint(12, osc_extent_search);
	ktprint(11, osc_enter_cache_try);
	ktprint(12, osc_enter_cache_atomic);
	ktprint(11, cl_loi_list_lock);
	ktprint(11, osc_extent_release);
	ktprint(11, osc_enter_cache);
	ktprint(12, ptlrpc_prep_set);
	ktprint(12, ych_cl_loi_list_lock);
	ktprint(12, ych_osc_enter_cache_try_1);
	ktprint(12, application_unlock_and_unplug);
	ktprint(12, cli_lock_after_unplug);
	ktprint(12, ych_osc_enter_cache_try_2);
	ktprint(12, ptlrpc_set_destroy);
	//ktprint(12, osc_enter_cache_wait);
	//ktprint(12, application_io_unplug);
	//ktprint(12, application_check_req);
	//ktprint(13, application_check_set);
	//ktprint(14, ptlrpc_send_new_req);
	//ktprint(14, application_cond_resched);
	//ktprint(14, application_check_set_bot_1);
	//ktprint(14, application_check_set_bot);
	//ktprint(14, application_check_set_RPC);
	//ktprint(14, application_check_set_middle);
	//ktprint(14, application_check_set_middle_1);
	//ktprint(14, application_check_set_INTERPRET);
	//ktprint(14, application_check_set_atomic);
	//ktprint(14, application_check_set_for);
	// ktprint(13, osc_io_unplug0_sync);
	// ktprint(13, ptlrpc_check_set);
	//ktprint(8, vvp_io_commit_sync);
	//ktprint(9, cl_sync_io_wait_submit_sync);
	//ktprint(10, cl_sync_io_wait_event_idle);
	// ktprint(6, vvp_io_lseek_start);
	// ktprint(6, vvp_io_fault_start);
	// ktprint(6, vvp_io_setattr_start);
	//ktprint(6, vvp_io_read_start);
	//ktprint(7, generic_file_read_iter);
	ktprint(4, cl_io_end);
	ktprint(6, osc_io_end);
	ktprint(6, osc_io_fsync_end);
	ktprint(7, osc_io_fsync_end_wait_for_completion);
	ktprint(7, osc_cache_wait_range);
	ktprint(4, cl_io_unlock);
	ktprint(4, cl_io_iter_fini);
	ktprint(4, cl_io_rw_advance);


	//ktprint(2, rq_lock);
	//ktprint(2, kiet_spin_lock);
	pr_info("\n");

	// ktprint(0, ksocknal_process_receive);
	// ktprint(0, ksocknal_process_receive_wait);
	// ktprint(0, ksocknal_process_transmit);

	ktprint(0, ptlrpcd_check);
	ktprint(1, ptlrpc_check_set);
	ktprint(2, ptlrpc_check_set_rq_lock);
	ktprint(2, ptlrpc_req_interpret);
	ktprint(3, brw_interpret);
	ktprint(4, osc_brw_fini_request);
	ktprint(4, brw_cl_loi_list_lock);
	ktprint(4, brw_osc_io_unplug);
	//ktprint(1, wait_woken);

	// ktreset(ksocknal_process_receive);
	// ktreset(ksocknal_process_receive_wait);
	// ktreset(ksocknal_process_transmit);

	// ktprint(0, osc_io_submit);

	// ktprint(0, ll_intent_file_open);
	// ktprint(1, md_intent_lock_ll_open);
	// ktprint(3, mdc_enqueue_base_mdt);
	// ktprint(4, ldlm_cli_enqueue_mdt);
	// ktprint(5, ptlrpc_queue_wait_mdt);
	// // ktprint(9, ptlrpc_wait_event_abortable_mdt);
	// ktprint(6, ptlrpc_wait_event_idle_mdt);
	// ktprint(7, rq_lock);
	// ktprint(5, ptlrpc_get_mod_rpc_slot);
	// ktprint(6, obd_get_mod_rpc_slot);
	// ktprint(7, obd_get_mod_rpc_slot_wait);
	// ktprint(5, obd_get_request_slot);
	// ktprint(7, obd_get_request_slot_wait);
	// ktprint(0, ll_lookup_it);
	// ktprint(1, md_intent_lock);
	// ktprint(3, lmv_intent_lock);
	// ktprint(4, lmv_intent_open);
	// ktprint(5, md_intent_lock_lmv);
	// // ktprint(6, mdc_enqueue_base_mdt);
	// // ktprint(0, claim_mod_rpc_function);
	// // ktprint(0, ptlrpc_main);
	// ktprint(0, ll_file_release);
	// ktprint(0, ll_getattr_dentry);

	// io_submit_one_in_lustre = NULL;
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Client File System");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

module_init(lustre_init);
module_exit(lustre_exit);
