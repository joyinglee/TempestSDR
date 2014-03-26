/*******************************************************************************
 * Copyright (c) 2014 Martin Marinov.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Public License v3.0
 * which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/gpl.html
 *
 * Contributors:
 *     Martin Marinov - initial API and implementation
 ******************************************************************************/

#include "frameratedetector.h"
#include "internaldefinitions.h"
#include <stdio.h>
#include <assert.h>

#define MIN_FRAMERATE (40)
#define MAX_FRAMERATE (90)

#define FRAMERATE_RUNS (50)

// a state machine conserves energy
#define FRAMERATEDETECTOR_STATE_UNKNOWN (0)
#define FRAMERATEDETECTOR_STATE_SAMPLE_ACCURACY (1)
#define FRAMERATEDETECTOR_STATE_OFF (3)

#define FRAMERATEDETECTOR_OCCURANCES_COUNT (2)

#define FRAMERATEDETECTOR_DESIRED_FRAMERATE_ACCURACY (0.00001)

#define FRAMERATEDETECTOR_RUN_EVERY_N_SECONDS (5)

inline static double frameratedetector_fitvalue(float * data, int offset, int length) {
	double sum = 0.0;
	int i;
	for (i = 0; i < length; i++) {
		const float val1 = data[i];
		const float val2 = data[i+offset];
		const double difff = val1 - val2;
		sum += difff * difff;
	}
	return sum / (double) length;
}

inline static double frameratedetector_fitvalue_subpixel(float * data, int offset, int length, float subpixel) {
	double sum = 0.0;
	int i;

	if (subpixel >= 0) {
		const float subpixelcomplement = 1.0f - subpixel;
		for (i = 0; i < length; i++) {
			const float val1 = data[i];

			const int ioffset = i + offset;
			const float val2 = data[ioffset] * subpixelcomplement + data[ioffset+1] * subpixel;
			const double difff = val1 - val2;
			sum += difff * difff;
		}
	} else {
		subpixel = - subpixel;
		const float subpixelcomplement = 1.0f - subpixel;
		for (i = 0; i < length; i++) {
			const float val1 = data[i] * subpixelcomplement + data[i+1] * subpixel;

			const float val2 = data[i+offset];
			const double difff = val1 - val2;
			sum += difff * difff;
		}
	}
	return sum / (double) length;
}

// estimate the next repetition of a size of datah length starting from data
// for distances from startlength to endlength in samples
inline static int frameratedetector_estimatedirectlength(float * data, int length, int endlength, int startlength, float * bestfitvalue) {
	int bestlength = startlength;
	int l = startlength;
	*bestfitvalue = frameratedetector_fitvalue(data, l, length);
	l++;
	while (l < endlength) {
		const float fitvalue = frameratedetector_fitvalue(data, l, length);
		if (fitvalue < *bestfitvalue) {
			*bestfitvalue = fitvalue;
			bestlength = l;
		}
		l++;
	}

	return bestlength;
}

inline static double frameratedetector_estimatedirectlength_subpixel(float * data, int length, int roughlength, float * bestfitvalue, int iterations) {
	float bestlength = 0.0f;

	*bestfitvalue = frameratedetector_fitvalue_subpixel(data, roughlength, length, 0.0f);
	float subpixel;
	const float step = 2.0f / (float) iterations;
	for (subpixel = -1.0f; subpixel <= 1.0f; subpixel += step) {
		const float fitvalue = frameratedetector_fitvalue_subpixel(data, roughlength, length, subpixel);
		if (fitvalue < *bestfitvalue) {
			*bestfitvalue = fitvalue;
			bestlength = subpixel;
		}
	}

	return roughlength + bestlength;
}

double framedetector_estimatelinelength(tsdr_lib_t * tsdr, float * data, int size, uint32_t samplerate) {
	const int maxlength = samplerate / (double) (MIN_FRAMERATE * tsdr->height);
	const int minlength = samplerate / (double) (MAX_FRAMERATE * tsdr->height);

	float best1, best2, best3;
	const int bestintestimate = frameratedetector_estimatedirectlength(data, minlength, maxlength, minlength, &best1);

	// we would expect the next image to be 2*bestintestimates away
	const double doublebest = frameratedetector_estimatedirectlength(data, bestintestimate, 2*bestintestimate + 2, 2*bestintestimate - 2, &best2) / 2.0;
	const double triplebest = frameratedetector_estimatedirectlength(data, doublebest, 3*doublebest + 2, 3*doublebest - 2, &best3) / 3.0;

	return (best1 < best2 && best1 < best3) ? (bestintestimate) : ((best2 < best3) ? (doublebest) : (triplebest));
}

/**
 * NOTE: For trying to estimate the size of the full frame, the error would be
 * err = 2 * samplerate / ((samplerate/framerate)^2 - 1)
 *  = 0.0009 for mirics/LCD at 60 fps
 *
 * For trying to estimate the size of a line
 * err = 2 * samplerate / (((samplerate/(framerate*tsdr->height))^2 - 1) * tsdr->height)
 *  = 0.8406 for mirics/LCD at 60 fps
 */

void frameratedetector_runontodata(frameratedetector_t * frameratedetector) {

	if (frameratedetector->state == FRAMERATEDETECTOR_STATE_UNKNOWN) {
		// State 1 of the state machine
		// Obtain rough estimation, CPU intensive

		const double linelength = framedetector_estimatelinelength(frameratedetector->tsdr, frameratedetector->data, frameratedetector->desireddatalength, frameratedetector->samplerate);

		const int maxlength = (linelength+3)*frameratedetector->tsdr->height;
		const int minlength = (linelength-3)*frameratedetector->tsdr->height;

		// estimate the length of a horizontal line in samples
		float bestfit;
		int crudelength = frameratedetector_estimatedirectlength(frameratedetector->data, minlength, maxlength, minlength, &bestfit);


		//const double maxerror = frameratedetector->samplerate / (double) (crudelength * (crudelength-1));

		frameratedetector->minlength = minlength;
		if (frameratedetector->roughsize == crudelength) {
			// if we see the same length being calculated twice, switch state
				frameratedetector->state = FRAMERATEDETECTOR_STATE_SAMPLE_ACCURACY;
				//printf("Change of state!\n");fflush(stdout);
		}

		frameratedetector->roughsize = crudelength;
	}

	if (frameratedetector->state == FRAMERATEDETECTOR_STATE_SAMPLE_ACCURACY) {
		// State 2 of the state machine, refine
		float bestsubfit;
			double length = frameratedetector_estimatedirectlength_subpixel(frameratedetector->data, frameratedetector->minlength, frameratedetector->roughsize, &bestsubfit, 100);

			const double fps = frameratedetector->samplerate / length;

			if (frameratedetector->fps == 0)
				frameratedetector->fps = fps;
			else {
				const double newfps  = 0.01*fps + 0.99*frameratedetector->fps;
				const double diff = frameratedetector->fps - newfps;
				const double absdiff = (diff < 0) ? (-diff) : (diff);
				if (absdiff < FRAMERATEDETECTOR_DESIRED_FRAMERATE_ACCURACY) {
					if (frameratedetector->count_numer++ > FRAMERATEDETECTOR_OCCURANCES_COUNT) {
						frameratedetector->state = FRAMERATEDETECTOR_STATE_OFF;
						frameratedetector->size = 0;
						frameratedetector->samp_counter = 0;
						//printf("Switching off!\n");
					}
				} else
					frameratedetector->count_numer = 0;
			}

			if (frameratedetector->tsdr->params_int[PARAM_INT_AUTOPIXELRATE])
				frameratedetector->setframerate(frameratedetector->tsdr, frameratedetector->fps);
			//printf("%f bestfit %f crudelength %d length %f\n", fps, bestsubfit, frameratedetector->roughsize, length); fflush(stdout);
	}

}

void frameratedetector_thread(void * ctx) {
	frameratedetector_t * frameratedetector = (frameratedetector_t *) ctx;

	int first = 1;
	while (1) {
		int waitres = THREAD_OK;

		if (first) {
			mutex_waitforever(&frameratedetector->processing_mutex);
			first = 0;
		} else
			waitres = mutex_wait(&frameratedetector->processing_mutex);

		if (!frameratedetector->alive) break;
		if (waitres == THREAD_TIMEOUT) continue;

		frameratedetector_runontodata(frameratedetector);
		frameratedetector->processing = 0;
	}

	mutex_free(&frameratedetector->processing_mutex);
}

void frameratedetector_init(frameratedetector_t * frameratedetector, frameratedetector_setframerate_function f) {
	frameratedetector->setframerate = f;

	frameratedetector->data = NULL;
	frameratedetector->size = 0;
	frameratedetector->data_size = 0;

	frameratedetector->processing = 0;
	frameratedetector->samp_counter = 0;
}

void frameratedetector_flushcachedestimation(frameratedetector_t * frameratedetector) {
	frameratedetector->roughsize = 0;
	frameratedetector->state = FRAMERATEDETECTOR_STATE_UNKNOWN;
	frameratedetector->fps = 0;
	frameratedetector->count_numer = 0;
}

void frameratedetector_startthread(frameratedetector_t * frameratedetector) {
	frameratedetector_flushcachedestimation(frameratedetector);

	frameratedetector->processing = 0;
	frameratedetector->alive = 1;
	mutex_init(&frameratedetector->processing_mutex);

	thread_start(frameratedetector_thread, frameratedetector);
}

void frameratedetector_stopthread(frameratedetector_t * frameratedetector) {
	frameratedetector->alive = 0;
	mutex_signal(&frameratedetector->processing_mutex);
}

void frameratedetector_run(frameratedetector_t * frameratedetector, tsdr_lib_t * tsdr, float * data, int size, uint32_t samplerate, int reset) {

	// if processing has not finished, wait
	if (frameratedetector->processing) return;

	if (frameratedetector->state == FRAMERATEDETECTOR_STATE_OFF) {
		frameratedetector->samp_counter+=size;
		if (frameratedetector->samp_counter > FRAMERATEDETECTOR_RUN_EVERY_N_SECONDS * samplerate) {
			// rerun the algorithm every n seconds
			frameratedetector->state = FRAMERATEDETECTOR_STATE_UNKNOWN;
			frameratedetector->size = 0;
		}
		return;
	}

	if (reset) {
		frameratedetector->size = 0;
		return;
	}

	frameratedetector->tsdr = tsdr;
	frameratedetector->samplerate = samplerate;

	// we need to have at least two frames of data present
	frameratedetector->desireddatalength = 2.0 * samplerate / (double) (MIN_FRAMERATE);

	// resize the data to fit
	if (frameratedetector->desireddatalength > frameratedetector->data_size) {
		frameratedetector->data_size = frameratedetector->desireddatalength;
		if (frameratedetector->data == NULL)
			frameratedetector->data = malloc(sizeof(float)*frameratedetector->data_size);
		else
			frameratedetector->data = realloc((void *) frameratedetector->data, sizeof(float)*frameratedetector->data_size);
	}

	const int newsize = frameratedetector->size + size;
	if (newsize < frameratedetector->desireddatalength) {
		// copy the data into the buffer
		memcpy(&frameratedetector->data[frameratedetector->size], data, size * sizeof(float));
		frameratedetector->size = newsize;
	} else {
		const int rem = frameratedetector->desireddatalength - frameratedetector->size;

		memcpy(&frameratedetector->data[frameratedetector->size], data, rem * sizeof(float));

		// run algorithm
		frameratedetector->processing = 1;
		mutex_signal(&frameratedetector->processing_mutex);

		frameratedetector->size = 0;
	}

}

void frameratedetector_free(frameratedetector_t * frameratedetector) {
	free (frameratedetector->data);
}
