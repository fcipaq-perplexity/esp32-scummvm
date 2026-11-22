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

#include "common/scummsys.h"

#include "esp-mixer.h"
#include "common/system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tablet_audio.h"

#define TAG "EspMixerManager"

void EspMixerManager::audioTaskStub(void *param) {
	EspMixerManager *obj=(EspMixerManager*)param;
	obj->audioTask();
}

void EspMixerManager::audioTask() {
	byte *buf=(byte*)heap_caps_calloc(_bufSize, 1, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
	int64_t frame_time_us=((int64_t)(_bufSize/4)*1000000ULL)/(int64_t)_freq;
	while(1) {
		int skip=0;
		if (!_audioSuspended) {
			int64_t t=esp_timer_get_time();
			_mixer->mixCallback(buf, _bufSize);
			t=esp_timer_get_time()-t;
			if (t > frame_time_us) {
				ESP_LOGW(TAG, "Audio frame calc overrun: took %d us to calc %d us worth of audio", (int)t, (int)frame_time_us);
				skip=1;
			}
		} else {
			memset(buf, 0, _bufSize);
		}
		if (tablet_audio_available()) {
			//Blocks until the I2S DMA takes the samples; this paces the loop
			if (!skip) tablet_audio_write(buf, _bufSize);
		} else {
			//No audio output on this board: keep the mixer running in real time
			vTaskDelay(pdMS_TO_TICKS(frame_time_us/1000));
		}
	}
}


EspMixerManager::EspMixerManager(int freq, int bufSize)
	:
	_freq(freq),
	_bufSize(bufSize) {
}

EspMixerManager::~EspMixerManager() {
}

void EspMixerManager::init() {
	_mixer = new Audio::MixerImpl(_freq);
	assert(_mixer);

	//Volume and speaker/headphones are set from the on-board menu settings (tablet_settings)
	esp_err_t ret=tablet_audio_init(_freq);
	if (ret!=ESP_OK) ESP_LOGW(TAG, "No audio output (%s)", esp_err_to_name(ret));

	//Audio actually is quite important; let any free core take it.
	xTaskCreatePinnedToCore(audioTaskStub, "audio", 1024*16, (void*)this, 7, NULL, tskNO_AFFINITY);

	_mixer->setReady(true);
}

void EspMixerManager::suspendAudio() {
	_audioSuspended = true;
}

int EspMixerManager::resumeAudio() {
	if (!_audioSuspended)
		return -2;

	_audioSuspended = false;
	return 0;
}

