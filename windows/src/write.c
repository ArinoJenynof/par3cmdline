
// avoid error of MSVC
#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blake3/blake3.h"
#include "libpar3.h"
#include "common.h"
#include "hash.h"
#include "packet.h"
#include "write.h"


// Write Index File
int write_index_file(PAR3_CTX *par3_ctx)
{
	size_t write_size;
	FILE *fp;

	fp = fopen(par3_ctx->par_filename, "wb");
	if (fp == NULL){
		perror("Failed to open Index File");
		return RET_FILE_IO_ERROR;
	}

	// Creator Packet
	write_size = par3_ctx->creator_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->creator_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Creator Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// Start Packet
	write_size = par3_ctx->start_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->start_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Start Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// Matrix Packet
	write_size = par3_ctx->matrix_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->matrix_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Matrix Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// File Packet
	write_size = par3_ctx->file_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->file_packet, 1, write_size, fp) != write_size){
			perror("Failed to write File Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// Directory Packet
	write_size = par3_ctx->dir_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->dir_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Directory Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// Root Packet
	write_size = par3_ctx->root_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->root_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Root Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// External Data Packet
	write_size = par3_ctx->ext_data_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->ext_data_packet, 1, write_size, fp) != write_size){
			perror("Failed to write External Data Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// Comment Packet
	write_size = par3_ctx->comment_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->comment_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Comment Packet on Index File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	if (fclose(fp) != 0){
		perror("Failed to close Index File");
		return RET_FILE_IO_ERROR;
	}

	if (par3_ctx->noise_level >= -1)
		printf("Wrote index file, %s\n", offset_file_name(par3_ctx->par_filename));

	return 0;
}

/*
Redundancy of critical packets;
number of blocks = 0 ~ 1 : number of copies = 1
number of blocks = 2 ~ 3 : number of copies = 2
number of blocks = 4 ~ 7 : number of copies = 3
number of blocks = 8 ~ 15 : number of copies = 4
number of blocks = 16 ~ 31 : number of copies = 5
number of blocks = 32 ~ 63 : number of copies = 6
number of blocks = 64 ~ 127 : number of copies = 7
number of blocks = 128 ~ 255 : number of copies = 8
number of blocks = 256 ~ 511 : number of copies = 9
number of blocks = 512 ~ 1023 : number of copies = 10
number of blocks = 1024 ~ 2047 : number of copies = 11
number of blocks = 2048 ~ 4095 : number of copies = 12
number of blocks = 4096 ~ 8191 : number of copies = 13
number of blocks = 8192 ~ 16383 : number of copies = 14
number of blocks = 16384 ~ 32767 : number of copies = 15
number of blocks = 32768 ~ 65535 : number of copies = 16
*/
static int write_data_packet(PAR3_CTX *par3_ctx, char *filename, uint64_t each_start, uint64_t each_count)
{
	uint8_t *work_buf, *common_packet, packet_header[56];
	uint32_t file_index, file_read;
	int64_t slice_index;
	uint64_t num, file_offset;
	size_t block_size, read_size, tail_offset;
	size_t write_size, write_size2;
	size_t packet_count, packet_to, packet_from;
	size_t common_packet_size, packet_size, packet_offset;
	PAR3_FILE_CTX *file_list;
	PAR3_SLICE_CTX *slice_list;
	PAR3_BLOCK_CTX *block_list;
	FILE *fp_write, *fp_read;
	blake3_hasher hasher;

	block_size = par3_ctx->block_size;
	work_buf = par3_ctx->work_buf;
	file_list = par3_ctx->input_file_list;
	slice_list = par3_ctx->slice_list;
	block_list = par3_ctx->block_list;
	common_packet = par3_ctx->common_packet;
	common_packet_size = par3_ctx->common_packet_size;

	// How many repetition of common packet.
	packet_count = 0;	// reduce 1, because put 1st copy at first.
	for (num = 2; num <= each_count; num *= 2)	// log2(each_count)
		packet_count++;
	//printf("each_count = %I64u, repetition = %zu\n", each_count, packet_count);
	packet_count *= par3_ctx->common_packet_count;
	//printf("number of repeated packets = %zu\n", packet_count);

	fp_write = fopen(filename, "wb");
	if (fp_write == NULL){
		perror("Failed to open Archive File");
		return RET_FILE_IO_ERROR;
	}

	// Creator Packet
	write_size = par3_ctx->creator_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->creator_packet, 1, write_size, fp_write) != write_size){
			perror("Failed to write Creator Packet on Archive File");
			fclose(fp_write);
			return RET_FILE_IO_ERROR;
		}
	}

	// First common packets
	write_size = common_packet_size;
	if (fwrite(common_packet, 1, write_size, fp_write) != write_size){
		perror("Failed to write first common packets on Archive File");
		fclose(fp_write);
		return RET_FILE_IO_ERROR;
	}

	// Data Packet and repeated common packets
	file_read = 0xFFFFFFFF;
	fp_read = NULL;
	packet_from = 0;
	packet_offset = 0;
	for (num = each_start; num < each_start + each_count; num++){
		// data size in the block
		write_size = block_list[num].size;

		// packet header
		make_packet_header(packet_header, 56 + write_size, par3_ctx->set_id, "PAR DAT\0", 0);

		// The index of the input block
		memcpy(packet_header + 48, &num, 8);

		// Read block data from file.
		if (block_list[num].state & 1){	// including full size data
			slice_index = block_list[num].slice;
			while (slice_index != -1){
				if (slice_list[slice_index].size == block_size)
					break;
				slice_index = slice_list[slice_index].next;
			}
			if (slice_index == -1){	// When there is no valid slice.
				printf("Mapping information for block[%I64u] is wrong.\n", num);
				fclose(fp_write);
				if (fp_read != NULL)
					fclose(fp_read);
				return RET_LOGIC_ERROR;
			}

			// Read one slice from a file.
			file_index = slice_list[slice_index].file;
			file_offset = slice_list[slice_index].offset;
			read_size = slice_list[slice_index].size;
			if ( (fp_read == NULL) || (file_index != file_read) ){
				if (fp_read != NULL){	// Close previous input file.
					fclose(fp_read);
					fp_read = NULL;
				}
				fp_read = fopen(file_list[file_index].name, "rb");
				if (fp_read == NULL){
					perror("Failed to open input file");
					fclose(fp_write);
					return RET_FILE_IO_ERROR;
				}
				file_read = file_index;
			}
			if (file_offset > 0){
				if (_fseeki64(fp_read, file_offset, SEEK_SET) != 0){
					perror("Failed to seek input file");
					fclose(fp_read);
					fclose(fp_write);
					return RET_FILE_IO_ERROR;
				}
			}
			if (fread(work_buf, 1, read_size, fp_read) != read_size){
				perror("Failed to read full slice on input file");
				fclose(fp_read);
				fclose(fp_write);
				return RET_FILE_IO_ERROR;
			}

		} else {	// tail data only (one tail or packed tails)
			tail_offset = 0;
			while (tail_offset < write_size){	// Read tails until data end.
				slice_index = block_list[num].slice;
				while (slice_index != -1){
					//printf("block = %I64u, size = %zu, offset = %zu, slice = %I64d\n", num, write_size, tail_offset, slice_index);
					// Even when chunk tails are overlaped, it will find tail slice of next position.
					if ( (slice_list[slice_index].tail_offset + slice_list[slice_index].size > tail_offset) &&
							(slice_list[slice_index].tail_offset <= tail_offset) ){
						break;
					}
					slice_index = slice_list[slice_index].next;
				}
				if (slice_index == -1){	// When there is no valid slice.
					printf("Mapping information for block[%I64u] is wrong.\n", num);
					fclose(fp_write);
					if (fp_read != NULL)
						fclose(fp_read);
					return RET_LOGIC_ERROR;
				}

				// Read one slice from a file.
				file_index = slice_list[slice_index].file;
				file_offset = slice_list[slice_index].offset;
				read_size = slice_list[slice_index].size;
				if ( (fp_read == NULL) || (file_index != file_read) ){
					if (fp_read != NULL){	// Close previous input file.
						fclose(fp_read);
						fp_read = NULL;
					}
					fp_read = fopen(file_list[file_index].name, "rb");
					if (fp_read == NULL){
						perror("Failed to open input file");
						fclose(fp_write);
						return RET_FILE_IO_ERROR;
					}
					file_read = file_index;
				}
				if (file_offset > 0){
					if (_fseeki64(fp_read, file_offset, SEEK_SET) != 0){
						perror("Failed to seek input file");
						fclose(fp_read);
						fclose(fp_write);
						return RET_FILE_IO_ERROR;
					}
				}
				if (fread(work_buf + tail_offset, 1, read_size, fp_read) != read_size){
					perror("Failed to read tail slice on input file");
					fclose(fp_read);
					fclose(fp_write);
					return RET_FILE_IO_ERROR;
				}
				tail_offset += read_size;
			}

			// Zero fill rest bytes
//			if (write_size < block_size)
//				memset(work_buf + write_size, 0, block_size - write_size);
		}

		// Calculate checksum of block to confirm that input file was not changed.
		if (crc64(work_buf, write_size, 0) != block_list[num].crc){
			printf("Checksum of block[%I64u] is different.\n", num);
			fclose(fp_read);
			fclose(fp_write);
			return RET_LOGIC_ERROR;
		}

		// Calculate checksum of packet here.
		blake3_hasher_init(&hasher);
		blake3_hasher_update(&hasher, packet_header + 24, 24 + 8);
		blake3_hasher_update(&hasher, work_buf, write_size);
		blake3_hasher_finalize(&hasher, packet_header + 8, 16);

		// Write packet header and data on file.
		if (fwrite(packet_header, 1, 56, fp_write) != 56){
			perror("Failed to write Data Packet on Archive File");
			fclose(fp_write);
			fclose(fp_read);
			return RET_FILE_IO_ERROR;
		}
		if (fwrite(work_buf, 1, write_size, fp_write) != write_size){
			perror("Failed to write Data Packet on Archive File");
			fclose(fp_write);
			fclose(fp_read);
			return RET_FILE_IO_ERROR;
		}

		// How many common packets to write here.
		write_size = 0;
		write_size2 = 0;
		packet_to = packet_count * (num - each_start + 1) / each_count;
		//printf("write from %zu to %zu\n", packet_from, packet_to);
		while (packet_to - packet_from > 0){
			// Read packet size of each packet from packet_offset, and add them.
			memcpy(&packet_size, common_packet + packet_offset + write_size + 24, 8);
			write_size += packet_size;
			packet_from++;
			if (packet_offset + write_size >= common_packet_size)
				break;
		}
		while (packet_to - packet_from > 0){
			// Read packet size of each packet from the first, and add them.
			memcpy(&packet_size, common_packet + write_size2 + 24, 8);
			write_size2 += packet_size;
			packet_from++;
		}

		// Write common packets
		if (write_size > 0){
			//printf("packet_offset = %zu, write_size = %zu, total = %zu\n", packet_offset, write_size, packet_offset + write_size);
			if (fwrite(common_packet + packet_offset, 1, write_size, fp_write) != write_size){
				perror("Failed to write repeated common packet on Archive File");
				fclose(fp_write);
				fclose(fp_read);
				return RET_FILE_IO_ERROR;
			}
			// This offset doesn't exceed common_packet_size.
			packet_offset += write_size;
			if (packet_offset >= common_packet_size)
				packet_offset -= common_packet_size;
		}
		if (write_size2 > 0){
			//printf("write_size2 = %zu = packet_offset\n", write_size2);
			if (fwrite(common_packet, 1, write_size2, fp_write) != write_size2){
				perror("Failed to write repeated common packet on Archive File");
				fclose(fp_write);
				fclose(fp_read);
				return RET_FILE_IO_ERROR;
			}
			// Current offset is saved.
			packet_offset = write_size2;
		}
	}

	// Comment Packet
	write_size = par3_ctx->comment_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->comment_packet, 1, write_size, fp_write) != write_size){
			perror("Failed to write Comment Packet on Archive File");
			fclose(fp_write);
			if (fp_read != NULL)
				fclose(fp_read);
			return RET_FILE_IO_ERROR;
		}
	}

	if (fp_read != NULL){
		if (fclose(fp_read) != 0){
			perror("Failed to close input file");
			fclose(fp_write);
			return RET_FILE_IO_ERROR;
		}
	}
	if (fclose(fp_write) != 0){
		perror("Failed to close Archive File");
		return RET_FILE_IO_ERROR;
	}

	return 0;
}

// Write PAR3 files with Data packets (input blocks)
int write_archive_file(PAR3_CTX *par3_ctx)
{
	char filename[_MAX_PATH], recovery_file_scheme;
	int digit_num1, digit_num2;
	uint32_t file_count;
	size_t len, region_size;
	uint64_t block_count, base_num;
	uint64_t each_start, each_count, max_count;

	block_count = par3_ctx->block_count;
	if (block_count == 0)
		return 0;
	recovery_file_scheme = par3_ctx->recovery_file_scheme;
	file_count = par3_ctx->recovery_file_count;

	// Allocate memory to read one input block and parity.
	region_size = (par3_ctx->block_size + 1 + 7) & ~8;
	par3_ctx->work_buf = malloc(region_size);
	if (par3_ctx->work_buf == NULL){
		perror("Failed to allocate memory for input data");
		return RET_MEMORY_ERROR;
	}

	// Remove the last ".par3" from base PAR3 filename.
	strcpy(filename, par3_ctx->par_filename);
	len = strlen(filename);
	if (strcmp(filename + len - 5, ".par3") == 0){
		len -= 5;
		filename[len] = 0;
		//printf("len = %zu, base name = %s\n", len, filename);
	}

	// Calculate block count and digits max.
	calculate_digit_max(par3_ctx, block_count, 0, &base_num, &max_count, &digit_num1, &digit_num2);
	if (len + 11 + digit_num1 + digit_num2 >= _MAX_PATH){	// .part#+#.par3
		printf("PAR3 filename will be too long.\n");
		return RET_FILE_IO_ERROR;
	}

	// Write each PAR3 file.
	each_start = 0;
	while (block_count > 0){
		if (file_count > 0){
			if (recovery_file_scheme == 'u'){	// Uniform
				each_count = max_count;
				if (base_num > 0){
					base_num--;
					if (base_num == 0)
						max_count--;
				}

			} else {	// Variable (multiply by 2)
				each_count = base_num;
				base_num *= 2;
				if (each_count > block_count)
					each_count = block_count;
			}

		} else {
			if (recovery_file_scheme == 'u'){	// Uniform
				each_count = block_count;

			} else if (recovery_file_scheme == 'l'){	// Limit size
				each_count = base_num;
				base_num *= 2;
				if (each_count > max_count)
					each_count = max_count;
				if (each_count > block_count)
					each_count = block_count;

			} else {	// Power of 2
				each_count = base_num;
				base_num *= 2;
				if (each_count > block_count)
					each_count = block_count;
			}
		}

		sprintf(filename + len, ".part%0*I64u+%0*I64u.par3", digit_num1, each_start, digit_num2, each_count);
		if (write_data_packet(par3_ctx, filename, each_start, each_count) != 0){
			return RET_FILE_IO_ERROR;
		}
		if (par3_ctx->noise_level >= -1)
			printf("Wrote archive file, %s\n", offset_file_name(filename));

		each_start += each_count;
		block_count -= each_count;
	}

	free(par3_ctx->work_buf);
	par3_ctx->work_buf = NULL;

	return 0;
}


// Recovery Data packet with dummy recovery block
static int write_recovery_packet(PAR3_CTX *par3_ctx, char *filename, uint64_t each_start, uint64_t each_count)
{
	uint8_t *buf_p, *common_packet, packet_header[89];
	uint64_t num;
	size_t block_size, region_size;
	size_t write_size, write_size2;
	size_t packet_count, packet_to, packet_from;
	size_t common_packet_size, packet_size, packet_offset;
	FILE *fp;
	blake3_hasher hasher;

	block_size = par3_ctx->block_size;
	common_packet = par3_ctx->common_packet;
	common_packet_size = par3_ctx->common_packet_size;

	region_size = (block_size + 1 + 7) & ~8;
	buf_p = par3_ctx->recovery_data;
	if (par3_ctx->ecc_method & 0x1000){
		// Move to the position of starting recovery block
		buf_p += (each_start - par3_ctx->first_recovery_block) * region_size;
	}

	// How many repetition of common packet.
	packet_count = 0;	// reduce 1, because put 1st copy at first.
	for (num = 2; num <= each_count; num *= 2)	// log2(each_count)
		packet_count++;
	//printf("each_count = %I64u, repetition = %zu\n", each_count, packet_count);
	packet_count *= par3_ctx->common_packet_count;
	//printf("number of repeated packets = %zu\n", packet_count);

	fp = fopen(filename, "wb");
	if (fp == NULL){
		perror("Failed to open Recovery File");
		return RET_FILE_IO_ERROR;
	}

	// Creator Packet
	write_size = par3_ctx->creator_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->creator_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Creator Packet on Recovery File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	// First common packets
	write_size = common_packet_size;
	if (fwrite(common_packet, 1, write_size, fp) != write_size){
		perror("Failed to write first common packets on Recovery File");
		fclose(fp);
		return RET_FILE_IO_ERROR;
	}

	// Common items in packet header of Recovery Data Packets
	memset(packet_header + 8, 0, 16);	// Zero fill checksum of packet as a sign of not calculated yet
	memcpy(packet_header + 48, par3_ctx->root_packet + 8, 16);	// The checksum from the Root packet
	memcpy(packet_header + 64, par3_ctx->matrix_packet + 8, 16);
	packet_header[88] = 0;	// Zero for dummy data

	// Recovery Data Packet and repeated common packets
	packet_from = 0;
	packet_offset = 0;
	for (num = each_start; num < each_start + each_count; num++){
		// packet header
		make_packet_header(packet_header, 88 + block_size, par3_ctx->set_id, "PAR REC\0", 0);

		// The index of the recovery block
		memcpy(packet_header + 80, &num, 8);

		// When there are enough memory to keep all recovery blocks,
		// recovery blocks were created already.
		if (par3_ctx->ecc_method & 0x1000){
			// Check parity of recovery block to confirm that calculation was correct.
			if (region_check_parity(buf_p, region_size, block_size) != 0){
				printf("Parity of block[%I64u] is different.\n", num);
				fclose(fp);
				return RET_LOGIC_ERROR;
			}

			// Calculate checksum of packet here.
			blake3_hasher_init(&hasher);
			blake3_hasher_update(&hasher, packet_header + 24, 24 + 40);
			blake3_hasher_update(&hasher, buf_p, block_size);
			blake3_hasher_finalize(&hasher, packet_header + 8, 16);

			// Write packet header and recovery data on file.
			if (fwrite(packet_header, 1, 88, fp) != 88){
				perror("Failed to write Recovery Data Packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			if (fwrite(buf_p, 1, block_size, fp) != block_size){
				perror("Failed to write Recovery Data Packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			buf_p += region_size;

		} else {
			// Save position of each recovery block for later wariting.
			

			// Write packet header and dummy data on file.
			if (fwrite(packet_header, 1, 88, fp) != 88){
				perror("Failed to write Recovery Data Packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			// Write zero bytes as dummy
			if (block_size > 1){
				if (_fseeki64(fp, block_size - 1, SEEK_CUR) != 0){
					perror("Failed to seek Recovery File");
					fclose(fp);
					return RET_FILE_IO_ERROR;
				}
			}
			if (fwrite(packet_header + 88, 1, 1, fp) != 1){
				perror("Failed to write Recovery Data Packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
		}

		// How many common packets to write here.
		write_size = 0;
		write_size2 = 0;
		packet_to = packet_count * (num - each_start + 1) / each_count;
		//printf("write from %zu to %zu\n", packet_from, packet_to);
		while (packet_to - packet_from > 0){
			// Read packet size of each packet from packet_offset, and add them.
			memcpy(&packet_size, common_packet + packet_offset + write_size + 24, 8);
			write_size += packet_size;
			packet_from++;
			if (packet_offset + write_size >= common_packet_size)
				break;
		}
		while (packet_to - packet_from > 0){
			// Read packet size of each packet from the first, and add them.
			memcpy(&packet_size, common_packet + write_size2 + 24, 8);
			write_size2 += packet_size;
			packet_from++;
		}

		// Write common packets
		if (write_size > 0){
			//printf("packet_offset = %zu, write_size = %zu, total = %zu\n", packet_offset, write_size, packet_offset + write_size);
			if (fwrite(common_packet + packet_offset, 1, write_size, fp) != write_size){
				perror("Failed to write repeated common packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			// This offset doesn't exceed common_packet_size.
			packet_offset += write_size;
			if (packet_offset >= common_packet_size)
				packet_offset -= common_packet_size;
		}
		if (write_size2 > 0){
			//printf("write_size2 = %zu = packet_offset\n", write_size2);
			if (fwrite(common_packet, 1, write_size2, fp) != write_size2){
				perror("Failed to write repeated common packet on Recovery File");
				fclose(fp);
				return RET_FILE_IO_ERROR;
			}
			// Current offset is saved.
			packet_offset = write_size2;
		}
	}

	// Comment Packet
	write_size = par3_ctx->comment_packet_size;
	if (write_size > 0){
		if (fwrite(par3_ctx->comment_packet, 1, write_size, fp) != write_size){
			perror("Failed to write Comment Packet on Recovery File");
			fclose(fp);
			return RET_FILE_IO_ERROR;
		}
	}

	if (fclose(fp) != 0){
		perror("Failed to close Recovery File");
		return RET_FILE_IO_ERROR;
	}

	return 0;
}

// Write PAR3 files with Recovery Data packets (recovery blocks are not written yet)
int write_recovery_file(PAR3_CTX *par3_ctx)
{
	char filename[_MAX_PATH], recovery_file_scheme;
	int digit_num1, digit_num2;
	uint32_t file_count;
	uint64_t block_count, base_num, first_num;
	uint64_t each_start, each_count, max_count;
	size_t len;

	block_count = par3_ctx->recovery_block_count;
	if (block_count == 0)
		return 0;
	recovery_file_scheme = par3_ctx->recovery_file_scheme;
	file_count = par3_ctx->recovery_file_count;
	first_num = par3_ctx->first_recovery_block;

	// Remove the last ".par3" from base PAR3 filename.
	strcpy(filename, par3_ctx->par_filename);
	len = strlen(filename);
	if (strcmp(filename + len - 5, ".par3") == 0){
		len -= 5;
		filename[len] = 0;
		//printf("len = %zu, base name = %s\n", len, filename);
	}

	// Calculate block count and digits max.
	calculate_digit_max(par3_ctx, block_count, first_num, &base_num, &max_count, &digit_num1, &digit_num2);
	if (len + 10 + digit_num1 + digit_num2 >= _MAX_PATH){	// .vol#+#.par3
		printf("PAR3 filename will be too long.\n");
		return RET_FILE_IO_ERROR;
	}

	// Write each PAR3 file.
	each_start = first_num;
	while (block_count > 0){
		if (file_count > 0){
			if (recovery_file_scheme == 'u'){	// Uniform
				each_count = max_count;
				if (base_num > 0){
					base_num--;
					if (base_num == 0)
						max_count--;
				}

			} else {	// Variable (multiply by 2)
				each_count = base_num;
				base_num *= 2;
				if (each_count > block_count)
					each_count = block_count;
			}

		} else {
			if (recovery_file_scheme == 'u'){	// Uniform
				each_count = block_count;

			} else if (recovery_file_scheme == 'l'){	// Limit size
				each_count = base_num;
				base_num *= 2;
				if (each_count > max_count)
					each_count = max_count;
				if (each_count > block_count)
					each_count = block_count;

			} else {	// Power of 2
				each_count = base_num;
				base_num *= 2;
				if (each_count > block_count)
					each_count = block_count;
			}
		}

		sprintf(filename + len, ".vol%0*I64u+%0*I64u.par3", digit_num1, each_start, digit_num2, each_count);
		if (write_recovery_packet(par3_ctx, filename, each_start, each_count) != 0){
			return RET_FILE_IO_ERROR;
		}
		if (par3_ctx->noise_level >= -1)
			printf("Wrote recovery file, %s\n", offset_file_name(filename));

		each_start += each_count;
		block_count -= each_count;
	}

	return 0;
}

