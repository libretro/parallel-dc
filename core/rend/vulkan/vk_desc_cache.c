/* See desc_cache.h. */
#include <stdlib.h>
#include <string.h>
#include "desc_cache.h"

static u32 hash(const TexDescKey *k)
{
	u64 h = k->view0 * 0x9E3779B97F4A7C15ull;
	h ^= k->view1 + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
	h ^= ((u64)k->sampler0 << 32 | k->sampler1) * 0xC2B2AE3D27D4EB4Full;
	return (u32)(h ^ (h >> 32));
}

static bool same(const vk_desc_entry *e, const TexDescKey *k)
{
	return e->key.view0 == k->view0 && e->key.view1 == k->view1 && e->key.sampler0 == k->sampler0
			&& e->key.sampler1 == k->sampler1;
}

static void insert(const vk_desc_cache *c, vk_desc_entry *into, const vk_desc_entry *e)
{
	u32 i = hash(&e->key) & (c->cap - 1);
	while (into[i].set != VK_NULL_HANDLE)
		i = (i + 1) & (c->cap - 1);
	into[i] = *e;
}

/* The tables @size places each (a power of two), the entries there are
 * moved over; the free stack as large as both can need */
static void grow(vk_desc_cache *c, u32 size)
{
	vk_desc_entry *t = (vk_desc_entry *)calloc(size, sizeof(vk_desc_entry));
	vk_desc_entry *s = (vk_desc_entry *)calloc(size, sizeof(vk_desc_entry));
	VkDescriptorSet *f = (VkDescriptorSet *)malloc((size + VK_DESC_BATCH) * sizeof(VkDescriptorSet));
	const u32 old = c->cap;
	u32 i;
	if (t == NULL || s == NULL || f == NULL)
		abort();
	c->cap = size;
	for (i = 0; i < old; i++)
		if (c->table[i].set != VK_NULL_HANDLE)
			insert(c, t, &c->table[i]);
	if (c->nfree != 0)
		memcpy(f, c->freeSets, c->nfree * sizeof(VkDescriptorSet));
	free(c->table);
	free(c->spare);
	free(c->freeSets);
	c->table = t;
	c->spare = s;
	c->freeSets = f;
}

/* VK_DESC_BATCH more free sets from the pool */
static bool allocate(vk_desc_cache *c)
{
	VkDescriptorSetLayout layouts[VK_DESC_BATCH];
	VkDescriptorSetAllocateInfo info;
	u32 i;
	for (i = 0; i < VK_DESC_BATCH; i++)
		layouts[i] = c->layout;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	info.descriptorPool = vkc.descriptorPool;
	info.descriptorSetCount = VK_DESC_BATCH;
	info.pSetLayouts = layouts;
	if (c->count + c->nfree > c->cap /* (room in freeSets for every set there is) */
			|| vkAllocateDescriptorSets(vkc.device, &info, c->freeSets + c->nfree) != VK_SUCCESS)
		return false;
	c->nfree += VK_DESC_BATCH;
	return true;
}

void vk_desc_cache_set_layout(vk_desc_cache *c, VkDescriptorSetLayout layout)
{
	c->layout = layout;
	if (c->cap == 0)
	{
		u32 i;
		grow(c, VK_DESC_RESERVE);
		/* (the texture atlas disc's frames draw with some 60) */
		for (i = 0; i < VK_DESC_START / VK_DESC_BATCH; i++)
			if (!allocate(c))
				break;
	}
}

VkDescriptorSet vk_desc_cache_find(vk_desc_cache *c, const TexDescKey *key)
{
	u32 i;
	if (c->cap == 0)
		return VK_NULL_HANDLE;
	for (i = hash(key) & (c->cap - 1); c->table[i].set != VK_NULL_HANDLE; i = (i + 1) & (c->cap - 1))
		if (same(&c->table[i], key))
		{
			c->table[i].used = c->use;
			return c->table[i].set;
		}
	return VK_NULL_HANDLE;
}

VkDescriptorSet vk_desc_cache_add(vk_desc_cache *c, const TexDescKey *key)
{
	vk_desc_entry e;
	if ((c->count + 1) * 2 > c->cap)
		grow(c, c->cap == 0 ? 64 : c->cap * 2);
	if (c->nfree == 0 && !allocate(c))
		return VK_NULL_HANDLE;
	e.key = *key;
	e.used = c->use;
	e.set = c->freeSets[--c->nfree];
	insert(c, c->table, &e);
	c->count++;
	return e.set;
}

void vk_desc_cache_next_use(vk_desc_cache *c)
{
	if (c->cap != 0)
	{
		vk_desc_entry *t;
		u32 i;
		memset(c->spare, 0, c->cap * sizeof(vk_desc_entry));
		c->count = 0;
		for (i = 0; i < c->cap; i++)
		{
			if (c->table[i].set == VK_NULL_HANDLE)
				continue;
			if (c->table[i].used == c->use)
			{
				insert(c, c->spare, &c->table[i]);
				c->count++;
			}
			else
				c->freeSets[c->nfree++] = c->table[i].set;
		}
		t = c->table;
		c->table = c->spare;
		c->spare = t;
	}
	c->use++;
}

void vk_desc_cache_term(vk_desc_cache *c)
{
	if (c->cap != 0)
	{
		u32 i;
		for (i = 0; i < c->cap; i++)
			if (c->table[i].set != VK_NULL_HANDLE)
				c->freeSets[c->nfree++] = c->table[i].set;
		if (c->nfree != 0)
			vkFreeDescriptorSets(vkc.device, vkc.descriptorPool, c->nfree, c->freeSets);
	}
	free(c->table);
	free(c->spare);
	free(c->freeSets);
	c->table = c->spare = NULL;
	c->freeSets = NULL;
	c->cap = c->count = c->nfree = 0;
}
