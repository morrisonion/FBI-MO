#pragma once

#define MOTHEME_ENTRY_NAME_MAX 256

typedef struct motheme_entry_s {
    u16 method;
    u32 compressedSize;
    u32 size;
    u32 offset;
    char name[MOTHEME_ENTRY_NAME_MAX];
} motheme_entry;

typedef struct motheme_s {
    FILE* fd;
    motheme_entry* entries;
    u32 count;
} motheme;

Result motheme_open(motheme* z, const char* path);
void motheme_close(motheme* z);

int motheme_find(motheme* z, const char* name);
int motheme_find_preview(motheme* z, u32 n);
u32 motheme_count_previews(motheme* z);

Result motheme_read_entry(motheme* z, int index, u32 maxSize, void** data, u32* size);
Result motheme_read_name(motheme* z, char* out, size_t outSize);
Result motheme_extract_theme(motheme* z, const char* destDir);
