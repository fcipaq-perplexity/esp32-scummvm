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

#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include "esp_log.h"
#include "posixesp-fs-factory.h"

#define TAG "main"

#include "../../../../../config.h"

#include "common/scummsys.h"

#include "backends/modular-backend.h"
#include "esp-mutex.h"
#include "base/main.h"
#include "backends/saves/default/default-saves.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "common/config-manager.h"
#include "esp-graphics.h"
#include "esp-mixer.h"
#include "gui/debugger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "engines/engine.h"
#include "mmc.h"
#include "tablet_hw.h"
#include "tablet_lcd.h"
#include "tablet_settings.h"
#include "tablet_ui.h"

class OSystem_esp32 : public ModularMixerBackend, public ModularGraphicsBackend, Common::EventSource {
public:
	OSystem_esp32(bool silenceLogs);
	virtual ~OSystem_esp32();

	virtual void initBackend();

	virtual bool pollEvent(Common::Event &event);

	virtual Common::MutexInternal *createMutex();
	virtual uint32 getMillis(bool skipRecord = false);
	virtual void delayMillis(uint msecs);
	virtual void getTimeAndDate(TimeDate &td, bool skipRecord = false) const;

	virtual void quit();

	virtual void logMessage(LogMessageType::Type type, const char *message);

	virtual void addSysArchivesToSearchSet(Common::SearchSet &s, int priority);

protected:
	virtual Common::Path getDefaultConfigFileName() override;
	virtual Common::Path getDefaultLogFileName() override;

private:
	timeval _startTime;
	bool _silenceLogs;
	int _was_touched;
	bool _mousedown_queued;
	Common::Point _last_mouse_pos;
	int64_t _last_ts_time_us;
	bool _ui_was_active;
	PauseToken _ui_pause;
};

OSystem_esp32::OSystem_esp32(bool silenceLogs) :
	_silenceLogs(silenceLogs) {
	_fsFactory = new POSIXESPFilesystemFactory();
	_last_ts_time_us = 0;
	_was_touched = 0;
	_mousedown_queued = false;
	_ui_was_active = false;
}

OSystem_esp32::~OSystem_esp32() {
}


void OSystem_esp32::initBackend() {
	gettimeofday(&_startTime, 0);

	_timerManager = new DefaultTimerManager();
	_eventManager = new DefaultEventManager(this);
	_savefileManager = new DefaultSaveFileManager();
	EspGraphicsManager *gfx = new EspGraphicsManager();
	_graphicsManager = gfx;
	gfx->init();
	_mixerManager = new EspMixerManager(44100, 2048);
	_mixerManager->init();

	ConfMan.registerDefault("extrapath", Common::Path("/sdcard/scummvm/extras/"));
	ConfMan.registerDefault("iconspath", Common::Path("/sdcard/scummvm/icons/"));
	ConfMan.registerDefault("pluginspath", Common::Path("/sdcard/scummvm/plugins/"));
	ConfMan.registerDefault("savepath", Common::Path("/sdcard/scummvm/saves/"));
	ConfMan.registerDefault("themepath", Common::Path("/sdcard/scummvm/themes/"));

	BaseBackend::initBackend();
}

bool OSystem_esp32::pollEvent(Common::Event &event) {
	((DefaultTimerManager *)getTimerManager())->checkTimers();

	event.type=Common::EVENT_INVALID;

	//The on-board menu (menu button) pauses the engine while it is open
	bool ui=tablet_ui_active();
	if (ui!=_ui_was_active) {
		_ui_was_active=ui;
		if (ui) {
			if (g_engine) _ui_pause=g_engine->pauseEngine();
		} else {
			_ui_pause.clear();
		}
		_mousedown_queued=false;
		if (ui && _was_touched==1) {
			//finger was down when the menu opened: release it
			_was_touched=0;
			event.type=Common::EVENT_LBUTTONUP;
			event.mouse=_last_mouse_pos;
			return true;
		}
		_was_touched=0;
	}
	if (ui) return false;

	if (_mousedown_queued) {
		event.type = Common::EVENT_LBUTTONDOWN;
		_mousedown_queued=false;
		return true;
	}

	if ((esp_timer_get_time()-_last_ts_time_us)>(1000000/60)) {
		_last_ts_time_us=esp_timer_get_time();
		Common::Point pos;
		EspGraphicsManager *gfx=(EspGraphicsManager *)_graphicsManager;
		int touched=gfx->getTouch(pos);
		if (touched>2) touched=2;
		if (touched==1) {
			if (_was_touched==0) _mousedown_queued=true;
//			ESP_LOGI(TAG, "ts %d,%d", pos.x, pos.y);
			event.type = Common::EVENT_MOUSEMOVE;
			event.mouse = pos;
			_last_mouse_pos = pos;
		} else if (touched==0 && _was_touched==1) {
//			ESP_LOGI(TAG, "ts up");
			event.type = Common::EVENT_LBUTTONUP;
			event.mouse = _last_mouse_pos;
		} else if (touched==2 && _was_touched!=2) {
			event.type = Common::EVENT_VIRTUAL_KEYBOARD;
		}
		_was_touched=touched;
		if (event.type!=Common::EVENT_INVALID) return true;
	}

	return false;
}

Common::MutexInternal *OSystem_esp32::createMutex() {
	return createEspMutexInternal();
}

uint32 OSystem_esp32::getMillis(bool skipRecord) {
	uint64_t t_us=esp_timer_get_time();
	return (uint32)(t_us/1000ULL);
}

void OSystem_esp32::delayMillis(uint msecs) {
//	ESP_LOGI(TAG, "delayMillis %d", msecs);
	vTaskDelay(pdMS_TO_TICKS(msecs));
}

void OSystem_esp32::getTimeAndDate(TimeDate &td, bool skipRecord) const {
	time_t curTime = time(0);
	struct tm t = *localtime(&curTime);
	td.tm_sec = t.tm_sec;
	td.tm_min = t.tm_min;
	td.tm_hour = t.tm_hour;
	td.tm_mday = t.tm_mday;
	td.tm_mon = t.tm_mon;
	td.tm_year = t.tm_year;
	td.tm_wday = t.tm_wday;
}

void OSystem_esp32::quit() {
	exit(0);
}

void OSystem_esp32::logMessage(LogMessageType::Type type, const char *message) {
	if (_silenceLogs)
		return;

	FILE *output = 0;

	if (type == LogMessageType::kInfo || type == LogMessageType::kDebug)
		output = stdout;
	else
		output = stderr;

	fputs(message, output);
	fflush(output);
}

void OSystem_esp32::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
	s.add("engine-data", new Common::FSDirectory("/sdcard/scummvm/", 4), priority);
	s.add("gui/themes", new Common::FSDirectory("/sdcard/scummvm/", 4), priority);
}

Common::Path OSystem_esp32::getDefaultConfigFileName() {
	return "/sdcard/scummvm/scummvm.ini";
}

Common::Path OSystem_esp32::getDefaultLogFileName() {
	return "/sdcard/scummvm/scummvm.log";
}

OSystem *OSystem_esp32_create(bool silenceLogs) {
	return new OSystem_esp32(silenceLogs);
}

extern "C" {

void main_task(void *param) {
	// Invoke the actual ScummVM main entry point:
//	const char *argv[]={"scummvm", "-d", "11"};
	const char *argv[]={"scummvm"};
	int res = scummvm_main(sizeof(argv)/sizeof(argv[0]), argv);
	ESP_LOGW(TAG, "Scummvm_main done");
	g_system->destroy();
}

int app_main() {
	//Keep the board powered after the power button is released
	tablet_power_hold();

	esp_err_t ret=tablet_lcd_init();
	if (ret!=ESP_OK) {
		ESP_LOGE(TAG, "Display init failed (%s), halted", esp_err_to_name(ret));
		return 0;
	}

	bool sd_ok=sdcard_mount_blkcache("/sdcard", 15);
	if (sd_ok) tablet_settings_load();
	tablet_settings_apply();		//backlight on

	if (tablet_battery_init()==ESP_OK) ESP_LOGI(TAG, "Battery measurement running");
	if (tablet_ui_init()!=ESP_OK) ESP_LOGW(TAG, "On-board menu not available");
	ret=tablet_touch_init();
	if (ret!=ESP_OK) ESP_LOGW(TAG, "No touch input (%s)", esp_err_to_name(ret));
	tablet_button_init();

	if (!sd_ok) {
		ESP_LOGE(TAG, "ScummVM needs an SD card with the scummvm folder, halted");
		return 0;
	}

	g_system = OSystem_esp32_create(false);
	assert(g_system);

	int stack_depth=512*1024;
	StaticTask_t *taskbuf=(StaticTask_t*)calloc(1, sizeof(StaticTask_t));
	uint8_t *stackbuf=(uint8_t*)calloc(stack_depth, 1);
	xTaskCreateStaticPinnedToCore(main_task, "main", stack_depth, NULL, 2, (StackType_t*)stackbuf, taskbuf, 0);
	return 0;
}

} //extern c