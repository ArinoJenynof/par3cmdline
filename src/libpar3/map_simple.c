#include "libpar3.h"

#include "../blake3/blake3.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hash.h"


// map input file slices into input blocks without deduplication
int map_input_block_simple(PAR3_CTX *par3_ctx)
{
	uint8_t *work_buf, buf_tail[40];
	int progress_old, progress_now;
	uint32_t num, num_pack;
	uint32_t input_file_count, chunk_index;
	uint64_t block_size, tail_size, file_offset, tail_offset;
	uint64_t block_count, block_index, slice_index, index;
	uint64_t progress_total, progress_step;
	PAR3_FILE_CTX *file_p;
	PAR3_CHUNK_CTX *chunk_p;
	PAR3_SLICE_CTX *slice_p, *slice_list;
	PAR3_BLOCK_CTX *block_p, *block_list;
	FILE *fp;
	blake3_hasher hasher;
	time_t time_old, time_now;
	clock_t clock_now;

	// Copy variables from context to local.
	input_file_count = par3_ctx->input_file_count;
	block_size = par3_ctx->block_size;
	block_count = par3_ctx->block_count;
	if ( (input_file_count == 0) || (block_size == 0) || (block_count == 0) )
		return RET_LOGIC_ERROR;

	// When no deduplication, number of chunks may be same as number of input files.
	// Note, empty file won't use Chunk Description.
	chunk_p = malloc(sizeof(PAR3_CHUNK_CTX) * input_file_count);
	if (chunk_p == NULL){
		perror("Failed to allocate memory for chunk description");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->chunk_list = chunk_p;

	// When no deduplication, number of input file slice is same as number of input blocks.
	slice_p = malloc(sizeof(PAR3_SLICE_CTX) * block_count);
	if (slice_p == NULL){
		perror("Failed to allocate memory for input file slices");
		return RET_MEMORY_ERROR;
	}
	slice_list = slice_p;
	par3_ctx->slice_list = slice_p;

	// When no deduplication, number of input blocks is calculable.
	block_p = malloc(sizeof(PAR3_BLOCK_CTX) * block_count);
	if (block_p == NULL){
		perror("Failed to allocate memory for input blocks");
		return RET_MEMORY_ERROR;
	}
	block_list = block_p;
	par3_ctx->block_list = block_p;

	// Allocate memory to store file data temporary.
	work_buf = malloc(block_size);
	if (work_buf == NULL){
		perror("Failed to allocate memory for input data");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->work_buf = work_buf;

	if (par3_ctx->noise_level >= 0){
		printf("\nComputing hash:\n");
		progress_total = par3_ctx->total_file_size;
		progress_step = 0;
		progress_old = 0;
		time_old = time(NULL);
		clock_now = clock();
	}

	// Read data of input files on memory
	num_pack = 0;
	chunk_index = 0;
	block_index = 0;
	slice_index = 0;
	file_p = par3_ctx->input_file_list;
	for (num = 0; num < input_file_count; num++){
		blake3_hasher_init(&hasher);
		if (file_p->size == 0){	// Skip empty files.
			blake3_hasher_finalize(&hasher, file_p->hash, 16);
			file_p++;
			continue;
		}
		if (par3_ctx->noise_level >= 2){
			printf("file size = %"PRIu64" \"%s\"\n", file_p->size, file_p->name);
		}

		fp = fopen(file_p->name, "rb");
		if (fp == NULL){
			perror("Failed to open input file");
			return RET_FILE_IO_ERROR;
		}

		// When no deduplication, chunk's index is same as file's index.
		file_p->chunk = chunk_index;	// single chunk in each file
		file_p->chunk_num = 1;
		chunk_p->size = file_p->size;	// file size = chunk size
		chunk_p->block = block_index;

		// Read full size blocks
		file_offset = 0;
		while (file_offset + block_size <= file_p->size){
			// read full block from input file
			if (fread(work_buf, 1, (size_t)block_size, fp) != (size_t)block_size){
				perror("Failed to read full size chunk on input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}

			// Print progress percent
			if ( (par3_ctx->noise_level >= 0) && (par3_ctx->noise_level <= 2) ){
				progress_step += block_size;
				time_now = time(NULL);
				if (time_now != time_old){
					time_old = time_now;
					progress_now = (int)((progress_step * 1000) / progress_total);
					if (progress_now != progress_old){
						progress_old = progress_now;
						printf("%d.%d%%\r", progress_now / 10, progress_now % 10);	// 0.0% ~ 100.0%
					}
				}
			}

			// calculate CRC-64 of the first 16 KB
			if (file_offset + block_size < 16384){
				file_p->crc = crc64(work_buf, (size_t)block_size, file_p->crc);
			} else if (file_offset < 16384){
				file_p->crc = crc64(work_buf, (size_t)(16384 - file_offset), file_p->crc);
			}
			blake3_hasher_update(&hasher, work_buf, (size_t)block_size);

			// set block info
			block_p->slice = slice_index;
			block_p->size = block_size;
			block_p->crc = crc64(work_buf, (size_t)block_size, 0);
			blake3(work_buf, (size_t)block_size, block_p->hash);
			block_p->state = 1 | 64;

			// set slice info
			slice_p->chunk = chunk_index;
			slice_p->file = num;
			slice_p->offset = file_offset;
			slice_p->size = block_size;
			slice_p->block = block_index;
			slice_p->tail_offset = 0;
			slice_p->next = -1;
			if (par3_ctx->noise_level >= 3){
				printf("new block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64"\n",
						block_index, slice_index, chunk_index, num, file_offset);
			}
			slice_p++;
			slice_index++;

			file_offset += block_size;
			block_p++;
			block_index++;
		}

		// Calculate size of chunk tail, and read it.
		tail_size = file_p->size - file_offset;
		//printf("tail_size = %"PRIu64", file size = %"PRIu64", offset %"PRIu64"\n", tail_size, file_p->size, file_offset);
		if (tail_size >= 40){
			// read chunk tail from input file
			if (fread(work_buf, 1, (size_t)tail_size, fp) != (size_t)tail_size){
				perror("Failed to read tail chunk on input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}

			// Print progress percent
			if ( (par3_ctx->noise_level >= 0) && (par3_ctx->noise_level <= 2) ){
				progress_step += tail_size;
				time_now = time(NULL);
				if (time_now != time_old){
					time_old = time_now;
					progress_now = (int)((progress_step * 1000) / progress_total);
					if (progress_now != progress_old){
						progress_old = progress_now;
						printf("%d.%d%%\r", progress_now / 10, progress_now % 10);	// 0.0% ~ 100.0%
					}
				}
			}

			// calculate checksum of chunk tail
			chunk_p->tail_crc = crc64(work_buf, 40, 0);
			blake3(work_buf, (size_t)tail_size, chunk_p->tail_hash);

			// search existing tails to check available space
			tail_offset = 0;
			for (index = 0; index < slice_index; index++){
				if ( (slice_list[index].next == -1) && (slice_list[index].size < block_size) ){	// the last tail in the block
					if (slice_list[index].tail_offset + slice_list[index].size + tail_size <= block_size){
						// When tail can fit in the space, put the tail there.
						tail_offset = slice_list[index].tail_offset + slice_list[index].size;
						break;
					}
				}
			}
			//printf("tail_offset = %"PRId64"\n", tail_offset);

			if (tail_offset == 0){	// Put tail in new block
				if (par3_ctx->noise_level >= 3){
					printf("n t block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64"\n",
							block_index, slice_index, chunk_index, num, file_offset, tail_size);
				}

				// set slice info
				slice_p->block = block_index;
				slice_p->tail_offset = 0;

				// set chunk tail info
				chunk_p->tail_block = block_index;
				chunk_p->tail_offset = 0;

				// set block info (block for tails don't store checksum)
				block_p->slice = slice_index;
				block_p->size = tail_size;
				block_p->crc = crc64(work_buf, (size_t)tail_size, 0);
				block_p->state = 2 | 64;
				block_p++;
				block_index++;

			} else {	// Put tail after another tail
				if (par3_ctx->noise_level >= 3){
					printf("a t block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64", offset %"PRId64"\n",
							slice_list[index].block, slice_index, chunk_index, num, file_offset, tail_size, tail_offset);
				}
				slice_list[index].next = slice_index;	// update "next" item in the front tail

				// set slice info
				slice_p->block = slice_list[index].block;
				slice_p->tail_offset = tail_offset;

				// set chunk tail info
				chunk_p->tail_block = slice_list[index].block;
				chunk_p->tail_offset = tail_offset;
				num_pack++;

				// update block info
				block_list[slice_p->block].size = tail_offset + tail_size;
				block_list[slice_p->block].crc = crc64(work_buf, (size_t)tail_size, block_list[slice_p->block].crc);
			}

			// calculate CRC-64 of the first 16 KB
			if (file_offset + tail_size < 16384){
				file_p->crc = crc64(work_buf, (size_t)tail_size, file_p->crc);
			} else if (file_offset < 16384){
				file_p->crc = crc64(work_buf, (size_t)(16384 - file_offset), file_p->crc);
			}
			blake3_hasher_update(&hasher, work_buf, (size_t)tail_size);

			// set common slice info
			slice_p->file = num;
			slice_p->offset = file_offset;
			slice_p->size = tail_size;
			slice_p->chunk = chunk_index;
			slice_p->next = -1;
			slice_p++;
			slice_index++;

		} else if (tail_size > 0){
			// When tail size is 1~39 bytes, it's saved in File Packet.
			if (fread(buf_tail, 1, (size_t)tail_size, fp) != (size_t)tail_size){
				perror("Failed to read tail chunk on input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			memset(buf_tail + tail_size, 0, 40 - tail_size);	// zero fill the rest bytes
			if (par3_ctx->noise_level >= 3){
				printf("    block no  : slice no  chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64"\n",
						chunk_index, num, file_offset, tail_size);
			}

			// Print progress percent
			if ( (par3_ctx->noise_level >= 0) && (par3_ctx->noise_level <= 2) ){
				progress_step += tail_size;
				time_now = time(NULL);
				if (time_now != time_old){
					time_old = time_now;
					progress_now = (int)((progress_step * 1000) / progress_total);
					if (progress_now != progress_old){
						progress_old = progress_now;
						printf("%d.%d%%\r", progress_now / 10, progress_now % 10);	// 0.0% ~ 100.0%
					}
				}
			}

			// calculate CRC-64 of the first 16 KB
			if (file_offset + tail_size < 16384){
				file_p->crc = crc64(buf_tail, (size_t)tail_size, file_p->crc);
			} else if (file_offset < 16384){
				file_p->crc = crc64(buf_tail, (size_t)(16384 - file_offset), file_p->crc);
			}
			blake3_hasher_update(&hasher, buf_tail, (size_t)tail_size);

			// copy 1 ~ 39 bytes
			memcpy(&(chunk_p->tail_crc), buf_tail, 8);
			memcpy(chunk_p->tail_hash, buf_tail + 8, 16);
			memcpy(&(chunk_p->tail_block), buf_tail + 24, 8);
			memcpy(&(chunk_p->tail_offset), buf_tail + 32, 8);
		}

		blake3_hasher_finalize(&hasher, file_p->hash, 16);
		if (fclose(fp) != 0){
			perror("Failed to close input file");
			return RET_FILE_IO_ERROR;
		}

		file_p++;
		chunk_p++;	// Each input file contains single chunk description.
		chunk_index++;
	}

	// Release temporary buffer.
	free(work_buf);
	par3_ctx->work_buf = NULL;

	if (par3_ctx->noise_level >= 0){
		if (par3_ctx->noise_level <= 2){
			if (progress_step < progress_total)
				printf("Didn't finish progress. %"PRIu64" / %"PRIu64"\n", progress_step, progress_total);
		}
		clock_now = clock() - clock_now;
		printf("done in %.1f seconds.\n", (double)clock_now / CLOCKS_PER_SEC);
		printf("\n");
	}

	// Re-allocate memory for actual number of chunk description
	if (par3_ctx->noise_level >= 0){
		printf("Number of chunk description = %u (max %u)\n", chunk_index, input_file_count);
	}
	if (chunk_index < input_file_count){
		if (chunk_index > 0){
			chunk_p = realloc(par3_ctx->chunk_list, sizeof(PAR3_CHUNK_CTX) * chunk_index);
			if (chunk_p == NULL){
				perror("Failed to re-allocate memory for chunk description");
				return RET_MEMORY_ERROR;
			}
			par3_ctx->chunk_list = chunk_p;
		} else {
			free(par3_ctx->chunk_list);
			par3_ctx->chunk_list = NULL;
		}
	}
	par3_ctx->chunk_count = chunk_index;

	// Check actual number of slice info
	if (slice_index != block_count){
		printf("Number of input file slices = %"PRIu64" (max %"PRIu64")\n", slice_index, block_count);
		return RET_LOGIC_ERROR;
	}
	par3_ctx->slice_count = slice_index;

	// Update actual number of input blocks
	if (block_index < block_count){
		block_count = block_index;
		par3_ctx->block_count = block_count;

		// realloc
		block_p = realloc(par3_ctx->block_list, sizeof(PAR3_BLOCK_CTX) * block_count);
		if (block_p == NULL){
			perror("Failed to re-allocate memory for input blocks");
			return RET_MEMORY_ERROR;
		}
		par3_ctx->block_list = block_p;
	}
	if (par3_ctx->noise_level >= 0){
		printf("Actual block count = %"PRIu64", Tail packing = %u\n", block_count, num_pack);
	}

	return 0;
}


// map chunk tails, when there are no input blocks.
int map_chunk_tail(PAR3_CTX *par3_ctx)
{
	uint8_t buf_tail[40];
	uint32_t num;
	uint32_t input_file_count, chunk_index;
	uint64_t tail_size;
	PAR3_FILE_CTX *file_p;
	PAR3_CHUNK_CTX *chunk_p;
	FILE *fp;
	blake3_hasher hasher;

	// Copy variables from context to local.
	input_file_count = par3_ctx->input_file_count;
	if (par3_ctx->block_count != 0)
		return RET_LOGIC_ERROR;

	// When no deduplication, number of chunks may be same as number of input files.
	// Note, empty file won't use Chunk Description.
	chunk_p = malloc(sizeof(PAR3_CHUNK_CTX) * input_file_count);
	if (chunk_p == NULL){
		perror("Failed to allocate memory for chunk description");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->chunk_list = chunk_p;

	// Read data of input files on memory
	chunk_index = 0;
	file_p = par3_ctx->input_file_list;
	for (num = 0; num < input_file_count; num++){
		blake3_hasher_init(&hasher);
		if (file_p->size == 0){	// Skip empty files.
			blake3_hasher_finalize(&hasher, file_p->hash, 16);
			file_p++;
			continue;
		}
		if (par3_ctx->noise_level >= 2){
			printf("file size = %"PRIu64" \"%s\"\n", file_p->size, file_p->name);
		}

		fp = fopen(file_p->name, "rb");
		if (fp == NULL){
			perror("Failed to open input file");
			return RET_FILE_IO_ERROR;
		}

		// When no deduplication, chunk's index is same as file's index.
		file_p->chunk = chunk_index;	// single chunk in each file
		file_p->chunk_num = 1;
		chunk_p->size = file_p->size;	// file size = chunk size
		chunk_p->block = 0;

		tail_size = file_p->size;
		// When tail size is 1~39-bytes, it's saved in File Packet.
		if (fread(buf_tail, 1, (size_t)tail_size, fp) != (size_t)tail_size){
			perror("Failed to read tail chunk on input file");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
		memset(buf_tail + tail_size, 0, 40 - tail_size);	// zero fill the rest bytes

		// calculate CRC-64 of the first 16 KB
		if (tail_size < 16384){
			file_p->crc = crc64(buf_tail, (size_t)tail_size, file_p->crc);
		} else {
			file_p->crc = crc64(buf_tail, 16384, file_p->crc);
		}
		blake3_hasher_update(&hasher, buf_tail, (size_t)tail_size);

		// copy 1 ~ 39 bytes
		memcpy(&(chunk_p->tail_crc), buf_tail, 8);
		memcpy(chunk_p->tail_hash, buf_tail + 8, 16);
		memcpy(&(chunk_p->tail_block), buf_tail + 24, 8);
		memcpy(&(chunk_p->tail_offset), buf_tail + 32, 8);

		blake3_hasher_finalize(&hasher, file_p->hash, 16);
		if (fclose(fp) != 0){
			perror("Failed to close input file");
			return RET_FILE_IO_ERROR;
		}

		file_p++;
		chunk_p++;	// Each input file contains single chunk description.
		chunk_index++;
	}

	// Re-allocate memory for actual number of chunk description
	if (par3_ctx->noise_level >= 0){
		printf("Number of chunk description = %u (max %u)\n\n", chunk_index, input_file_count);
	}
	if (chunk_index < input_file_count){
		if (chunk_index > 0){
			chunk_p = realloc(par3_ctx->chunk_list, sizeof(PAR3_CHUNK_CTX) * chunk_index);
			if (chunk_p == NULL){
				perror("Failed to re-allocate memory for chunk description");
				return RET_MEMORY_ERROR;
			}
			par3_ctx->chunk_list = chunk_p;
		} else {
			free(par3_ctx->chunk_list);
			par3_ctx->chunk_list = NULL;
		}
	}
	par3_ctx->chunk_count = chunk_index;

	return 0;
}


// map input file slices into input blocks without reading file
int map_input_block_trial(PAR3_CTX *par3_ctx)
{
	uint32_t num, num_pack;
	uint32_t input_file_count, chunk_index;
	uint64_t block_size, tail_size, file_offset, tail_offset;
	uint64_t block_count, block_index, slice_index, index;
	PAR3_FILE_CTX *file_p;
	PAR3_CHUNK_CTX *chunk_p;
	PAR3_SLICE_CTX *slice_p, *slice_list;
	PAR3_BLOCK_CTX *block_p, *block_list;

	// Copy variables from context to local.
	input_file_count = par3_ctx->input_file_count;
	block_size = par3_ctx->block_size;
	block_count = par3_ctx->block_count;
	if ( (input_file_count == 0) || (block_size == 0) || (block_count == 0) )
		return RET_LOGIC_ERROR;

	// When no deduplication, number of chunks may be same as number of input files.
	// Note, empty file won't use Chunk Description.
	chunk_p = malloc(sizeof(PAR3_CHUNK_CTX) * input_file_count);
	if (chunk_p == NULL){
		perror("Failed to allocate memory for chunk description");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->chunk_list = chunk_p;

	// When no deduplication, number of input file slice is same as number of input blocks.
	slice_p = malloc(sizeof(PAR3_SLICE_CTX) * block_count);
	if (slice_p == NULL){
		perror("Failed to allocate memory for input file slices");
		return RET_MEMORY_ERROR;
	}
	slice_list = slice_p;
	par3_ctx->slice_list = slice_p;

	// When no deduplication, number of input blocks is calculable.
	block_p = malloc(sizeof(PAR3_BLOCK_CTX) * block_count);
	if (block_p == NULL){
		perror("Failed to allocate memory for input blocks");
		return RET_MEMORY_ERROR;
	}
	block_list = block_p;
	par3_ctx->block_list = block_p;

	// Read data of input files on memory
	num_pack = 0;
	chunk_index = 0;
	block_index = 0;
	slice_index = 0;
	file_p = par3_ctx->input_file_list;
	for (num = 0; num < input_file_count; num++){
		if (file_p->size == 0){	// Skip empty files.
			file_p++;
			continue;
		}
		if (par3_ctx->noise_level >= 2){
			printf("file size = %"PRIu64" \"%s\"\n", file_p->size, file_p->name);
		}

		// When no deduplication, chunk's index is same as file's index.
		file_p->chunk = chunk_index;	// single chunk in each file
		file_p->chunk_num = 1;
		chunk_p->size = file_p->size;	// file size = chunk size
		chunk_p->block = block_index;

		// Not calculate CRC-64 of the first 16 KB
		file_p->crc = 0;

		// Read full size blocks
		file_offset = 0;
		while (file_offset + block_size <= file_p->size){
			// set block info
			block_p->slice = slice_index;
			block_p->size = block_size;
			block_p->crc = 0;
			memset(block_p->hash, 0, 16);	// Not calculate hash
			block_p->state = 1 | 64 | 128;

			// set slice info
			slice_p->chunk = chunk_index;
			slice_p->file = num;
			slice_p->offset = file_offset;
			slice_p->size = block_size;
			slice_p->block = block_index;
			slice_p->tail_offset = 0;
			slice_p->next = -1;
			if (par3_ctx->noise_level >= 3){
				printf("new block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64"\n",
						block_index, slice_index, chunk_index, num, file_offset);
			}
			slice_p++;
			slice_index++;

			file_offset += block_size;
			block_p++;
			block_index++;
		}

		// Calculate size of chunk tail, and read it.
		tail_size = file_p->size - file_offset;
		//printf("tail_size = %"PRIu64", file size = %"PRIu64", offset %"PRIu64"\n", tail_size, file_p->size, file_offset);
		if (tail_size >= 40){
			// Not calculate checksum of chunk tail
			chunk_p->tail_crc = 0;
			memset(chunk_p->tail_hash, 0, 16);

			// search existing tails to check available space
			tail_offset = 0;
			for (index = 0; index < slice_index; index++){
				if ( (slice_list[index].next == -1) && (slice_list[index].size < block_size) ){	// the last tail in the block
					if (slice_list[index].tail_offset + slice_list[index].size + tail_size <= block_size){
						// When tail can fit in the space, put the tail there.
						tail_offset = slice_list[index].tail_offset + slice_list[index].size;
						break;
					}
				}
			}
			//printf("tail_offset = %"PRId64"\n", tail_offset);

			if (tail_offset == 0){	// Put tail in new block
				if (par3_ctx->noise_level >= 3){
					printf("n t block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64"\n",
							block_index, slice_index, chunk_index, num, file_offset, tail_size);
				}

				// set slice info
				slice_p->block = block_index;
				slice_p->tail_offset = 0;

				// set chunk tail info
				chunk_p->tail_block = block_index;
				chunk_p->tail_offset = 0;

				// set block info (block for tails don't store checksum)
				block_p->slice = slice_index;
				block_p->size = tail_size;
				block_p->state = 2 | 64;
				block_p++;
				block_index++;

			} else {	// Put tail after another tail
				if (par3_ctx->noise_level >= 3){
					printf("a t block[%2"PRIu64"] : slice[%2"PRIu64"] chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64", offset %"PRId64"\n",
							slice_list[index].block, slice_index, chunk_index, num, file_offset, tail_size, tail_offset);
				}
				slice_list[index].next = slice_index;	// update "next" item in the front tail

				// set slice info
				slice_p->block = slice_list[index].block;
				slice_p->tail_offset = tail_offset;

				// set chunk tail info
				chunk_p->tail_block = slice_list[index].block;
				chunk_p->tail_offset = tail_offset;
				num_pack++;

				// update block info
				block_list[slice_p->block].size = tail_offset + tail_size;
			}

			// set common slice info
			slice_p->file = num;
			slice_p->offset = file_offset;
			slice_p->size = tail_size;
			slice_p->chunk = chunk_index;
			slice_p->next = -1;
			slice_p++;
			slice_index++;

		} else if (tail_size > 0){
			if (par3_ctx->noise_level >= 3){
				printf("    block no  : slice no  chunk[%2u] file %d, offset %"PRIu64", tail size %"PRIu64"\n",
						chunk_index, num, file_offset, tail_size);
			}

			// Reset
			chunk_p->tail_crc = 0;
			memset(chunk_p->tail_hash, 0, 16);
			chunk_p->tail_block = 0;
			chunk_p->tail_offset = 0;
		}

		// Not calculate file hash
		memset(file_p->hash, 0, 16);

		file_p++;
		chunk_p++;	// Each input file contains single chunk description.
		chunk_index++;
	}

	// Re-allocate memory for actual number of chunk description
	if (par3_ctx->noise_level >= 0){
		printf("Number of chunk description = %u (max %u)\n", chunk_index, input_file_count);
	}
	if (chunk_index < input_file_count){
		if (chunk_index > 0){
			chunk_p = realloc(par3_ctx->chunk_list, sizeof(PAR3_CHUNK_CTX) * chunk_index);
			if (chunk_p == NULL){
				perror("Failed to re-allocate memory for chunk description");
				return RET_MEMORY_ERROR;
			}
			par3_ctx->chunk_list = chunk_p;
		} else {
			free(par3_ctx->chunk_list);
			par3_ctx->chunk_list = NULL;
		}
	}
	par3_ctx->chunk_count = chunk_index;

	// Check actual number of slice info
	if (slice_index != block_count){
		printf("Number of input file slices = %"PRIu64" (max %"PRIu64")\n", slice_index, block_count);
		return RET_LOGIC_ERROR;
	}
	par3_ctx->slice_count = slice_index;

	// Update actual number of input blocks
	if (block_index < block_count){
		block_count = block_index;
		par3_ctx->block_count = block_count;

		// realloc
		block_p = realloc(par3_ctx->block_list, sizeof(PAR3_BLOCK_CTX) * block_count);
		if (block_p == NULL){
			perror("Failed to re-allocate memory for input blocks");
			return RET_MEMORY_ERROR;
		}
		par3_ctx->block_list = block_p;
	}
	if (par3_ctx->noise_level >= 0){
		printf("Actual block count = %"PRIu64", Tail packing = %u\n", block_count, num_pack);
	}

	return 0;
}

