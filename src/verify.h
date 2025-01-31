
int check_directory(PAR3_CTX *par3_ctx, char *path, int64_t offset);
void check_input_directory(PAR3_CTX *par3_ctx, uint32_t *missing_dir_count, uint32_t *bad_dir_count);

int verify_input_file(PAR3_CTX *par3_ctx, uint32_t *missing_file_count, uint32_t *damaged_file_count, uint32_t *bad_file_count);
int verify_extra_file(PAR3_CTX *par3_ctx, uint32_t *missing_file_count, uint32_t *damaged_file_count, uint32_t *misnamed_file_count);


// Find available slices in an input file
int check_complete_file(PAR3_CTX *par3_ctx, char *filename, uint32_t file_id,
	uint64_t current_size, uint64_t *offset_next);

int check_damaged_file(PAR3_CTX *par3_ctx, char *filename,
	uint64_t file_size, uint64_t file_offset, uint64_t *file_damage, uint8_t *file_hash);

