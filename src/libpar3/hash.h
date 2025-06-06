
// CRC-64-ISO
uint64_t crc64(const uint8_t *buf, size_t size, uint64_t crc);
uint64_t crc64_zero(size_t size, uint64_t crc);

// table setup for slide window search
void init_crc_slide_table(PAR3_CTX *par3_ctx, int flag_usage);
uint64_t crc_slide_byte(uint64_t crc, uint8_t byteNew, uint8_t byteOld, uint64_t window_table[256]);

// for sort and search CRC-64
int64_t crc_list_compare(PAR3_CTX *par3_ctx, uint64_t crc, uint8_t *buf, uint8_t hash[16]);
void crc_list_add(PAR3_CTX *par3_ctx, uint64_t crc, uint64_t index);
int crc_list_make(PAR3_CTX *par3_ctx);
void crc_list_replace(PAR3_CTX *par3_ctx, uint64_t crc, uint64_t index);

int64_t cmp_list_search(uint64_t crc, PAR3_CMP_CTX *cmp_list, int64_t count);
int64_t cmp_list_search_index(uint64_t crc, int64_t id, PAR3_CMP_CTX *cmp_list, int64_t count);


// BLAKE3
void blake3(const uint8_t *buf, size_t size, uint8_t *hash);


// parity bytes in the region
void region_create_parity(uint8_t *buf, size_t region_size);
int region_check_parity(uint8_t *buf, size_t region_size);

// parity bytes in the region for Leopard-RS (ALTMAP)
void leo_region_create_parity(uint8_t *buf, size_t region_size);
int leo_region_check_parity(uint8_t *buf, size_t region_size);
void leo_region_restore(uint8_t *buf, size_t region_size);

