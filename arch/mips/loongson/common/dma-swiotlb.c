#include <linux/mm.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/swiotlb.h>
#include <linux/bootmem.h>

#include <asm/bootinfo.h>
#include <boot_param.h>
#include <loongson-pch.h>
#include <dma-coherence.h>
#include <loongson.h>

extern bool loongson_acpiboot_flag;
static inline void *dma_to_virt(struct device *dev, dma_addr_t dma_addr)
{
	return phys_to_virt(dma_to_phys(dev, dma_addr));
}

static void *loongson_dma_alloc_coherent(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp, struct dma_attrs *attrs)
{
	void *ret;

	if (dma_alloc_from_coherent(dev, size, dma_handle, &ret))
		return ret;

	/* ignore region specifiers */
	gfp &= ~(__GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM);

#ifdef CONFIG_ZONE_DMA
	if (dev == NULL)
		gfp |= __GFP_DMA;
	else if (dev->coherent_dma_mask <= DMA_BIT_MASK(24))
		gfp |= __GFP_DMA;
	else
#endif
#ifdef CONFIG_ZONE_DMA32
	if (dev == NULL)
		gfp |= __GFP_DMA32;
	else if (dev->coherent_dma_mask <= DMA_BIT_MASK(32))
		gfp |= __GFP_DMA32;
	else
#endif
	;
	gfp |= __GFP_NORETRY|__GFP_NOWARN;

	ret = swiotlb_alloc_coherent(dev, size, dma_handle, gfp);
	if (!plat_device_is_coherent(dev)) {
		dma_cache_wback_inv((unsigned long)dma_to_virt(dev, *dma_handle), size);
		ret = UNCAC_ADDR(ret);
	}
	mb();

	return ret;
}

static void loongson_dma_free_coherent(struct device *dev, size_t size,
				void *vaddr, dma_addr_t dma_handle, struct dma_attrs *attrs)
{
	int order = get_order(size);

	if (dma_release_from_coherent(dev, order, vaddr))
		return;

	if (!plat_device_is_coherent(dev)) {
		vaddr = CAC_ADDR(vaddr);
		dma_cache_wback_inv((unsigned long)dma_to_virt(dev, dma_handle), size);
	}
	swiotlb_free_coherent(dev, size, vaddr, dma_handle);
}

#define PCIE_DMA_ALIGN 16

static dma_addr_t loongson_dma_map_page(struct device *dev, struct page *page,
				unsigned long offset, size_t size,
				enum dma_data_direction dir,
				struct dma_attrs *attrs)
{
	dma_addr_t daddr;

	if (offset % PCIE_DMA_ALIGN)
		daddr = swiotlb_map_page(dev, page, offset, size, dir, &dev->archdata.dma_attrs);
	else
		daddr = swiotlb_map_page(dev, page, offset, size, dir, NULL);

	if (!plat_device_is_coherent(dev))
		dma_cache_sync(dev, dma_to_virt(dev, daddr), size, dir);
	mb();

	return daddr;
}

static void loongson_dma_unmap_page(struct device *dev, dma_addr_t dev_addr,
			size_t size, enum dma_data_direction dir,
			struct dma_attrs *attrs)
{
	if (!plat_device_is_coherent(dev))
		dma_cache_sync(dev, dma_to_virt(dev, dev_addr), size, dir);
	swiotlb_unmap_page(dev, dev_addr, size, dir, attrs);
}

static int loongson_dma_map_sg(struct device *dev, struct scatterlist *sgl,
				int nents, enum dma_data_direction dir,
				struct dma_attrs *attrs)
{
	int i, r;
	struct scatterlist *sg;

	r = swiotlb_map_sg_attrs(dev, sgl, nents, dir,
					&dev->archdata.dma_attrs);
	if (!plat_device_is_coherent(dev)) {
		for_each_sg(sgl, sg, nents, i)
			dma_cache_sync(dev, dma_to_virt(dev, sg->dma_address), sg->length, dir);
	}
	mb();

	return r;
}

static void loongson_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
			int nelems, enum dma_data_direction dir,
			struct dma_attrs *attrs)
{
	int i;
	struct scatterlist *sg;

	if (!plat_device_is_coherent(dev) && dir != DMA_TO_DEVICE) {
		for_each_sg(sgl, sg, nelems, i)
			dma_cache_sync(dev, dma_to_virt(dev, sg->dma_address), sg->length, dir);
	}

	swiotlb_unmap_sg_attrs(dev, sgl, nelems, dir, attrs);
}

static void loongson_dma_sync_single_for_cpu(struct device *dev, dma_addr_t dev_addr,
			size_t size, enum dma_data_direction dir)
{
	if (!plat_device_is_coherent(dev))
		dma_cache_sync(dev, dma_to_virt(dev, dev_addr), size, dir);
	swiotlb_sync_single_for_cpu(dev, dev_addr, size, dir);
}

static void loongson_dma_sync_single_for_device(struct device *dev,
				dma_addr_t dma_handle, size_t size,
				enum dma_data_direction dir)
{
	swiotlb_sync_single_for_device(dev, dma_handle, size, dir);
	if (!plat_device_is_coherent(dev))
		dma_cache_sync(dev, dma_to_virt(dev, dma_handle), size, dir);
	mb();
}

static void loongson_dma_sync_sg_for_cpu(struct device *dev,
				struct scatterlist *sgl, int nents,
				enum dma_data_direction dir)
{
	int i;
	struct scatterlist *sg;

	if (!plat_device_is_coherent(dev)) {
		for_each_sg(sgl, sg, nents, i) {
			dma_cache_sync(dev, dma_to_virt(dev,
				sg->dma_address), sg->length, dir);
		}
	}
	swiotlb_sync_sg_for_cpu(dev, sgl, nents, dir);
}

static void loongson_dma_sync_sg_for_device(struct device *dev,
				struct scatterlist *sgl, int nents,
				enum dma_data_direction dir)
{
	int i;
	struct scatterlist *sg;

	swiotlb_sync_sg_for_device(dev, sgl, nents, dir);
	if (!plat_device_is_coherent(dev)) {
		for_each_sg(sgl, sg, nents, i) {
			dma_cache_sync(dev, dma_to_virt(dev,
				sg->dma_address), sg->length, dir);
		}
	}
	mb();
}


static dma_addr_t loongson_ls2h_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	return (paddr < 0x10000000) ? paddr : (paddr - 0x80000000);
}

static phys_addr_t loongson_ls2h_dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	return (daddr < 0x10000000) ? daddr : (daddr + 0x80000000);
}

static dma_addr_t loongson_rs780_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	long nid;
	dma_addr_t daddr;

	daddr = (paddr < 0x10000000) ?
			(paddr | 0x0000000080000000) : paddr;
#ifdef CONFIG_PHYS48_TO_HT40
	 /* We extract 2bit node id (bit 44~47, only bit 44~45 used now) from
	  * Loongson3's 48bit address space and embed it into 40bit */
	nid = (paddr >> 44) & 0x3;
	daddr = ((nid << 44 ) ^ daddr) | (nid << 37);
#endif
	return daddr;
}

static phys_addr_t loongson_rs780_dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	long nid;

	daddr = (daddr < 0x90000000 && daddr >= 0x80000000) ?
			(daddr & 0x0fffffff) : daddr;
#ifdef CONFIG_PHYS48_TO_HT40
	nid = (daddr >> 37) & 0x3;
	daddr = ((nid << 37 ) ^ daddr) | (nid << 44);
#endif
	return daddr;
}

extern u32 node_id_offset;
static dma_addr_t loongson_ls7a_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
#ifdef CONFIG_PHYS48_TO_HT40
	long nid;
	 /* We extract 2bit node id (bit 44~47, only bit 44~45 used now) from
	  * Loongson3's 48bit address space and embed it into 40bit */
	nid = (paddr >> 44) & 0x3;
	paddr = ((nid << 44 ) ^ paddr) | (nid << (36 + node_id_offset));
#endif
	return paddr;
}

static phys_addr_t loongson_ls7a_dma_to_phys(struct device *dev, dma_addr_t daddr)
{
#ifdef CONFIG_PHYS48_TO_HT40
	long nid;
	nid = (daddr >> (36 + node_id_offset)) & 0x3;
	daddr = ((nid << (36 + node_id_offset)) ^ daddr) | (nid << 44);
#endif
	return daddr;
}

static dma_addr_t loongson_virt_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	return paddr;
}

static phys_addr_t loongson_virt_dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	return daddr;
}

static dma_addr_t loongson_phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	int i;
	dma_addr_t daddr = 0UL;

	for (i = 0; i < LOONGSON3_BOOT_MEM_MAP; i++){
		if(( paddr >= ls_phy_map[i].mem_start) && (paddr < (ls_phy_map[i].mem_start + ls_phy_map[i].mem_size))){
			daddr = ls_dma_map[i].mem_start + (paddr - ls_phy_map[i].mem_start);
			break;
		}
	}

	if (!daddr)
		printk("phys_to_dma error, daddr is 0x%llx\n", daddr);
	return daddr;
}

static phys_addr_t loongson_dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	int i;
	phys_addr_t paddr = 0UL;

	for (i = 0; i < LOONGSON3_BOOT_MEM_MAP; i++){
		if(( daddr >= ls_dma_map[i].mem_start) && (daddr < (ls_dma_map[i].mem_start + ls_dma_map[i].mem_size))){
			paddr = ls_phy_map[i].mem_start + (daddr - ls_dma_map[i].mem_start);
			break;
		}
	}
	if (!daddr)
		printk("dma_to_phys error, paddr is 0x%llx\n", paddr);
	return paddr;

}

struct loongson_dma_map_ops {
	struct dma_map_ops dma_map_ops;
	dma_addr_t (*phys_to_dma)(struct device *dev, phys_addr_t paddr);
	phys_addr_t (*dma_to_phys)(struct device *dev, dma_addr_t daddr);
};

dma_addr_t phys_to_dma(struct device *dev, phys_addr_t paddr)
{
	struct loongson_dma_map_ops *ops = container_of(get_dma_ops(dev),
					struct loongson_dma_map_ops, dma_map_ops);

	return ops->phys_to_dma(dev, paddr);
}

phys_addr_t dma_to_phys(struct device *dev, dma_addr_t daddr)
{
	struct loongson_dma_map_ops *ops = container_of(get_dma_ops(dev),
					struct loongson_dma_map_ops, dma_map_ops);

	return ops->dma_to_phys(dev, daddr);
}

static int loongson_dma_set_mask(struct device *dev, u64 mask)
{
	extern u32 loongson_dma_mask_bits;

	if (mask > DMA_BIT_MASK(loongson_dma_mask_bits)) {
		*dev->dma_mask = DMA_BIT_MASK(loongson_dma_mask_bits);
		return -EIO;
	}

	*dev->dma_mask = mask;

	return 0;
}

static struct loongson_dma_map_ops loongson_linear_dma_map_ops = {
	.dma_map_ops = {
		.alloc = loongson_dma_alloc_coherent,
		.free = loongson_dma_free_coherent,
		.map_page = loongson_dma_map_page,
		.unmap_page = loongson_dma_unmap_page,
		.map_sg = loongson_dma_map_sg,
		.unmap_sg = loongson_dma_unmap_sg,
		.sync_single_for_cpu = loongson_dma_sync_single_for_cpu,
		.sync_single_for_device = loongson_dma_sync_single_for_device,
		.sync_sg_for_cpu = loongson_dma_sync_sg_for_cpu,
		.sync_sg_for_device = loongson_dma_sync_sg_for_device,
		.mapping_error = swiotlb_dma_mapping_error,
		.dma_supported = swiotlb_dma_supported,
		.set_dma_mask = loongson_dma_set_mask
	},
	.phys_to_dma = loongson_rs780_phys_to_dma,
	.dma_to_phys = loongson_rs780_dma_to_phys
};

void __init plat_swiotlb_setup(void)
{
	bool support_dma_map = false;
	swiotlb_init(1);
	mips_dma_map_ops = &loongson_linear_dma_map_ops.dma_map_ops;
	
	if (loongson_acpiboot_flag) {
		support_dma_map = true;
	} else {
		if (emap->vers >= 2)
			support_dma_map = true;	
	}

	if(support_dma_map && ls_dma_map[0].mem_size){
		loongson_linear_dma_map_ops.phys_to_dma = loongson_phys_to_dma;
		loongson_linear_dma_map_ops.dma_to_phys = loongson_dma_to_phys;
	}else{
		if (loongson_pch && loongson_pch->board_type == LS2H) {
			loongson_linear_dma_map_ops.phys_to_dma = loongson_ls2h_phys_to_dma;
			loongson_linear_dma_map_ops.dma_to_phys = loongson_ls2h_dma_to_phys;
		} else if (loongson_pch && loongson_pch->board_type == LS7A) {
			loongson_linear_dma_map_ops.phys_to_dma = loongson_ls7a_phys_to_dma;
			loongson_linear_dma_map_ops.dma_to_phys = loongson_ls7a_dma_to_phys;
		}
	}

	if (cpu_guestmode) {
		loongson_linear_dma_map_ops.phys_to_dma = loongson_virt_phys_to_dma;
		loongson_linear_dma_map_ops.dma_to_phys = loongson_virt_dma_to_phys;
	}
}
