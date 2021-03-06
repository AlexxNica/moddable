/*
 * Copyright (c) 2018  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 * 
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 * 
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 * 
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "xsmc.h"
#include "mc.xs.h"			// for xsID_ values

#ifndef MODDEF_AUDIOOUT_STREAMS
	#define MODDEF_AUDIOOUT_STREAMS (4)
#endif
#ifndef MODDEF_AUDIOOUT_BITSPERSAMPLE
	#define MODDEF_AUDIOOUT_BITSPERSAMPLE (16)
#endif
#ifndef MODDEF_AUDIOOUT_QUEUELENGTH
	#define MODDEF_AUDIOOUT_QUEUELENGTH (8)
#endif
#if ESP32
	#ifndef MODDEF_AUDIOOUT_I2S_NUM
		#define MODDEF_AUDIOOUT_I2S_NUM (0)
	#endif
	#ifndef MODDEF_AUDIOOUT_I2S_BCK_PIN
		#define MODDEF_AUDIOOUT_I2S_BCK_PIN (26)
	#endif
	#ifndef MODDEF_AUDIOOUT_I2S_LR_PIN
		#define MODDEF_AUDIOOUT_I2S_LR_PIN (25)
	#endif
	#ifndef MODDEF_AUDIOOUT_I2S_DATAOUT_PIN
		#define MODDEF_AUDIOOUT_I2S_DATAOUT_PIN (22)
	#endif
#endif

#if MODDEF_AUDIOOUT_STREAMS > 4
	#error "can't mix over 4 streams"
#endif
#if (8 != MODDEF_AUDIOOUT_BITSPERSAMPLE) && (16 != MODDEF_AUDIOOUT_BITSPERSAMPLE)
	#error "bitsPerSample must be 8 or 16"
#endif

#if MODDEF_AUDIOOUT_BITSPERSAMPLE == 8
	#define OUTPUTSAMPLETYPE uint8_t
#elif MODDEF_AUDIOOUT_BITSPERSAMPLE == 16
	#define OUTPUTSAMPLETYPE int16_t
#endif

#if defined(__APPLE__)
	#import <AudioUnit/AudioUnit.h>
	#import <AudioToolbox/AudioToolbox.h>
	#define kAudioQueueBufferCount (2)
#elif ESP32
	#include "xsesp.h"
	#include "freertos/FreeRTOS.h"
	#include "freertos/task.h"
	#include "freertos/semphr.h"
	#include "driver/i2s.h"

	enum {
		kStateIdle = 0,
		kStatePlaying = 1,
		kStateTerminated = 2
	};
#endif

typedef struct {
	void	*samples;
	int		sampleCount;		// 0 means this is a callback with value of (uintptr_t)samples
	int		position;
	int		repeat;				// alwauys 1 for callback, negative for infinite
} modAudioQueueElementRecord, *modAudioQueueElement;

typedef struct {
	int		elementCount;
	modAudioQueueElementRecord	element[MODDEF_AUDIOOUT_QUEUELENGTH];
} modAudioOutStreamRecord, *modAudioOutStream;

typedef struct {
	xsMachine				*the;
	xsSlot					obj;

	uint16_t				sampleRate;
	uint8_t					numChannels;
	uint8_t					bitsPerSample;
	uint8_t					bytesPerFrame;

	int						activeStreamCount;
	modAudioOutStream		activeStream[MODDEF_AUDIOOUT_STREAMS];

	int						streamCount;

#if defined(__APPLE__)
	AudioQueueRef			audioQueue;
	AudioQueueBufferRef		buffer[kAudioQueueBufferCount];
	pthread_mutex_t			mutex;
	CFRunLoopTimerRef		callbackTimer;
	CFRunLoopRef			runLoop;
#elif ESP32
	SemaphoreHandle_t 		mutex;
	TaskHandle_t			task;

	uint8_t					state;		// 0 idle, 1 playing, 2 terminated

	uint32_t				buffer[128];
#endif

	int						pendingCallbackCount;
	xsIntegerValue			pendingCallbacks[MODDEF_AUDIOOUT_QUEUELENGTH];

	modAudioOutStreamRecord	stream[1];		// must be last
} modAudioOutRecord, *modAudioOut;

static void updateActiveStreams(modAudioOut out);
#if defined(__APPLE__)
static void audioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);
#elif ESP32
static void audioOutLoop(void *pvParameter);
#endif
static void audioMix(modAudioOut out, int samplesToGenerate, OUTPUTSAMPLETYPE *output);
static void endOfElement(modAudioOut out, modAudioOutStream stream);

void xs_audioout_destructor(void *data)
{
	modAudioOut out = data;

	if (!out)
		return;

#if defined(__APPLE__)
	if (out->callbackTimer)
		CFRunLoopTimerInvalidate(out->callbackTimer);

	if (out->audioQueue) {
		int i;

		AudioQueueStop(out->audioQueue, true);

		for (i = 0; i < kAudioQueueBufferCount; i++)
			if (out->buffer[i])
				AudioQueueFreeBuffer(out->audioQueue, out->buffer[i]);

		AudioQueueDispose(out->audioQueue, true);
	}

	pthread_mutex_destroy(&out->mutex);
#elif ESP32
	out->state = kStateTerminated;
	xTaskNotify(out->task, kStateTerminated, eSetValueWithOverwrite);

	vSemaphoreDelete(out->mutex);
#endif

	c_free(out);
}

void xs_audioout(xsMachine *the)
{
	int i;
	modAudioOut out;
	uint16_t sampleRate;
	uint8_t numChannels;
	uint8_t bitsPerSample;
	int streamCount;

	xsmcVars(1);

	xsmcGet(xsVar(0), xsArg(0), xsID_sampleRate);
	sampleRate = xsmcToInteger(xsVar(0));
	if ((sampleRate < 8000) || (sampleRate > 48000))
		xsRangeError("invalid sample rate");

	xsmcGet(xsVar(0), xsArg(0), xsID_numChannels);
	numChannels = xsmcToInteger(xsVar(0));
	if ((1 != numChannels) && (2 != numChannels))
		xsRangeError("bad numChannels");

	xsmcGet(xsVar(0), xsArg(0), xsID_bitsPerSample);
	bitsPerSample = xsmcToInteger(xsVar(0));
	if (MODDEF_AUDIOOUT_BITSPERSAMPLE != bitsPerSample)
		xsRangeError("bad bitsPerSample");

	if (!xsmcHas(xsArg(0), xsID_streams))
		streamCount = 1;
	else {
		xsmcGet(xsVar(0), xsArg(0), xsID_streams);
		streamCount = xsmcToInteger(xsVar(0));
		if ((streamCount < 1) || (streamCount > MODDEF_AUDIOOUT_STREAMS))
			xsRangeError("bad streamCount");
	}

	out = (modAudioOut)c_calloc(sizeof(modAudioOutRecord) + (sizeof(modAudioOutStreamRecord) * (streamCount - 1)), 1);
	if (!out)
		xsUnknownError("no memory");
	xsmcSetHostData(xsThis, out);

	out->the = the;
	out->obj = xsThis;

	out->streamCount = streamCount;
	out->sampleRate = sampleRate;
	out->numChannels = numChannels;
	out->bitsPerSample = bitsPerSample;
	out->bytesPerFrame = (bitsPerSample * numChannels) >> 3;
	out->streamCount = streamCount;

#if defined(__APPLE__)
	OSStatus err;
	AudioStreamBasicDescription desc = {0};

	desc.mBitsPerChannel = bitsPerSample;
	desc.mBytesPerFrame = out->bytesPerFrame;
	desc.mBytesPerPacket = desc.mBytesPerFrame;
	desc.mChannelsPerFrame = numChannels;
#if MODDEF_AUDIOOUT_BITSPERSAMPLE == 8
	desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
#elif MODDEF_AUDIOOUT_BITSPERSAMPLE == 16
	desc.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
#endif
	desc.mFormatID = kAudioFormatLinearPCM;
	desc.mFramesPerPacket = 1;
	desc.mSampleRate = sampleRate;

	pthread_mutex_init(&out->mutex, NULL);

	out->runLoop = CFRunLoopGetCurrent();

	err = AudioQueueNewOutput(&desc, audioQueueCallback, out, NULL, NULL, 0, &out->audioQueue);
	if (noErr != err)
		xsUnknownError("can't create output");

	// 2 buffers, 1/32 of a second each
	for (i = 0; i < kAudioQueueBufferCount; i++)
		AudioQueueAllocateBuffer(out->audioQueue, (((out->bitsPerSample * out->numChannels) >> 3) * out->sampleRate) >> 5, &out->buffer[i]);
#elif ESP32
	out->state = kStateIdle;
	out->mutex = xSemaphoreCreateMutex();

	xTaskCreate(audioOutLoop, "audioOut", 768, out, 7, &out->task);
#endif
}

void xs_audioout_close(xsMachine *the)
{
	xs_audioout_destructor(xsmcGetHostData(xsThis));
	xsmcSetHostData(xsThis, NULL);
}

void xs_audioout_start(xsMachine *the)
{
	modAudioOut out = xsmcGetHostData(xsThis);
	int i;

#if defined(__APPLE__)
	for (i = 0; i < kAudioQueueBufferCount; i++)
		audioQueueCallback(out, out->audioQueue, out->buffer[i]);

	AudioQueueStart(out->audioQueue, NULL);
#elif ESP32
	out->state = kStatePlaying;
	xTaskNotify(out->task, kStatePlaying, eSetValueWithOverwrite);
#endif
}

void xs_audioout_stop(xsMachine *the)
{
	modAudioOut out = xsmcGetHostData(xsThis);

#if defined(__APPLE__)
	AudioQueueStop(out->audioQueue, true);
#elif ESP32
	out->state = kStateIdle;
	xTaskNotify(out->task, kStateIdle, eSetValueWithOverwrite);
#endif
}

void xs_audioout_enqueue(xsMachine *the)
{
	modAudioOut out = xsmcGetHostData(xsThis);
	int stream, argc = xsmcArgc;
	int repeat = 1, sampleOffset = 0, samplesToUse = -1, bufferSamples;
	uint8_t *buffer;
	uint16_t sampleRate;
	uint8_t numChannels;
	uint8_t bitsPerSample;
	modAudioQueueElement element;

	xsmcVars(1);

	stream = xsmcToInteger(xsArg(0));
	if ((stream < 0) || (stream >= out->streamCount))
		xsRangeError("invalid stream");

	if (MODDEF_AUDIOOUT_QUEUELENGTH == out->stream[stream].elementCount)
		xsUnknownError("queue full");

	if (1 == argc) {
#if defined(__APPLE__)
		pthread_mutex_lock(&out->mutex);
#elif ESP32
		xSemaphoreTake(out->mutex, portMAX_DELAY);
#endif
		out->stream[stream].elementCount = 0;		// flush queue
#if defined(__APPLE__)
		pthread_mutex_unlock(&out->mutex);
#elif ESP32
		xSemaphoreGive(out->mutex);
#endif
		return;
	}

	if ((2 == argc) && ((xsNumberType == xsmcTypeOf(xsArg(1))) || (xsIntegerType == xsmcTypeOf(xsArg(1))))) {
		// callback id
#if defined(__APPLE__)
		pthread_mutex_lock(&out->mutex);
#elif ESP32
		xSemaphoreTake(out->mutex, portMAX_DELAY);
#endif
		element = &out->stream[stream].element[out->stream[stream].elementCount];
		element->samples = (void *)xsmcToInteger(xsArg(1));
		element->sampleCount = 0;
		element->position = 0;
		element->repeat = 1;
		goto done;
	}

	if (argc > 2) {
		if ((xsNumberType == xsmcTypeOf(xsArg(2))) && (C_INFINITY == xsmcToNumber(xsArg(2))))
			repeat = -1;
		else
			repeat = xsmcToInteger(xsArg(2));
		if (argc > 3) {
			sampleOffset = xsmcToInteger(xsArg(3));
			if (argc > 4)
				samplesToUse = xsmcToInteger(xsArg(4));
		}
	}

	buffer = xsmcGetHostData(xsArg(1));
	if (('m' != buffer[0]) || ('a' != buffer[1]) || (1 != buffer[2]))
		xsUnknownError("bad header");

	bitsPerSample = c_read8(buffer + 3);
	sampleRate = c_read16(buffer + 4);
	numChannels = c_read8(buffer + 6);
	bufferSamples = c_read32(buffer + 8);
	if ((bitsPerSample != out->bitsPerSample) || (sampleRate != out->sampleRate) || (numChannels != out->numChannels))
		xsUnknownError("format doesn't match output");

	buffer += 12;

	if (sampleOffset >= bufferSamples)
		xsUnknownError("invalid offset");

	if ((samplesToUse < 0) || ((sampleOffset + samplesToUse) > bufferSamples))
		samplesToUse = bufferSamples - sampleOffset;

#if defined(__APPLE__)
	pthread_mutex_lock(&out->mutex);
#elif ESP32
	xSemaphoreTake(out->mutex, portMAX_DELAY);
#endif

	element = &out->stream[stream].element[out->stream[stream].elementCount];
	element->samples = buffer + (sampleOffset * out->bytesPerFrame);
	element->sampleCount = samplesToUse;
	element->position = 0;
	element->repeat = repeat;

done:
	out->stream[stream].elementCount += 1;

	if (1 == out->stream[stream].elementCount)
		updateActiveStreams(out);

#if defined(__APPLE__)
	pthread_mutex_unlock(&out->mutex);
#elif ESP32
	xSemaphoreGive(out->mutex);
#endif

	xsResult = xsThis;
}

// note: updateActiveStreams relies on caller to lock mutex
void updateActiveStreams(modAudioOut out)
{
	int i;

	out->activeStreamCount = 0;
	for (i = 0; i < out->streamCount; i++) {
		if (0 == out->stream[i].elementCount)
			continue;
		out->activeStream[out->activeStreamCount++] = &out->stream[i];
	}
}

#if defined(__APPLE__)
void audioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef buffer)
{
	modAudioOut out = inUserData;
	int samplesToGenerate = buffer->mAudioDataBytesCapacity / out->bytesPerFrame;
	OUTPUTSAMPLETYPE *output = (OUTPUTSAMPLETYPE *)buffer->mAudioData;

	buffer->mAudioDataByteSize = samplesToGenerate * out->bytesPerFrame;

	pthread_mutex_lock(&out->mutex);

	audioMix(out, samplesToGenerate, output);

	pthread_mutex_unlock(&out->mutex);

	AudioQueueEnqueueBuffer(out->audioQueue, buffer, 0, NULL);
}

static void invokeCallbacks(CFRunLoopTimerRef timer, void *info)
{
	modAudioOut out = info;

	out->callbackTimer = NULL;

	xsBeginHost(out->the);
	xsmcVars(1);

	while (out->pendingCallbackCount) {
		int id;

		pthread_mutex_lock(&out->mutex);
		id = out->pendingCallbacks[0];

		out->pendingCallbackCount -= 1;
		if (out->pendingCallbackCount)
			c_memcpy(out->pendingCallbacks, out->pendingCallbacks + 1, out->pendingCallbackCount * sizeof(xsIntegerValue));
		pthread_mutex_unlock(&out->mutex);

		xsmcSetInteger(xsVar(0), id);
		xsCall1(out->obj, xsID_callback, xsVar(0));		//@@ unsafe to close inside callback
	}

	xsEndHost(out->the);
}

// note: queueCallback relies on caller to lock mutex
static void queueCallback(modAudioOut out, xsIntegerValue id)
{
	if (out->pendingCallbackCount < MODDEF_AUDIOOUT_QUEUELENGTH) {
		out->pendingCallbacks[out->pendingCallbackCount++] = id;

		if (1 == out->pendingCallbackCount) {
			CFRunLoopTimerContext context = {0};
			context.info = out;
			out->callbackTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0, 0, 0, invokeCallbacks, &context);
			CFRunLoopAddTimer(out->runLoop, out->callbackTimer, kCFRunLoopCommonModes);
		}
	}
	else
		printf("audio callback queue full\n");
}

#elif ESP32
void audioOutLoop(void *pvParameter)
{
	modAudioOut out = pvParameter;

	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,	// Only TX
		.sample_rate = out->sampleRate,
		.bits_per_sample = 16,
		.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,	// 2-channels
		.communication_format = I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
		.dma_buf_count = 2,
		.dma_buf_len = sizeof(out->buffer) / out->bytesPerFrame,		// dma_buf_len is in frames, not bytes
		.use_apll = 0,
		.intr_alloc_flags = 0
	};
	i2s_pin_config_t pin_config = {
		.bck_io_num = MODDEF_AUDIOOUT_I2S_BCK_PIN,
		.ws_io_num = MODDEF_AUDIOOUT_I2S_LR_PIN,
		.data_out_num = MODDEF_AUDIOOUT_I2S_DATAOUT_PIN,
		.data_in_num = -1	// unused
	};
	i2s_driver_install(MODDEF_AUDIOOUT_I2S_NUM, &i2s_config, 0, NULL);
	i2s_set_pin(MODDEF_AUDIOOUT_I2S_NUM, &pin_config);
	i2s_set_clk(MODDEF_AUDIOOUT_I2S_NUM, out->sampleRate, out->bitsPerSample, out->numChannels);

	while (kStateTerminated != out->state) {
		if (kStateIdle == out->state) {
			uint32_t newState;

			xTaskNotifyWait(0, 0, &newState, portMAX_DELAY);
			if (kStateTerminated == newState)
				break;

			if (kStateIdle == newState) {
				c_memset(out->buffer, 0, sizeof(out->buffer));
				// write silence into i2s buffers (why 4 writes instead of 2?)
				i2s_write_bytes(MODDEF_AUDIOOUT_I2S_NUM, (const char *)out->buffer, sizeof(out->buffer), portMAX_DELAY);
				i2s_write_bytes(MODDEF_AUDIOOUT_I2S_NUM, (const char *)out->buffer, sizeof(out->buffer), portMAX_DELAY);
				i2s_write_bytes(MODDEF_AUDIOOUT_I2S_NUM, (const char *)out->buffer, sizeof(out->buffer), portMAX_DELAY);
				i2s_write_bytes(MODDEF_AUDIOOUT_I2S_NUM, (const char *)out->buffer, sizeof(out->buffer), portMAX_DELAY);
			}
			continue;
		}

		xSemaphoreTake(out->mutex, portMAX_DELAY);
		audioMix(out, sizeof(out->buffer) / out->bytesPerFrame, (OUTPUTSAMPLETYPE *)out->buffer);
		xSemaphoreGive(out->mutex);

		i2s_write_bytes(MODDEF_AUDIOOUT_I2S_NUM, (const char *)out->buffer, sizeof(out->buffer), portMAX_DELAY);
	}

	// from here, "out" is invalid
	i2s_driver_uninstall(MODDEF_AUDIOOUT_I2S_NUM);

	vTaskDelete(NULL);	// "If it is necessary for a task to exit then have the task call vTaskDelete( NULL ) to ensure its exit is clean."
}

static void deliverCallbacks(void *the, void *refcon, uint8_t *message, uint16_t messageLength)
{
	modAudioOut out = refcon;

	xsBeginHost(out->the);
	xsmcVars(1);

	while (out->pendingCallbackCount) {
		int id;

		xSemaphoreTake(out->mutex, portMAX_DELAY);
		id = out->pendingCallbacks[0];

		out->pendingCallbackCount -= 1;
		if (out->pendingCallbackCount)
			c_memcpy(out->pendingCallbacks, out->pendingCallbacks + 1, out->pendingCallbackCount * sizeof(xsIntegerValue));
		xSemaphoreGive(out->mutex);

		xsmcSetInteger(xsVar(0), id);
		xsCall1(out->obj, xsID_callback, xsVar(0));		//@@ unsafe to close inside callback
	}

	xsEndHost(out->the);
}

static void queueCallback(modAudioOut out, xsIntegerValue id)
{
	if (out->pendingCallbackCount < MODDEF_AUDIOOUT_QUEUELENGTH) {
		out->pendingCallbacks[out->pendingCallbackCount++] = id;

		if (1 == out->pendingCallbackCount)
			modMessagePostToMachine(out->the, NULL, 0, deliverCallbacks, out);
	}
	else
		printf("audio callback queue full\n");
}

#endif

void audioMix(modAudioOut out, int samplesToGenerate, OUTPUTSAMPLETYPE *output)
{
	uint8_t bytesPerFrame = out->bytesPerFrame;

	while (samplesToGenerate) {
		switch (out->activeStreamCount) {
			case 0:
				samplesToGenerate *= out->numChannels;
				while (samplesToGenerate--)
					*output++ = 0;
				samplesToGenerate = 0;
				break;

			case 1: {
				modAudioOutStream stream = out->activeStream[0];
				modAudioQueueElement element = stream->element;
				int use = element->sampleCount - element->position;
				if (use > samplesToGenerate)
					use = samplesToGenerate;
				c_memcpy(output, (element->position * bytesPerFrame) + (uint8_t *)element->samples, use * bytesPerFrame);

				output = (OUTPUTSAMPLETYPE *)((use * bytesPerFrame) + (uint8_t *)output);
				samplesToGenerate -= use;
				element->position += use;

				if (element->position == element->sampleCount)
					endOfElement(out, stream);
				}
				break;

#if MODDEF_AUDIOOUT_STREAMS > 1
			case 2: {
				modAudioOutStream stream0 = out->activeStream[0];
				modAudioOutStream stream1 = out->activeStream[1];
				modAudioQueueElement element0 = stream0->element;
				modAudioQueueElement element1 = stream1->element;
				int use0 = element0->sampleCount - element0->position;
				int use1 = element1->sampleCount - element1->position;
				int use = (use0 < use1) ? use0 : use1;
				if (use > samplesToGenerate)
					use = samplesToGenerate;

				OUTPUTSAMPLETYPE *s0 = (OUTPUTSAMPLETYPE *)((element0->position * bytesPerFrame) + (uint8_t *)element0->samples);
				OUTPUTSAMPLETYPE *s1 = (OUTPUTSAMPLETYPE *)((element1->position * bytesPerFrame) + (uint8_t *)element1->samples);
				int count = use * out->numChannels;
				while (count--)
					*output++ = *s0++ + *s1++;

				samplesToGenerate -= use;
				element0->position += use;
				element1->position += use;

				if (element0->position == element0->sampleCount)
					endOfElement(out, stream0);
				if (element1->position == element1->sampleCount)
					endOfElement(out, stream1);
				}
				break;
#endif

#if MODDEF_AUDIOOUT_STREAMS > 2
			case 3: {
				modAudioOutStream stream0 = out->activeStream[0];
				modAudioOutStream stream1 = out->activeStream[1];
				modAudioOutStream stream2 = out->activeStream[2];
				modAudioQueueElement element0 = stream0->element;
				modAudioQueueElement element1 = stream1->element;
				modAudioQueueElement element2 = stream2->element;
				int use0 = element0->sampleCount - element0->position;
				int use1 = element1->sampleCount - element1->position;
				int use2 = element2->sampleCount - element2->position;
				int use = (use0 < use1) ? use0 : use1;
				if (use > use2) use = use2;
				if (use > samplesToGenerate)
					use = samplesToGenerate;

				OUTPUTSAMPLETYPE *s0 = (OUTPUTSAMPLETYPE *)((element0->position * bytesPerFrame) + (uint8_t *)element0->samples);
				OUTPUTSAMPLETYPE *s1 = (OUTPUTSAMPLETYPE *)((element1->position * bytesPerFrame) + (uint8_t *)element1->samples);
				OUTPUTSAMPLETYPE *s2 = (OUTPUTSAMPLETYPE *)((element2->position * bytesPerFrame) + (uint8_t *)element2->samples);
				int count = use * out->numChannels;
				while (count--)
					*output++ = *s0++ + *s1++ + *s2++;

				samplesToGenerate -= use;
				element0->position += use;
				element1->position += use;
				element2->position += use;

				if (element0->position == element0->sampleCount)
					endOfElement(out, stream0);
				if (element1->position == element1->sampleCount)
					endOfElement(out, stream1);
				if (element2->position == element2->sampleCount)
					endOfElement(out, stream2);
				}
				break;
#endif

#if MODDEF_AUDIOOUT_STREAMS > 3
			case 4: {
				modAudioOutStream stream0 = out->activeStream[0];
				modAudioOutStream stream1 = out->activeStream[1];
				modAudioOutStream stream2 = out->activeStream[2];
				modAudioOutStream stream3 = out->activeStream[3];
				modAudioQueueElement element0 = stream0->element;
				modAudioQueueElement element1 = stream1->element;
				modAudioQueueElement element2 = stream2->element;
				modAudioQueueElement element3 = stream3->element;
				int use0 = element0->sampleCount - element0->position;
				int use1 = element1->sampleCount - element1->position;
				int use2 = element2->sampleCount - element2->position;
				int use3 = element3->sampleCount - element3->position;
				int use = (use0 < use1) ? use0 : use1;
				if (use > use2) use = use2;
				if (use > use3) use = use3;
				if (use > samplesToGenerate)
					use = samplesToGenerate;

				OUTPUTSAMPLETYPE *s0 = (OUTPUTSAMPLETYPE *)((element0->position * bytesPerFrame) + (uint8_t *)element0->samples);
				OUTPUTSAMPLETYPE *s1 = (OUTPUTSAMPLETYPE *)((element1->position * bytesPerFrame) + (uint8_t *)element1->samples);
				OUTPUTSAMPLETYPE *s2 = (OUTPUTSAMPLETYPE *)((element2->position * bytesPerFrame) + (uint8_t *)element2->samples);
				OUTPUTSAMPLETYPE *s3 = (OUTPUTSAMPLETYPE *)((element3->position * bytesPerFrame) + (uint8_t *)element3->samples);
				int count = use * out->numChannels;
				while (count--)
					*output++ = *s0++ + *s1++ + *s2++ + *s3++;

				samplesToGenerate -= use;
				element0->position += use;
				element1->position += use;
				element2->position += use;
				element3->position += use;

				if (element0->position == element0->sampleCount)
					endOfElement(out, stream0);
				if (element1->position == element1->sampleCount)
					endOfElement(out, stream1);
				if (element2->position == element2->sampleCount)
					endOfElement(out, stream2);
				if (element3->position == element3->sampleCount)
					endOfElement(out, stream3);
				}
				break;
#endif
		}
	}
}

// note: endOfElement relies on caller to lock mutex
void endOfElement(modAudioOut out, modAudioOutStream stream)
{
	modAudioQueueElement element = stream->element;

	element->position = 0;
	if (element->repeat < 0) {		// infinity... continues until more samples queued
		int i;
		for (i = 1; i < stream->elementCount; i++) {
			if (stream->element[i].sampleCount) {
				element->repeat = 0;
				break;
			}
		}
	}
	else
		element->repeat -= 1;

	while (0 == element->repeat) {
		if (0 == element->sampleCount)
			queueCallback(out, (xsIntegerValue)element->samples);

		stream->elementCount -= 1;
		if (stream->elementCount)
			c_memcpy(element, element + 1, sizeof(modAudioQueueElementRecord) * stream->elementCount);
		else {
			updateActiveStreams(out);
			break;
		}
	}
}
