snd-soc-core-objs := soc-core.o soc-dapm.o soc-jack.o soc-cache.o soc-utils.o
snd-soc-core-objs += soc-pcm.o soc-io.o soc-devres.o soc-ops.o
snd-soc-core-$(CONFIG_SND_SOC_COMPRESS) += soc-compress.o

ifneq ($(CONFIG_SND_SOC_TOPOLOGY),)
snd-soc-core-objs += soc-topology.o
endif

ifneq ($(CONFIG_SND_SOC_GENERIC_DMAENGINE_PCM),)
snd-soc-core-objs += soc-generic-dmaengine-pcm.o
endif

obj-$(CONFIG_SND_SOC)	+= snd-soc-core.o
obj-$(CONFIG_SND_SOC)	+= codecs/
obj-$(CONFIG_SND_SOC)	+= intel/
obj-$(CONFIG_SND_SOC)  += loongson/
