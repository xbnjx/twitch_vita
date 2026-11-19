/* Kick Vita — async image cache (video thumbnails, channel avatars).
 * A worker thread downloads JPEG bytes for a caller-given URL; the main
 * thread decodes and uploads them as vita2d textures (GPU objects stay on
 * the render thread). Entries are keyed by an opaque string (video id,
 * channel id, ...) so the same URL is never re-queued while cached. */
#include "ui/thumbs.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/gxm.h>

#include <vita_https.h>
#include <webp/decode.h>
#include <jpeglib.h>
#include <jerror.h>

#include "app.h"
#include "log.h"

#define THUMB_SLOTS 16
#define THUMB_KEY_MAX 64
#define THUMB_URL_MAX 512
#define THUMB_MAX_JPEG (512 * 1024)

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_QUEUED,
    SLOT_DOWNLOADING,
    SLOT_DOWNLOADED,
    SLOT_READY,
    SLOT_FAILED
} SlotState;

typedef struct {
    char key[THUMB_KEY_MAX];
    char url[THUMB_URL_MAX];
    SlotState state;
    unsigned char *jpeg;
    size_t jpeg_len;
    vita2d_texture *texture;
    unsigned last_used;
    /* Failures used to be permanent AND self-pinning: the key-hit branch
     * below refreshes last_used before looking at the state, so a failed
     * slot stayed most-recently-used and could never even be evicted for a
     * fresh attempt. One dropped request left that thumbnail black for the
     * rest of the session. These two allow a bounded retry instead. */
    unsigned attempts;
    unsigned failed_at;
} ThumbSlot;

/* Two further tries, spaced far enough apart that a row redrawn every frame
 * does not turn a retry into a request flood. g_tick counts vt_thumbs_get()
 * calls, so this is "a few hundred rows drawn", not wall-clock. */
#define THUMB_MAX_ATTEMPTS 3
#define THUMB_RETRY_TICKS 600

static ThumbSlot g_slots[THUMB_SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker;
static volatile int g_running;
static volatile int g_cancel;
static unsigned g_tick;
static VitaHttpsClient *g_thumb_client;

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} JpegBuf;

static size_t jpeg_write(const void *data, size_t size, void *opaque) {
    JpegBuf *buf = opaque;
    if (buf->len + size > buf->cap) return 0;
    memcpy(buf->data + buf->len, data, size);
    buf->len += size;
    return size;
}

/* Normalises a thumbnail URL into something fetchable.
 *
 * 1. The TV client returns channel avatars PROTOCOL-RELATIVE — the parsed
 *    value really is "//yt3.googleusercontent.com/...", with no scheme
 *    (confirmed from the device log, 2026-08-28: 39 of 39 channels parsed an
 *    avatar and every request still failed). Video thumbnails come from a
 *    different host with a full https:// URL, which is why only the channel
 *    rows stayed empty. Prepend the scheme.
 * 2. googleusercontent encodes the wanted image format in the size suffix:
 *    "-rw" is WebP, "-rj" is JPEG, and vita2d decodes no WebP. The avatars
 *    observed so far already say "-rj", so this half is belt-and-braces. */
static void copy_url(char *dst, size_t cap, const char *url) {
    if (!url || !url[0]) {
        if (cap > 0) dst[0] = '\0';
        return;
    }
    char tmp[THUMB_URL_MAX];
    if (url[0] == '/' && url[1] == '/')
        snprintf(tmp, sizeof tmp, "https:%s", url);
    else
        snprintf(tmp, sizeof tmp, "%s", url);

    /* Replace template tokens: {width} -> 440, %{width} -> 440, {height} -> 248, %{height} -> 248 */
    char buf[THUMB_URL_MAX];
    char *src = tmp;
    char *out = buf;
    size_t rem = sizeof(buf) - 1;
    int replaced = 0;

    while (*src && rem > 0) {
        if (strncmp(src, "%{width}", 8) == 0) {
            int n = snprintf(out, rem, "440");
            out += n; rem -= n; src += 8; replaced = 1;
        } else if (strncmp(src, "{width}", 7) == 0) {
            int n = snprintf(out, rem, "440");
            out += n; rem -= n; src += 7; replaced = 1;
        } else if (strncmp(src, "%{height}", 9) == 0) {
            int n = snprintf(out, rem, "248");
            out += n; rem -= n; src += 9; replaced = 1;
        } else if (strncmp(src, "{height}", 8) == 0) {
            int n = snprintf(out, rem, "248");
            out += n; rem -= n; src += 8; replaced = 1;
        } else {
            *out++ = *src++;
            rem--;
        }
    }
    *out = '\0';

    snprintf(dst, cap, "%s", replaced ? buf : tmp);
}

struct custom_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void custom_jpeg_error_exit(j_common_ptr cinfo) {
    struct custom_jpeg_error_mgr *myerr = (struct custom_jpeg_error_mgr *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

/* Custom JPEG decoder that works with any standard JPEG (including SOI + DQT without APP0) */
static vita2d_texture *load_jpeg_buffer(const void *buf, size_t len) {
    if (!buf || len < 4) return NULL;
    const unsigned char *b = (const unsigned char *)buf;
    if (b[0] != 0xFF || b[1] != 0xD8) return NULL;

    struct jpeg_decompress_struct cinfo;
    struct custom_jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = custom_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)buf, len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    /* Downscale if larger than 480x270 to save Vita GPU/RAM */
    if (cinfo.image_width > 480 || cinfo.image_height > 270) {
        cinfo.scale_num = 1;
        cinfo.scale_denom = 2;
    }

    jpeg_start_decompress(&cinfo);

    SceGxmTextureFormat format = (cinfo.output_components == 1) ?
        SCE_GXM_TEXTURE_FORMAT_U8_R111 :
        SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;

    vita2d_texture *tex = vita2d_create_empty_texture_format(
        cinfo.output_width, cinfo.output_height, format);

    if (!tex) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    unsigned char *datap = (unsigned char *)vita2d_texture_get_datap(tex);
    int stride = vita2d_texture_get_stride(tex);

    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *row_ptr = datap + cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return tex;
}

static vita2d_texture *load_image_buffer(const void *buf, size_t len) {
    if (!buf || len < 4) return NULL;

    /* Check WebP (RIFF....WEBP) */
    if (len >= 12 && memcmp(buf, "RIFF", 4) == 0 && memcmp((const char *)buf + 8, "WEBP", 4) == 0) {
        int orig_w = 0, orig_h = 0;
        if (WebPGetInfo((const uint8_t *)buf, len, &orig_w, &orig_h) && orig_w > 0 && orig_h > 0) {
            int w = orig_w, h = orig_h;
            /* Downscale to max 240x135 for Vita thumbnails to preserve GPU memory */
            const int max_w = 240, max_h = 135;
            if (w > max_w || h > max_h) {
                float scale_w = (float)max_w / orig_w;
                float scale_h = (float)max_h / orig_h;
                float s = scale_w < scale_h ? scale_w : scale_h;
                w = (int)(orig_w * s);
                h = (int)(orig_h * s);
                if (w < 1) w = 1;
                if (h < 1) h = 1;
            }

            WebPDecoderConfig config;
            if (WebPInitDecoderConfig(&config)) {
                config.options.use_scaling = 1;
                config.options.scaled_width = w;
                config.options.scaled_height = h;

                vita2d_texture *tex = vita2d_create_empty_texture(w, h);
                if (tex) {
                    void *datap = vita2d_texture_get_datap(tex);
                    int stride = vita2d_texture_get_stride(tex);

                    config.output.colorspace = MODE_RGBA;
                    config.output.u.RGBA.rgba = (uint8_t *)datap;
                    config.output.u.RGBA.stride = stride;
                    config.output.u.RGBA.size = stride * h;
                    config.output.is_external_memory = 1;

                    if (WebPDecode((const uint8_t *)buf, len, &config) == VP8_STATUS_OK) {
                        WebPFreeDecBuffer(&config.output);
                        return tex;
                    }
                    vita2d_free_texture(tex);
                }
                WebPFreeDecBuffer(&config.output);
            }
        }
        return NULL;
    }

    /* Check PNG */
    if (len >= 8 && memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return vita2d_load_PNG_buffer(buf);
    }

    /* Check JPEG: try custom libjpeg first (handles FF D8 FF DB and non-APP0), fallback to vita2d */
    if (len >= 2 && ((const unsigned char *)buf)[0] == 0xFF && ((const unsigned char *)buf)[1] == 0xD8) {
        vita2d_texture *tex = load_jpeg_buffer(buf, len);
        if (tex) return tex;
    }

    /* Fallback to default */
    return vita2d_load_JPEG_buffer(buf, len);
}

static int download_jpeg(const char *url, unsigned char **out, size_t *out_len) {
    JpegBuf buf = { malloc(THUMB_MAX_JPEG), 0, THUMB_MAX_JPEG };
    if (!buf.data) return -1;
    VitaHttpsRequest request = {
        .url = url,
        .write = jpeg_write,
        .write_opaque = &buf,
        .cancel_flag = &g_cancel,
    };
    VitaHttpsResponse response = {0};
    int ret = vita_https_perform(g_thumb_client, &request, &response);
    if (ret < 0 || response.status_code != 200 || buf.len == 0) {
        free(buf.data);
        return -1;
    }
    /* A typical thumbnail is 10-25 KiB; keeping the whole 256 KiB download
     * buffer pinned until the main thread pumps it (which it does not during
     * playback) parked up to 12 MiB of idle heap next to the decoder. */
    unsigned char *shrunk = realloc(buf.data, buf.len);
    if (shrunk) buf.data = shrunk;
    *out = buf.data;
    *out_len = buf.len;
    return 0;
}

static void *worker_main(void *opaque) {
    (void)opaque;
    while (g_running) {
        char url[THUMB_URL_MAX] = {0};
        char key[THUMB_KEY_MAX] = {0};
        int slot_index = -1;
        pthread_mutex_lock(&g_lock);
        /* Most recently drawn first: that is what is on screen now. Queue
         * order was request order, so after a fast scroll the visible rows
         * waited behind every row scrolled past, each a full fetch. */
        for (int i = 0; i < THUMB_SLOTS; i++) {
            if (g_slots[i].state != SLOT_QUEUED) continue;
            if (slot_index < 0 ||
                g_slots[i].last_used > g_slots[slot_index].last_used)
                slot_index = i;
        }
        if (slot_index >= 0) {
            g_slots[slot_index].state = SLOT_DOWNLOADING;
            g_slots[slot_index].attempts++;
            memcpy(url, g_slots[slot_index].url, sizeof url);
            memcpy(key, g_slots[slot_index].key, sizeof key);
        }
        pthread_mutex_unlock(&g_lock);
        if (slot_index < 0) {
            sceKernelDelayThread(30000);
            continue;
        }
        unsigned char *jpeg = NULL;
        size_t len = 0;
        int ret = download_jpeg(url, &jpeg, &len);
        pthread_mutex_lock(&g_lock);
        ThumbSlot *slot = &g_slots[slot_index];
        /* the slot may have been recycled for a different key while
         * downloading */
        if (slot->state == SLOT_DOWNLOADING &&
            strcmp(slot->key, key) == 0) {
            if (ret == 0) {
                slot->jpeg = jpeg;
                slot->jpeg_len = len;
                slot->state = SLOT_DOWNLOADED;
                jpeg = NULL;
            } else {
                slot->state = SLOT_FAILED;
                slot->failed_at = g_tick;
            }
        }
        pthread_mutex_unlock(&g_lock);
        /* Logged after the unlock: vt_log() writes to the memory card, and
         * holding g_lock across that would stall the worker and pair this
         * lock with the I/O lock. Download and decode failures were
         * indistinguishable before — both just left a black square. */
        if (ret != 0) vt_log("thumb download failed: %.150s", url);
        free(jpeg);
    }
    return NULL;
}

int vt_thumbs_init(void) {
    VitaHttpsClientConfig config = {
        .user_agent = "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko)",
        .connect_timeout_ms = 8000,
        .request_timeout_ms = 15000,
    };
    g_thumb_client = vita_https_client_create(&config);
    if (!g_thumb_client) return -1;
    g_cancel = 0;
    g_running = 1;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    int rc = pthread_create(&g_worker, &attr, worker_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        g_running = 0;
        return -1;
    }
    return 0;
}

/* Safe to call while the worker is mid-download: a slot in SLOT_DOWNLOADING
 * owns no jpeg or texture yet (recycle_slot freed both before queueing), and
 * the worker re-checks state+key under the lock before storing its result —
 * a cleared slot fails that check, so the finished download is discarded and
 * freed by the worker itself. Main thread only: it frees GPU textures. */
void vt_thumbs_clear(void) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < THUMB_SLOTS; i++) {
        free(g_slots[i].jpeg);
        if (g_slots[i].texture) vita2d_free_texture(g_slots[i].texture);
        memset(&g_slots[i], 0, sizeof g_slots[i]);
    }
    pthread_mutex_unlock(&g_lock);
}

void vt_thumbs_term(void) {
    g_cancel = 1;
    g_running = 0;
    pthread_join(g_worker, NULL);
    vt_thumbs_clear();
    if (g_thumb_client) vita_https_client_destroy(g_thumb_client);
    g_thumb_client = NULL;
}

static void recycle_slot(ThumbSlot *slot, const char *key, const char *url) {
    free(slot->jpeg);
    slot->jpeg = NULL;
    slot->jpeg_len = 0;
    if (slot->texture) {
        vita2d_free_texture(slot->texture);
        slot->texture = NULL;
    }
    snprintf(slot->key, sizeof slot->key, "%s", key);
    copy_url(slot->url, sizeof slot->url, url);
    slot->state = SLOT_QUEUED;
    slot->last_used = g_tick;
    slot->attempts = 0;
    slot->failed_at = 0;
}

vita2d_texture *vt_thumbs_get(const char *key, const char *url) {
    if (!key || !key[0] || !url || !url[0]) return NULL;
    vita2d_texture *texture = NULL;
    pthread_mutex_lock(&g_lock);
    g_tick++;
    ThumbSlot *victim = NULL;
    unsigned oldest = 0xFFFFFFFFu;
    int found = 0;
    for (int i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot *slot = &g_slots[i];
        if (slot->key[0] && strcmp(slot->key, key) == 0) {
            slot->last_used = g_tick;
            if (slot->state == SLOT_READY) texture = slot->texture;
            /* Give a failed slot another go: the causes seen in practice —
             * a dropped request, a format the decoder rejects — are not all
             * permanent, and without this the row can never recover. */
            else if (slot->state == SLOT_FAILED &&
                     slot->attempts < THUMB_MAX_ATTEMPTS &&
                     g_tick - slot->failed_at > THUMB_RETRY_TICKS)
                slot->state = SLOT_QUEUED;
            found = 1;
            break;
        }
        /* don't steal a slot the worker is writing to */
        if (slot->state != SLOT_DOWNLOADING && slot->last_used < oldest) {
            oldest = slot->last_used;
            victim = slot;
        }
    }
    if (!found && victim) recycle_slot(victim, key, url);
    pthread_mutex_unlock(&g_lock);
    return texture;
}

void vt_thumbs_pump(void) {
    int uploads = 0;
    /* Details of one decode failure, carried out of the locked region so the
     * card write below cannot stall the worker. The first four bytes are the
     * decisive part: "46464952" is RIFF, i.e. a WebP the decoder will never
     * accept; "E0FFD8FF" is a real JPEG it rejected anyway, which would point
     * at the texture allocation rather than the format. */
    char failed_url[THUMB_URL_MAX] = {0};
    unsigned magic = 0, failed_len = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < THUMB_SLOTS && uploads < 2; i++) {
        ThumbSlot *slot = &g_slots[i];
        if (slot->state != SLOT_DOWNLOADED) continue;
        slot->texture = load_image_buffer(slot->jpeg, slot->jpeg_len);
        if (!slot->texture && !failed_url[0]) {
            snprintf(failed_url, sizeof failed_url, "%s", slot->url);
            failed_len = (unsigned)slot->jpeg_len;
            if (slot->jpeg_len >= 4) memcpy(&magic, slot->jpeg, 4);
        }
        free(slot->jpeg);
        slot->jpeg = NULL;
        slot->jpeg_len = 0;
        if (slot->texture) {
            slot->state = SLOT_READY;
        } else {
            slot->state = SLOT_FAILED;
            slot->failed_at = g_tick;
        }
        uploads++;
    }
    pthread_mutex_unlock(&g_lock);
    if (failed_url[0])
        vt_log("thumb decode failed: len=%u magic=%08X %.130s", failed_len,
               magic, failed_url);
}
