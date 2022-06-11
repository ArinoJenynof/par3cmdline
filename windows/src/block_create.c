// avoid error of MSVC
#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libpar3.h"
#include "galois.h"
#include "hash.h"
#include "reedsolomon.h"


// Try to allocate memory for all recovery blocks.
int allocate_recovery_block(PAR3_CTX *par3_ctx)
{
	size_t alloc_size, region_size;

	// Allocate tables before blocks.
	if (par3_ctx->galois_poly == 0x1100B){	// 16-bit Galois Field (0x1100B).
		par3_ctx->galois_table = gf16_create_table(par3_ctx->galois_poly);

	} else if (par3_ctx->galois_poly == 0x11D){	// 8-bit Galois Field (0x11D).
		par3_ctx->galois_table = gf8_create_table(par3_ctx->galois_poly);

	} else {
		printf("Galois Field (0x%X) isn't supported.\n", par3_ctx->galois_poly);
		return RET_LOGIC_ERROR;
	}
	if (par3_ctx->galois_table == NULL){
		printf("Failed to create tables for Galois Field (0x%X)\n", par3_ctx->galois_poly);
		return RET_MEMORY_ERROR;
	}

	// Only when it uses Reed-Solomon Erasure Codes.
	if ((par3_ctx->ecc_method & 1) == 0)
		return 0;

	// Set memory alignment of block data to be 4.
	// Increase at least 1 byte as checksum.
	region_size = (par3_ctx->block_size + 4 + 3) & ~3;
	if (par3_ctx->noise_level >= 2){
		printf("Aligned size of block data = %zu\n", region_size);
	}

	// Limited memory usage
	alloc_size = region_size * par3_ctx->recovery_block_count;
	if ( (par3_ctx->memory_limit > 0) && (alloc_size > par3_ctx->memory_limit) )
		return 0;

	par3_ctx->block_data = malloc(alloc_size);
	if (par3_ctx->block_data == NULL){
		// When it cannot allocate memory, it will retry later.
		par3_ctx->ecc_method &= ~0x1000;
	} else {
		par3_ctx->ecc_method |= 0x1000;	// Keep all recovery blocks on memory
	}

	return 0;
}

// This supports Reed-Solomon Erasure Codes on 8-bit or 16-bit Galois Field.
// GF tables and recovery blocks were allocated already.
int create_recovery_block(PAR3_CTX *par3_ctx)
{
	uint8_t *work_buf;
	uint8_t gf_size;
	int galois_poly;
	int block_count, x_index;
	int file_index, file_prev;
	int progress_old, progress_now;
	size_t block_size, region_size;
	size_t data_size, read_size;
	size_t tail_offset, tail_gap;
	int64_t slice_index, file_offset;
	PAR3_FILE_CTX *file_list;
	PAR3_SLICE_CTX *slice_list;
	PAR3_BLOCK_CTX *block_list;
	FILE *fp;
	time_t time_old, time_now;
	clock_t clock_now;

	if (par3_ctx->recovery_block_count == 0)
		return -1;

	// GF tables and recovery blocks must be stored on memory.
	if ( (par3_ctx->galois_table == NULL) || (par3_ctx->block_data == NULL) )
		return -1;

	// Only when it uses Reed-Solomon Erasure Codes.
	if ((par3_ctx->ecc_method & 1) == 0)
		return -1;

	block_size = par3_ctx->block_size;
	block_count = (int)(par3_ctx->block_count);
	gf_size = par3_ctx->gf_size;
	galois_poly = par3_ctx->galois_poly;
	file_list = par3_ctx->input_file_list;
	slice_list = par3_ctx->slice_list;
	block_list = par3_ctx->block_list;

	// Allocate memory to read one input block and parity.
	region_size = (block_size + 4 + 3) & ~3;
	work_buf = malloc(region_size);
	if (work_buf == NULL){
		perror("Failed to allocate memory for input data");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->work_buf = work_buf;

	if (par3_ctx->noise_level >= 0){
		printf("\nComputing recovery blocks:\n");
		progress_old = 0;
		time_old = time(NULL);
		clock_now = clock();
	}

	// Reed-Solomon Erasure Codes
	file_prev = -1;
	fp = NULL;
	for (x_index = 0; x_index < block_count; x_index++){
		// Read each input block from input files.
		data_size = block_list[x_index].size;
		if (block_list[x_index].state & 1){	// including full size data
			slice_index = block_list[x_index].slice;
			while (slice_index != -1){
				if (slice_list[slice_index].size == block_size)
					break;
				slice_index = slice_list[slice_index].next;
			}
			if (slice_index == -1){	// When there is no valid slice.
				printf("Mapping information for block[%d] is wrong.\n", x_index);
				if (fp != NULL)
					fclose(fp);
				return RET_LOGIC_ERROR;
			}

			// Read one slice from a file.
			file_index = slice_list[slice_index].file;
			file_offset = slice_list[slice_index].offset;
			read_size = data_size;
			if (par3_ctx->noise_level >= 2){
				printf("Reading %zu bytes of slice[%I64d] for input block[%d]\n", read_size, slice_index, x_index);
			}
			if ( (fp == NULL) || (file_index != file_prev) ){
				if (fp != NULL){	// Close previous input file.
					fclose(fp);
					fp = NULL;
				}
				fp = fopen(file_list[file_index].name, "rb");
				if (fp == NULL){
					perror("Failed to open input file");
					return RET_FILE_IO_ERROR;
				}
				file_prev = file_index;
			}
			if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
				perror("Failed to seek input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			if (fread(work_buf, 1, read_size, fp) != read_size){
				perror("Failed to read slice on input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}

		} else {	// tail data only (one tail or packed tails)
			if (par3_ctx->noise_level >= 2){
				printf("Reading %I64u bytes for input block[%d]\n", data_size, x_index);
			}
			tail_offset = 0;
			while (tail_offset < data_size){	// Read tails until data end.
				slice_index = block_list[x_index].slice;
				while (slice_index != -1){
					//printf("block = %I64u, size = %zu, offset = %zu, slice = %I64d\n", x_index, data_size, tail_offset, slice_index);
					// Even when chunk tails are overlaped, it will find tail slice of next position.
					if ( (slice_list[slice_index].tail_offset + slice_list[slice_index].size > tail_offset) &&
							(slice_list[slice_index].tail_offset <= tail_offset) ){
						break;
					}
					slice_index = slice_list[slice_index].next;
				}
				if (slice_index == -1){	// When there is no valid slice.
					printf("Mapping information for block[%d] is wrong.\n", x_index);
					if (fp != NULL)
						fclose(fp);
					return RET_LOGIC_ERROR;
				}

				// Read one slice from a file.
				tail_gap = tail_offset - slice_list[slice_index].tail_offset;	// This tail slice may start before tail_offset.
				//printf("tail_gap for slice[%I64d] = %zu.\n", slice_index, tail_gap);
				file_index = slice_list[slice_index].file;
				file_offset = slice_list[slice_index].offset + tail_gap;
				read_size = slice_list[slice_index].size - tail_gap;
				if ( (fp == NULL) || (file_index != file_prev) ){
					if (fp != NULL){	// Close previous input file.
						fclose(fp);
						fp = NULL;
					}
					fp = fopen(file_list[file_index].name, "rb");
					if (fp == NULL){
						perror("Failed to open input file");
						return RET_FILE_IO_ERROR;
					}
					file_prev = file_index;
				}
				if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
					perror("Failed to seek input file");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}
				if (fread(work_buf + tail_offset, 1, read_size, fp) != read_size){
					perror("Failed to read tail slice on input file");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}
				tail_offset += read_size;
			}

			// Zero fill rest bytes
			if (data_size < block_size)
				memset(work_buf + data_size, 0, block_size - data_size);
		}

		// Calculate checksum of block to confirm that input file was not changed.
		if (crc64(work_buf, data_size, 0) != block_list[x_index].crc){
			printf("Checksum of block[%d] is different.\n", x_index);
			fclose(fp);
			return RET_LOGIC_ERROR;
		}

		// Calculate parity bytes in the region
		if (gf_size == 2){
			gf16_region_create_parity(galois_poly, work_buf, region_size, block_size);
		} else if (gf_size == 1){
			gf8_region_create_parity(galois_poly, work_buf, region_size, block_size);
		} else {
			region_create_parity(work_buf, region_size, block_size);
		}

		// Multipy one input block for all recovery blocks.
		rs_create_one_all(par3_ctx, x_index);

		// Print progress percent
		if ( (par3_ctx->noise_level >= 0) && (par3_ctx->noise_level <= 1) ){
			time_now = time(NULL);
			if (time_now != time_old){
				time_old = time_now;
				// Because block_count is 16-bit value, "int" (32-bit signed integer) is enough.
				progress_now = (x_index * 1000) / block_count;
				if (progress_now != progress_old){
					progress_old = progress_now;
					printf("%d.%d%%\r", progress_now / 10, progress_now % 10);	// 0.0% ~ 100.0%
				}
			}
		}
	}
	if (fp != NULL){
		if (fclose(fp) != 0){
			perror("Failed to close input file");
			return RET_FILE_IO_ERROR;
		}
	}

	free(work_buf);
	par3_ctx->work_buf = NULL;

	if (par3_ctx->noise_level >= 0){
		clock_now = clock() - clock_now;
		printf("done in %.1f seconds.\n", (double)clock_now / CLOCKS_PER_SEC);
		printf("\n");
	}

	return 0;
}

// This keeps all input blocks and recovery blocks partially by spliting every block.
// GF tables and recovery blocks were allocated already.
int write_recovery_block(PAR3_CTX *par3_ctx)
{
	uint8_t *work_buf, *buf_p;
	uint8_t gf_size;
	int ret, galois_poly;
	int file_index, file_prev;
	uint32_t split_count;
	size_t read_size;
	int64_t slice_index, file_offset;
	uint64_t block_size, block_count, recovery_block_count;
	uint64_t alloc_size, region_size, split_size;
	uint64_t data_size, part_size, split_offset;
	uint64_t tail_offset, tail_gap;
	uint64_t block_crc, index;
	PAR3_FILE_CTX *file_list;
	PAR3_SLICE_CTX *slice_list;
	PAR3_BLOCK_CTX *block_list;
	FILE *fp;

	block_size = par3_ctx->block_size;
	block_count = par3_ctx->block_count;
	recovery_block_count = par3_ctx->recovery_block_count;
	gf_size = par3_ctx->gf_size;
	galois_poly = par3_ctx->galois_poly;
	file_list = par3_ctx->input_file_list;
	slice_list = par3_ctx->slice_list;
	block_list = par3_ctx->block_list;

	if (recovery_block_count == 0)
		return -1;

printf("\n This function isn't made yet.\n");
return -1;

	// Set required memory size at first
	region_size = (block_size + 4 + 3) & ~3;
	alloc_size = region_size * (block_count + recovery_block_count);

// for test split
par3_ctx->memory_limit = (alloc_size + 1) / 2;

	// Limited memory usage
	if ( (par3_ctx->memory_limit > 0) && (alloc_size > par3_ctx->memory_limit) ){
		split_count = (uint32_t)((alloc_size + par3_ctx->memory_limit - 1) / par3_ctx->memory_limit);
		split_size = (block_size + split_count - 1) / split_count;	// This is splitted block size to fit in limited memory.
		split_size = (split_size + 1) & ~1;	// aligned to 2 bytes for 8 or 16-bit Galois Field
	} else {
		split_count = 1;
		split_size = block_size;
	}
	printf("split_count = %u, split_size = %I64u\n", split_count, split_size);

	// Allocate memory to keep all splitted blocks.
	region_size = (split_size + 4 + 3) & ~3;
	alloc_size = region_size * (block_count + recovery_block_count);
	printf("region_size = %I64u, alloc_size = %I64u\n", region_size, alloc_size);
	work_buf = malloc(alloc_size);
	if (work_buf == NULL){
		perror("Failed to allocate memory for block data");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->work_buf = work_buf;

	if (par3_ctx->noise_level >= 0){
		printf("\nComputing recovery blocks:\n");
	}

	// This file access style would support all Error Correction Codes.
	for (split_offset = 0; split_offset < block_size; split_offset += split_size){
		buf_p = work_buf;	// Starting position of input blocks
		file_prev = -1;
		fp = NULL;

		// Read all input blocks on memory
		for (index = 0; index < block_count; index++){
			// Read each input block from input files.
			data_size = block_list[index].size;
			if (block_list[index].state & 1){	// including full size data
				slice_index = block_list[index].slice;
				while (slice_index != -1){
					if (slice_list[slice_index].size == block_size)
						break;
					slice_index = slice_list[slice_index].next;
				}
				if (slice_index == -1){	// When there is no valid slice.
					printf("Mapping information for block[%I64u] is wrong.\n", index);
					if (fp != NULL)
						fclose(fp);
					return RET_LOGIC_ERROR;
				}

				// Read a part of slice from a file.
				part_size = data_size - split_offset;
				if (part_size > split_size)
					part_size = split_size;
				file_index = slice_list[slice_index].file;
				file_offset = slice_list[slice_index].offset + split_offset;
				read_size = part_size;
				if (par3_ctx->noise_level >= 2){
					printf("Reading %zu bytes of slice[%I64d] for input block[%I64u]\n", read_size, slice_index, index);
				}
				if ( (fp == NULL) || (file_index != file_prev) ){
					if (fp != NULL){	// Close previous input file.
						fclose(fp);
						fp = NULL;
					}
					fp = fopen(file_list[file_index].name, "rb");
					if (fp == NULL){
						perror("Failed to open input file");
						return RET_FILE_IO_ERROR;
					}
					file_prev = file_index;
				}
				if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
					perror("Failed to seek input file");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}
				if (fread(buf_p, 1, read_size, fp) != read_size){
					perror("Failed to read slice on input file");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}

			} else if (data_size > split_offset){	// tail data only (one tail or packed tails)
				part_size = data_size - split_offset;
				if (part_size > split_size)
					part_size = split_size;
				if (par3_ctx->noise_level >= 2){
					printf("Reading %I64u bytes for input block[%I64u]\n", part_size, index);
				}
				tail_offset = split_offset;

				while (tail_offset < split_offset + part_size){	// Read tails until data end.
					slice_index = block_list[index].slice;
					while (slice_index != -1){
						//printf("block = %I64u, size = %zu, offset = %zu, slice = %I64d\n", index, data_size, tail_offset, slice_index);
						// Even when chunk tails are overlaped, it will find tail slice of next position.
						if ( (slice_list[slice_index].tail_offset + slice_list[slice_index].size > tail_offset) &&
								(slice_list[slice_index].tail_offset <= tail_offset) ){
							break;
						}
						slice_index = slice_list[slice_index].next;
					}
					if (slice_index == -1){	// When there is no valid slice.
						printf("Mapping information for block[%I64u] is wrong.\n", index);
						if (fp != NULL)
							fclose(fp);
						return RET_LOGIC_ERROR;
					}

					// Read one slice from a file.
					tail_gap = tail_offset - slice_list[slice_index].tail_offset;	// This tail slice may start before tail_offset.
					//printf("tail_gap for slice[%I64d] = %zu.\n", slice_index, tail_gap);
					file_index = slice_list[slice_index].file;
					file_offset = slice_list[slice_index].offset + tail_gap;
					read_size = slice_list[slice_index].size - tail_gap;
					if ( (fp == NULL) || (file_index != file_prev) ){
						if (fp != NULL){	// Close previous input file.
							fclose(fp);
							fp = NULL;
						}
						fp = fopen(file_list[file_index].name, "rb");
						if (fp == NULL){
							perror("Failed to open input file");
							return RET_FILE_IO_ERROR;
						}
						file_prev = file_index;
					}
					if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
						perror("Failed to seek input file");
						fclose(fp);
						return RET_FILE_IO_ERROR;
					}
					if (fread(buf_p + tail_offset - split_offset, 1, read_size, fp) != read_size){
						perror("Failed to read tail slice on input file");
						fclose(fp);
						return RET_FILE_IO_ERROR;
					}
					tail_offset += read_size;
				}
			}

			// Calculate checksum of block to confirm that input file was not changed.
			if (split_offset == 0){
				block_crc = 0;
			} else {
				memcpy(&block_crc, block_list[index].hash, 8);	// Use previous CRC value
			}
			if (data_size > split_offset){	// When there is slice data to process.
				block_crc = crc64(buf_p, part_size, block_crc);

				// Calculate parity bytes in the region
				if (gf_size == 2){
					gf16_region_create_parity(galois_poly, buf_p, region_size, part_size);
				} else if (gf_size == 1){
					gf8_region_create_parity(galois_poly, buf_p, region_size, part_size);
				} else {
					region_create_parity(buf_p, region_size, part_size);
				}
			}
			if (split_offset + split_size >= block_size){	// At the last
				if (block_crc != block_list[index].crc){
					printf("Checksum of block[%I64u] is different.\n", index);
					fclose(fp);
					return RET_LOGIC_ERROR;
				}
			} else {
				memcpy(block_list[index].hash, &block_crc, 8);	// Save this CRC value
			}

			buf_p += region_size;	// Goto next partial block
		}
		if (fp != NULL){
			if (fclose(fp) != 0){
				perror("Failed to close input file");
				return RET_FILE_IO_ERROR;
			}
			fp = NULL;
		}

		// Create all recovery blocks on memory
		if (par3_ctx->ecc_method & 1){	// Reed-Solomon Erasure Codes
			rs_create_all(par3_ctx, region_size);
		}

		// Write all recovery blocks on recovery files
		part_size = data_size - split_offset;
		if (part_size > split_size)
			part_size = split_size;
		buf_p = work_buf + region_size * block_count;	// Starting position of recovery blocks
		file_prev = -1;
		fp = NULL;
		for (index = 0; index < recovery_block_count; index++){

			// Check parity of recovery block to confirm that calculation was correct.
			if (gf_size == 2){
				ret = gf16_region_check_parity(galois_poly, buf_p, region_size, part_size);
			} else if (gf_size == 1){
				ret = gf8_region_check_parity(galois_poly, buf_p, region_size, part_size);
			} else {
				ret = region_check_parity(buf_p, region_size, part_size);
			}
			if (ret != 0){
				printf("Parity of recovery block[%I64u] is different.\n", index);
				if (fp != NULL)
					fclose(fp);
				return RET_LOGIC_ERROR;
			}




			buf_p += region_size;
		}
	}



printf("\n This function isn't made yet.\n");
return -1;

	return 0;
}

