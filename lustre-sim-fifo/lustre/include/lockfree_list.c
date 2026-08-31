#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/hash.h>
#include <linux/sched.h>
#include <linux/fs.h>

#include "../include/lockfree_list.h"
#include "../include/calclock.h"

//#define mem_alloc(size) kmalloc(size, GFP_ATOMIC)
#define mem_alloc(size) kmalloc(size, GFP_KERNEL)

static bool marked( struct LNode* node)
{
	if (!node)
		return false;
	return (unsigned long long)(node) & 0x1;
}

static struct LNode* unmark( struct LNode* node)
{
	return (struct LNode*)((unsigned long long)(node) & 0xFFFFFFFFFFFFFFFE);
}

/* ych	*/
static bool TryMarkNode(struct LNode *lock)
{
	while (true) {	
		struct LNode *orig = lock->next;
		unsigned long long _marked;

		if (marked(orig))
			return false;

		_marked = (unsigned long long)orig + 1;

		if (cmpxchg(&lock->next, orig, 
				(struct LNode *)_marked) == orig)
			return true;
	}
}
/* ych	*/

static void rlock_node_rcu_free(struct rcu_head *head)
{
	struct LNode *node = container_of(head, struct LNode, rcu);

//	printk("[%s] start! from %ps\n", __func__, __builtin_return_address(0));
//	printk("current->comm = %s\n", current->comm);
	kfree(node);
}

static void DeleteNode(struct LNode* lock)
{
	while (true) {
		 struct LNode* orig = lock->next;
		unsigned long long _marked = (unsigned long long)orig + 1;
		// BUG_ON(!marked((struct LNode *)_marked));
		if (cmpxchg(&lock->next, orig, (struct LNode*)_marked) == orig) {
			// pr_info("Physically deleted \n");
			// pr_debug("LFS: LNode[%d-%d] Logically deleted!\n",
			// 		lock->start, lock->end);
			break;
		}
	}
}

static struct list_head* DeleteNodeLH(struct LNode* lock)
{
	struct list_head* inode = NULL;
	unsigned long long _marked;
	while (true) {
		struct LNode* orig = lock->next;
		_marked = (unsigned long long)orig + 1;
		// BUG_ON(!marked((struct LNode *)_marked));
		if (!marked((struct LNode *)_marked)){
			return NULL;
		}
		if (cmpxchg(&lock->next, orig, (struct LNode*)_marked) == orig) {
			// pr_debug("LFS: LNode[%d-%d] Logically deleted!\n",
			// 		lock->start, lock->end);
			inode = lock->inode;
			break;
		}
	}
	return inode;
}


static int InsertNodeRW(struct LNode** listrl, struct LNode* lock, bool try)
{
	 struct LNode** prev;
	 struct LNode* cur;
	 //printk("[%s] sizeof(struct LNode)=%zu\n", __func__, sizeof(struct LNode)); // 32 bytes

restart:
	rcu_read_lock();
	prev = listrl;
	cur = *prev;

	while (true) {

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct LNode* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		lock->next = cur;
		if (cmpxchg(prev, cur, lock) == cur) {
			int ret = 0;
			rcu_read_unlock();
			return ret;
		}
		cur = *prev;

	}
	rcu_read_unlock();
	return -1;
}

#if HASH_MODE

static inline u32 hash_list_head(const struct list_head *list)
{
    // Use kernel's built-in hash_ptr function
	struct inode *inode = list_entry(list, struct inode, i_io_list);
    return hash_ptr(&inode->i_ino, ilog2(BUCKET_CNT));
}

#endif

/* ych	*/
void MarkInodeNodes(struct ListRL *list_rl, struct list_head *inode)
{
	struct LNode *cur;
	struct LNode *next;

#if HASH_MODE
	int i;

	if (!list_rl || !inode)
		return;

	i = hash_list_head(inode);

	rcu_read_lock();
	cur = list_rl->head[i];
#else
	if (!list_rl || !inode)
		return;

	rcu_read_lock();
	cur = list_rl->head;
#endif

	while (cur) {
		next = cur->next;

		if (marked(next)) {
			cur = unmark(next);
			continue;
		}

		if (cur->inode == inode) {
			TryMarkNode(cur);
			next = cur->next;
		}
		cur = unmark(next);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(MarkInodeNodes);
/* ych	*/

static void
InitNode(struct LNode *node, struct list_head *inode)
{
	node->inode = inode;
	node->next = NULL;
}

struct LNode *AllocInodeNode(struct list_head *inode)
{
	struct LNode *lnode;

	lnode = kmalloc(sizeof(*lnode), GFP_KERNEL);
	if (!lnode)
		lnode = kmalloc(sizeof(*lnode), GFP_ATOMIC);

	if (!lnode)
		return NULL;

	InitNode(lnode, inode);

	return lnode;
}
EXPORT_SYMBOL(AllocInodeNode);

void InsertInodePrealloc(struct ListRL *list_rl,
		struct LNode *lnode, bool writer)
{
	int ret = 0;

#if HASH_MODE
	int i;

	if (!lnode)
		return;

	i = hash_list_head(lnode->inode);

	do {
		ret = InsertNodeRW(&list_rl->head[i], lnode, false);
	} while (ret);

#else
	__iget(list_entry(lnode->inode, struct inode, i_io_list));

	do {
		ret = InsertNodeRW(&list_rl->head, lnode, false);
	} while (ret);
#endif
}
EXPORT_SYMBOL(InsertInodePrealloc);

struct RangeLock* __InsertInode(struct ListRL *list_rl,
	struct list_head *inode, bool try)
{
	struct LNode *lnode;
	int ret = 0;

#if HASH_MODE
	int i;

	lnode = mem_alloc(sizeof(struct LNode));
	if (!lnode)
		goto free_rl;
	InitNode(lnode, inode);

	i = hash_list_head(inode);
	do {
		ret = InsertNodeRW(&list_rl->head[i], lnode, false);
	} while	(ret);
	return NULL;

#else
	rl = mem_alloc(sizeof(struct RangeLock));
	if (!rl)
		return NULL;
	lnode = mem_alloc(sizeof(struct LNode));
	if (!lnode){
		goto free_rl;
	}
	InitNode(lnode, inode);

	/* ych	*/
	/*
	 * Each queued LNode owns one inode reference.
	 * inode_io_list_move_locked() holds inode->i_lock.
	 */
	__iget(list_entry(inode, struct inode, i_io_list));
	/* ych	*/

	do {
		ret = InsertNodeRW(&list_rl->head, lnode, false);
		if (try && ret < 0)
			goto free_rl;
	} while(ret);

	rl->node = lnode;
	return rl;
#endif
free_rl:
	return NULL;
}
EXPORT_SYMBOL(__InsertInode);


void MutexRangeRelease(struct RangeLock* rl)
{
#if HASH_MODE
	int i;
	if (!rl || rl->bucket > BUCKET_CNT)
		return;

	if (rl->bucket == ALL_RANGE) {
		for (i = 0 ; i < BUCKET_CNT ; i++) {
			DeleteNode(rl->node[i]);
		}
	} else
		DeleteNode(rl->node[rl->bucket]);
#else
	DeleteNode(rl->node);
#endif
	kfree(rl);
}
EXPORT_SYMBOL(MutexRangeRelease);

struct list_head* __FetchHead(struct LNode** listrl)
{
	struct list_head* inode = NULL;
	struct LNode** prev;
	struct LNode* cur;
	/* ych	*/
	struct inode *vfs_inode;
	/* ych	*/

restart:
	rcu_read_lock();
	prev = listrl;
	cur = *prev;

	while (true) {
		if (!cur) {
			break;
		}

		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}

		if (cur && marked(cur->next)) {
			struct LNode* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		if (cur) {
			inode = DeleteNodeLH(cur);
			/* ych	*/
			if (!inode){
				rcu_read_unlock();
				goto restart;
			}

			vfs_inode = list_entry(inode, struct inode, i_io_list);

			if (!igrab(vfs_inode)) {
				inode = NULL;
				rcu_read_unlock();
				goto restart;
			}
			/* ych	*/
			break;
		}

	}
// failed:
	rcu_read_unlock();
	return inode;
}

/* ych	*/
// Delete from the node where cur->next is NULL, not directly from cur
struct list_head* __FetchTail(struct LNode** listrl)
{
	struct list_head* inode = NULL;
	struct LNode** prev;
	struct LNode* cur;

restart:
	rcu_read_lock();
	prev = listrl;
	cur = *prev;

	while (true) {
		if (!cur) {
			break;
		}

#if 1 // ych, cur node can be marked
		if (marked(cur)) {
			rcu_read_unlock();
			goto restart;
		}
#endif // ych

		if (cur && marked(cur->next)) {
			struct LNode* next = unmark(cur->next);

			if (cmpxchg(prev, cur, next) == cur) {
				call_rcu(&cur->rcu, rlock_node_rcu_free);
			}
			cur = next;
			continue;
		}

		/* ych	*/
		if (cur && unmark(cur->next) == NULL) {
			inode = DeleteNodeLH(cur);

			if (!inode) {
				rcu_read_unlock();
				goto restart;
			}
#if 1
			if (cmpxchg(prev, cur, NULL) == cur)
				call_rcu(&cur->rcu, rlock_node_rcu_free);
#endif
			break;
		}

		prev = &cur->next;
		cur = cur->next;
	}

	rcu_read_unlock();
	return inode;
}
/* ych	*/

#if HASH_MODE

static inline u32 hash_current_pid(void)
{
    pid_t pid = current->pid;  // Get the current PID
    return hash_ptr((void *)(unsigned long)pid, ilog2(BUCKET_CNT));
}

#endif

struct list_head* FetchHead(struct ListRL *listrl)
{
#if HASH_MODE
	int j;
	int i = atomic_fetch_add_relaxed(1, &listrl->rr) % BUCKET_CNT;
	if (listrl->head == NULL)
		return NULL;
	for (j = 1; j < BUCKET_CNT; j++) {
		if (listrl->head[i] == NULL){
			i = atomic_fetch_add_relaxed(1, &listrl->rr) % BUCKET_CNT;
		}
		else
			return __FetchHead(&listrl->head[i]);
	}
	return NULL;
#else
	if (listrl->head == NULL)
		return NULL;
	return __FetchHead(&listrl->head);
#endif
}
EXPORT_SYMBOL(FetchHead);

/* ych	*/
struct list_head* FetchTail(struct ListRL *listrl)
{
#if HASH_MODE
	int j;
	int i = atomic_fetch_add_relaxed(1, &listrl->rr) % BUCKET_CNT;
	if (listrl->head == NULL)
		return NULL;
	for (j = 1; j < BUCKET_CNT; j++) {
		if (listrl->head[i] == NULL){
			i = atomic_fetch_add_relaxed(1, &listrl->rr) % BUCKET_CNT;
		}
		else
			return __FetchTail(&listrl->head[i]);
	}
	return NULL;
#else
	if (listrl->head == NULL)
		return NULL;
	return __FetchTail(&listrl->head);
#endif
}
EXPORT_SYMBOL(FetchTail);
/* ych	*/

void bucket_init(struct ListRL *list_rl)
{
	printk("[%s] start! from %ps\n", __func__, __builtin_return_address(0));
#if HASH_MODE
	// memset(list_rl->head, 0, sizeof(list_rl->head));
	memset(list_rl->head, 0, sizeof(*list_rl->head) * BUCKET_CNT);
	pr_info("Size to memset: %zu\n", sizeof(list_rl->head));
	pr_info("Size of memset 2nd case: %zu\n", sizeof(*list_rl->head) * BUCKET_CNT);
	atomic_set(&list_rl->rr, 0);
#else
	list_rl->head = NULL;
#endif
}
EXPORT_SYMBOL(bucket_init);

