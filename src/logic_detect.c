/*
 * logic_detect.c - automatic logic IC detection
 *
 * This file is a part of Minipro.
 *
 * Minipro is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Minipro is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "minipro.h"
#include "database.h"
#include "logic_detect.h"

/* The firmware numbers the ZIF socket pins in the TL866II+ 40-pin frame:
 * chip pin 1 sits at ZIF pin 1, the right hand column of the chip is
 * aligned to ZIF pin 40 (chip pin p > n/2 is at ZIF pin p + 40 - n). */
#define FRAME_PINS 40
#define ZIF_PINS   64 /* room for the programmer specific pin arrays */

/* Result of the passive socket probe */
typedef struct probe {
	int pin_count;		    /* detected package pin count (0 = none) */
	int uneven;		    /* left/right column disagree */
	uint8_t occupied[FRAME_PINS]; /* ZIF pin (index+1) is connected */
	uint8_t gnd[FRAME_PINS];      /* ZIF pin (index+1) looks like GND */
} probe_t;

/* Candidate list built from the database */
typedef struct candidate {
	device_t *device;
	uint32_t vcc_mask; /* chip pins marked V in the vectors */
	uint32_t gnd_mask; /* chip pins marked G in the vectors */
	int tier;	   /* 0: power pins match the probe exactly, 1: GND only */
	int layout_count;  /* how many candidates share vcc/gnd layout */
} candidate_t;

typedef struct candidates {
	candidate_t *list;
	int count;
	int allocated;
} candidates_t;

/* Map a ZIF frame pin to the chip pin number */
static int zif_to_chip(int zif, int pin_count)
{
	return zif <= pin_count / 2 ? zif : zif - (FRAME_PINS - pin_count);
}

/* Passive socket probe.
 * Every pin gets a pull-up, then one pin at a time is driven low and all
 * pins are read back. A pin that belongs to the chip drags the chip GND
 * pin(s) low through its substrate (ESD) diode, so the coupling pattern
 * reveals which socket pins are connected to a chip and which of them are
 * the power pins: for CMOS chips only the ground pin(s) are dragged low,
 * for bipolar TTL chips the VCC pin is dragged along as well (through the
 * input structures). No supply voltage is applied to the chip. */
static int probe_socket(minipro_handle_t *handle, probe_t *probe)
{
	uint8_t dir[ZIF_PINS], state[ZIF_PINS], zif[ZIF_PINS];
	static uint8_t low[FRAME_PINS][FRAME_PINS]; /* [driven][pin] */
	int pulled[FRAME_PINS] = { 0 };

	memset(probe, 0, sizeof(*probe));
	memset(low, 0, sizeof(low));

	for (int p = 0; p < FRAME_PINS; p++) {
		memset(dir, MP_PIN_DIRECTION_IN | MP_PIN_PULLUP, sizeof(dir));
		memset(state, 0x00, sizeof(state));
		memset(zif, 0x00, sizeof(zif));
		dir[p] = MP_PIN_DIRECTION_OUT;
		if (minipro_reset_state(handle) ||
		    minipro_set_zif_direction(handle, dir) ||
		    minipro_set_zif_state(handle, state) ||
		    minipro_get_zif_state(handle, zif))
			return EXIT_FAILURE;
		for (int q = 0; q < FRAME_PINS; q++) {
			if (q != p && !zif[q]) {
				low[p][q] = 1;
				probe->occupied[p] = 1;
				probe->occupied[q] = 1;
				pulled[q]++;
			}
		}
	}
	if (minipro_reset_state(handle))
		return EXIT_FAILURE;

	/* Package size from the outermost occupied pins of each column */
	int left = 0, right = 0, occupied = 0;
	for (int p = 0; p < FRAME_PINS; p++) {
		if (!probe->occupied[p])
			continue;
		occupied++;
		if (p < FRAME_PINS / 2)
			left = p + 1;
		else if (!right)
			right = FRAME_PINS - p;
	}
	if (!occupied)
		return EXIT_SUCCESS;
	int half = left > right ? left : right;
	probe->pin_count = 2 * half;
	probe->uneven = (left != right);

	/* Power pins are dragged low by (nearly) every other chip pin */
	int max = 0;
	for (int p = 0; p < FRAME_PINS; p++)
		if (pulled[p] > max)
			max = pulled[p];
	for (int p = 0; p < FRAME_PINS; p++)
		if (max >= 2 && pulled[p] * 4 >= max * 3)
			probe->gnd[p] = 1;

	return EXIT_SUCCESS;
}

/* Collect the V and G pins of a vector table as chip pin bit masks */
static void power_masks(device_t *device, uint32_t *vcc, uint32_t *gnd)
{
	*vcc = *gnd = 0;
	if (!device->vectors)
		return;
	for (int i = 0; i < device->package_details.pin_count && i < 32;
	     i++) {
		if (device->vectors[i] == LOGIC_V)
			*vcc |= 1u << i;
		if (device->vectors[i] == LOGIC_G)
			*gnd |= 1u << i;
	}
}

static void free_device(device_t *device)
{
	if (!device)
		return;
	free(device->vectors);
	free(device);
}

/* list_logic_devices callback: take ownership of the device */
static int collect_cb(device_t *device, void *ctx)
{
	candidates_t *c = ctx;
	if (c->count == c->allocated) {
		int n = c->allocated ? c->allocated * 2 : 64;
		candidate_t *list = realloc(c->list, n * sizeof(*list));
		if (!list) {
			free_device(device);
			return EXIT_FAILURE;
		}
		c->list = list;
		c->allocated = n;
	}
	candidate_t *cand = &c->list[c->count++];
	memset(cand, 0, sizeof(*cand));
	cand->device = device;
	power_masks(device, &cand->vcc_mask, &cand->gnd_mask);
	return EXIT_SUCCESS;
}

/* Sort candidates: best probe match first, then common power pin layouts */
static int compare_candidates(const void *a, const void *b)
{
	const candidate_t *ca = a, *cb = b;
	if (ca->tier != cb->tier)
		return ca->tier - cb->tier;
	if (ca->layout_count != cb->layout_count)
		return cb->layout_count - ca->layout_count;
	if (ca->gnd_mask != cb->gnd_mask)
		return ca->gnd_mask < cb->gnd_mask ? -1 : 1;
	if (ca->vcc_mask != cb->vcc_mask)
		return ca->vcc_mask < cb->vcc_mask ? -1 : 1;
	return 0;
}

static void print_pin_list(uint32_t mask)
{
	int first = 1;
	for (int i = 0; i < 32; i++) {
		if (mask & (1u << i)) {
			fprintf(stderr, "%s%d", first ? "" : ",", i + 1);
			first = 0;
		}
	}
}

int logic_ic_detect(minipro_handle_t *handle)
{
	probe_t probe;
	candidates_t cands = { 0 };
	int ret = EXIT_FAILURE;

	if (!handle->minipro_logic_ic_check ||
	    !handle->minipro_set_zif_direction) {
		fprintf(stderr,
			"%s: automatic logic IC detection is not supported.\n",
			handle->model);
		return EXIT_FAILURE;
	}

	/* Step 1: passive probe */
	fprintf(stderr, "Probing the ZIF socket...\n");
	if (probe_socket(handle, &probe)) {
		fprintf(stderr, "Socket probe failed.\n");
		return EXIT_FAILURE;
	}
	if (!probe.pin_count) {
		fprintf(stderr, "No chip detected in the ZIF socket.\n");
		return EXIT_FAILURE;
	}

	uint32_t gnd_mask = 0;
	int gnd_count = 0;
	for (int p = 0; p < FRAME_PINS; p++) {
		if (probe.gnd[p]) {
			int chip_pin = zif_to_chip(p + 1, probe.pin_count);
			if (chip_pin >= 1 && chip_pin <= 32)
				gnd_mask |= 1u << (chip_pin - 1);
			gnd_count++;
		}
	}
	fprintf(stderr, "Detected a %d pin package", probe.pin_count);
	if (gnd_count) {
		fprintf(stderr, ", power pin(s) at ");
		print_pin_list(gnd_mask);
	}
	fprintf(stderr, ".\n");
	if (probe.uneven)
		fprintf(stderr,
			"Warning: the two pin columns have different lengths, "
			"check for bad pin contact.\n");
	if (!gnd_count)
		fprintf(stderr,
			"Warning: no power pin found, trying all power pin "
			"layouts.\n");

	/* Step 2: collect candidates from the database */
	db_data_t db_data;
	memset(&db_data, 0, sizeof(db_data));
	db_data.logicic_path = handle->cmdopts->logicic_path;
	db_data.infoic_path = handle->cmdopts->infoic_path;
	db_data.prog_version = handle->version;
	if (list_logic_devices(&db_data, probe.pin_count, collect_cb,
			       &cands)) {
		fprintf(stderr, "Error reading the logic IC database.\n");
		goto out;
	}

	/* Keep only candidates whose power pins fit the probe result:
	 * tier 0: VCC and GND pins are exactly the probed power pins (TTL),
	 * tier 1: the GND pins are a subset of the probed power pins (CMOS,
	 * where VCC is usually not visible in the probe). */
	int n = 0;
	for (int i = 0; i < cands.count; i++) {
		candidate_t *c = &cands.list[i];
		uint32_t power = c->vcc_mask | c->gnd_mask;
		if (!gnd_count || power == gnd_mask)
			c->tier = 0;
		else if ((c->gnd_mask & gnd_mask) == c->gnd_mask)
			c->tier = 1;
		else {
			free_device(c->device);
			continue;
		}
		cands.list[n++] = *c;
	}
	cands.count = n;
	if (!cands.count) {
		fprintf(stderr,
			"No logic IC with %d pins and this power pin layout "
			"in the database.\n",
			probe.pin_count);
		goto out;
	}

	/* Rank power pin layouts by how common they are so that the
	 * usual VCC/GND placement is tried before the exotic ones. */
	for (int i = 0; i < cands.count; i++) {
		cands.list[i].layout_count = 0;
		for (int j = 0; j < cands.count; j++)
			if (cands.list[j].vcc_mask == cands.list[i].vcc_mask &&
			    cands.list[j].gnd_mask == cands.list[i].gnd_mask)
				cands.list[i].layout_count++;
	}
	qsort(cands.list, cands.count, sizeof(candidate_t),
	      compare_candidates);

	/* Optional test voltage (-o vcc=<value>), default is the database
	 * value of each device (5 V for most chips) */
	int vcc = -1;
	if (handle->cmdopts->logic_vcc) {
		const parameters_t *table = get_logic_vcc_table();
		for (; table->name; table++) {
			if (!strcasecmp(table->name,
					handle->cmdopts->logic_vcc)) {
				vcc = table->value;
				break;
			}
		}
		if (vcc < 0) {
			fprintf(stderr, "Invalid VCC value '%s'. Use one of: ",
				handle->cmdopts->logic_vcc);
			for (table = get_logic_vcc_table(); table->name;
			     table++)
				fprintf(stderr, "%s%s", table->name,
					(table + 1)->name ? ", " : "\n");
			goto out;
		}
		fprintf(stderr, "Testing at VCC = %s V.\n",
			handle->cmdopts->logic_vcc);
	}

	/* Step 3: run the test vectors of every candidate */
	fprintf(stderr, "Testing %d candidate(s)...\n", cands.count);
	int matches = 0, tested = 0, best_errors = -1;
	const char *best_name = NULL;
	uint32_t layout_vcc = cands.list[0].vcc_mask;
	uint32_t layout_gnd = cands.list[0].gnd_mask;
	for (int i = 0; i < cands.count; i++) {
		candidate_t *c = &cands.list[i];
		if (c->vcc_mask != layout_vcc || c->gnd_mask != layout_gnd) {
			/* New power pin layout */
			if (matches) {
				fprintf(stderr,
					"Skipping %d candidate(s) with a "
					"different VCC/GND layout.\n",
					cands.count - i);
				break;
			}
			layout_vcc = c->vcc_mask;
			layout_gnd = c->gnd_mask;
			fprintf(stderr, "Trying layout VCC on pin ");
			print_pin_list(layout_vcc);
			fprintf(stderr, ", GND on pin ");
			print_pin_list(layout_gnd);
			fprintf(stderr, "...\n");
		}
		if (vcc >= 0)
			c->device->voltages.vcc = (uint8_t)vcc;
		handle->device = c->device;
		int errors = minipro_logic_ic_check(handle);
		handle->device = NULL;
		tested++;
		if (errors == MP_LOGIC_OVC) {
			fprintf(stderr, "  %-40s overcurrent\n",
				c->device->name);
			continue;
		}
		if (errors < 0) {
			fprintf(stderr, "  %-40s test error\n",
				c->device->name);
			continue;
		}
		if (errors)
			fprintf(stderr, "  %-40s %d errors\n",
				c->device->name, errors);
		if (!errors) {
			fprintf(stderr, "  %-40s MATCH\n", c->device->name);
			matches++;
		} else if (best_errors < 0 || errors < best_errors) {
			best_errors = errors;
			best_name = c->device->name;
		}
	}

	fprintf(stderr, "\n");
	if (matches) {
		fprintf(stderr, "%d matching device(s) found out of %d tested.\n",
			matches, tested);
		ret = EXIT_SUCCESS;
	} else {
		fprintf(stderr, "No matching device found (%d tested).\n",
			tested);
		if (best_name)
			fprintf(stderr, "Closest: %s with %d errors.\n",
				best_name, best_errors);
	}

out:
	minipro_reset_state(handle);
	for (int i = 0; i < cands.count; i++)
		free_device(cands.list[i].device);
	free(cands.list);
	return ret;
}
