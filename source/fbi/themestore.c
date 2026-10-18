#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <curl/curl.h>
#include <jansson.h>

#include "resources.h"
#include "section.h"
#include "task/uitask.h"
#include "../core/core.h"

#define THEMESTORE_URL "https://raw.githubusercontent.com/morrisonion/FBI-MO-Theme-Store/main/themestore.json"

#define FBI_DIR "sdmc:/fbi"
#define THEMESTORE_DIR "sdmc:/fbi/themes"
#define THEMESTORE_JSON "sdmc:/fbi/themes/themestore.json"
#define THEME_DIR "sdmc:/fbi/theme"
#define THEME_TMP_DIR "sdmc:/fbi/theme_new"

#define THEMESTORE_MAX_THEMES 300
#define THEMESTORE_URL_MAX 384

typedef struct {
    char name[96];
    char author[64];
    char description[256];
    char url[THEMESTORE_URL_MAX];
    bool reset;
} store_theme;

static struct {
    store_theme* themes;
    list_item* items;
    u32 count;
} store;

typedef enum {
    JOB_INSTALL,
    JOB_DOWNLOAD,
    JOB_PREVIEW
} job_mode;

typedef struct {
    store_theme* theme;
    job_mode mode;
} job_data;

static struct {
    char path[FILE_PATH_MAX];
    char title[96];
    u32 tex;
    u32 count;
    u32 index;
    bool texAllocated;
} preview;

static void store_free() {
    free(store.themes);
    free(store.items);
    store.themes = NULL;
    store.items = NULL;
    store.count = 0;
}

static void store_local_path(const store_theme* t, char* out, size_t size) {
    char base[200];
    if(t->author[0] != '\0') {
        snprintf(base, sizeof(base), "%s - %s", t->author, t->name);
    } else {
        snprintf(base, sizeof(base), "%s", t->name);
    }

    for(char* c = base; *c != '\0'; c++) {
        if(!isalnum((unsigned char) *c) && strchr(" -_.()", *c) == NULL) {
            *c = '_';
        }
    }

    snprintf(out, size, THEMESTORE_DIR "/%s.motheme", base);
}

static size_t store_curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    return fwrite(ptr, size, nmemb, (FILE*) userdata);
}

static Result store_curl_download(const char* url, FILE* fd) {
    CURL* curl = curl_easy_init();
    if(curl == NULL) {
        return R_APP_CURL_INIT_FAILED;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101 FBI-MO");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long) CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, store_curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*) fd);

    Result res = 0;
    CURLcode ret = curl_easy_perform(curl);
    if(ret == CURLE_HTTP_RETURNED_ERROR) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        res = R_APP_HTTP_ERROR_BASE + code;
    } else if(ret != CURLE_OK) {
        res = R_APP_CURL_ERROR_BASE + ret;
    }

    curl_easy_cleanup(curl);
    return res;
}

typedef struct {
    const char* url;
    const char* dest;
    Result res;
} store_download_job;

static void store_download_thread(void* arg) {
    store_download_job* job = (store_download_job*) arg;

    char part[FILE_PATH_MAX];
    snprintf(part, sizeof(part), "%s.part", job->dest);

    FILE* fd = fopen(part, "wb");
    if(fd == NULL) {
        job->res = R_APP_BAD_DATA;
        return;
    }

    Result res = store_curl_download(job->url, fd);
    long written = ftell(fd);
    fclose(fd);

    if(R_SUCCEEDED(res) && written <= 0) {
        res = R_APP_BAD_DATA;
    }

    if(R_SUCCEEDED(res)) {
        remove(job->dest);
        if(rename(part, job->dest) != 0) {
            res = R_APP_BAD_DATA;
        }
    } else {
        remove(part);
    }

    job->res = res;
}

static Result store_download_file(const char* url, const char* dest) {
    store_download_job job = {url, dest, R_APP_THREAD_CREATE_FAILED};

    Thread thread = threadCreate(store_download_thread, &job, 0x10000, 0x18, 1, false);
    if(thread == NULL) {
        return R_APP_THREAD_CREATE_FAILED;
    }

    threadJoin(thread, U64_MAX);
    threadFree(thread);

    return job.res;
}

static void store_remove_dir(const char* path) {
    FS_Archive sd = 0;
    if(R_SUCCEEDED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path* fsPath = fs_make_path_utf8(path + strlen("sdmc:"));
        if(fsPath != NULL) {
            FSUSER_DeleteDirectoryRecursively(sd, *fsPath);
            fs_free_path_utf8(fsPath);
        }

        FSUSER_CloseArchive(sd);
    }
}

static bool store_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static Result store_install(const char* zipPath, char* nameOut, size_t nameSize) {
    motheme z;
    Result res = motheme_open(&z, zipPath);
    if(R_FAILED(res)) {
        return res;
    }

    if(R_FAILED(motheme_read_name(&z, nameOut, nameSize))) {
        nameOut[0] = '\0';
    }

    store_remove_dir(THEME_TMP_DIR);
    res = motheme_extract_theme(&z, THEME_TMP_DIR);
    motheme_close(&z);

    if(R_SUCCEEDED(res)) {
        store_remove_dir(THEME_DIR);
        if(rename(THEME_TMP_DIR, THEME_DIR) != 0) {
            res = R_APP_BAD_DATA;
        }
    }

    if(R_FAILED(res)) {
        store_remove_dir(THEME_TMP_DIR);
    } else {

        resources_load();
    }

    return res;
}

static bool preview_load(u32 index) {
    motheme z;
    if(R_FAILED(motheme_open(&z, preview.path))) {
        return false;
    }

    bool ok = false;
    int idx = motheme_find_preview(&z, index);
    if(idx >= 0) {
        void* data = NULL;
        u32 size = 0;
        if(R_SUCCEEDED(motheme_read_entry(&z, idx, 4 * 1024 * 1024, &data, &size))) {
            if(!preview.texAllocated) {
                preview.tex = screen_allocate_free_texture();
                preview.texAllocated = true;
            }

            ok = screen_load_texture_memory(preview.tex, data, size, true);
            free(data);
        }
    }

    motheme_close(&z);
    return ok;
}

static void preview_close() {
    if(preview.texAllocated) {
        screen_unload_texture(preview.tex);
        preview.texAllocated = false;
    }
}

static void preview_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    if(!preview.texAllocated) {
        return;
    }

    u32 tw = 0;
    u32 th = 0;
    screen_get_texture_size(&tw, &th, preview.tex);
    if(tw == 0 || th == 0) {
        return;
    }

    float sx = (x2 - x1) / tw;
    float sy = (y2 - y1) / th;
    float scale = sx < sy ? sx : sy;
    float w = tw * scale;
    float h = th * scale;

    screen_draw_texture(preview.tex, x1 + (x2 - x1 - w) / 2, y1 + (y2 - y1 - h) / 2, w, h);
}

static void preview_show();

static void preview_on_response(ui_view* view, void* data, u32 response) {
    if(response == 0 && preview.count > 1) {
        preview.index = (preview.index + 1) % preview.count;
        if(preview_load(preview.index)) {
            preview_show();
            return;
        }
    }

    preview_close();
}

static void preview_show() {
    snprintf(preview.title, sizeof(preview.title), "Preview %lu/%lu", preview.index + 1, preview.count);

    if(preview.count > 1) {
        static const char* options[2] = {"Next", "Close"};
        static u32 buttons[2] = {KEY_A, KEY_B};
        prompt_display_multi_choice(preview.title, "", COLOR_TEXT, options, buttons, 2, NULL, preview_draw_top, preview_on_response);
    } else {
        prompt_display_notify(preview.title, "", COLOR_TEXT, NULL, preview_draw_top, preview_on_response);
    }
}

static void themestore_job(ui_view* view, void* data, float* progress, char* text) {
    job_data* job = (job_data*) data;
    store_theme* theme = job->theme;

    char path[FILE_PATH_MAX];
    store_local_path(theme, path, sizeof(path));

    char name[96] = "";
    Result res = 0;

    mkdir(FBI_DIR, 0777);
    mkdir(THEMESTORE_DIR, 0777);

    if(job->mode != JOB_PREVIEW || !store_file_exists(path)) {
        res = store_download_file(theme->url, path);
    }

    if(R_SUCCEEDED(res) && job->mode == JOB_INSTALL) {
        res = store_install(path, name, sizeof(name));
    }

    if(R_SUCCEEDED(res) && job->mode == JOB_PREVIEW) {
        string_copy(preview.path, path, sizeof(preview.path));

        motheme z;
        if(R_SUCCEEDED(res = motheme_open(&z, path))) {
            preview.count = motheme_count_previews(&z);
            motheme_close(&z);
        }
    }

    ui_pop();
    info_destroy(view);

    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, job->mode == JOB_INSTALL ? "Failed to install theme." : (job->mode == JOB_PREVIEW ? "Failed to load theme preview." : "Failed to download theme."));
    } else if(job->mode == JOB_INSTALL) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Installed \"%s\".", name[0] != '\0' ? name : theme->name);
        prompt_display_notify("Success", msg, COLOR_TEXT, NULL, NULL, NULL);
    } else if(job->mode == JOB_DOWNLOAD) {
        char msg[FILE_PATH_MAX + 32];
        snprintf(msg, sizeof(msg), "Saved to\n%s", path);
        prompt_display_notify("Success", msg, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        preview.index = 0;
        if(preview.count == 0) {
            prompt_display_notify("Preview", "This theme has no previews.", COLOR_TEXT, NULL, NULL, NULL);
        } else if(preview_load(0)) {
            preview_show();
        } else {
            error_display(NULL, NULL, "Failed to load preview image.");
        }
    }

    free(job);
}

static void themestore_job_start(store_theme* theme, job_mode mode) {
    job_data* job = (job_data*) calloc(1, sizeof(job_data));
    if(job == NULL) {
        error_display(NULL, NULL, "Failed to allocate theme job data.");
        return;
    }

    job->theme = theme;
    job->mode = mode;

    const char* title = mode == JOB_INSTALL ? "Installing Theme" : (mode == JOB_PREVIEW ? "Loading Preview" : "Downloading Theme");
    if(info_display(title, "", false, job, themestore_job, NULL) == NULL) {
        free(job);
    }
}

static void themestore_reset_response(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        store_remove_dir(THEME_DIR);
        resources_load();

        prompt_display_notify("Success", "Default theme restored.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void themestore_draw_details(const store_theme* t, float x1, float y1, float x2, float y2) {
    if(t == NULL) {
        return;
    }

    float y = y1 + 5;
    float w = 0;
    float h = 0;

    screen_get_string_size_wrap(&w, &h, t->name, 0.6f, 0.6f, x2 - x1 - 10);
    screen_draw_string_wrap(t->name, x1 + 5, y, 0.6f, 0.6f, COLOR_TEXT, false, x2 - 5);
    y += h + 4;

    char line[160];
    if(!t->reset) {
        snprintf(line, sizeof(line), "Author: %s", t->author[0] != '\0' ? t->author : "Unknown");
        screen_draw_string(line, x1 + 5, y, 0.5f, 0.5f, COLOR_TEXT, false);
        y += screen_get_font_height(0.5f) + 2;

        y += 4;
    }

    if(t->description[0] != '\0') {
        screen_draw_string_wrap(t->description, x1 + 5, y, 0.5f, 0.5f, COLOR_TEXT, false, x2 - 5);
    }
}

static list_item action_install = {"Install Theme", COLOR_TEXT, (void*) 1};
static list_item action_download = {"Download Only", COLOR_TEXT, (void*) 2};
static list_item action_preview = {"Preview", COLOR_TEXT, (void*) 3};

static void themestore_action_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    themestore_draw_details((store_theme*) data, x1, y1, x2, y2);
}

static void themestore_action_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    store_theme* theme = (store_theme*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);
        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        job_mode mode = (job_mode) ((u32) selected->data - 1);

        ui_pop();
        list_destroy(view);

        themestore_job_start(theme, mode);
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &action_install);
        linked_list_add(items, &action_download);
        linked_list_add(items, &action_preview);
    }
}

static void themestore_action_open(store_theme* theme) {
    if(theme->reset) {
        prompt_display_yes_no("Confirmation", "Restore the default theme?", COLOR_TEXT, NULL, NULL, themestore_reset_response);
        return;
    }

    list_display(theme->name, "A: Select, B: Return", theme, themestore_action_update, themestore_action_draw_top);
}

static void themestore_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL && selected->data != NULL) {
        themestore_draw_details((store_theme*) selected->data, x1, y1, x2, y2);
    }
}

static void themestore_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);
        store_free();
        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        themestore_action_open((store_theme*) selected->data);
        return;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < store.count; i++) {
            linked_list_add(items, &store.items[i]);
        }
    }
}

static void json_copy_string(char* dst, size_t size, json_t* obj, const char* key) {
    json_t* val = json_object_get(obj, key);
    if(json_is_string(val)) {
        string_copy(dst, json_string_value(val), size);
    }
}

static Result themestore_parse() {
    json_error_t err;
    json_t* root = json_load_file(THEMESTORE_JSON, 0, &err);
    if(root == NULL) {
        return R_APP_PARSE_FAILED;
    }

    json_t* arr = json_is_object(root) ? json_object_get(root, "themes") : NULL;
    if(!json_is_array(arr)) {
        json_decref(root);
        return R_APP_BAD_DATA;
    }

    u32 total = (u32) json_array_size(arr);
    if(total > THEMESTORE_MAX_THEMES) {
        total = THEMESTORE_MAX_THEMES;
    }

    store.themes = (store_theme*) calloc(total + 1, sizeof(store_theme));
    store.items = (list_item*) calloc(total + 1, sizeof(list_item));
    if(store.themes == NULL || store.items == NULL) {
        json_decref(root);
        store_free();
        return R_APP_OUT_OF_MEMORY;
    }

    string_copy(store.themes[0].name, "Reset to Default Theme", sizeof(store.themes[0].name));
    string_copy(store.themes[0].description, "Resets the theme to the default.", sizeof(store.themes[0].description));
    store.themes[0].reset = true;
    store.count = 1;

    for(u32 i = 0; i < total; i++) {
        json_t* obj = json_array_get(arr, i);
        if(!json_is_object(obj)) {
            continue;
        }

        store_theme* t = &store.themes[store.count];
        json_copy_string(t->name, sizeof(t->name), obj, "name");
        json_copy_string(t->author, sizeof(t->author), obj, "author");
        json_copy_string(t->description, sizeof(t->description), obj, "description");
        json_copy_string(t->url, sizeof(t->url), obj, "url");

        if(t->name[0] == '\0' || t->url[0] == '\0') {
            memset(t, 0, sizeof(store_theme));
            continue;
        }

        store.count++;
    }

    for(u32 i = 0; i < store.count; i++) {
        list_item* item = &store.items[i];
        const store_theme* t = &store.themes[i];

        if(t->author[0] != '\0') {
            snprintf(item->name, sizeof(item->name), "%s - %s", t->name, t->author);
        } else {
            string_copy(item->name, t->name, sizeof(item->name));
        }

        item->color = COLOR_TEXT;
        item->data = (void*) t;
    }

    json_decref(root);
    return 0;
}

static void themestore_load(ui_view* view, void* data, float* progress, char* text) {
    mkdir(FBI_DIR, 0777);
    mkdir(THEMESTORE_DIR, 0777);

    Result res = store_download_file(THEMESTORE_URL, THEMESTORE_JSON);
    Result dlRes = res;
    if(R_FAILED(res) && store_file_exists(THEMESTORE_JSON)) {
        res = 0;
    }

    if(R_SUCCEEDED(res)) {
        store_free();
        res = themestore_parse();
        if(R_FAILED(res)) {
            dlRes = res;
        }
    }

    ui_pop();
    info_destroy(view);

    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, R_FAILED(dlRes) ? dlRes : res, "Failed to load the theme store.");
        return;
    }

    if(list_display("Theme Store", "A: Select, B: Return", NULL, themestore_update, themestore_draw_top) == NULL) {
        store_free();
    }
}

void themestore_open() {
    info_display("Loading Theme Store", "", false, NULL, themestore_load, NULL);
}