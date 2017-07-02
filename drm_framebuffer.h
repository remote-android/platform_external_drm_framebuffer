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

#ifndef _DRM_FRAMEBUFFER_H_
#define _DRM_FRAMEBUFFER_H_

#include <sys/cdefs.h>
#include <hardware/fb.h>

__BEGIN_DECLS

struct drm_framebuffer;

int drm_framebuffer_open(int fd, struct drm_framebuffer **fb, struct hw_device_t **dev);
void drm_framebuffer_import(struct drm_framebuffer *fb, buffer_handle_t handle);

__END_DECLS

#endif /* _DRM_FRAMEBUFFER_H_ */
