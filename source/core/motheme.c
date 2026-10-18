#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <zlib.h>

#include "error.h"
#include "fs.h"
#include "motheme.h"
#include "stringutil.h"

#define MOTHEME_CHUNK (16 * 1024)
#define MOTHEME_MAX_CD_SIZE (1024 * 1024)

static u16 rd16(const u8* p) {
    return (u16) (p[0] | (p[1] << 8));
}

static u32 rd32(const u8* p) {
    return (u32) p[0] | ((u32) p[1] << 8) | ((u32) p[2] << 16) | ((u32) p[3] << 24);
}

Result motheme_open(motheme* z, const char* path) {
    if(z == NULL || path == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    memset(z, 0, sizeof(motheme));

    z->fd = fopen(path, "rb");
    if(z->fd == NULL) {
        return R_APP_BAD_DATA;
    }

    Result res = R_APP_BAD_DATA;
    u8* tail = NULL;
    u8* cd = NULL;

    do {
        if(fseek(z->fd, 0, SEEK_END) != 0) break;
        long fileSize = ftell(z->fd);
        if(fileSize < 22) break;

        long tailSize = fileSize < 65557 ? fileSize : 65557;
        tail = (u8*) malloc((size_t) tailSize);
        if(tail == NULL) {
            res = R_APP_OUT_OF_MEMORY;
            break;
        }

        if(fseek(z->fd, fileSize - tailSize, SEEK_SET) != 0 || fread(tail, 1, (size_t) tailSize, z->fd) != (size_t) tailSize) break;

        long eocd = -1;
        for(long i = tailSize - 22; i >= 0; i--) {
            if(rd32(tail + i) == 0x06054b50) {
                eocd = i;
                break;
            }
        }

        if(eocd < 0) break;

        u32 total = rd16(tail + eocd + 10);
        u32 cdSize = rd32(tail + eocd + 12);
        u32 cdOffset = rd32(tail + eocd + 16);
        if(total == 0 || cdSize == 0 || cdSize > MOTHEME_MAX_CD_SIZE || cdOffset >= (u32) fileSize) break;

        cd = (u8*) malloc(cdSize);
        z->entries = (motheme_entry*) calloc(total, sizeof(motheme_entry));
        if(cd == NULL || z->entries == NULL) {
            res = R_APP_OUT_OF_MEMORY;
            break;
        }

        if(fseek(z->fd, cdOffset, SEEK_SET) != 0 || fread(cd, 1, cdSize, z->fd) != cdSize) break;

        u32 pos = 0;
        bool ok = true;
        for(u32 i = 0; i < total; i++) {
            if(pos + 46 > cdSize || rd32(cd + pos) != 0x02014b50) {
                ok = false;
                break;
            }

            u32 nameLen = rd16(cd + pos + 28);
            u32 extraLen = rd16(cd + pos + 30);
            u32 commentLen = rd16(cd + pos + 32);
            if(pos + 46 + nameLen + extraLen + commentLen > cdSize) {
                ok = false;
                break;
            }

            motheme_entry* e = &z->entries[i];
            e->method = rd16(cd + pos + 10);
            e->compressedSize = rd32(cd + pos + 20);
            e->size = rd32(cd + pos + 24);
            e->offset = rd32(cd + pos + 42);

            u32 copyLen = nameLen < MOTHEME_ENTRY_NAME_MAX - 1 ? nameLen : MOTHEME_ENTRY_NAME_MAX - 1;
            memcpy(e->name, cd + pos + 46, copyLen);
            e->name[copyLen] = '\0';
            for(char* c = e->name; *c != '\0'; c++) {
                if(*c == '\\') {
                    *c = '/';
                }
            }

            pos += 46 + nameLen + extraLen + commentLen;
        }

        if(!ok) break;

        z->count = total;
        res = 0;
    } while(0);

    free(tail);
    free(cd);

    if(R_FAILED(res)) {
        motheme_close(z);
    }

    return res;
}

void motheme_close(motheme* z) {
    if(z == NULL) {
        return;
    }

    if(z->fd != NULL) {
        fclose(z->fd);
        z->fd = NULL;
    }

    free(z->entries);
    z->entries = NULL;
    z->count = 0;
}

int motheme_find(motheme* z, const char* name) {
    for(u32 i = 0; i < z->count; i++) {
        if(strcmp(z->entries[i].name, name) == 0) {
            return (int) i;
        }
    }

    return -1;
}

static bool motheme_is_preview(const motheme_entry* e) {
    size_t len = strlen(e->name);
    return strncmp(e->name, "Previews/", 9) == 0 && len > 13 && strcasecmp(e->name + len - 4, ".png") == 0;
}

u32 motheme_count_previews(motheme* z) {
    u32 n = 0;
    for(u32 i = 0; i < z->count; i++) {
        if(motheme_is_preview(&z->entries[i])) {
            n++;
        }
    }

    return n;
}

int motheme_find_preview(motheme* z, u32 n) {
    u32 seen = 0;
    for(u32 i = 0; i < z->count; i++) {
        if(motheme_is_preview(&z->entries[i])) {
            if(seen == n) {
                return (int) i;
            }

            seen++;
        }
    }

    return -1;
}

static Result motheme_stream(motheme* z, const motheme_entry* e, FILE* out, u8* mem) {
    u8 lh[30];
    if(fseek(z->fd, e->offset, SEEK_SET) != 0 || fread(lh, 1, 30, z->fd) != 30 || rd32(lh) != 0x04034b50) {
        return R_APP_BAD_DATA;
    }

    if(fseek(z->fd, e->offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET) != 0) {
        return R_APP_BAD_DATA;
    }

    u8* inBuf = (u8*) malloc(MOTHEME_CHUNK);
    u8* outBuf = out != NULL ? (u8*) malloc(MOTHEME_CHUNK) : NULL;
    if(inBuf == NULL || (out != NULL && outBuf == NULL)) {
        free(inBuf);
        free(outBuf);
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = 0;
    u32 remaining = e->compressedSize;

    if(e->method == 0) {
        u32 written = 0;
        while(remaining > 0 && R_SUCCEEDED(res)) {
            u32 n = remaining < MOTHEME_CHUNK ? remaining : MOTHEME_CHUNK;
            if(fread(inBuf, 1, n, z->fd) != n) {
                res = R_APP_BAD_DATA;
            } else if(out != NULL) {
                if(fwrite(inBuf, 1, n, out) != n) {
                    res = R_APP_BAD_DATA;
                }
            } else {
                memcpy(mem + written, inBuf, n);
            }

            written += n;
            remaining -= n;
        }
    } else if(e->method == 8) {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));

        if(inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            res = R_APP_BAD_DATA;
        } else {
            int ret = Z_OK;
            while(ret != Z_STREAM_END && R_SUCCEEDED(res)) {
                if(zs.avail_in == 0) {
                    if(remaining == 0) {
                        res = R_APP_BAD_DATA;
                        break;
                    }

                    u32 n = remaining < MOTHEME_CHUNK ? remaining : MOTHEME_CHUNK;
                    if(fread(inBuf, 1, n, z->fd) != n) {
                        res = R_APP_BAD_DATA;
                        break;
                    }

                    remaining -= n;
                    zs.next_in = inBuf;
                    zs.avail_in = n;
                }

                if(out != NULL) {
                    zs.next_out = outBuf;
                    zs.avail_out = MOTHEME_CHUNK;
                } else {
                    zs.next_out = mem + zs.total_out;
                    zs.avail_out = e->size - (u32) zs.total_out;
                }

                ret = inflate(&zs, Z_NO_FLUSH);
                if(ret != Z_OK && ret != Z_STREAM_END) {
                    res = R_APP_BAD_DATA;
                    break;
                }

                if(out != NULL) {
                    size_t produced = MOTHEME_CHUNK - zs.avail_out;
                    if(produced > 0 && fwrite(outBuf, 1, produced, out) != produced) {
                        res = R_APP_BAD_DATA;
                    }
                }
            }

            if(R_SUCCEEDED(res) && out == NULL && zs.total_out != e->size) {
                res = R_APP_BAD_DATA;
            }

            inflateEnd(&zs);
        }
    } else {
        res = R_APP_NOT_IMPLEMENTED;
    }

    free(inBuf);
    free(outBuf);
    return res;
}

Result motheme_read_entry(motheme* z, int index, u32 maxSize, void** data, u32* size) {
    if(z == NULL || index < 0 || (u32) index >= z->count || data == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    const motheme_entry* e = &z->entries[index];
    if(e->size == 0 || e->size > maxSize) {
        return R_APP_OUT_OF_RANGE;
    }

    u8* buf = (u8*) malloc(e->size + 1);
    if(buf == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = motheme_stream(z, e, NULL, buf);
    if(R_SUCCEEDED(res)) {
        buf[e->size] = '\0';
        *data = buf;
        if(size != NULL) {
            *size = e->size;
        }
    } else {
        free(buf);
    }

    return res;
}

Result motheme_read_name(motheme* z, char* out, size_t outSize) {
    int idx = motheme_find(z, "name.txt");
    if(idx < 0) {
        return R_APP_BAD_DATA;
    }

    char* text = NULL;
    Result res = motheme_read_entry(z, idx, 1024, (void**) &text, NULL);
    if(R_SUCCEEDED(res)) {

        char* end = text + strcspn(text, "\r\n");
        *end = '\0';

        char* start = text;
        if((u8) start[0] == 0xEF && (u8) start[1] == 0xBB && (u8) start[2] == 0xBF) {
            start += 3;
        }

        while(*start == ' ' || *start == '\t') start++;
        end = start + strlen(start);
        while(end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';

        if(*start == '\0') {
            res = R_APP_BAD_DATA;
        } else {
            string_copy(out, start, outSize);
        }

        free(text);
    }

    return res;
}

static void motheme_mkdirs(const char* path) {
    char tmp[FILE_PATH_MAX];
    string_copy(tmp, path, sizeof(tmp));

    char* p = strchr(tmp, '/');
    if(p == NULL) {
        return;
    }

    for(p++; *p != '\0'; p++) {
        if(*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }

    mkdir(tmp, 0777);
}

static bool motheme_path_safe(const char* rel) {
    if(rel[0] == '/' || strchr(rel, ':') != NULL) {
        return false;
    }

    const char* seg = rel;
    while(*seg != '\0') {
        const char* end = strchr(seg, '/');
        size_t len = end != NULL ? (size_t) (end - seg) : strlen(seg);
        if(len == 2 && seg[0] == '.' && seg[1] == '.') {
            return false;
        }

        if(end == NULL) break;
        seg = end + 1;
    }

    return true;
}

Result motheme_extract_theme(motheme* z, const char* destDir) {
    if(z == NULL || destDir == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    motheme_mkdirs(destDir);

    u32 files = 0;
    for(u32 i = 0; i < z->count; i++) {
        const motheme_entry* e = &z->entries[i];
        if(strncmp(e->name, "theme/", 6) != 0 || e->name[6] == '\0') {
            continue;
        }

        const char* rel = e->name + 6;
        if(!motheme_path_safe(rel)) {
            return R_APP_BAD_DATA;
        }

        char dest[FILE_PATH_MAX];
        snprintf(dest, sizeof(dest), "%s/%s", destDir, rel);

        size_t len = strlen(rel);
        if(rel[len - 1] == '/') {
            motheme_mkdirs(dest);
            continue;
        }

        char parent[FILE_PATH_MAX];
        string_copy(parent, dest, sizeof(parent));
        char* slash = strrchr(parent, '/');
        if(slash != NULL) {
            *slash = '\0';
            motheme_mkdirs(parent);
        }

        FILE* out = fopen(dest, "wb");
        if(out == NULL) {
            return R_APP_BAD_DATA;
        }

        Result res = motheme_stream(z, e, out, NULL);
        fclose(out);
        if(R_FAILED(res)) {
            return res;
        }

        files++;
    }

    return files > 0 ? 0 : R_APP_BAD_DATA;
}
