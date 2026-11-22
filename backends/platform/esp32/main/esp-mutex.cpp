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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"


#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"

#include "esp-mutex.h"

class EspMutexInternal final : public Common::MutexInternal {
public:
	EspMutexInternal();
	~EspMutexInternal() override;

	bool lock() override;
	bool unlock() override;

private:
	StaticSemaphore_t *_buf;
	SemaphoreHandle_t _mutex;
};


//The mutex control block goes to PSRAM: xSemaphoreCreateRecursiveMutex() takes it from internal
//RAM, which is short on the tablet boards (LCD bounce buffers), and returns NULL when it runs out.
EspMutexInternal::EspMutexInternal() {
	_buf=(StaticSemaphore_t*)heap_caps_calloc(1, sizeof(StaticSemaphore_t), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
	if (!_buf) _buf=(StaticSemaphore_t*)heap_caps_calloc(1, sizeof(StaticSemaphore_t), MALLOC_CAP_8BIT);
	_mutex=_buf ? xSemaphoreCreateRecursiveMutexStatic(_buf) : NULL;
	if (!_mutex) {
		ESP_LOGE("EspMutex", "Cannot create mutex: out of memory");
		abort();
	}
}

EspMutexInternal::~EspMutexInternal() {
	vSemaphoreDelete(_mutex);
	heap_caps_free(_buf);
}

bool EspMutexInternal::lock() {
	xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
	return true;
}

bool EspMutexInternal::unlock() {
	xSemaphoreGiveRecursive(_mutex);
	return true;
}

Common::MutexInternal *createEspMutexInternal() {
	return new EspMutexInternal();
}

