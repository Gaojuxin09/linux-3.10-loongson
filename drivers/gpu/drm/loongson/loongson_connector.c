/*
 * Copyright (c) 2018 Loongson Technology Co., Ltd.
 * Authors:
 *	Zhu Chen <zhuchen@loongson.cn>
 *	Fang Yaling <fangyaling@loongson.cn>
 *	Zhang Dandan <zhangdandan@loongson.cn>
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/export.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>
#include <linux/pm_runtime.h>
#include <drm/drmP.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_edid.h>

#include "loongson_drv.h"
#include "config-ch7034.h"

#define adapter_to_i2c_client(d) container_of(d, struct i2c_client, adapter)

#define DVO_I2C_NAME "loongson_dvo_i2c"

static struct eep_info{
	struct i2c_adapter *adapter;
	unsigned short addr;
}eeprom_info[2];

struct i2c_client * loongson_drm_i2c_client[2];

/**
 * loongson_connector_best_encoder
 *
 * @connector: point to the drm_connector structure
 *
 * Select the best encoder for the given connector.Used by both the helpers(in the
 * drm_atomic_helper_check_modeset() function)and in the legacy CRTC helpers
 */
static struct drm_encoder *loongson_connector_best_encoder(struct drm_connector
						  *connector)
{
	int enc_id = connector->encoder_ids[0];
	/* pick the encoder ids */
	if (enc_id)
		return drm_encoder_find(connector->dev, enc_id);
	return NULL;
}


/**
 * loongson_do_probe_ddc_edid
 *
 * @adapter: I2C device adapter
 *
 * Try to fetch EDID information by calling I2C driver functions
 */
static bool loongson_do_probe_ddc_edid(struct i2c_adapter *adapter, unsigned int id, unsigned char *buf)
{
	unsigned char start = 0x0;
	unsigned int che_tmp = 0;
	unsigned int i;
	struct i2c_msg msgs[] = {
		{
			.addr = 0x50,
			.flags = 0,
			.len = 1,
			.buf = &start,
		},{
			.addr = 0x50,
			.flags = I2C_M_RD,
			.len = EDID_LENGTH * 2,
			.buf = buf,
		}
	};
	if (i2c_transfer(adapter, msgs, 2) == 2) {
		if (buf[126] != 0) {
			buf[126] = 0;
			che_tmp = 0;
			for(i = 0;i < 127;i++) {
				che_tmp += buf[i];
			}
			buf[127] = 256-(che_tmp)%256;
		}
		if (!drm_edid_block_valid(buf, 0, true, NULL)) {
                        dev_warn_once(&adapter->dev, "Invalid EDID block\n");
                        return false;
                }
        } else {
                 dev_warn_once(&adapter->dev, "unable to read EDID block\n");
                 return false;
        }
        return true;
}

/**
 * loongson_i2c_connector
 *
 * According to i2c bus,acquire screen information
 */
static bool loongson_i2c_connector(unsigned int id, unsigned char *buf)
{
	if (eeprom_info[id].adapter)
		return loongson_do_probe_ddc_edid(eeprom_info[id].adapter, id, buf);
	else
		return false;
}


/**
 * loongson_vga_get_modes
 *
 * @connetcor: central DRM connector control structure
 *
 * Fill in all modes currently valid for the sink into the connector->probed_modes list.
 * It should also update the EDID property by calling drm_mode_connector_update_edid_property().
 */
static int loongson_vga_get_modes(struct drm_connector *connector)
{
	struct drm_device *dev = connector->dev;
	struct loongson_drm_device *ldev = dev->dev_private;
        enum loongson_edid_method ledid_method;
        unsigned char *buf = kmalloc(EDID_LENGTH *2, GFP_KERNEL);
	struct edid *edid = NULL;
	bool dret = false;
	int ret = 0;

	if (!buf) {
                dev_warn(dev->dev, "Unable to allocate memory for EDID block\n");
                return 0;
        }
	ledid_method = ldev->connector_vbios[drm_connector_index(connector)]->edid_method;

	DRM_DEBUG("connecotro_id = %d\n",connector->index);
	if (ledid_method == edid_method_i2c) {
                dret = loongson_i2c_connector(connector->index, buf);
                if (dret) {
                        edid = (struct edid *)buf;
                        drm_mode_connector_update_edid_property(connector, edid);
                        ret = drm_add_edid_modes(connector, edid);
                }
        }
	kfree(buf);
	return ret;
}

/*
 * loongson_vga_mode_valid
 *
 * @connector: point to the drm connector
 * @mode: point to the drm_connector structure
 *
 * Validate a mode for a connector, irrespective of the specific display configuration
 */
static int loongson_vga_mode_valid(struct drm_connector *connector,
				 struct drm_display_mode *mode)
{
	struct drm_device *dev = connector->dev;
	struct loongson_drm_device *ldev = (struct loongson_drm_device*)dev->dev_private;
	int id = connector->index;

        if(mode->hdisplay % 64)
		return MODE_BAD;
	if(mode->hdisplay > ldev->crtc_vbios[id]->crtc_max_weight || mode->vdisplay > ldev->crtc_vbios[id]->crtc_max_height)
		return MODE_BAD;
	return MODE_OK;
}

/**
 * loongson_i2c_destroy
 *
 * @i2c: point to loongson_i2c_chan
 *
 * Destroy i2c adapter
 */
void loongson_i2c_destroy(struct loongson_i2c_chan *i2c)
{
	if (!i2c)
		return;
	i2c_del_adapter(&i2c->adapter);
	kfree(i2c);
}

/* Power on when there is noconnector,support hotplug */
static bool poweron_flags = true;

/**
 * loongson_vga_boots
 *
 * Power on when the host is not connected to any monitor
 */
static enum drm_connector_status loongson_vga_boots(enum drm_connector_status status,
                                                        enum drm_connector_status ret)
{
        if (status == connector_status_unknown &&
			ret == connector_status_disconnected)
        {
                ret = connector_status_connected;
        }
        if (status == connector_status_connected &&
			ret == connector_status_connected)
        {
                poweron_flags = false;
        }
        if (poweron_flags == true && status == connector_status_connected &&
			ret == connector_status_disconnected)
        {
                ret =connector_status_connected;
        }
        return ret;
}

/**
 * loongson_vga_detect
 *
 * @connector: point to drm_connector
 * @force: bool
 *
 * Check to see if anything is attached to the connector.
 * The parameter force is set to false whilst polling,
 * true when checking the connector due to a user request
 */
static enum drm_connector_status loongson_vga_detect(struct drm_connector
						   *connector, bool force)
{
	struct drm_device *dev = connector->dev;
	struct loongson_drm_device *ldev = dev->dev_private;
        enum drm_connector_status ret = connector_status_disconnected;
	enum loongson_edid_method ledid_method;
	int i;
	enum drm_connector_status status;
	status = connector->status;

	ledid_method = ldev->connector_vbios[connector->index]->edid_method;

	DRM_DEBUG("loongson_vga_detect connect_id=%d, ledid_method=%d\n", connector->index, ledid_method);

	if (ledid_method == edid_method_i2c) {
	        i = pm_runtime_get_sync(connector->dev->dev);
        	if (i < 0)
			ret = connector_status_disconnected;
	        i = loongson_vga_get_modes(connector);
        	if (i)
		{
			DRM_DEBUG("loongson_vga_detect: connected");
			ret = connector_status_connected;
		}
		ret = loongson_vga_boots(status,ret);

		pm_runtime_mark_last_busy(connector->dev->dev);
		pm_runtime_put_autosuspend(connector->dev->dev);
	} else if (ledid_method == edid_method_phy) {//TODO: need add more status check in this type
		ret = connector_status_connected;
	} else if (ledid_method == edid_method_null ||
			ledid_method == edid_method_vbios) {
		ret = connector_status_connected;
	}

  	return ret;
}

/**
 * loongson_connector_destroy
 *
 * @connector: point to the drm_connector structure
 *
 * Clean up connector resources
 */
static void loongson_connector_destroy(struct drm_connector *connector)
{
	struct loongson_connector *loongson_connector = to_loongson_connector(connector);
	loongson_i2c_destroy(loongson_connector->i2c);
	drm_connector_cleanup(connector);
	kfree(connector);
}


/**
 * These provide the minimum set of functions required to handle a connector
 *
 * Helper operations for connectors.These functions are used
 * by the atomic and legacy modeset helpers and by the probe helpers.
 */
static const struct drm_connector_helper_funcs loongson_vga_connector_helper_funcs = {
        .get_modes = loongson_vga_get_modes,
        .mode_valid = loongson_vga_mode_valid,
        .best_encoder = loongson_connector_best_encoder,
};


/**
 * These provide the minimum set of functions required to handle a connector
 *
 * Control connectors on a given device.
 * The functions below allow the core DRM code to control connectors,
 * enumerate available modes and so on.
 */
static const struct drm_connector_funcs loongson_vga_connector_funcs = {
        .dpms = drm_helper_connector_dpms,
        .detect = loongson_vga_detect,
        .fill_modes = drm_helper_probe_single_connector_modes,
        .destroy = loongson_connector_destroy,
};


static const unsigned short normal_i2c[] = { 0x50, I2C_CLIENT_END };




#ifdef CONFIG_DRM_LOONGSON_VGA_PLATFORM

static const struct i2c_device_id dvi_eep_ids[] = {
	{ "dvi-eeprom-edid", 0 },
	{ /* END OF LIST */ }
};

static const struct i2c_device_id vga_eep_ids[] = {
	{ "eeprom-edid", 2 },
	{ /* END OF LIST */ }
};
MODULE_DEVICE_TABLE(i2c, dvi_eep_ids);
MODULE_DEVICE_TABLE(i2c, vga_eep_ids);

static int dvi_eep_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	eeprom_info[0].adapter = client->adapter;
	eeprom_info[0].addr = client->addr;
	return 0;
}


static int vga_eep_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	eeprom_info[1].adapter = client->adapter;
	eeprom_info[1].addr = client->addr;
	return 0;
}



static int eep_remove(struct i2c_client *client)
{
	i2c_unregister_device(client);
	return 0;
}
static struct i2c_driver vga_eep_driver = {
	.driver = {
		.name = "vga_eep-edid",
		.owner = THIS_MODULE,
	},
	.probe = vga_eep_probe,
	.remove = eep_remove,
	.id_table = vga_eep_ids,
};

static struct i2c_driver dvi_eep_driver = {
	.driver = {
		.name = "dvi_eep-edid",
		.owner = THIS_MODULE,
	},
	.probe = dvi_eep_probe,
	.remove = eep_remove,
	.id_table = dvi_eep_ids,
};



#endif


/**
 * loongson_vga_init
 *
 * @dev: drm device
 * @connector_id:
 *
 * Vga is the interface between host and monitor
 * This function is to init vga
 */
struct drm_connector *loongson_vga_init(struct drm_device *dev,unsigned int connector_id)
{
	struct drm_connector *connector;
	struct loongson_connector *loongson_connector;
	struct loongson_drm_device *ldev = (struct loongson_drm_device*)dev->dev_private;
	struct i2c_adapter * i2c_adap;
	struct i2c_board_info i2c_info;


	loongson_connector = kzalloc(sizeof(struct loongson_connector), GFP_KERNEL);
	if (!loongson_connector)
		return NULL;

#ifdef CONFIG_DRM_LOONGSON_VGA_PLATFORM
	if(connector_id == 0){
		i2c_add_driver(&dvi_eep_driver);
	}else{
		i2c_add_driver(&vga_eep_driver);
	}
#else
	i2c_adap = i2c_get_adapter(ldev->connector_vbios[connector_id]->i2c_id);
	memset(&i2c_info, 0, sizeof(struct i2c_board_info));
	strlcpy(i2c_info.type, DVO_I2C_NAME, I2C_NAME_SIZE);
	i2c_info.addr = normal_i2c[0];
	loongson_drm_i2c_client[connector_id] = i2c_new_device(i2c_adap, &i2c_info);
	i2c_put_adapter(i2c_adap);

	if(loongson_drm_i2c_client[connector_id] != NULL){
		eeprom_info[connector_id].adapter= loongson_drm_i2c_client[connector_id]->adapter;
		eeprom_info[connector_id].addr = 0x50;
	}else{
		return NULL;
	}
#endif
	connector = &loongson_connector->base;

	drm_connector_init(dev, connector,
			   &loongson_vga_connector_funcs, DRM_MODE_CONNECTOR_VGA);

	drm_connector_helper_add(connector, &loongson_vga_connector_helper_funcs);

	drm_connector_register(connector);
	return connector;
}

static void loongson_connector_reset_ch7034(struct i2c_adapter *adapter)
{
	int i;
	unsigned char *buf = kmalloc(2*sizeof(unsigned char), GFP_KERNEL);
	struct i2c_msg msgs = {
		.addr = 0x75,
		.flags = 0,
		.len = 2,
		.buf = buf,
	};

	if (!buf){
		dev_warn(&adapter->dev, "unable to allocate memory for CH7034"
			"block.\n");
		return;
	}
	for (i = 0; i < 131; i++) {
		msgs.buf[0] = CH7034_VGA_REG_TABLE[0][i][0];
		msgs.buf[1] = CH7034_VGA_REG_TABLE[0][i][1];
		if (i2c_transfer(adapter, &msgs, 1) != 1)
			printk("i %d buf 0x%x 0x%x\n", i, msgs.buf[0], msgs.buf[1]);
	}
	kfree(buf);
	return;
}

#define OFFSET0 0xc7
#define OFFSET1 0x1e
#define OFFSET2 0x1a
static void loongson_connector_reset_ch9022(struct i2c_adapter *adapter)
{
	int cnt = 100;
	unsigned char rbuf;
	unsigned char offset;
	unsigned char *wbuf = kmalloc(2*sizeof(unsigned char), GFP_KERNEL);
	struct i2c_msg msgsw = {
		.addr =0x39,
		.flags = 0,
		.len = 2,
		.buf = wbuf,
	};
	struct i2c_msg msgsr[] = {
		{
			.addr = 0x39,
			.flags = 0,
			.len = 1,
			.buf = &offset,
		},{
			.addr = 0x39,
			.flags = 1,
			.len = 1,
			.buf = &rbuf,
		}
	};

	/*write 0xc7 0x00*/
	msgsw.buf[0] = OFFSET0;
	msgsw.buf[1] = 0x00;
	if (i2c_transfer(adapter, &msgsw, 1) != 1) {
		DRM_INFO("write 0xc7 err\n");
		goto wfree;
	}

        /*read 0x1b*/
	offset =0x1b;
	msgsr[0].buf = &offset;
	if (i2c_transfer(adapter, msgsr, 2) != 2) {
		DRM_INFO("read 0x1b error\n");
		goto wfree;
	}

	/*read 0x1c*/
	offset =0x1c;
	msgsr[0].buf = &offset;
	if (i2c_transfer(adapter, msgsr, 2) != 2) {
		DRM_INFO("read 0x1c error\n");
		goto wfree;
	}

	/*read 0x1d*/
	offset =0x1d;
	msgsr[0].buf = &offset;
	if (i2c_transfer(adapter, msgsr, 2) != 2) {
		DRM_INFO("read 0x1d error\n");
		goto wfree;
	}

	/* read 0x1e*/
	offset =OFFSET1;
	msgsr[0].buf = &offset;
	if (i2c_transfer(adapter, msgsr, 2) != 2) {
		DRM_INFO("read 0x1e error\n");
		goto wfree;
	}

	/* write 0x1e*/
	rbuf &= ~(0x3);
	msgsw.buf[0] = OFFSET1;
	msgsw.buf[1] = rbuf;
	if (i2c_transfer(adapter, &msgsw, 1) != 1) {
		DRM_INFO("write 0x1e err\n");
		goto wfree;
	}

	/* read 0x1a */
	offset = OFFSET2;
	msgsr[0].buf = &offset;
	if (i2c_transfer(adapter, msgsr, 2) != 2) {
		DRM_INFO("read 0x1a err\n");
		goto wfree;
	}

	/* write 0x1a */
	rbuf &= ~(1 << 4);
	msgsw.buf[0] = OFFSET2;
	msgsw.buf[1] = rbuf;
	if (i2c_transfer(adapter, &msgsw, 1) != 1) {
		DRM_INFO("write 0x1a err\n");
		goto wfree;
	}
	DRM_INFO("reset 9022 success\n");

wfree:
	kfree(wbuf);
}

#define I2C_9022_GPIO_ID   6
#define I2C_7034_GPIO_ID   7

void loongson_connector_resume(struct loongson_drm_device *ldev)
{
	int i;

	for (i = 0; i<ldev->num_crtc; i++) {
		DRM_INFO("loongson_connector_resume connector_id=%d i2c_id=%d\n", i, ldev->connector_vbios[i]->i2c_id);
		if (ldev->connector_vbios[i]->i2c_id == I2C_7034_GPIO_ID) {
			loongson_connector_reset_ch7034(eeprom_info[i].adapter);
		} else if (ldev->connector_vbios[i]->i2c_id == I2C_9022_GPIO_ID) {
			loongson_connector_reset_ch9022(eeprom_info[i].adapter);
		}
	}
}
