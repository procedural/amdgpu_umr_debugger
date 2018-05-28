/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Tom St Denis <tom.stdenis@amd.com>
 *
 */
#include "umrapp.h"

struct umr_profiler_hit {
	uint32_t
		vmid,
		inst_dw0,
		inst_dw1,
		shader_size;

	uint64_t
		pc,
		base_addr;
};

struct umr_profiler_rle {
	struct umr_profiler_hit data;
	uint32_t cnt;
};

struct umr_profiler_shaders {
	uint32_t total_cnt, nhits;
	struct umr_profiler_rle *hits;
};

static int comp_hits(const void *A, const void *B)
{
	return memcmp(A, B, sizeof(struct umr_profiler_hit));
}

static int comp_shaders(const void *A, const void *B)
{
	const struct umr_profiler_shaders *a = A, *b = B;
	return b->total_cnt - a->total_cnt;
}

void umr_profiler(struct umr_asic *asic, int samples, int delay)
{
	struct umr_profiler_hit *ophit, *phit;
	struct umr_profiler_rle *prle;
	struct umr_profiler_shaders *shaders;
	struct umr_wave_data *owd, *wd;
	struct umr_pm4_stream *stream;
	struct umr_shaders_pgm *shader;
	unsigned nitems, nmax, nshaders, x, y, z, found;

	nmax = samples;
	nitems = 0;
	ophit = phit = calloc(nmax, sizeof *phit);

	if (!asic->mmio_accel.reglist)
		umr_create_mmio_accel(asic);

	while (samples--) {
		fprintf(stderr, "%5u samples left\r", samples);
		fflush(stderr);
		do {
			umr_sq_cmd_halt_waves(asic, UMR_SQ_CMD_RESUME);
			if (delay)
				usleep(delay);
			umr_sq_cmd_halt_waves(asic, UMR_SQ_CMD_HALT);
			wd = umr_scan_wave_data(asic);
		} while (!wd);

		// grab PM4 stream for these halted waves
		// in theory if waves are halted the packet
		// processor is also halted so we can grab the
		// stream.  This isn't 100% though it seems so race
		// conditions might occur.
		stream = umr_pm4_decode_ring(asic, asic->options.ring_name[0] ? asic->options.ring_name : "gfx", 1);

		// loop through data ...
		while (wd) {
			phit[nitems].vmid = wd->ws.hw_id.vm_id;
			phit[nitems].pc = ((uint64_t)wd->ws.pc_hi << 32) | wd->ws.pc_lo;
			phit[nitems].inst_dw0 = wd->ws.wave_inst_dw0;
			phit[nitems].inst_dw1 = wd->ws.wave_inst_dw1;

			// try to find shader in PM4 stream
			shader = NULL;
			if (stream)
				shader = umr_find_shader_in_stream(stream, phit[nitems].vmid, phit[nitems].pc);
			if (shader) {
				uint32_t inst[2];

				// grab info about shader including the opcodes
				// since the WAVE_STATUS INST_DWx registers might
				// suffer from race conditions
				phit[nitems].base_addr = shader->addr;
				phit[nitems].shader_size = shader->size;
				if (umr_read_vram(asic, shader->vmid, phit[nitems].pc, 8, &inst[0]) < 0) {
					fprintf(stderr, "[ERROR]: Could not read shader at address %u:0x%llx\n", (unsigned)shader->vmid, (unsigned long long)phit[nitems].pc);
				} else {
					phit[nitems].inst_dw0 = inst[0];
					phit[nitems].inst_dw1 = inst[1];
				}

				// shader is a copy of the shader data from the stream
				free(shader);
			} else {
				phit[nitems].base_addr = 0;
				phit[nitems].shader_size = 0;
			}
			++nitems;

			// grow the hit array as needed by steps of 1000 entries
			if (nitems == nmax) {
				nmax += 1000;
				ophit = realloc(phit, nmax * sizeof(*phit));
				phit = ophit;
				memset(&phit[nitems], 0, (nmax - nitems) * sizeof(phit[0]));
			}

			owd = wd->next;
			free(wd);
			wd = owd;
		}

		if (stream)
			umr_free_pm4_stream(stream);
	}
	umr_sq_cmd_halt_waves(asic, UMR_SQ_CMD_RESUME);

	// sort all hits by address/size/etc so we can
	// RLE compress them.  The compression tells us how often
	// a particular 'hit' occurs.
	qsort(phit, nitems, sizeof(*phit), comp_hits);
	prle = calloc(nitems, sizeof *prle);
	for (z = y = 0, x = 1; x < nitems; x++) {
		if (memcmp(&phit[x], &phit[y], sizeof(*phit))) {
			prle[z].data = phit[y];
			prle[z++].cnt = x - y;
			y = x;
		}
	}

	// group RLE hits by what shader they belong to
	shaders = calloc(z, sizeof(shaders[0]));
	for (nshaders = x = 0; x < z; x++) {
		found = 0;

		// find a home for this RLE hit
		for (y = 0; y < nshaders; y++) {
			if (shaders[y].hits) {
				if (shaders[y].hits[0].data.vmid == prle[x].data.vmid &&
				    shaders[y].hits[0].data.base_addr == prle[x].data.base_addr &&
				    shaders[y].hits[0].data.shader_size == prle[x].data.shader_size) {
						// this is a match so append to end of list
						shaders[y].hits[shaders[y].nhits++] = prle[x];
						shaders[y].total_cnt += prle[x].cnt;
						found = 1;
						break; // don't need to compare any more
					}
			}
		}

		if (!found) {
			shaders[nshaders].hits = calloc(z, sizeof(shaders[nshaders].hits[0]));
			shaders[nshaders].hits[shaders[nshaders].nhits++] = prle[x];
			shaders[nshaders++].total_cnt = prle[x].cnt;
		}
	}

	// sort shaders so the busiest are first
	qsort(shaders, nshaders, sizeof(shaders[0]), comp_shaders);
	for (x = 0; x < nshaders; x++) {
		uint32_t sum = 0;
		if (shaders[x].hits) {
			char **strs;
			uint32_t *data;

			printf("\n\nShader %u@0x%llx (%lu bytes): total hits: %lu\n",
				shaders[x].hits[0].data.vmid,
				(unsigned long long)shaders[x].hits[0].data.base_addr,
				(unsigned long)shaders[x].hits[0].data.shader_size,
				(unsigned long)shaders[x].total_cnt);

			// disasm shader
			strs = calloc(shaders[x].hits[0].data.shader_size/4, sizeof(strs[0]));
			data = calloc(1, shaders[x].hits[0].data.shader_size);
			if (umr_read_vram(asic, shaders[x].hits[0].data.vmid, shaders[x].hits[0].data.base_addr, shaders[x].hits[0].data.shader_size, data) < 0) {
				fprintf(stderr, "[ERROR]: Cannot read shader at %u:0x%llx\n", (unsigned)shaders[x].hits[0].data.vmid, (unsigned long long)shaders[x].hits[0].data.base_addr);
				free(strs);
				free(data);
				continue;
			}
			umr_llvm_disasm(asic, (uint8_t *)data, shaders[x].hits[0].data.shader_size, 0xFFFFFFFF, strs);

			for (z = 0; z < shaders[x].hits[0].data.shader_size; z += 4) {
				unsigned cnt=0, pct;

				// find this offset in the hits so we know the hit count
				for (y = 0; y < shaders[x].nhits; y++) {
					if (shaders[x].hits[y].data.pc == (shaders[x].hits[0].data.base_addr + z)) {
						cnt = shaders[x].hits[y].cnt;
						break;
					}
				}

				// compute percentage for this address and then
				// colour code the line
				pct = (100 * cnt) / shaders[x].total_cnt;
				if (pct >= 30)
					printf(RED);
				else if (pct >= 20)
					printf(YELLOW);
				else if (pct >= 10)
					printf(GREEN);

				printf("\tshader[0x%llx + 0x%04llx] = 0x%08lx %-60s ",
					(unsigned long long)shaders[x].hits[0].data.base_addr,
					(unsigned long long)z,
					(unsigned long)data[z/4],
					strs[z/4]);
				free(strs[z/4]);

				if (cnt)
					printf("(%5u hits, %3u %%)", cnt, pct);
				sum += cnt;

				printf("\n%s", RST);
			}
			if (sum != shaders[x].total_cnt)
				printf("Sum mismatch: %lu != %lu\n", (unsigned long)sum, (unsigned long)shaders[x].total_cnt);
			free(strs);
			free(data);
		}
	}

	for (x = 0; x < nshaders; x++)
		free(shaders[x].hits);
	free(shaders);
	free(prle);
	free(phit);
}
