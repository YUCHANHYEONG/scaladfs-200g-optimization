#ifndef LOCKFREE_LIST_H
#define LOCKFREE_LIST_H

#include <linux/types.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#define HASH_MODE (1)
#if HASH_MODE
#define BUCKET_CNT (4)
#endif

#define MAX_SIZE (0xFFFFFFFF)
#define ALL_RANGE (0xFFFFFFFF)

struct LNode {
	struct list_head *inode;
	 struct LNode* next;
	struct rcu_head rcu;
};

struct ListRL {
#if HASH_MODE
	 struct LNode* head[BUCKET_CNT];
	 atomic_t rr; //round robin
#else
	 struct LNode* head;
#endif
};

struct RangeLock {
#if HASH_MODE
	struct LNode* node[BUCKET_CNT];
	unsigned int bucket;
#else
	struct LNode* node;
#endif
};

#if HASH_MODE
#else
struct RangeLock* MutexRangeAcquire(struct ListRL* list_rl,
												unsigned int start,
												unsigned int end, bool try);
#endif
// void MutexRangeRelease(struct RangeLock* rl);
struct list_head* FetchHead(struct ListRL *listrl);

/* ych	*/
struct list_head* FetchTail(struct ListRL *listrl);
/* ych	*/

struct RangeLock* __InsertInode(struct ListRL *list_rl,
										struct list_head *inode, bool try);

static inline struct RangeLock* RWRangeTryAcquire(struct ListRL* list_rl,
		struct list_head *inode, bool writer)
{
	return __InsertInode(list_rl, inode, true);
}

static inline struct RangeLock* InsertInode(struct ListRL* list_rl,
		struct list_head *inode, bool writer)
{
	return __InsertInode(list_rl, inode, false);
}

/* ych	*/
void MarkInodeNodes(struct ListRL *list_rl, struct list_head *inode);
/* ych	*/

void bucket_init(struct ListRL *list_rl);

#endif /* LOCKFREE_LIST_H */
