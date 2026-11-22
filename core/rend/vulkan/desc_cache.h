/* The polygons' texture descriptor sets of one frame slot, kept from one use
 * of the slot to the next. A set is written once, for the views and samplers
 * it holds, and bound as it is for as long as the frames that use it keep
 * using them: what tells two views apart is the serial each was made with
 * (Texture::ReadOnlyViewSerial()), never a handle or a texture, which can
 * be made again at the same address. One a use of the slot did not bind is
 * written again for another - by then the frame that last bound it is done
 * on the GPU, which is what the slot's ring is for.
 *
 * Plain arrays: an open-addressed table, made again from its survivors at
 * each use of the slot into a second one as large, and a stack of free
 * sets; nothing is allocated once they are as large as the frames need. */
#ifndef VK_DESC_CACHE_H
#define VK_DESC_CACHE_H

#include <string.h>
#include "vk_context.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TexDescKey
{
	u64 view0, view1;   /* the views' serials; view1 0 where there is one */
	u32 sampler0, sampler1;
} TexDescKey;

typedef struct vk_desc_entry
{
	TexDescKey key;
	u64 used;
	VkDescriptorSet set;	/* null: an empty place */
} vk_desc_entry;

typedef struct vk_desc_cache
{
	VkDescriptorSetLayout layout;
	vk_desc_entry *table;
	vk_desc_entry *spare;
	VkDescriptorSet *freeSets;	/* cap + VK_DESC_BATCH: every set there is fits */
	u32 cap, count, nfree;
	u64 use;
} vk_desc_cache;

enum { VK_DESC_BATCH = 16, VK_DESC_RESERVE = 256, VK_DESC_START = 64 };

static INLINE void vk_desc_cache_construct(vk_desc_cache *c)
{
	memset(c, 0, sizeof(*c));
	c->use = 1;
}

/* (the tables and a batch of sets made now, with the drawer: one that
 * first draws while the game runs - a render to a texture - allocates
 * nothing then) */
void vk_desc_cache_set_layout(vk_desc_cache *c, VkDescriptorSetLayout layout);
/* The set for @key, bound by this use of the slot: one written for it
 * already, or VK_NULL_HANDLE - and then vk_desc_cache_add() has to have
 * one written */
VkDescriptorSet vk_desc_cache_find(vk_desc_cache *c, const TexDescKey *key);
/* A set for @key that the caller writes, then binds */
VkDescriptorSet vk_desc_cache_add(vk_desc_cache *c, const TexDescKey *key);
/* The slot is used again, and the GPU is done with its last use: the sets
 * that use did not bind are free to be written for others */
void vk_desc_cache_next_use(vk_desc_cache *c);
/* every set back to the pool, the tables freed */
void vk_desc_cache_term(vk_desc_cache *c);

#ifdef __cplusplus
}

#include "vulkan_context.h"

/* The C++ renderers' view, until they are C too */
class TexDescCache
{
public:
	TexDescCache() { vk_desc_cache_construct(&c); }
	~TexDescCache() { vk_desc_cache_term(&c); }
	TexDescCache(const TexDescCache&) = delete;
	TexDescCache& operator=(const TexDescCache&) = delete;
	/* (the frame slots' are in a vector: moved, the sets go with it) */
	TexDescCache(TexDescCache&& o) noexcept { c = o.c; vk_desc_cache_construct(&o.c); }
	TexDescCache& operator=(TexDescCache&& o) noexcept
	{
		if (this != &o)
		{
			vk_desc_cache_term(&c);
			c = o.c;
			vk_desc_cache_construct(&o.c);
		}
		return *this;
	}

	void SetLayout(vk::DescriptorSetLayout layout) { vk_desc_cache_set_layout(&c, (VkDescriptorSetLayout)layout); }
	vk::DescriptorSet Find(const TexDescKey& key) { return vk::DescriptorSet(vk_desc_cache_find(&c, &key)); }
	vk::DescriptorSet Add(const TexDescKey& key) { return vk::DescriptorSet(vk_desc_cache_add(&c, &key)); }
	void NextUse() { vk_desc_cache_next_use(&c); }

private:
	vk_desc_cache c;
};
#endif

#endif
