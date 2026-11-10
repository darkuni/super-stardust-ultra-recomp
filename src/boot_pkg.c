/* See boot_pkg.h. Follows scripts/pkg_extract.py step by step:
 *   ENTRY_KEYS[3] --RSA(PkgDerivedKey3)--> dk3
 *   sha256(IMAGE_KEY entry header || dk3) --> iv|key; AES-CBC(IMAGE_KEY) --RSA(Fake)--> ekpfs
 *   HMAC-SHA256(ekpfs, 1 || pfs seed) --> tweak key | data key
 *   outer PFS image: AES-XTS, 0x1000 sectors -> a PFSC (zlib, fixed-size blocks) holding the inner PFS
 *   inner PFS: header, 0xA8-byte inodes, dirents; superroot -> uroot -> game files. */
#include "boot_pkg.h"
#include "boot_crypto.h"
#include "boot_keys.h"
#include "boot_util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define XTS_SECTOR 0x1000u
#define INODE_SIZE 0xA8u
enum { PFS_FILE = 2, PFS_DIR = 3, PFS_CUR = 4, PFS_PARENT = 5 };

typedef struct {
    uint32_t id, flags1, flags2, offset, size;
    uint8_t raw[32];
} Entry;
typedef struct {
    uint16_t mode;
    uint32_t flags, blocks, loc;
    uint64_t size;
} Inode;
typedef struct {
    FILE *f;
    char content_id[37];
    uint64_t pfs_offset, pfs_size;
    Entry *entries;
    uint32_t entry_count;
    uint8_t dk3[32];
    BootAes128 data_key, tweak_key;
    uint64_t pfsc_offset, block_size, num_blocks;
    uint64_t *sector_map;
    Inode *inodes;
    size_t inode_count;
    uint64_t superroot;
    char *err;
    size_t err_size;
} Pkg;

static int fail(Pkg *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int fail(Pkg *p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->err, p->err_size, fmt, args);
    va_end(args);
    return -1;
}
static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint64_t be64(const uint8_t *b) { return (uint64_t)be32(b) << 32 | be32(b + 4); }
static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint64_t le64(const uint8_t *b) { return (uint64_t)le32(b) | (uint64_t)le32(b + 4) << 32; }

static int read_raw(Pkg *p, uint64_t offset, void *out, size_t size) {
    if (boot_seek(p->f, offset) || fread(out, 1, size, p->f) != size)
        return fail(p, "the PKG is shorter than its header says (incomplete download?)");
    return 0;
}
static const Entry *entry(const Pkg *p, uint32_t id) {
    for (uint32_t i = 0; i < p->entry_count; ++i)
        if (p->entries[i].id == id) return &p->entries[i];
    return NULL;
}
static uint8_t *entry_data(Pkg *p, const Entry *e) {
    uint8_t *data = malloc(e->size ? e->size : 1);
    if (data && read_raw(p, e->offset, data, e->size)) { free(data); return NULL; }
    return data;
}
static void entry_ivkey(const Pkg *p, const Entry *e, uint8_t ivkey[32]) {
    uint8_t buf[64];
    memcpy(buf, e->raw, 32);
    memcpy(buf + 32, p->dk3, 32);
    boot_sha256(buf, 64, ivkey);
}
/* An entry's bytes, AES-CBC decrypted when flagged (sce_sys files such as license.dat). */
static uint8_t *decrypt_entry(Pkg *p, const Entry *e) {
    uint8_t *data = entry_data(p, e);
    if (data && (e->flags1 & 0x80000000u)) {
        uint8_t ivkey[32];
        entry_ivkey(p, e, ivkey);
        boot_aes128_cbc_decrypt(ivkey + 16, ivkey, data, e->size & ~15u);
    }
    return data;
}

static int open_pkg(Pkg *p, const char *path) {
    p->f = boot_fopen(path, "rb");
    if (!p->f) return fail(p, "cannot open %s", path);
    uint8_t h[0x1000];
    if (read_raw(p, 0, h, sizeof(h))) return -1;
    if (be32(h) != 0x7F434E54) return fail(p, "not a PS4 PKG");
    memcpy(p->content_id, h + 0x40, 36);
    p->content_id[36] = 0;
    p->entry_count = be32(h + 0x10);
    uint32_t table = be32(h + 0x18);
    p->pfs_offset = be64(h + 0x410);
    p->pfs_size = be64(h + 0x418);
    if (!p->entry_count || p->entry_count > 4096) return fail(p, "bad PKG entry table");
    p->entries = calloc(p->entry_count, sizeof(Entry));
    uint8_t *raw = malloc((size_t)p->entry_count * 32);
    if (!p->entries || !raw || read_raw(p, table, raw, (size_t)p->entry_count * 32)) { free(raw); return -1; }
    for (uint32_t i = 0; i < p->entry_count; ++i) {
        Entry *e = &p->entries[i];
        const uint8_t *r = raw + i * 32;
        e->id = be32(r); e->flags1 = be32(r + 8); e->flags2 = be32(r + 12); e->offset = be32(r + 16); e->size = be32(r + 20);
        memcpy(e->raw, r, 32);
    }
    free(raw);
    return 0;
}

static int derive_keys(Pkg *p) {
    const Entry *ek = entry(p, 0x10), *ik = entry(p, 0x20);
    if (!ek || !ik || ek->size < 32 + 7 * 32 + 4 * 256 || ik->size != 256) return fail(p, "PKG without its key entries");
    if (((ik->flags2 >> 12) & 0xF) != 3)
        return fail(p, "this is a retail PKG (image key %u); only fake PKGs can be unpacked", (ik->flags2 >> 12) & 0xF);
    uint8_t *keys = entry_data(p, ek);
    if (!keys) return -1;
    int n = boot_rsa2048_decrypt(BOOT_PKGDERIVEDKEY3KEYSET_N, BOOT_PKGDERIVEDKEY3KEYSET_D, keys + 32 + 7 * 32 + 3 * 256,
                                 p->dk3, sizeof(p->dk3));
    free(keys);
    if (n < 32) return fail(p, "PKG key check failed (not a fake PKG?)");
    uint8_t ivkey[32];
    entry_ivkey(p, ik, ivkey);
    uint8_t *image_key = entry_data(p, ik);
    if (!image_key) return -1;
    boot_aes128_cbc_decrypt(ivkey + 16, ivkey, image_key, 256);
    uint8_t ekpfs[32];
    n = boot_rsa2048_decrypt(BOOT_FAKEKEYSET_N, BOOT_FAKEKEYSET_D, image_key, ekpfs, sizeof(ekpfs));
    free(image_key);
    if (n < 32) return fail(p, "PKG image key check failed (not a fake PKG?)");
    uint8_t seed[20] = {1, 0, 0, 0}, d[32];
    if (read_raw(p, p->pfs_offset + 0x370, seed + 4, 16)) return -1;
    boot_hmac_sha256(ekpfs, 32, seed, sizeof(seed), d);
    boot_aes128_init(&p->tweak_key, d);
    boot_aes128_init(&p->data_key, d + 16);
    return 0;
}

/* Outer PFS image bytes [offset, offset+size): AES-XTS sectors numbered from the image start. */
static int read_pfs(Pkg *p, uint64_t offset, uint8_t *out, size_t size) {
    uint64_t start = offset & ~(uint64_t)(XTS_SECTOR - 1);
    uint64_t end = (offset + size + XTS_SECTOR - 1) & ~(uint64_t)(XTS_SECTOR - 1);
    size_t span = (size_t)(end - start);
    uint8_t *buf = malloc(span);
    if (!buf) return fail(p, "out of memory");
    if (read_raw(p, p->pfs_offset + start, buf, span)) { free(buf); return -1; }
    for (size_t i = 0; i < span; i += XTS_SECTOR)
        boot_aes128_xts_decrypt(&p->data_key, &p->tweak_key, start / XTS_SECTOR + i / XTS_SECTOR, buf + i, XTS_SECTOR);
    memcpy(out, buf + (offset - start), size);
    free(buf);
    return 0;
}

static int open_pfsc(Pkg *p) {
    p->pfsc_offset = 0;
    uint64_t limit = p->pfs_size < 0x4000000 ? p->pfs_size : 0x4000000;
    for (uint64_t o = 0x20000; o < limit; o += 0x10000) {
        uint8_t magic[4];
        if (read_pfs(p, o, magic, 4)) return -1;
        if (!memcmp(magic, "PFSC", 4)) { p->pfsc_offset = o; break; }
    }
    if (!p->pfsc_offset) return fail(p, "PFSC image not found in the PKG (wrong keys?)");
    uint8_t h[0x30];
    if (read_pfs(p, p->pfsc_offset, h, sizeof(h))) return -1;
    p->block_size = le64(h + 0x10);
    uint64_t block_offsets = le64(h + 0x18), data_length = le64(h + 0x28);
    if (!p->block_size || p->block_size > (1u << 24)) return fail(p, "bad PFSC block size");
    p->num_blocks = data_length / p->block_size;
    size_t map_size = (size_t)(p->num_blocks + 1) * 8;
    uint8_t *raw = malloc(map_size);
    p->sector_map = malloc(map_size);
    if (!raw || !p->sector_map || read_pfs(p, p->pfsc_offset + block_offsets, raw, map_size)) { free(raw); return -1; }
    for (uint64_t i = 0; i <= p->num_blocks; ++i) p->sector_map[i] = le64(raw + i * 8);
    free(raw);
    return 0;
}

/* Inner PFS blocks [first, first+count), decompressed, into out (count*block_size bytes). */
static int read_blocks(Pkg *p, uint64_t first, uint64_t count, uint8_t *out) {
    if (first + count > p->num_blocks) return fail(p, "PFS block %llu outside the image", (unsigned long long)(first + count));
    uint64_t lo = p->sector_map[first], hi = p->sector_map[first + count];
    if (hi < lo) return fail(p, "bad PFSC block map");
    uint8_t *comp = malloc((size_t)(hi - lo) ? (size_t)(hi - lo) : 1);
    if (!comp) return fail(p, "out of memory");
    if (read_pfs(p, p->pfsc_offset + lo, comp, (size_t)(hi - lo))) { free(comp); return -1; }
    for (uint64_t b = 0; b < count; ++b) {
        uint64_t s = p->sector_map[first + b] - lo, e = p->sector_map[first + b + 1] - lo;
        uint8_t *dst = out + b * p->block_size;
        if (e - s == p->block_size) {
            memcpy(dst, comp + s, (size_t)p->block_size);
            continue;
        }
        uLongf n = (uLongf)p->block_size;
        if (uncompress(dst, &n, comp + s, (uLong)(e - s)) != Z_OK) { free(comp); return fail(p, "PFS block %llu does not decompress", (unsigned long long)(first + b)); }
        memset(dst + n, 0, (size_t)(p->block_size - n));
    }
    free(comp);
    return 0;
}

static int load_fs(Pkg *p) {
    uint8_t *h = malloc((size_t)p->block_size);
    if (!h) return fail(p, "out of memory");
    if (read_blocks(p, 0, 1, h)) { free(h); return -1; }
    uint16_t mode = le16(h + 0x1C);
    uint32_t fs_block = le32(h + 0x20);
    uint64_t dinode_count = le64(h + 0x30), dinode_blocks = le64(h + 0x40);
    p->superroot = le64(h + 0x48);
    free(h);
    if (fs_block != p->block_size) return fail(p, "unexpected PFS block size %#x", fs_block);
    if (mode & 3) return fail(p, "unexpected PFS mode %#x", mode);
    if (!dinode_blocks || dinode_blocks > 4096) return fail(p, "bad PFS inode table");
    uint8_t *data = malloc((size_t)(dinode_blocks * p->block_size));
    if (!data) return fail(p, "out of memory");
    if (read_blocks(p, 1, dinode_blocks, data)) { free(data); return -1; }
    size_t per_block = (size_t)(p->block_size / INODE_SIZE);
    p->inodes = calloc((size_t)dinode_blocks * per_block, sizeof(Inode));
    if (!p->inodes) { free(data); return fail(p, "out of memory"); }
    for (uint64_t b = 0; b < dinode_blocks; ++b) {
        const uint8_t *blk = data + b * p->block_size;
        for (size_t i = 0; i < per_block; ++i) {
            const uint8_t *r = blk + i * INODE_SIZE;
            uint16_t m = le16(r);
            if (!m) break;
            Inode *n = &p->inodes[p->inode_count++];
            n->mode = m; n->flags = le32(r + 4); n->size = le64(r + 8); n->blocks = le32(r + 0x60); n->loc = le32(r + 0x64);
        }
    }
    free(data);
    if (p->inode_count < dinode_count) return fail(p, "read %zu of %llu PFS inodes", p->inode_count, (unsigned long long)dinode_count);
    return 0;
}

typedef struct { char name[256]; uint32_t ino; int type; } Dirent;
static int read_dir(Pkg *p, uint64_t ino, Dirent **out, size_t *count) {
    *out = NULL; *count = 0;
    if (ino >= p->inode_count) return fail(p, "bad PFS directory inode");
    const Inode *node = &p->inodes[ino];
    if (!node->blocks || node->blocks > 4096) return fail(p, "bad PFS directory size");
    uint8_t *data = malloc((size_t)(node->blocks * p->block_size));
    if (!data) return fail(p, "out of memory");
    if (read_blocks(p, node->loc, node->blocks, data)) { free(data); return -1; }
    size_t cap = 0;
    for (uint64_t b = 0; b < node->blocks; ++b) {
        size_t pos = (size_t)(b * p->block_size), end = (size_t)((b + 1) * p->block_size);
        while (pos + 16 <= end) {
            int32_t d_ino = (int32_t)le32(data + pos), d_type = (int32_t)le32(data + pos + 4);
            int32_t nlen = (int32_t)le32(data + pos + 8), esize = (int32_t)le32(data + pos + 12);
            if (!d_ino || esize <= 0) break;
            if (nlen < 0 || nlen > 255 || pos + 16 + (size_t)nlen > end) { free(data); return fail(p, "bad PFS directory entry"); }
            if (*count == cap) {
                cap = cap ? cap * 2 : 32;
                Dirent *grown = realloc(*out, cap * sizeof(Dirent));
                if (!grown) { free(data); return fail(p, "out of memory"); }
                *out = grown;
            }
            Dirent *d = &(*out)[(*count)++];
            memcpy(d->name, data + pos + 16, (size_t)nlen);
            d->name[nlen] = 0;
            d->ino = (uint32_t)d_ino;
            d->type = d_type;
            pos += (size_t)esize;
        }
    }
    free(data);
    return 0;
}

static void close_pkg(Pkg *p) {
    if (p->f) fclose(p->f);
    free(p->entries); free(p->sector_map); free(p->inodes);
}
static int open_all(Pkg *p, const char *path, char *err, size_t err_size) {
    memset(p, 0, sizeof(*p));
    p->err = err; p->err_size = err_size;
    if (open_pkg(p, path) || derive_keys(p) || open_pfsc(p) || load_fs(p)) { close_pkg(p); return -1; }
    return 0;
}

static void sfo_value(const uint8_t *sfo, size_t size, const char *key, char *out, size_t out_size) {
    out[0] = 0;
    if (size < 20 || le32(sfo) != 0x46535000) return;
    uint32_t keys = le32(sfo + 8), values = le32(sfo + 12), count = le32(sfo + 16);
    for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= size; ++i) {
        const uint8_t *r = sfo + 20 + i * 16;
        uint32_t koff = le16(r), fmt = le16(r + 2), len = le32(r + 4), doff = le32(r + 12);
        if (keys + koff >= size || fmt == 0x404) continue;
        if (strncmp((const char *)sfo + keys + koff, key, size - keys - koff)) continue;
        if (values + doff + len > size) return;
        size_t n = len < out_size - 1 ? len : out_size - 1;
        memcpy(out, sfo + values + doff, n);
        out[n] = 0;
        return;
    }
}

int boot_pkg_info(const char *path, BootPkgInfo *info, char *err, size_t err_size) {
    memset(info, 0, sizeof(*info));
    Pkg p;
    memset(&p, 0, sizeof(p));
    p.err = err; p.err_size = err_size;
    if (open_pkg(&p, path)) { close_pkg(&p); return -1; }
    snprintf(info->content_id, sizeof(info->content_id), "%s", p.content_id);
    const Entry *e = entry(&p, 0x1000);
    int r = 0;
    if (!e || (e->flags1 & 0x80000000u)) r = fail(&p, "PKG without a readable param.sfo");
    else {
        uint8_t *sfo = entry_data(&p, e);
        if (!sfo) r = -1;
        else {
            sfo_value(sfo, e->size, "TITLE_ID", info->title_id, sizeof(info->title_id));
            sfo_value(sfo, e->size, "CATEGORY", info->category, sizeof(info->category));
            sfo_value(sfo, e->size, "APP_VER", info->app_ver, sizeof(info->app_ver));
            sfo_value(sfo, e->size, "TITLE", info->title, sizeof(info->title));
            free(sfo);
        }
    }
    close_pkg(&p);
    return r;
}

/* PKG metadata entries copied to sce_sys, as shadPS4 and pkg_extract.py name them. */
static const char *entry_name(uint32_t id, char *buf, size_t size) {
    static const struct { uint32_t id; const char *name; } names[] = {
        {0x0400, "license.dat"}, {0x0401, "license.info"}, {0x0402, "nptitle.dat"}, {0x0403, "npbind.dat"},
        {0x0404, "selfinfo.dat"}, {0x0406, "imageinfo.dat"}, {0x0407, "target-deltainfo.dat"},
        {0x0408, "origin-deltainfo.dat"}, {0x0409, "psreserved.dat"}, {0x1000, "param.sfo"},
        {0x1001, "playgo-chunk.dat"}, {0x1002, "playgo-chunk.sha"}, {0x1003, "playgo-manifest.xml"},
        {0x1004, "pronunciation.xml"}, {0x1005, "pronunciation.sig"}, {0x1006, "pic1.png"},
        {0x1007, "pubtoolinfo.dat"}, {0x1008, "app/playgo-chunk.dat"}, {0x1009, "app/playgo-chunk.sha"},
        {0x100A, "app/playgo-manifest.xml"}, {0x100B, "shareparam.json"}, {0x100C, "shareoverlayimage.png"},
        {0x100D, "save_data.png"}, {0x100E, "shareprivacyguardimage.png"}, {0x1200, "icon0.png"},
        {0x1220, "pic0.png"}, {0x1240, "snd0.at9"}, {0x1280, "icon0.dds"}, {0x12A0, "pic0.dds"},
        {0x12C0, "pic1.dds"}, {0x1400, "trophy/trophy00.trp"}};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (names[i].id == id) return names[i].name;
    snprintf(buf, size, "%u", id);
    return buf;
}

/* A file name from the PKG may not leave the output folder. */
static int safe_name(const char *rel) {
    if (!*rel || rel[0] == '/' || rel[0] == '\\' || strchr(rel, ':')) return 0;
    for (const char *s = rel; *s;) {
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') ++e;
        if ((e - s == 2 && s[0] == '.' && s[1] == '.') || (e - s == 1 && s[0] == '.') || e == s) return 0;
        s = *e ? e + 1 : e;
    }
    return 1;
}

typedef struct { char rel[1024]; uint32_t ino; int type; } Item;
typedef struct { Item *items; size_t count, cap; } Items;
static int walk(Pkg *p, uint64_t ino, const char *prefix, Items *out, int depth) {
    if (depth > 32) return fail(p, "PFS folders nested too deep");
    Dirent *list;
    size_t count;
    if (read_dir(p, ino, &list, &count)) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (list[i].type == PFS_CUR || list[i].type == PFS_PARENT) continue;
        if (list[i].type != PFS_FILE && list[i].type != PFS_DIR) continue;
        if (out->count == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            Item *grown = realloc(out->items, out->cap * sizeof(Item));
            if (!grown) { free(list); return fail(p, "out of memory"); }
            out->items = grown;
        }
        Item *it = &out->items[out->count++];
        if (!boot_path(it->rel, sizeof(it->rel), "%s%s%s", prefix, *prefix ? "/" : "", list[i].name) || !safe_name(it->rel)) {
            free(list);
            return fail(p, "unsafe file name in the PKG: %s", list[i].name);
        }
        it->ino = list[i].ino;
        it->type = list[i].type;
        if (it->type == PFS_DIR) {
            char rel[1024];
            memcpy(rel, it->rel, sizeof(rel));
            if (walk(p, it->ino, rel, out, depth + 1)) { free(list); return -1; }
        }
    }
    free(list);
    return 0;
}
static int list_files(Pkg *p, Items *items) {
    Dirent *root;
    size_t count;
    memset(items, 0, sizeof(*items));
    if (read_dir(p, p->superroot, &root, &count)) return -1;
    int64_t uroot = -1;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(root[i].name, "uroot")) uroot = root[i].ino;
    free(root);
    if (uroot < 0) return fail(p, "no uroot in the PKG's file system");
    if (walk(p, (uint64_t)uroot, "", items, 0)) { free(items->items); return -1; }
    return 0;
}

uint64_t boot_pkg_payload_size(const char *path) {
    char err[256];
    Pkg p;
    if (open_all(&p, path, err, sizeof(err))) return 0;
    Items items;
    uint64_t total = 0;
    if (!list_files(&p, &items)) {
        for (size_t i = 0; i < items.count; ++i)
            if (items.items[i].type == PFS_FILE && items.items[i].ino < p.inode_count) total += p.inodes[items.items[i].ino].size;
        free(items.items);
    }
    close_pkg(&p);
    return total;
}

static int join(char *out, size_t size, const char *dir, const char *rel) {
    if (!boot_path(out, size, "%s/%s", dir, rel)) return 0;
#ifdef _WIN32
    for (char *c = out; *c; ++c) if (*c == '/') *c = '\\';
#endif
    return 1;
}
static void parent_dir(const char *path, char *out, size_t size) {
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/'), *back = strrchr(out, '\\');
    if (back > slash) slash = back;
    if (slash) *slash = 0;
}

int boot_pkg_extract(const char *path, const char *out_dir, BootPkgProgress progress, void *ctx, char *err, size_t err_size) {
    Pkg p;
    if (open_all(&p, path, err, err_size)) return -1;
    Items items;
    if (list_files(&p, &items)) { close_pkg(&p); return -1; }
    uint64_t total = 0, done = 0;
    for (size_t i = 0; i < items.count; ++i)
        if (items.items[i].type == PFS_FILE) {
            if (items.items[i].ino >= p.inode_count) { free(items.items); close_pkg(&p); return fail(&p, "bad PFS file inode"); }
            total += p.inodes[items.items[i].ino].size;
        }
    char dst[4096], dir[4096], tmp[4200], name[32];
    int r = 0;
    /* PKG metadata entries -> sce_sys (as shadPS4 does); the PFS copy wins where both exist. */
    if (!join(dir, sizeof(dir), out_dir, "sce_sys") || boot_mkdir_p(dir)) r = fail(&p, "cannot create %s", dir);
    for (uint32_t i = 0; !r && i < p.entry_count; ++i) {
        const Entry *e = &p.entries[i];
        if (e->id < 0x400) continue;
        char rel[64];
        snprintf(rel, sizeof(rel), "sce_sys/%s", entry_name(e->id, name, sizeof(name)));
        if (!join(dst, sizeof(dst), out_dir, rel)) { r = fail(&p, "path too long"); break; }
        parent_dir(dst, dir, sizeof(dir));
        uint8_t *data = decrypt_entry(&p, e);
        if (!data) { r = -1; break; }
        if (boot_mkdir_p(dir) || boot_write_file(dst, data, e->size)) r = fail(&p, "cannot write %s", dst);
        free(data);
    }
    const uint64_t chunk_blocks = (16u << 20) / p.block_size ? (16u << 20) / p.block_size : 1;
    uint8_t *buf = r ? NULL : malloc((size_t)(chunk_blocks * p.block_size));
    if (!r && !buf) r = fail(&p, "out of memory");
    for (size_t i = 0; !r && i < items.count; ++i) {
        const Item *it = &items.items[i];
        if (!join(dst, sizeof(dst), out_dir, it->rel)) { r = fail(&p, "path too long: %s", it->rel); break; }
        if (it->type == PFS_DIR) {
            if (boot_mkdir_p(dst)) r = fail(&p, "cannot create %s", dst);
            continue;
        }
        const Inode *node = &p.inodes[it->ino];
        if (node->flags & 1) { r = fail(&p, "%s: PFS-compressed file not supported", it->rel); break; }
        parent_dir(dst, dir, sizeof(dir));
        snprintf(tmp, sizeof(tmp), "%s.part", dst);
        /* BOOT_PKG_RESUME (development: an unpack cut short): files already complete are kept
         * and a .part file goes on from its last whole block. Not for updates, whose files
         * replace ones of the same size. */
        static int resume = -1;
        if (resume < 0) resume = getenv("BOOT_PKG_RESUME") != NULL;
        uint64_t have = 0, skip_blocks = 0;
        int64_t have_time = 0;
        if (resume && !boot_stat(dst, &have, &have_time) && have == node->size) {
            done += node->size;
            continue;
        }
        if (resume && !boot_stat(tmp, &have, &have_time)) skip_blocks = have / p.block_size;
        FILE *fo = boot_mkdir_p(dir) ? NULL : boot_fopen(tmp, skip_blocks ? "r+b" : "wb");
        if (fo && skip_blocks && boot_seek(fo, skip_blocks * p.block_size)) { fclose(fo); fo = NULL; }
        if (!fo) { r = fail(&p, "cannot write %s", dst); break; }
        uint64_t remaining = node->size, b = 0;
        if (skip_blocks * p.block_size < node->size) {
            b = skip_blocks;
            remaining -= skip_blocks * p.block_size;
            done += skip_blocks * p.block_size;
        }
        while (!r && remaining) {
            uint64_t n = node->blocks - b < chunk_blocks ? node->blocks - b : chunk_blocks;
            if (!n) { r = fail(&p, "%s: file larger than its blocks", it->rel); break; }
            if (read_blocks(&p, node->loc + b, n, buf)) { r = -1; break; }
            uint64_t w = n * p.block_size < remaining ? n * p.block_size : remaining;
            if (fwrite(buf, 1, (size_t)w, fo) != (size_t)w) r = fail(&p, "cannot write %s (disk full?)", dst);
            remaining -= w; done += w; b += n;
            if (progress) progress(ctx, done, total);
        }
        if (fclose(fo) && !r) r = fail(&p, "cannot write %s (disk full?)", dst);
        if (!r && boot_rename(tmp, dst)) r = fail(&p, "cannot replace %s", dst);
        if (r) boot_remove(tmp);
    }
    free(buf);
    free(items.items);
    close_pkg(&p);
    return r;
}
