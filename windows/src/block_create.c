// avoid error of MSVC
#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		printf("16-bit Galois Field isn't implemented yet.\n");
		return RET_LOGIC_ERROR;

	} else if (par3_ctx->galois_poly == 0x11D){	// 8-bit Galois Field (0x11D).
		par3_ctx->galois_table = gf8_create_table(par3_ctx->galois_poly);
	}
	if (par3_ctx->galois_table == NULL){
		printf("Failed to create tables for Galois Field (0x%X)\n", par3_ctx->galois_poly);
		return RET_MEMORY_ERROR;
	}

	// Only when it uses Reed-Solomon Erasure Codes.
	if ((par3_ctx->ecc_method & 1) == 0)
		return 0;

	// Set memory alignment of block data to be 8 for 64-bit OS.
	// Increase at least 1 byte as checksum.
	region_size = (par3_ctx->block_size + 1 + 7) & ~8;
	if (par3_ctx->noise_level >= 2){
		printf("Aligned size of block data = %zu\n", region_size);
	}
	alloc_size = region_size * par3_ctx->recovery_block_count;

	// Limited memory usage
	if ( (par3_ctx->memory_limit > 0) && (alloc_size > par3_ctx->memory_limit) )
		return 0;

	par3_ctx->recovery_data = malloc(alloc_size);
	if (par3_ctx->recovery_data == NULL){
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
	int block_count, x_index;
	int file_index, file_read;
	size_t block_size, region_size;
	size_t data_size, read_size, tail_offset;
	int64_t slice_index, file_offset;
	PAR3_FILE_CTX *file_list;
	PAR3_SLICE_CTX *slice_list;
	PAR3_BLOCK_CTX *block_list;
	FILE *fp;

	if (par3_ctx->recovery_block_count == 0)
		return -1;

	// GF tables and recovery blocks must be stored on memory.
	if ( (par3_ctx->galois_table == NULL) || (par3_ctx->recovery_data == NULL) )
		return -1;

	// Only when it uses Reed-Solomon Erasure Codes.
	if ((par3_ctx->ecc_method & 1) == 0)
		return -1;

	block_size = par3_ctx->block_size;
	block_count = (int)(par3_ctx->block_count);
	file_list = par3_ctx->input_file_list;
	slice_list = par3_ctx->slice_list;
	block_list = par3_ctx->block_list;

	// Allocate memory to read one input block and parity.
	region_size = (block_size + 1 + 7) & ~8;
	work_buf = malloc(region_size);
	if (work_buf == NULL){
		perror("Failed to allocate memory for input data");
		return RET_MEMORY_ERROR;
	}
	par3_ctx->work_buf = work_buf;

	// Reed-Solomon Erasure Codes
	file_read = -1;
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
			read_size = slice_list[slice_index].size;
			if ( (fp == NULL) || (file_index != file_read) ){
				if (fp != NULL){	// Close previous input file.
					fclose(fp);
					fp = NULL;
				}
				fp = fopen(file_list[file_index].name, "rb");
				if (fp == NULL){
					perror("Failed to open input file");
					return RET_FILE_IO_ERROR;
				}
				file_read = file_index;
			}
			if (file_offset > 0){
				if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
					perror("Failed to seek input file");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}
			}
			if (fread(work_buf, 1, read_size, fp) != read_size){
				perror("Failed to read full slice on input file");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}

		} else {	// tail data only (one tail or packed tails)
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
				file_index = slice_list[slice_index].file;
				file_offset = slice_list[slice_index].offset;
				read_size = slice_list[slice_index].size;
				if ( (fp == NULL) || (file_index != file_read) ){
					if (fp != NULL){	// Close previous input file.
						fclose(fp);
						fp = NULL;
					}
					fp = fopen(file_list[file_index].name, "rb");
					if (fp == NULL){
						perror("Failed to open input file");
						return RET_FILE_IO_ERROR;
					}
					file_read = file_index;
				}
				if (file_offset > 0){
					if (_fseeki64(fp, file_offset, SEEK_SET) != 0){
						perror("Failed to seek input file");
						fclose(fp);
						return RET_FILE_IO_ERROR;
					}
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
		region_create_parity(work_buf, region_size, block_size);

		// Multipy one input block for all recovery blocks.
		rs_create_one_all(par3_ctx, x_index);
	}
	if (fp != NULL){
		if (fclose(fp) != 0){
			perror("Failed to close input file");
			return RET_FILE_IO_ERROR;
		}
	}

	free(par3_ctx->work_buf);
	par3_ctx->work_buf = NULL;

/*
{	// for debug
	int x, y, z, w;
	unsigned char buf1[256], buf2[256], buf3[256];

	for (y = 1; y < 256; y++){
		// z = 1 / y
		z = gf8_divide(par3_ctx->galois_table, 1, y);
		w = gf8_reciprocal(par3_ctx->galois_table, y);
		if (z != w){
			printf("Error: y = %d, 1/y = %d, %d\n", y, z, w);
			break;
		}

		for (x = 0; x < 256; x++){
			// z = x * y
			z = gf8_multiply(par3_ctx->galois_table, x, y);
			w = gf8_fast_multiply(par3_ctx->galois_table, x, y);
			if (z != w){
				printf("Error: x = %d, y = %d, x*y = %d, %d\n", x, y, z, w);
				break;
			} else {
				w = gf8_divide(par3_ctx->galois_table, z, y);
				if (w != x){
					printf("Error: x = %d, y = %d, x*y = %d, x*y/y = %d\n", x, y, z, w);
					x = y = 256;
					break;
				}
				if (x > 0){
					w = gf8_divide(par3_ctx->galois_table, z, x);
					if (w != y){
						printf("Error: x = %d, y = %d, x*y = %d, x*y/x = %d\n", x, y, z, w);
						x = y = 256;
						break;
					}
				}
			}
		}
	}

	for (x = 0; x < 256; x++){
		buf1[x] = x;
		buf2[x] = 255;
		buf3[x] = gf8_multiply(par3_ctx->galois_table, x, 255) ^ 255;
	}
	gf8_region_multiply(par3_ctx->galois_table, buf1, 255, 256, buf2, 1);
	if (memcmp(buf2, buf3, 256) != 0){
		printf("Error: gf8_region_multiply\n");
	}

	printf("\n GF8 test end !\n");
}
*/

	return 0;
}

