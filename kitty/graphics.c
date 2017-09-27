/*
 * graphics.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "graphics.h"
#include "state.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <zlib.h>
#include <png.h>

#define REPORT_ERROR(fmt, ...) { fprintf(stderr, fmt, __VA_ARGS__); fprintf(stderr, "\n"); }

static inline bool
mmap_img_file(GraphicsManager UNUSED *self, Image *img) {
    off_t file_sz = lseek(img->load_data.fd, 0, SEEK_END);
    if (file_sz == -1) { REPORT_ERROR("Failed to seek in image file with error: [%d] %s", errno, strerror(errno)); return false; }
    lseek(img->load_data.fd, 0, SEEK_SET);
    void *addr = mmap(0, file_sz, PROT_READ, MAP_PRIVATE, img->load_data.fd, 0);
    if (addr == MAP_FAILED) { REPORT_ERROR("Failed to map image file with error: [%d] %s", errno, strerror(errno)); return false; }
    img->load_data.mapped_file = addr;
    img->load_data.mapped_file_sz = file_sz;
    return true;
}

GraphicsManager*
grman_realloc(GraphicsManager *old, index_type lines, index_type columns) {
    GraphicsManager *self = (GraphicsManager *)GraphicsManager_Type.tp_alloc(&GraphicsManager_Type, 0);
    self->lines = lines; self->columns = columns;
    if (old == NULL) {
        self->images_capacity = 64;
        self->images = calloc(self->images_capacity, sizeof(Image));
        if (self->images == NULL) {
            Py_CLEAR(self); return NULL;
        }
    } else {
        self->images_capacity = old->images_capacity; self->images = old->images; self->image_count = old->image_count;
        old->images = NULL;
        grman_free(old);
    }
    return self;
}

static inline void
free_load_data(LoadData *ld) {
    free(ld->buf); ld->buf_used = 0; ld->buf_capacity = 0;
    ld->buf = NULL;

    if (ld->mapped_file) munmap(ld->mapped_file, ld->mapped_file_sz);
    ld->mapped_file = NULL; ld->mapped_file_sz = 0;
    if (ld->fd > 0) close(ld->fd);
    ld->fd = -1; 
}

static inline void
free_image(Image *img) {
    // TODO: free the texture if texture_id is not zero
    free_load_data(&(img->load_data));
}

GraphicsManager*
grman_free(GraphicsManager* self) {
    for (size_t i = 0; i < self->image_count; i++) free_image(self->images + i);
    free(self->images);
    Py_TYPE(self)->tp_free((PyObject*)self);
    return NULL;
}

static size_t internal_id_counter = 1;

static inline void*
ensure_space(void *array, size_t *capacity, size_t count, size_t item_size, bool initialize) {
    if (count < *capacity) return array;
    void *ans = realloc(array, (*capacity) * item_size * 2);
    if (ans == NULL) fatal("Out of memory re-allocating array.");
    if (initialize) {
        memset(((uint8_t*)array) + ((*capacity) * item_size), 0, ((*capacity) * item_size));
    }
    *capacity *= 2;
    return ans;
}

static inline Image*
find_or_create_image(GraphicsManager *self, uint32_t id, bool *existing) {
    if (id) {
        for (size_t i = 0; i < self->image_count; i++) {
            if (self->images[i].client_id == id) {
                *existing = true;
                return self->images + i;
            }
        }
    }
    *existing = false;
    self->images = ensure_space(self->images, &self->images_capacity, self->image_count, sizeof(Image), true);
    return self->images + self->image_count++;
}

static inline void
remove_from_array(void *array, size_t item_size, size_t idx, size_t array_count) {
    size_t num_to_right = array_count - 1 - idx;
    uint8_t *p = (uint8_t*)array;
    if (num_to_right > 0) memmove(p + (idx * item_size), p + ((idx + 1) * item_size), num_to_right * item_size);  
    memset(p + (item_size * (array_count - 1)), 0, item_size);
}

static inline void
remove_images(GraphicsManager *self, bool(*predicate)(Image*)) {
    for (size_t i = self->image_count; i-- > 0;) {
        if (predicate(self->images + i)) {
            free_image(self->images + i);
            remove_from_array(self->images, sizeof(Image), i, self->image_count--);
        }
    }
}

static inline Image*
img_by_internal_id(GraphicsManager *self, size_t id) {
    for (size_t i = 0; i < self->image_count; i++) {
        if (self->images[i].internal_id == id) return self->images + i;
    }
    return NULL;
}

static inline bool
inflate_zlib(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
    bool ok = false;
    z_stream z;
    uint8_t *decompressed = malloc(img->load_data.data_sz);
    if (decompressed == NULL) fatal("Out of memory allocating decompression buffer");
    z.zalloc = Z_NULL;
    z.zfree = Z_NULL;
    z.opaque = Z_NULL;
    z.avail_in = bufsz;
    z.next_in = (Bytef*)buf;
    z.avail_out = img->load_data.data_sz;
    z.next_out = decompressed;
    int ret;
    if ((ret = inflateInit(&z)) != Z_OK) {
        REPORT_ERROR("Failed to initialize inflate with error code: %d", ret);
        goto err;
    }
    if ((ret = inflate(&z, Z_FINISH)) != Z_STREAM_END) {
        REPORT_ERROR("Failed to inflate image data with error code: %d", ret);
        goto err;
    }
    if (z.avail_out) {
        REPORT_ERROR("Failed to inflate image data with error code: %d", ret);
        goto err;
    }
    free_load_data(&img->load_data);
    img->load_data.buf_capacity = img->load_data.data_sz;
    img->load_data.buf = decompressed;
    img->load_data.buf_used = img->load_data.data_sz - z.avail_out;
    ok = true;
err:
    inflateEnd(&z);
    if (!ok) free(decompressed);
    return ok;
}

struct fake_file { uint8_t *buf; size_t sz, cur; };

static void
read_png_from_buffer(png_structp png, png_bytep out, png_size_t length) {
    struct fake_file *f = png_get_io_ptr(png);
    if (f) {
        size_t amt = MIN(length, f->sz - f->cur);
        memcpy(out, f->buf + f->cur, amt);
        f->cur += amt;
    }
}

static inline bool
inflate_png(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
#define RE(...) { REPORT_ERROR(__VA_ARGS__); goto err; }
    bool ok = false;
    uint8_t *decompressed = NULL;
    png_structp png = NULL;
    png_infop info = NULL;
    png_bytep * row_pointers = NULL;
    struct fake_file f = {.buf = buf, .sz = bufsz};

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) RE("%s", "Failed to create PNG read structure");
    info = png_create_info_struct(png);
    if (!info) RE("%s", "Failed to create PNG info structure");

    if(setjmp(png_jmpbuf(png))) RE("%s", "Invalid PNG data");
    
    png_set_read_fn(png, &f, read_png_from_buffer);
    png_read_info(png, info);
    int width, height;
    png_byte color_type, bit_depth;
    width      = png_get_image_width(png, info);
    height     = png_get_image_height(png, info);
    color_type = png_get_color_type(png, info);
    bit_depth  = png_get_bit_depth(png, info);

    // Ensure we get RGBA data out of libpng
    if(bit_depth == 16) png_set_strip_16(png);
    if(color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
    if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);

    if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);

    // These color_type don't have an alpha channel then fill it with 0xff.
    if(color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if(color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    int rowbytes = png_get_rowbytes(png, info);
    size_t sz = rowbytes * height * sizeof(png_byte);
    decompressed = malloc(sz + 16);
    if (decompressed == NULL) RE("%s", "Out of memory allocating decompression buffer for PNG");
    row_pointers = malloc(height * sizeof(png_bytep));
    if (row_pointers == NULL) RE("%s", "Out of memory allocating row_pointers buffer for PNG");
    for (int i = 0; i < height; i++) row_pointers[height - 1 - i] = decompressed + i * rowbytes;
    png_read_image(png, row_pointers);

    free_load_data(&img->load_data);
    img->load_data.buf_capacity = sz; 
    img->load_data.buf = decompressed;
    img->load_data.buf_used = sz;
    img->width = width; img->height = height;
    ok = true;
err:
    if (png) png_destroy_read_struct(&png, info ? &info : NULL, NULL);
    if (!ok) free(decompressed);
    free(row_pointers);
    return ok;
#undef RE
}

static bool
add_trim_predicate(Image *img) {
    return !img->data_loaded || (!img->client_id && !img->refcnt);
}


static void
handle_add_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload) {
    bool existing, init_img = true;
    Image *img;
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    enum FORMATS { RGB=24, RGBA=32, PNG=100 };
    if (tt == 'd' && (g->more && self->loading_image)) init_img = false;
    if (init_img) {
        remove_images(self, add_trim_predicate);
        img = find_or_create_image(self, g->id, &existing);
        if (existing) {
            free_load_data(&img->load_data);
            img->data_loaded = false;
        } else {
            img->internal_id = internal_id_counter++;
            img->client_id = g->id;
        }
        img->width = g->data_width; img->height = g->data_height;
        size_t sz = img->width * img->height;
        switch(g->format) {
            case PNG:  
                sz *= 4;
                img->load_data.is_4byte_aligned = true;
                break;
            case RGB:
            case RGBA:
                sz *= g->format / 8;
                img->load_data.is_4byte_aligned = g->format == RGBA || (img->width % 4 == 0);
                break;
            default:
                REPORT_ERROR("Unknown image format: %u", g->format);
                return;
        }
        img->load_data.data_sz = sz;
        if (tt == 'd') {
            if (g->more) self->loading_image = img->internal_id;
            img->load_data.buf_capacity = sz + ((g->compressed || g->format == PNG) ? 1024 : 10);  // compression header
            img->load_data.buf = malloc(img->load_data.buf_capacity + 4);
            if (img->load_data.buf == NULL) fatal("Out of memory while allocating image load data buffer");
            img->load_data.buf_used = 0;
        }
    } else {
        img = img_by_internal_id(self, self->loading_image);
        if (img == NULL) {
            self->loading_image = 0;
            REPORT_ERROR("%s", "More payload loading refers to non-existent image");
            return;
        }
    }
    int fd;
    switch(tt) {
        case 'd':  // direct
            if (g->payload_sz >= img->load_data.buf_capacity - img->load_data.buf_used) {
                REPORT_ERROR("%s", "Too much data transmitted");
                return;
            }
            memcpy(img->load_data.buf + img->load_data.buf_used, payload, g->payload_sz);
            img->load_data.buf_used += g->payload_sz;
            if (!g->more) { img->data_loaded = true; self->loading_image = 0; }
            break;
        case 'f': // file
        case 't': // temporary file
        case 's': // POSIX shared memory
            if (tt == 's') fd = shm_open((const char*)payload, O_RDONLY, 0);
            else fd = open((const char*)payload, O_CLOEXEC | O_RDONLY);
            if (fd == -1) {
                REPORT_ERROR("Failed to open file for graphics transmission with error: [%d] %s", errno, strerror(errno));
                return;
            }
            img->load_data.fd = fd;
            img->data_loaded = mmap_img_file(self, img);
            if (tt == 't') unlink((const char*)payload);
            else if (tt == 's') shm_unlink((const char*)payload);
            break;
        default:
            REPORT_ERROR("Unknown transmission type: %c", g->transmission_type);
            return;
    }
    if (!img->data_loaded) return;
    bool needs_processing = g->compressed || g->format == PNG;
    if (needs_processing) {
        uint8_t *buf; size_t bufsz;
#define IB { if (img->load_data.buf) { buf = img->load_data.buf; bufsz = img->load_data.buf_used; } else { buf = img->load_data.mapped_file; bufsz = img->load_data.mapped_file_sz; } }
        switch(g->compressed) {
            case 'z':
                IB;
                if (!inflate_zlib(self, img, buf, bufsz)) {
                    img->data_loaded = false; return;
                }
                break;
            case 0:
                break;
            default:
                REPORT_ERROR("Unknown image compression: %c", g->compressed);
                img->data_loaded = false; return;
        }
        switch(g->format) {
            case PNG:
                IB;
                if (!inflate_png(self, img, buf, bufsz)) {
                    img->data_loaded = false; return;
                }
                break;
            default: break;
        }
#undef IB
        img->load_data.data = img->load_data.buf;
        if (img->load_data.buf_used < img->load_data.data_sz) {
            REPORT_ERROR("Insufficient image data: %zu < %zu", img->load_data.buf_used, img->load_data.data_sz);
            img->data_loaded = false;
        }
    } else {
        if (tt == 'd') {
            if (img->load_data.buf_used < img->load_data.data_sz) {
                REPORT_ERROR("Insufficient image data: %zu < %zu",  img->load_data.buf_used, img->load_data.data_sz);
                img->data_loaded = false;
            } else img->load_data.data = img->load_data.buf;
        } else {
            if (img->load_data.mapped_file_sz < img->load_data.data_sz) {
                REPORT_ERROR("Insufficient image data: %zu < %zu",  img->load_data.mapped_file_sz, img->load_data.data_sz);
                img->data_loaded = false;
            } else img->load_data.data = img->load_data.mapped_file;
        }
    }
}

void
grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload) {
    switch(g->action) {
        case 0:
        case 't':
            handle_add_command(self, g, payload);
            break;
        default:
            REPORT_ERROR("Unknown graphics command action: %c", g->action);
            break;
    }
}

void
grman_clear(GraphicsManager UNUSED *self) {
    // TODO: Implement this
}


// Boilerplate {{{
static PyObject *
new(PyTypeObject UNUSED *type, PyObject *args, PyObject UNUSED *kwds) {
    unsigned int lines, columns;
    if (!PyArg_ParseTuple(args, "II", &lines, &columns)) return NULL;
    PyObject *ans = (PyObject*)grman_realloc(NULL, lines, columns);
    if (ans == NULL) PyErr_NoMemory();
    return ans;
}

static void
dealloc(GraphicsManager* self) {
    grman_free(self);
}


PyTypeObject GraphicsManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.GraphicsManager",
    .tp_basicsize = sizeof(GraphicsManager),
    .tp_dealloc = (destructor)dealloc, 
    .tp_flags = Py_TPFLAGS_DEFAULT,        
    .tp_doc = "GraphicsManager",
    .tp_new = new,                
};

bool
init_graphics(PyObject *module) {
    if (PyType_Ready(&GraphicsManager_Type) < 0) return false;
    if (PyModule_AddObject(module, "GraphicsManager", (PyObject *)&GraphicsManager_Type) != 0) return false; 
    Py_INCREF(&GraphicsManager_Type);
    return true;
}
// }}}