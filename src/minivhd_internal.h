#ifndef MINIVHD_INTERNAL_H
#define MINIVHD_INTERNAL_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define MVHD_FOOTER_SIZE 512
#define MVHD_SPARSE_SIZE 1024

#define MVHD_SECTOR_SIZE 512
#define MVHD_MAX_BLOCK_SIZE 0x00200000

#define MVHD_SPARSE_BLK 0xffffffff

typedef enum MVHDType {
    MVHD_TYPE_FIXED = 2,
    MVHD_TYPE_DYNAMIC = 3,
    MVHD_TYPE_DIFF = 4
} MVHDType;

typedef struct MVHDSectorBitmap {
    uint8_t* curr_bitmap;
    int sector_count;
    int curr_block;
} MVHDSectorBitmap;

typedef struct MVHDFooter {
    uint8_t cookie[8];
    uint32_t features;
    uint32_t fi_fmt_vers;
    uint64_t data_offset;
    uint32_t timestamp;
    uint8_t cr_app[4];
    uint32_t cr_vers;
    uint8_t cr_host_os[4];
    uint64_t orig_sz;
    uint64_t curr_sz;
    struct {
        uint16_t cyl;
        uint8_t heads;
        uint8_t spt;
    } geom;
    uint32_t disk_type;
    uint32_t checksum;
    uint8_t uuid[16];
    uint8_t saved_st;
    uint8_t reserved[427];
} MVHDFooter;

typedef struct MVHDSparseHeader {
    uint8_t cookie[8];
    uint64_t data_offset;
    uint64_t bat_offset;
    uint32_t head_vers;
    uint32_t max_bat_ent;
    uint32_t block_sz;
    uint32_t checksum;
    uint8_t par_uuid[16];
    uint32_t par_timestamp;
    uint32_t reserved_1;
    uint8_t par_utf16_name[512];
    struct {
        uint32_t plat_code;
        uint32_t plat_data_space;
        uint32_t plat_data_len;
        uint32_t reserved;
        uint64_t plat_data_offset;
    } par_loc_entry[8];
    uint8_t reserved_2[256];
} MVHDSparseHeader;

typedef struct MVHDMeta MVHDMeta;
struct MVHDMeta {
    FILE* f;
    bool readonly;
    char* filename;
    struct MVHDMeta* parent;
    MVHDFooter footer;
    MVHDSparseHeader sparse;
    uint32_t* block_offset;
    int sect_per_block;
    MVHDSectorBitmap bitmap;
    int (*read_sectors)(MVHDMeta*, int, int, void*);
    int (*write_sectors)(MVHDMeta*, int, int, void*);
    int (*format_sectors)(MVHDMeta*, int, int);
};

#endif