#ifndef __COMMON_H__
#define __COMMON_H__

#if __linux__

#include <linux/limits.h>
#define _MAX_PATH PATH_MAX

// Windows system call 
int64_t _filelengthi64(int fd);

#elif _WIN32

#endif



char * offset_file_name(char *file_path);
int sanitize_file_name(char *name);

int get_absolute_path(char *absolute_path, char *relative_path, size_t max);
size_t path_copy(char *dst, char *src, size_t max);

size_t trim_text(uint8_t *text, size_t len);

int namez_add(char **namez, size_t *namez_len, size_t *namez_max, const char *str);
int namez_count(char *namez, size_t namez_len);
int namez_delete(char *namez, size_t *namez_len, char *entry);
char * namez_search(char *namez, size_t namez_len, char *match);
char * namez_get(char *namez, size_t namez_len, int index);
int namez_sort(char *namez, size_t namez_len);
size_t namez_maxlen(char *namez, size_t namez_len);

unsigned int mem_or8(unsigned char buf[8]);
unsigned int mem_or16(unsigned char buf[16]);

int popcount32(uint32_t x);
int roundup_log2(uint64_t x);
uint64_t next_pow2(uint64_t x);

#endif // __COMMON_H__
