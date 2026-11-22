/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL


#include "common/config-manager.h"
#include "common/str.h"
#include "common/textconsole.h"	// for warning() & error()
#include "common/translation.h"
#include "engines/engine.h"
#include "graphics/blit.h"
#include "gui/ThemeEngine.h"
#include "esp-graphics.h"
#include "tablet_lcd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "common/memstream.h"
#include "image/image_decoder.h"
#include "image/png.h"

#define TAG "EspGraphics"

//The panel is 720x1280; everything here is landscape, tablet_lcd rotates it.
#define SCREEN_WIDTH    TABLET_W
#define SCREEN_HEIGHT   TABLET_H

extern const uint8_t loading_png_start[] asm("_binary_loading_png_start");
extern const uint8_t loading_png_end[] asm("_binary_loading_png_end");

static const Graphics::PixelFormat kRgb565(2, 5, 6, 5, 0, 11, 5, 0, 0);

bool EspGraphicsManager::hasFeature(OSystem::Feature f) const {
	if (f==OSystem::kFeatureTouchscreen) return true;
	if (f==OSystem::kFeatureNoQuit) return true;
	if (f==OSystem::kFeatureStretchMode) return true;
	if (f==OSystem::kFeatureVirtualKeyboard) return true;
	return false;
}

void EspGraphicsManager::setFeatureState(OSystem::Feature f, bool enable) {

}

bool EspGraphicsManager::getFeatureState(OSystem::Feature f) const {
	return false;
}


void EspGraphicsManager::gfxTaskStub(void *param) {
	EspGraphicsManager *obj=(EspGraphicsManager*)param;
	obj->gfxTask();
}

//Returns the picture of surface fbno as packed RGB565. CLUT8 and other formats are converted into
//buf (reallocated if the size changed; dirtyOnly converts only the dirty rectangle), RGB565
//surfaces are returned as they are.
const uint16_t *EspGraphicsManager::toRgb565(int fbno, uint16_t *&buf, int &bufSize, bool dirtyOnly) {
	Graphics::Surface &surf=_surf[fbno];
	if (surf.format==kRgb565 && surf.pitch==surf.w*2) return (const uint16_t*)surf.getPixels();
	int need=surf.w*surf.h;
	Common::Rect r=_dirty[fbno];
	if (need!=bufSize || !buf) {
		free(buf);
		buf=(uint16_t*)heap_caps_calloc(need, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
		bufSize=buf?need:0;
		dirtyOnly=false;
		if (!buf) return nullptr;
	}
	if (!dirtyOnly) r=Common::Rect(0, 0, surf.w, surf.h);
	r.clip(Common::Rect(0, 0, surf.w, surf.h));
	if (surf.format.bytesPerPixel==1) {
		uint16_t pal16[256];
		for (int i=0; i<256; i++) {
			int cr=_pal[fbno][i*3+0];
			int cg=_pal[fbno][i*3+1];
			int cb=_pal[fbno][i*3+2];
			pal16[i]=((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3);
		}
		for (int y=r.top; y<r.bottom; y++) {
			const uint8_t *src=(const uint8_t*)surf.getBasePtr(r.left, y);
			uint16_t *dst=&buf[surf.w*y+r.left];
			for (int x=r.left; x<r.right; x++) *dst++=pal16[*src++];
		}
	} else {
		//Generic (slow) path for the other RGB formats
		for (int y=r.top; y<r.bottom; y++) {
			uint16_t *dst=&buf[surf.w*y+r.left];
			for (int x=r.left; x<r.right; x++) {
				uint8_t cr, cg, cb;
				surf.format.colorToRGB(surf.getPixel(x, y), cr, cg, cb);
				*dst++=((cr>>3)<<11)|((cg>>2)<<5)|(cb>>3);
			}
		}
	}
	return buf;
}

void EspGraphicsManager::gfxTask() {
	uint16_t *rgbfb=NULL;
	int rgbfb_size=0;

	while(1) {
		int fbno=0;
		if (xQueueReceive(_fb_num_q, (void*)(&fbno), portMAX_DELAY)) {
			if (fbno==-1) {
				tablet_lcd_present((const uint16_t*)_overlay.getPixels(), SCREEN_WIDTH, SCREEN_HEIGHT);
			} else if (_surf[fbno].w>0 && _surf[fbno].h>0) {
				const uint16_t *pic=toRgb565(fbno, rgbfb, rgbfb_size, true);
				//Dropped (ESP_ERR_INVALID_STATE) while the on-board menu is shown
				if (pic) tablet_lcd_present(pic, _surf[fbno].w, _surf[fbno].h);
			}
			xQueueSend(_fb_ret_q, &fbno, portMAX_DELAY);
		}
	}
}



void EspGraphicsManager::init() {
	//The overlay goes straight to the PPA; keep it cache line aligned for tablet_lcd_scale_to_screen
	void *ovl=heap_caps_aligned_calloc(128, SCREEN_WIDTH*SCREEN_HEIGHT, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	assert(ovl);
	_overlay.init(SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH*sizeof(uint16_t), ovl, getOverlayFormat());

	//Show 'Loading...' image
	Image::PNGDecoder d;
	Common::MemoryReadStream str=Common::MemoryReadStream(loading_png_start, loading_png_end-loading_png_start);
	if (d.loadStream(str)) {
		const Graphics::Surface *s=d.getSurface();
		uint16_t *ob=(uint16_t*)ovl;
		for (int y=0; y<s->h && y<SCREEN_HEIGHT; y++) {
			uint16_t *q=&ob[SCREEN_WIDTH*((SCREEN_HEIGHT-s->h)/2+y)+(SCREEN_WIDTH-s->w)/2];
			for (int x=0; x<s->w && x<SCREEN_WIDTH; x++) {
				uint8_t r, g, b;
				s->format.colorToRGB(s->getPixel(x, y), r, g, b);
				*q++=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
			}
		}
	}
	tablet_lcd_present((const uint16_t*)ovl, SCREEN_WIDTH, SCREEN_HEIGHT);

	_cur_fb=0;
	_fb_num_q=xQueueCreate(1, sizeof(int));
	_fb_ret_q=xQueueCreate(1, sizeof(int));

	int fbno=1;
	xQueueSend(_fb_ret_q, &fbno, portMAX_DELAY);
	xTaskCreatePinnedToCore(gfxTaskStub, "gfx", 6144, (void*)this, 6, NULL, 1);
}


void EspGraphicsManager::initSize(uint width, uint height, const Graphics::PixelFormat *format) {
	ESP_LOGI(TAG, "EspGraphicsManager::initSize %d %d", width, height);

	int fbno;
	//Wait until current frame is processed before we change things
	xQueueReceive(_fb_ret_q, (void*)(&fbno), portMAX_DELAY);
	xQueueSend(_fb_ret_q, &fbno, portMAX_DELAY);
	//Gfx task should be idle now.

	_width = width;
	_height = height;
	_format = format ? *format : Graphics::PixelFormat::createFormatCLUT8();
	for (int i=0; i<2; i++) {
		_surf[i].free();
		_surf[i].create(width, height, _format);
		_dirty[i]=Common::Rect(0, 0, width, height);
	}

}

Graphics::Surface *EspGraphicsManager::lockScreen() {
	return &_surf[_cur_fb];
}

void EspGraphicsManager::unlockScreen() {
	_dirty[_cur_fb]=Common::Rect(0,0,_surf[_cur_fb].w, _surf[_cur_fb].h);
}

void EspGraphicsManager::updateScreen() {
	//Nothing changed: skip the frame (saves a PPA pass and PSRAM bandwidth the LCD needs)
	if (_overlayVisible) {
		if (!_overlayDirty && _lastShownOverlay) return;
	} else {
		if (_dirty[_cur_fb].isEmpty() && !_lastShownOverlay) return;
	}

	//limit to 30fps
	if ((esp_timer_get_time()-_last_time_updated)<(1000000/30)) return;
	_last_time_updated=esp_timer_get_time();
	if (_overlayVisible) _overlayDirty=false;
	_lastShownOverlay=_overlayVisible;

	uint64_t t=esp_timer_get_time();

	int fbno;
	if (_overlayVisible) {
		fbno=-1;
	} else {
		fbno=_cur_fb;
		if (fbno==0) _cur_fb=1; else _cur_fb=0;
	}

	xQueueSend(_fb_num_q, &fbno, portMAX_DELAY);
	//wait until the other one is done
	int ret_fbno;
	xQueueReceive(_fb_ret_q, (void*)(&ret_fbno), portMAX_DELAY);
	if (fbno!=-1 && ret_fbno!=_cur_fb) {
		ESP_LOGW(TAG, "Huh, fbno != ret_fbno");
	}
	if (!_overlayVisible) {
		//use fb we're going to display as base of fb we're going to modify next
		memcpy(_surf[_cur_fb].getPixels(), _surf[fbno].getPixels(), _surf[fbno].pitch*_surf[fbno].h);
		memcpy(_pal[_cur_fb], _pal[fbno], 256*3);
		_dirty[_cur_fb]=Common::Rect(0, 0, 0, 0);
	}
	t=esp_timer_get_time()-t;
	if (t>33000) {
		//at most one warning every 5 s, the console must not slow things down further
		_slowCount++;
		int64_t now=esp_timer_get_time();
		if (now-_slowWarnTime>5000000) {
			ESP_LOGW(TAG, "updateScreen took %d us (%d slow frames in the last 5 s)", (int)t, _slowCount);
			_slowWarnTime=now;
			_slowCount=0;
		}
	}
}

void EspGraphicsManager::copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) {
	_surf[_cur_fb].copyRectToSurface(buf, pitch, x, y, w, h);
	if (_dirty[_cur_fb].top==_dirty[_cur_fb].bottom) {
		_dirty[_cur_fb].top=y;
		_dirty[_cur_fb].bottom=y+h;
		_dirty[_cur_fb].left=x;
		_dirty[_cur_fb].right=x+w;
	} else {
		if (_dirty[_cur_fb].top>y) _dirty[_cur_fb].top=y;
		if (_dirty[_cur_fb].bottom<y+h) _dirty[_cur_fb].bottom=y+h;
		if (_dirty[_cur_fb].left>x) _dirty[_cur_fb].left=x;
		if (_dirty[_cur_fb].right<x+w) _dirty[_cur_fb].right=x+w;
	}
}

void EspGraphicsManager::beginGFXTransaction() {
}

OSystem::TransactionError EspGraphicsManager::endGFXTransaction() {
	return OSystem::kTransactionSuccess;
}

void EspGraphicsManager::setPalette(const byte *colors, uint start, uint num) {
	int p=start*3;
	for (int i=0; i<num*3; i++) {
		if (p < 3*256) {
			_pal[_cur_fb][p++]=colors[i];
		}
	}
	//we need to recalculate the 16bit rgb surface.
	_dirty[_cur_fb]=Common::Rect(0,0,_surf[_cur_fb].w, _surf[_cur_fb].h);
}

void EspGraphicsManager::grabPalette(byte *colors, uint start, uint num) const {
	for (int i=0; i<num*3; i++) {
		colors[i]=_pal[_cur_fb][start*3+i];
	}
}

void EspGraphicsManager::copyRectToOverlay(const void *buf, int pitch, int x, int y, int w, int h) {
	_overlay.copyRectToSurface(buf, pitch, x, y, w, h);
	_overlayDirty=true;
}

void EspGraphicsManager::grabOverlay(Graphics::Surface &surface) const {
	surface.copyFrom(_overlay);
}

int16 EspGraphicsManager::getOverlayHeight() const {
	return SCREEN_HEIGHT;
}

int16 EspGraphicsManager::getOverlayWidth() const {
	return SCREEN_WIDTH;
}

void EspGraphicsManager::clearOverlay() {
	_overlayDirty=true;
	//The overlay starts out as the current game picture, scaled like on screen
	if (_surf[_cur_fb].w>0 && _surf[_cur_fb].h>0) {
		const uint16_t *pic=toRgb565(_cur_fb, _clearBuf, _clearBufSize, false);
		if (pic && tablet_lcd_scale_to_screen(pic, _surf[_cur_fb].w, _surf[_cur_fb].h,
				(uint16_t*)_overlay.getPixels())==ESP_OK) return;
	}
	memset(_overlay.getPixels(), 0, SCREEN_WIDTH*SCREEN_HEIGHT*sizeof(uint16_t));
}

int EspGraphicsManager::getTouch(Common::Point &pos) {
	uint16_t x[3], y[3];
	int num=tablet_touch_get(x, y, 3);
	if (num!=0) {
		if (_overlayVisible || _width==0 || _height==0) {
			pos.x=x[0];
			pos.y=y[0];
		} else {
			int ox, oy, ow, oh;
			tablet_lcd_viewport(_width, _height, &ox, &oy, &ow, &oh);
			int gx=((int)x[0]-ox)*(int)_width/ow;
			int gy=((int)y[0]-oy)*(int)_height/oh;
			pos.x=CLIP(gx, 0, (int)_width-1);
			pos.y=CLIP(gy, 0, (int)_height-1);
		}
	}
	return num;
}
