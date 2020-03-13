/*
 * Copyright (c) 2017 lambdadroid (https://github.com/lambdadroid)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define LOG_TAG "drm-fb"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <log/log.h>

#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_framebuffer.h"
#include <android/gralloc_handle.h>

#define SWAP_INTERVAL 1

struct drm_framebuffer {
	struct framebuffer_device_t device;

	int fd;
	uint32_t connector_id, crtc_id;
	drmModeModeInfo mode;

	uint32_t current_fb, next_fb;
	drmEventContext evctx;
	struct drm_framebuffer **fb_out;
};

static drmModeConnectorPtr fb0_find_connector(int fd, drmModeResPtr res)
{
	drmModeConnectorPtr connector;
	int i;

	connector = NULL;
	for (i = 0; i < res->count_connectors; ++i) {
		connector = drmModeGetConnector(fd, res->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			break;
		}

		drmModeFreeConnector(connector);
		connector = NULL;
	}

	return connector;
}

static uint32_t fb0_find_crtc(int fd, drmModeResPtr res, drmModeConnectorPtr connector)
{
	drmModeEncoderPtr encoder;
	int i;

	encoder = drmModeGetEncoder(fd, connector->encoders[0]);
	for (i = 0; i < res->count_crtcs; ++i) {
		if (encoder->possible_crtcs & (1 << i)) {
			drmModeFreeEncoder(encoder);
			return res->crtcs[i];
		}
	}

	drmModeFreeEncoder(encoder);
	return 0;
}

static drmModeModeInfoPtr fb0_find_preferred_mode(drmModeConnectorPtr connector)
{
	int i;
	drmModeModeInfoPtr mode = NULL;
	char value[PROPERTY_VALUE_MAX];
	uint32_t xres = 0, yres = 0, rate = 0;
	if (property_get("debug.drm.mode.force", value, NULL)) {
		/* parse <xres>x<yres>[@<refreshrate>] */
		if (sscanf(value, "%dx%d@%d", &xres, &yres, &rate) != 3) {
			rate = 0;
			if (sscanf(value, "%dx%d", &xres, &yres) != 2) {
				xres = yres = 0;
			}
		}
		ALOGI_IF(xres && yres, "force mode to %dx%d@%dHz", xres, yres, rate);
	}

	for (i = 0; i < connector->count_modes; ++i) {
		mode = &connector->modes[i];
		if (xres && yres) {
			if (mode->hdisplay == xres && mode->vdisplay == yres &&
					(!rate || mode->vrefresh == rate)) {
				break;
			}
		} else if (mode->type & DRM_MODE_TYPE_PREFERRED) {
			break;
		}
	}

	return mode;
}

static void fb0_handle_page_flip(
	__unused int fd, __unused unsigned int sequence,
	__unused unsigned int tv_sec, __unused unsigned int tv_usec,
	__unused void *data)
{
	struct drm_framebuffer *fb = data;
	fb->current_fb = fb->next_fb;
	fb->next_fb = 0;
}

static int fb0_init(struct drm_framebuffer *fb)
{
	drmModeResPtr res;
	drmModeConnectorPtr connector;
	drmModeModeInfoPtr mode;

	res = drmModeGetResources(fb->fd);

	connector = fb0_find_connector(fb->fd, res);
	if (!connector) {
		ALOGE("No connector found");
		drmModeFreeResources(res);
		return -ENODEV;
	}

	fb->connector_id = connector->connector_id;

	fb->crtc_id = fb0_find_crtc(fb->fd, res, connector);
	drmModeFreeResources(res);
	if (!fb->crtc_id) {
		ALOGE("No CRTC found");
		return -ENODEV;
	}

	ALOGI("Connector: %d, CRTC: %d", fb->connector_id, fb->crtc_id);

	mode = fb0_find_preferred_mode(connector);
	if (!mode) {
		ALOGE("No preferred mode found");
		drmModeFreeConnector(connector);
		return -ENODEV;
	}

	fb->mode = *mode;
	fb->current_fb = 0;
	fb->next_fb = 0;

	*(uint32_t*) &fb->device.flags = 0;
	*(uint32_t*) &fb->device.width = mode->hdisplay;
	*(uint32_t*) &fb->device.height = mode->vdisplay;
	*(int*) &fb->device.stride = mode->vdisplay;
	/* Note: The format specified here seems to be entirely ignored... */
	*(int*) &fb->device.format = HAL_PIXEL_FORMAT_RGBA_8888;
	*(float*) &fb->device.xdpi = mode->hdisplay * 25.4 / connector->mmWidth;
	*(float*) &fb->device.ydpi = mode->vdisplay * 25.4 / connector->mmHeight;
	*(float*) &fb->device.fps = mode->vrefresh;
	*(int*) &fb->device.minSwapInterval = SWAP_INTERVAL;
	*(int*) &fb->device.maxSwapInterval = SWAP_INTERVAL;

	memset(&fb->evctx, 0, sizeof(fb->evctx));
	fb->evctx.version = DRM_EVENT_CONTEXT_VERSION;
	fb->evctx.page_flip_handler = fb0_handle_page_flip;

	drmModeFreeConnector(connector);
	return 0;
}

static void fb0_await_page_flip(struct drm_framebuffer *fb)
{
	if (fb->next_fb) {
		/* There is another flip pending */
		drmHandleEvent(fb->fd, &fb->evctx);
		if (fb->next_fb) {
			ALOGE("drmHandleEvent returned without flipping");
			fb->current_fb = fb->next_fb;
			fb->next_fb = 0;
		}
	}
}

static int fb0_page_flip(struct drm_framebuffer *fb, int fb_id)
{
	int ret;

	/* Finish current page flip */
	fb0_await_page_flip(fb);

	ret = drmModePageFlip(fb->fd, fb->crtc_id, fb_id,
		DRM_MODE_PAGE_FLIP_EVENT, fb);
	if (ret) {
		ALOGE("Failed to perform page flip: %d", ret);
		if (errno != -EBUSY) {
			fb->current_fb = 0;
		}
		return errno;
	} else {
		fb->next_fb = fb_id;
	}

	return 0;
}

static int fb0_enable_crtc(struct drm_framebuffer *fb, uint32_t fb_id)
{
	int ret = drmModeSetCrtc(fb->fd, fb->crtc_id, fb_id, 0, 0,
			&fb->connector_id, 1, &fb->mode);
	if (ret) {
		ALOGE("Failed to enable CRTC: %d", ret);
	} else {
		fb->current_fb = fb_id;
	}

	return ret;
}

static int fb0_disable_crtc(struct drm_framebuffer *fb)
{
	int ret;

	/* Finish current page flip */
	fb0_await_page_flip(fb);

	ret = drmModeSetCrtc(fb->fd, fb->crtc_id, 0, 0, 0, NULL, 0, NULL);
	if (ret) {
		ALOGE("Failed to disable CRTC: %d", ret);
	} else {
		fb->current_fb = 0;
	}

	return ret;
}

static int fb0_post(struct framebuffer_device_t *fbdev, buffer_handle_t buffer)
{
	struct drm_framebuffer *fb = (struct drm_framebuffer *) fbdev;
	struct gralloc_handle_t *handle = gralloc_handle(buffer);
	uint32_t fb_id;
	if (!handle) {
		return -EINVAL;
	}

	fb_id = (uint32_t) handle->data;
	if (!fb_id) {
		return -EINVAL;
	}

	if (fb->current_fb == fb_id) {
		/* Already current */
		return 0;
	}

	if (fb->current_fb) {
		return fb0_page_flip(fb, fb_id);
	} else {
		return fb0_enable_crtc(fb, fb_id);
	}
}

static int fb0_enable_screen(struct framebuffer_device_t *fbdev, int enable)
{
	struct drm_framebuffer *fb = (struct drm_framebuffer *) fbdev;
	ALOGI("Updating screen state: %d", enable);

	/* Only need to disable screen here, will be re-enabled with next post */
	if (!enable && fb->current_fb) {
		return fb0_disable_crtc(fb);
	} else {
		return 0;
	}
}

static int fb0_composition_complete(__unused struct framebuffer_device_t *dev)
{
	return 0;
}

static int fb0_set_swap_interval(
	__unused struct framebuffer_device_t *window, int interval)
{
	if (interval != SWAP_INTERVAL) {
		return -EINVAL;
	}
	return 0;
}

static int fb0_close(struct hw_device_t *dev)
{
	struct drm_framebuffer *fb = (struct drm_framebuffer *) dev;
	*fb->fb_out = NULL;

	free(dev);
	return 0;
}

int drm_framebuffer_open(int fd, struct drm_framebuffer **fb_out, struct hw_device_t **dev)
{
	struct drm_framebuffer *fb;
	int ret;

	fb = calloc(1, sizeof(*fb));
	if (!fb) {
		return -ENOMEM;
	}

	fb->fd = fd;
	ret = fb0_init(fb);
	if (ret) {
		free(fb);
		return ret;
	}

	fb->device.common.tag = HARDWARE_DEVICE_TAG;
	fb->device.common.version = 0;
	fb->device.common.close = fb0_close;

	fb->device.setSwapInterval = fb0_set_swap_interval;
	fb->device.post = fb0_post;
	fb->device.compositionComplete = fb0_composition_complete;
	fb->device.enableScreen = fb0_enable_screen;
	fb->fb_out = fb_out;

	*fb_out = fb;
	*dev = &fb->device.common;
	return 0;
}

static uint32_t convert_android_to_drm_fb_format(uint32_t format)
{
	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
		/* Avoid using alpha bits for the framebuffer.
		 * They are not supported on older Intel GPUs for primary planes. */
	case HAL_PIXEL_FORMAT_RGBX_8888:
		return DRM_FORMAT_XBGR8888;
	case HAL_PIXEL_FORMAT_RGB_888:
		return DRM_FORMAT_BGR888;
	case HAL_PIXEL_FORMAT_RGB_565:
		return DRM_FORMAT_BGR565;
	case HAL_PIXEL_FORMAT_BGRA_8888:
		return DRM_FORMAT_ARGB8888;
	default:
		ALOGE("Unsupported framebuffer format: %u", format);
		return 0;
	}
}

static int fb0_add_fb(struct drm_framebuffer *fb, struct gralloc_handle_t *handle, uint32_t handle_id)
{
	uint32_t pitches[4] = { handle->stride, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint32_t handles[4] = { handle_id, 0, 0, 0 };

	return drmModeAddFB2(fb->fd, handle->width, handle->height,
		convert_android_to_drm_fb_format(handle->format),
		handles, pitches, offsets, (uint32_t*) &handle->data, 0);
}

void drm_framebuffer_import(struct drm_framebuffer *fb, buffer_handle_t buffer)
{
	struct gralloc_handle_t *handle = gralloc_handle(buffer);
	uint32_t handle_id;

	/* Ignore buffers that are not intended for usage with the framebuffer */
	if (!(handle->usage & GRALLOC_USAGE_HW_FB)) {
		return;
	}

	/* Lookup the handle for the prime fd.
	 * (The buffer should have already been imported by the gralloc HAL) */
	if (drmPrimeFDToHandle(fb->fd, handle->prime_fd, &handle_id)) {
		ALOGE("Failed to get handle from prime fd: %d", errno);
		return;
	}

	/* Add a framebuffer to the handle */
	fb0_add_fb(fb, handle, handle_id);
}
