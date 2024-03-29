#include <balloc.h>
#include <memory.h>
#include <debug.h>
#include <initramfs.h>


struct mboot_info {
	uint32_t flags;
	uint8_t ignore0[40];
	uint32_t mmap_size;
	uint32_t mmap_addr;
} __attribute__((packed));

struct mboot_mmap_entry {
	uint32_t size;
	uint64_t addr;
	uint64_t length;
	uint32_t type;
} __attribute__((packed));



#define BALLOC_MAX_RANGES	128
static struct memory_node balloc_nodes[BALLOC_MAX_RANGES];
static struct list_head balloc_free_list;

struct rb_tree free_ranges;
struct rb_tree memory_map;


static struct memory_node *balloc_alloc_node(void)
{
	BUG_ON(list_empty(&balloc_free_list) &&
				"Please, increase BALLOC_MAX_RANGES constant");

	struct list_head *node = balloc_free_list.next;

	list_del(node);
	return LL2MEMORY_NODE(node);
}

static void balloc_free_node(struct memory_node *node)
{
	list_add(&node->link.ll, &balloc_free_list);
}

static void __balloc_add_range(struct rb_tree *tree,
			unsigned long long from, unsigned long long to)
{
	struct rb_node **plink = &tree->root;
	struct rb_node *parent = 0;

	while (*plink) {
		struct memory_node *node = RB2MEMORY_NODE(*plink);

		parent = *plink;
		if (node->begin < from)
			plink = &parent->right;
		else
			plink = &parent->left;
	}

	struct memory_node *new = balloc_alloc_node();

	new->begin = from;
	new->end = to;

	rb_link(&new->link.rb, parent, plink);
	rb_insert(&new->link.rb, tree);

	struct memory_node *prev = RB2MEMORY_NODE(rb_prev(&new->link.rb));

	if (prev && prev->end >= new->begin) {
		new->begin = prev->begin;
		if (prev->end > new->end)
			new->end = prev->end;
		rb_erase(&prev->link.rb, tree);
		balloc_free_node(prev);
	}

	struct memory_node *next = RB2MEMORY_NODE(rb_next(&new->link.rb));

	if (next && next->begin <= new->end) {
		new->end = next->end;
		rb_erase(&next->link.rb, tree);
		balloc_free_node(next);
	}
}

static void __balloc_remove_range(struct rb_tree *tree,
			unsigned long long from, unsigned long long to)
{
	struct rb_node *link = tree->root;
	struct memory_node *ptr = 0;

	while (link) {
		struct memory_node *node = RB2MEMORY_NODE(link);

		if (node->end > from) {
			link = link->left;
			ptr = node;
		} else {
			link = link->right;
		}
	}

	while (ptr && ptr->begin < to) {
		struct memory_node *next =
					RB2MEMORY_NODE(rb_next(&ptr->link.rb));

		rb_erase(&ptr->link.rb, tree);
		if (ptr->begin < from)
			__balloc_add_range(tree, ptr->begin, from);
		if (ptr->end > to)
			__balloc_add_range(tree, to, ptr->end);
		balloc_free_node(ptr);
		ptr = next;
	}
}

uintptr_t __balloc_alloc(size_t size, uintptr_t align,
			uintptr_t from, uintptr_t to)
{
	struct rb_tree *tree = &free_ranges;
	struct rb_node *link = tree->root;
	struct memory_node *ptr = 0;

	while (link) {
		struct memory_node *node = RB2MEMORY_NODE(link);

		if (node->end > from) {
			link = link->left;
			ptr = node;
		} else {
			link = link->right;
		}
	}

	while (ptr && ptr->begin < to) {
		const unsigned long long b = ptr->begin > from
					? ptr->begin : from;
		const unsigned long long e = ptr->end < to ? ptr->end : to;
		const unsigned long long mask = align - 1;
		const unsigned long long addr = (b + mask) & ~mask;

		if (addr + size <= e) {
			rb_erase(&ptr->link.rb, tree);
			if (ptr->begin < addr)
				__balloc_add_range(tree, ptr->begin, addr);
			if (ptr->end > addr + size)
				__balloc_add_range(tree, addr + size, ptr->end);
			balloc_free_node(ptr);
			return addr;
		}

		ptr = RB2MEMORY_NODE(rb_next(&ptr->link.rb));
	}

	return to;
}

uintptr_t balloc_alloc(size_t size, uintptr_t from, uintptr_t to)
{
	/* The only situation when we would like a larger alignment is
	 * when we allocate page for a page table, in that case we would
	 * need PAGE_SIZE alignment, IOW it's quite reasonable default. */
	uintptr_t align = 64;

	if (size <= 32) align = 32;
	if (size <= 16) align = 16;
	if (size <= 8)  align = 8;

	return __balloc_alloc(size, align, from, to);
}

void balloc_free(uintptr_t begin, uintptr_t end)
{
	__balloc_add_range(&free_ranges, begin, end);
}


static void balloc_setup_nodes(void)
{
	list_init(&balloc_free_list);

	for (int i = 0; i != BALLOC_MAX_RANGES; ++i)
		balloc_free_node(&balloc_nodes[i]);
}

/* Initially we put all ranges in both memory map tree and free
 * ranges tree, and only after that we remove busy ranges from
 * free ranges tree, because sometimes BIOS/bootloader may report
 * a broken memory map with overlapped regions. It's not a problem
 * until overlapped regions have different attributes, i. e. one of
 * regions is free while other is reserved, and in that case our
 * algorithm guaratees that all regions that marked reserved in
 * the memory map won't be in the free ranges tree. */
static void balloc_parse_mmap(const struct mboot_info *info)
{
	BUG_ON((info->flags & (1ul << 6)) == 0);

	const uintptr_t begin = info->mmap_addr;
	const uintptr_t end = begin + info->mmap_size;
	uintptr_t ptr = begin;

	while (ptr + sizeof(struct mboot_mmap_entry) <= end) {
		const struct mboot_mmap_entry *entry =
					(const struct mboot_mmap_entry *)ptr;
		const unsigned long long rbegin = entry->addr;
		const unsigned long long rend = rbegin + entry->length;

		__balloc_add_range(&memory_map, rbegin, rend);
		__balloc_add_range(&free_ranges, rbegin, rend);
		ptr += entry->size + sizeof(entry->size);
	}

	extern char text_phys_begin[];
	extern char bss_phys_end[];

	const uintptr_t kbegin = (uintptr_t)text_phys_begin;
	const uintptr_t kend = (uintptr_t)bss_phys_end;

	__balloc_add_range(&memory_map, kbegin, kend);
	__balloc_add_range(&free_ranges, kbegin, kend);

	ptr = begin;
	while (ptr + sizeof(struct mboot_mmap_entry) <= end) {
		const struct mboot_mmap_entry *entry =
					(const struct mboot_mmap_entry *)ptr;
		const unsigned long long rbegin = entry->addr;
		const unsigned long long rend = rbegin + entry->length;

		if (entry->type != 1)
			__balloc_remove_range(&free_ranges, rbegin, rend);
		ptr += entry->size + sizeof(entry->size);
	}

	uint64_pair init_module = initramfs(info);
	__balloc_remove_range(&free_ranges, init_module.first, init_module.second);
	__balloc_remove_range(&free_ranges, kbegin, kend);
}

static void __balloc_dump_ranges(const struct rb_tree *tree)
{
	const struct memory_node *node = RB2MEMORY_NODE(rb_leftmost(tree));

	while (node) {
		printf("memory range: 0x%llx-0x%llx\n",
					(unsigned long long)node->begin,
					(unsigned long long)node->end);
		node = RB2MEMORY_NODE(rb_next(&node->link.rb));
	}
}

static void balloc_dump_ranges(void)
{
	printf("known memory ranges:\n");
	__balloc_dump_ranges(&memory_map);
	printf("free memory ranges:\n");
	__balloc_dump_ranges(&free_ranges);
}

uintptr_t balloc_memory(void)
{
	const struct memory_node *node =
				RB2MEMORY_NODE(rb_rightmost(&memory_map));

	return node->end;
}

void balloc_setup(const struct mboot_info *info)
{
	balloc_setup_nodes();
	balloc_parse_mmap(info);
	balloc_dump_ranges();
}
