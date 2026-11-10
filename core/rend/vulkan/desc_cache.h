#pragma once
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
#include "vulkan_context.h"
#include <stdlib.h>
#include <string.h>

struct TexDescKey
{
	u64 view0, view1;   /* the views' serials; view1 0 where there is one */
	u32 sampler0, sampler1;
};

class TexDescCache
{
public:
	TexDescCache() {}
	~TexDescCache() { Term(); }
	TexDescCache(const TexDescCache&) = delete;
	TexDescCache& operator=(const TexDescCache&) = delete;
	/* (the frame slots' are in a vector: moved, the sets go with it) */
	TexDescCache(TexDescCache&& o) noexcept { Take(o); }
	TexDescCache& operator=(TexDescCache&& o) noexcept
	{
		if (this != &o)
		{
			Term();
			Take(o);
		}
		return *this;
	}

	/* (the tables and a batch of sets made now, with the drawer: one that
	 * first draws while the game runs - a render to a texture - allocates
	 * nothing then) */
	void SetLayout(vk::DescriptorSetLayout layout)
	{
		this->layout = layout;
		if (cap == 0)
		{
			Grow(RESERVE);
			if (nfree == 0)
				Allocate();
		}
	}

	/* The set for @key, bound by this use of the slot: one written for it
	 * already, or a null one - and then Add() has to have one written */
	vk::DescriptorSet Find(const TexDescKey& key)
	{
		if (cap == 0)
			return vk::DescriptorSet();
		for (u32 i = Hash(key) & (cap - 1); table[i].set; i = (i + 1) & (cap - 1))
			if (Same(table[i], key))
			{
				table[i].used = use;
				return table[i].set;
			}
		return vk::DescriptorSet();
	}

	/* A set for @key that the caller writes, then binds */
	vk::DescriptorSet Add(const TexDescKey& key)
	{
		if ((count + 1) * 2 > cap)
			Grow(cap == 0 ? 64 : cap * 2);
		if (nfree == 0 && !Allocate())
			return vk::DescriptorSet();
		Entry e;
		e.key = key;
		e.used = use;
		e.set = freeSets[--nfree];
		Insert(table, e);
		count++;
		return e.set;
	}

	/* The slot is used again, and the GPU is done with its last use: the
	 * sets that use did not bind are free to be written for others */
	void NextUse()
	{
		if (cap != 0)
		{
			memset(spare, 0, cap * sizeof(Entry));
			count = 0;
			for (u32 i = 0; i < cap; i++)
			{
				if (!table[i].set)
					continue;
				if (table[i].used == use)
				{
					Insert(spare, table[i]);
					count++;
				}
				else
					freeSets[nfree++] = table[i].set;
			}
			Entry *t = table;
			table = spare;
			spare = t;
		}
		use++;
	}

private:
	struct Entry
	{
		TexDescKey key;
		u64 used;
		vk::DescriptorSet set;      /* null: an empty place */
	};

	static u32 Hash(const TexDescKey& k)
	{
		u64 h = k.view0 * 0x9E3779B97F4A7C15ull;
		h ^= k.view1 + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
		h ^= ((u64)k.sampler0 << 32 | k.sampler1) * 0xC2B2AE3D27D4EB4Full;
		return (u32)(h ^ (h >> 32));
	}
	static bool Same(const Entry& e, const TexDescKey& k)
	{
		return e.key.view0 == k.view0 && e.key.view1 == k.view1 && e.key.sampler0 == k.sampler0 && e.key.sampler1 == k.sampler1;
	}
	void Insert(Entry *into, const Entry& e)
	{
		u32 i = Hash(e.key) & (cap - 1);
		while (into[i].set)
			i = (i + 1) & (cap - 1);
		into[i] = e;
	}

	/* The tables @size places each (a power of two), the entries there are
	 * moved over; the free stack as large as both can need */
	void Grow(u32 size)
	{
		Entry *t = (Entry *)calloc(size, sizeof(Entry));
		Entry *s = (Entry *)calloc(size, sizeof(Entry));
		vk::DescriptorSet *f = (vk::DescriptorSet *)malloc((size + BATCH) * sizeof(vk::DescriptorSet));
		const u32 old = cap;
		cap = size;
		for (u32 i = 0; i < old; i++)
			if (table[i].set)
				Insert(t, table[i]);
		if (nfree != 0)
			memcpy(f, freeSets, nfree * sizeof(vk::DescriptorSet));
		free(table);
		free(spare);
		free(freeSets);
		table = t;
		spare = s;
		freeSets = f;
	}

	/* BATCH more free sets from the pool */
	bool Allocate()
	{
		vk::DescriptorSetLayout layouts[BATCH];
		for (u32 i = 0; i < BATCH; i++)
			layouts[i] = layout;
		const vk::DescriptorSetAllocateInfo info(VulkanContext::Instance()->GetDescriptorPool(), BATCH, layouts);
		if (VulkanContext::Instance()->GetDevice().allocateDescriptorSets(&info, freeSets) != vk::Result::eSuccess)
			return false;
		nfree = BATCH;
		return true;
	}

	/* every set back to the pool, the tables freed */
	void Term()
	{
		if (cap != 0)
		{
			vk::Device device = VulkanContext::Instance()->GetDevice();
			vk::DescriptorPool pool = VulkanContext::Instance()->GetDescriptorPool();
			for (u32 i = 0; i < cap; i++)
				if (table[i].set)
					freeSets[nfree++] = table[i].set;
			if (nfree != 0)
				device.freeDescriptorSets(pool, nfree, freeSets);
		}
		free(table);
		free(spare);
		free(freeSets);
		table = spare = nullptr;
		freeSets = nullptr;
		cap = count = nfree = 0;
	}

	void Take(TexDescCache& o)
	{
		layout = o.layout;
		table = o.table;
		spare = o.spare;
		freeSets = o.freeSets;
		cap = o.cap;
		count = o.count;
		nfree = o.nfree;
		use = o.use;
		o.table = o.spare = nullptr;
		o.freeSets = nullptr;
		o.cap = o.count = o.nfree = 0;
	}

	enum { BATCH = 16, RESERVE = 256 };
	vk::DescriptorSetLayout layout;
	Entry *table = nullptr;
	Entry *spare = nullptr;
	vk::DescriptorSet *freeSets = nullptr;  /* cap + BATCH: every set there is fits */
	u32 cap = 0, count = 0, nfree = 0;
	u64 use = 1;
};
