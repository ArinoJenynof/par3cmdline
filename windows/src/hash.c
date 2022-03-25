
// avoid error of MSVC
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC headers
#include <search.h>

#include "blake3/blake3.h"
#include "libpar3.h"
#include "hash.h"


/*
CRC-64-ISO
https://reveng.sourceforge.io/crc-catalogue/17plus.htm

The Go Authors (26 January 2017), The Go Programming Language, module src/hash/crc64/crc64_test.go
https://go.dev/src/hash/crc64/crc64_test.go

#define CRC64_POLY	0xD800000000000000	// CRC-64-ISO (little endian)
*/

// Basic function, which calculates each byte.
/*
uint64_t crc64(const uint8_t *buf, size_t size, uint64_t crc)
{
	uint64_t A;

	crc = ~crc;
	for (size_t i = 0; i < size; ++i){
		A = crc ^ buf[i];
		A = A << 56;
		crc = (crc >> 8) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
	}
	return ~crc;
}
*/

/*
Fast CRCs are from;
[1] Gam D. Nguyen, Fast CRCs, IEEE Transactions on Computers, vol. 58, no.
10, pp. 1321-1331, Oct. 2009.
*/

// Fast CRC function, which calculates 4 bytes per loop.
uint64_t crc64(const uint8_t *buf, size_t size, uint64_t crc)
{
	uint64_t A;

	crc = ~crc;

	// calculate each byte until 4-bytes alignment
	while ((size > 0) && (((size_t)buf) & 3)){
		A = crc ^ (*buf++);
		A = A << 56;
		crc = (crc >> 8) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
		size--;
	}

	// calculate 4-bytes per loop
	while (size >= 4){
		A = crc ^ (*((uint32_t *)buf));
		A = A << 32;

		// Below is same as this line;
		// crc = (crc >> 32) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
		A = A ^ (A >> 1);
		crc = (crc >> 32) ^ A ^ (A >> 3);

		size -= 4;
		buf += 4;
	}

	// calculate remaining bytes
	while (size > 0){
		A = crc ^ (*buf++);
		A = A << 56;
		crc = (crc >> 8) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
		size--;
	}

	return ~crc;
}


/*
While CRC-64-ISO doesn't need table look-up to calculate CRC, it may use table to reverse CRC.

How to construct reverse_table[256] for CRC-64-ISO on time is following theory by Yutaka Sawada.

top = crc64_table[i] >> 56;
crc64_reverse[top] = (crc64_table[i] << 8) | i;

How to calculate "A" from below equation.
top = A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);

  A7 A6 A5 A4 A3 A2 A1 A0  A-1 A-2 A-3 A-4
^    A7 A6 A5 A4 A3 A2 A1   A0
^          A7 A6 A5 A4 A3   A2  A1  A0
^             A7 A6 A5 A4   A3  A2  A1  A0
= T7 T6 T5 T4 T3 T2 T1 T0

A7 = T7
A6 = T6 ^ T7
A5 = T5 ^ T6 ^ T7
A4 = T4 ^ T5 ^ T6
A3 = T3 ^ T4 ^ T5
A2 = T2 ^ T3 ^ T4
A1 = T1 ^ T2 ^ T3 ^ T7
A0 = T0 ^ T1 ^ T2 ^ T6 ^ T7

Then,
A = T ^ (T >> 1) ^ (T >> 2) ^ (T >> 6) ^ (T >> 7);
Or,
A = T ^ (T >> 1) ^ (T >> 2); A = A ^ (A >> 6);

crc64_table[A] = ( A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4) ) << 56;
(crc64_table[A] << 8) = ( A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4) ) << 64; There are only A-1 A-2 A-3 A-4.
                      = (A << 63) ^ (A << 61) ^ (A << 60);

Thus,
A = T ^ (T >> 1) ^ (T >> 2) ^ (T >> 6) ^ (T >> 7);
crc64_reverse[T] = (A << 63) ^ (A << 61) ^ (A << 60) ^ A;
*/

// This updates CRC-64 of zeros without bit flipping.
uint64_t crc64_update_zero(size_t size, uint64_t crc)
{
	uint64_t A;

	// calculate 4-bytes per loop
	while (size >= 4){
		A = crc << 32;

		// Below is same as this line;
		// crc = (crc >> 32) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
		A = A ^ (A >> 1);
		crc = (crc >> 32) ^ A ^ (A >> 3);

		size -= 4;
	}

	while (size > 0){
		A = crc << 56;
		crc = (crc >> 8) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);
		size--;
	}

	return crc;
}

// This return window_mask.
static uint64_t init_slide_window(uint64_t window_size, uint64_t window_table[256])
{
	int i;
	uint64_t rr, window_mask;

	window_table[0] = 0; // This is always 0.
	for (i = 1; i < 256; i++){
		// calculate instant table of CRC-64-ISO
		rr = i;
		rr = rr << 56;
		rr = rr ^ (rr >> 1) ^ (rr >> 3) ^ (rr >> 4);
		window_table[i] = crc64_update_zero(window_size, rr);
	}

	window_mask = crc64_update_zero(window_size, ~0) ^ (~0);
	//printf("window_mask = 0x%016I64X, 0x%016I64X\n", window_mask, rr);

	return window_mask;
}

// table setup for slide window search
void init_crc_slide_table(PAR3_CTX *par3_ctx, int flag_usage)
{
	if (flag_usage & 1){
		// Creation needs block size only for deduplication.
		par3_ctx->window_mask = init_slide_window(par3_ctx->block_size, par3_ctx->window_table);
	}
	if (flag_usage & 2){
		// Verification needs 2 sizes for find blocks and chunk tails.
		par3_ctx->window_mask40 = init_slide_window(40, par3_ctx->window_table40);
	}

/*
	int i, j;
	uint64_t crc64_table[256], crc64_reverse[256], rr;

	for (i = 0; i < 256; i++){
		// CRC-64
		rr = i;
		for (j = 0; j < 8; j++){
			if (rr & 1){
				rr = (rr >> 1) ^ CRC64_POLY;
			} else {
				rr = (rr >> 1);
			}
		}
		crc64_table[i] = rr;

		// for debug
		rr = i;
		rr = rr << 56;
		rr = rr ^ (rr >> 1) ^ (rr >> 3) ^ (rr >> 4);
		if (crc64_table[i] != rr){	// compare results
			printf("crc64_table[%3d] = %016I64x, %016I64x\n", i, crc64_table[i], rr);
		}
	}

	// Locate table index from the lowest 1-byte of CRC.
	// Shift left the table by 8-bits, and put the index (0~255) in the lowest 1-byte.
	for (i = 0; i < 256; i++){
		crc64_reverse[(crc64_table[i] >> 56)] = (crc64_table[i] << 8) | i;
	}

	// for debug
	for (i = 0; i < 256; i++){
		// test on-time table construction
		rr = i ^ (i >> 1) ^ (i >> 2) ^ (i >> 6) ^ (i >> 7);
		rr = (rr << 63) ^ (rr << 61) ^ (rr << 60) ^ rr;
		if (crc64_reverse[i] != rr){	// compare results
			printf("crc64_reverse[%3d] = %016I64x, %016I64x\n", i, crc64_reverse[i], rr);
		}
	}
*/
}

// Slide the CRC-64-ISO along a buffer by one byte (removing the old and adding the new).
// crc = window_mask ^ crc_slide_byte(window_mask ^ crc, buffer[window], buffer[0], window_table);
uint64_t crc_slide_byte(uint64_t crc, uint8_t byteNew, uint8_t byteOld, uint64_t window_table[256])
{
	uint64_t A;

	// CRC-64-ISO doesn't use table look-up.
	A = crc ^ byteNew;
	A = A << 56;
	crc = (crc >> 8) ^ A ^ (A >> 1) ^ (A >> 3) ^ (A >> 4);

	return crc ^ window_table[byteOld];
}


// Compare CRC-64 values
static int compare_crc( const void *arg1, const void *arg2 )
{
	PAR3_CMP_CTX *cmp1_p, *cmp2_p;

	cmp1_p = ( PAR3_CMP_CTX * ) arg1;
	cmp2_p = ( PAR3_CMP_CTX * ) arg2;

	if (cmp1_p->crc < cmp2_p->crc)
		return -1;
	if (cmp1_p->crc > cmp2_p->crc)
		return 1;

	return 0;
}

// Compare CRC-64 of blocks
// When no match, return -1 ~ -2. When calculated fingerprint hash, return -3.
int64_t crc_list_compare(PAR3_CTX *par3_ctx, uint64_t count, uint64_t crc, uint8_t *buf, uint8_t hash[16])
{
	uint64_t index;
	PAR3_CMP_CTX cmp_key, *cmp_p, *cmp2_p;
	PAR3_BLOCK_CTX *block_p;

	if (count == 0)
		return -1;

	// Binary search
	cmp_key.crc = crc;
	cmp_p = (PAR3_CMP_CTX *)bsearch( &cmp_key, par3_ctx->crc_list, (size_t)count, sizeof(PAR3_CMP_CTX), compare_crc );
	if (cmp_p == NULL)
		return -2;

	block_p = par3_ctx->block_list;
	blake3(buf, par3_ctx->block_size, hash);
	if (memcmp(hash, block_p[cmp_p->index].hash, 16) == 0)
		return cmp_p->index;

	// Search lower items of same CRC-64
	cmp2_p = cmp_p;
	index = cmp_p - par3_ctx->crc_list;
	while (index > 0){
		index--;
		cmp2_p--;
		if (cmp2_p->crc != crc)
			break;
		if (memcmp(hash, block_p[cmp2_p->index].hash, 16) == 0)
			return cmp2_p->index;
	}

	// Search higher items of same CRC-64
	cmp2_p = cmp_p;
	index = cmp_p - par3_ctx->crc_list;
	while (index + 1 < count){
		index++;
		cmp2_p++;
		if (cmp2_p->crc != crc)
			break;
		if (memcmp(hash, block_p[cmp2_p->index].hash, 16) == 0)
			return cmp2_p->index;
	}

	return -3;
}

// When it adds new crc in list, it returns new count.
uint64_t crc_list_add(PAR3_CTX *par3_ctx, uint64_t count, uint64_t crc, uint64_t index)
{
	if (count >= par3_ctx->block_count)
		return count;

	// Add new item.
	par3_ctx->crc_list[count].crc = crc;
	par3_ctx->crc_list[count].index = index;
	count++;

	// Quick sort items.
	qsort( (void *)(par3_ctx->crc_list), (size_t)count, sizeof(PAR3_CMP_CTX), compare_crc );

	return count;
}

// Make list of crc for seaching full size blocks and chunk tails.
int crc_list_make(PAR3_CTX *par3_ctx)
{
	uint64_t count, tail_count, index, map_index;
	uint64_t block_count, chunk_count;
	uint64_t block_size, data_size;
	PAR3_MAP_CTX *map_list;
	PAR3_BLOCK_CTX *block_p;
	PAR3_CHUNK_CTX *chunk_p;
	PAR3_CMP_CTX *crc_list, *tail_list;

	if (par3_ctx->block_count == 0){
		par3_ctx->crc_count = 0;
		par3_ctx->tail_count = 0;
		return 0;
	}

	// Allocate list of CRC-64
	block_count = par3_ctx->block_count;
	crc_list = malloc(sizeof(PAR3_CMP_CTX) * block_count);
	if (crc_list == NULL){
		perror("Failed to allocate memory for comparison of CRC-64");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->crc_list = crc_list;

	// At this time, number of tails is unknown.
	// When a chunk size is multiple of block size, the chunk has no tail.
	chunk_count = par3_ctx->chunk_count;
	tail_list = malloc(sizeof(PAR3_CMP_CTX) * chunk_count);
	if (tail_list == NULL){
		perror("Failed to allocate memory for comparison of CRC-64");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->tail_list = tail_list;

	count = 0;
	tail_count = 0;
	map_list = par3_ctx->map_list;
	block_p = par3_ctx->block_list;
	chunk_p = par3_ctx->chunk_list;
	block_size = par3_ctx->block_size;

	// Find full size block, and set CRC of the block.
	for (index = 0; index < block_count; index++){
		data_size = block_p->size;
		if (data_size == block_size){	// check map for tails, too.
			map_index = block_p->map;
			data_size = map_list[map_index].size;
			while (data_size < block_size){
				map_index = map_list[map_index].next;
				if (map_index == -1)
					break;
				data_size = map_list[map_index].size;
			}
		}
		if (data_size == block_size){
			crc_list[count].crc = block_p->crc;
			crc_list[count].index = index;
			count++;
		}

		block_p++;
	}

	// Find chunk tail, and set CRC of the chunk.
	for (index = 0; index < chunk_count; index++){
		data_size = chunk_p->size;
		if (data_size % block_size >= 40){	// This chunk tail belongs to a block.
			tail_list[tail_count].crc = chunk_p->tail_crc;
			tail_list[tail_count].index = index;
			tail_count++;
		}

		chunk_p++;
	}

	// Re-allocate memory for actual number of CRC-64
	if (count < block_count){
		if (count > 0){
			crc_list = realloc(par3_ctx->crc_list, sizeof(PAR3_CMP_CTX) * count);
			if (crc_list == NULL){
				perror("Failed to re-allocate memory for comparison of CRC-64");
				return RET_MEMORY_ERROR;
			}
			par3_ctx->crc_list = crc_list;
		} else {
			free(par3_ctx->crc_list);
			par3_ctx->crc_list = NULL;
		}
	}
	if (tail_count < chunk_count){
		if (tail_count > 0){
			tail_list = realloc(par3_ctx->tail_list, sizeof(PAR3_CMP_CTX) * tail_count);
			if (tail_list == NULL){
				perror("Failed to re-allocate memory for comparison of CRC-64");
				return RET_MEMORY_ERROR;
			}
			par3_ctx->tail_list = tail_list;
		} else {
			free(par3_ctx->tail_list);
			par3_ctx->tail_list = NULL;
		}
	}

	// Quick sort items.
	if (count > 1){
		// CRC for full size block
		qsort( (void *)crc_list, (size_t)count, sizeof(PAR3_CMP_CTX), compare_crc );
	}
	if (tail_count > 1){
		// CRC for chunk tail
		qsort( (void *)tail_list, (size_t)tail_count, sizeof(PAR3_CMP_CTX), compare_crc );
	}

	par3_ctx->crc_count = count;
	par3_ctx->tail_count = tail_count;

	return 0;
}

/*
This BLAKE3 code is non-SIMD subset of portable version from below;
https://github.com/BLAKE3-team/BLAKE3

The official C implementation of BLAKE3.

This work is released into the public domain with CC0 1.0. Alternatively, it is
licensed under the Apache License 2.0.
*/

// One time calculation, which returns 16-bytes hash value.
void blake3(const uint8_t *buf, size_t size, uint8_t *hash)
{
	// Initialize the hasher.
	blake3_hasher hasher;
	blake3_hasher_init(&hasher);

	blake3_hasher_update(&hasher, buf, size);

	// Finalize the hash.
	blake3_hasher_finalize(&hasher, hash, 16);
}

