/* 
Copyright 2018 Sherman Perry

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#ifdef HAVE_UUID_H
#include <uuid/uuid.h>
#endif
#include "bswap.h"
#include "minivhd.h"

uint8_t VFT_CONECTIX_COOKIE[] = {'c', 'o', 'n', 'e', 'c', 't', 'i', 'x'};
uint8_t VFT_CREATOR[] = {'p','c', 'e', 'm'};
uint8_t VFT_CREATOR_HOST_OS[] = {'W', 'i', '2','k'};
uint8_t VHD_CXSPARSE_COOKIE[] = {'c', 'x', 's', 'p', 'a', 'r', 's', 'e'};

/* Internal functions */
static void mk_guid(uint8_t *guid);
static uint32_t vhd_calc_timestamp(void);
static void vhd_raw_foot_to_meta(VHDMeta *vhdm);
static void vhd_sparse_head_to_meta(VHDMeta *vhdm);
static void vhd_new_raw(VHDMeta *vhdm);
static int vhd_bat_from_file(VHDMeta *vhdm, FILE *f);
static void vhd_update_bat(VHDMeta *vhdm, FILE *f, int blk);
static uint32_t vhd_generate_be_checksum(VHDMeta *vhdm, uint32_t type);
static VHDError vhd_validate_checksum(VHDMeta *vhdm);
static void vhd_create_blk(VHDMeta *vhdm, FILE *f, int blk_num);

/* A UUID is required, but there are no restrictions on how it needs
   to be generated. */
static void mk_guid(uint8_t *guid)
{
#if defined(HAVE_UUID_H)
	uuid_generate(guid);
//#elif defined(HAVE_OBJBASE_H)
//	CoCreateGuid( (GUID *)guid);
#else
	int n;

	srand(time(NULL));
	for (n = 0; n < 16; n++)
	{
		guid[n] = rand();
	}
	guid[6] &= 0x0F;
	guid[6] |= 0x40;	/* Type 4 */
	guid[8] &= 0x3F;
	guid[8] |= 0x80;	/* Variant 1 */
#endif
}
/* Calculate the current timestamp. */
static uint32_t vhd_calc_timestamp(void)
{
        time_t start_time;
        time_t curr_time;
        double vhd_time;
        start_time = VHD_START_TS; /* 1 Jan 2000 00:00 */
        curr_time = time(NULL);
        vhd_time = difftime(curr_time, start_time);

        return (uint32_t)vhd_time;
}
time_t vhd_get_created_time(VHDMeta *vhdm)
{
        time_t vhd_time = (time_t)be32_to_cpu(vhdm->raw_footer[VHD_FOFF_TS]);
        time_t vhd_time_unix = VHD_START_TS + vhd_time;
        return vhd_time_unix;
}
/* Test if a file is a VHD. */
int vhd_file_is_vhd(FILE *f)
{
        uint8_t buffer[VHD_FOOTER_SZ];
        fseeko64(f, -VHD_FOOTER_SZ, SEEK_END);
        fread(buffer, 1, VHD_FOOTER_SZ, f);
        int valid_vhd = 0;
        // Check for valid cookie
        if (strncmp((char*)VFT_CONECTIX_COOKIE, (char*)buffer, 8) == 0)
                valid_vhd = 1;
        return valid_vhd;
}
/* Perform a basic integrity check of the VHD footer and sparse header. */
VHDError vhd_check_validity(VHDMeta *vhdm)
{
        VHDError status, chksum_status;
        chksum_status = vhd_validate_checksum(vhdm);
        if (vhdm->type != VHD_FIXED && vhdm->type != VHD_DYNAMIC)
                return status = VHD_ERR_TYPE_UNSUPPORTED;
        else if (vhdm->curr_size < ((uint64_t)vhdm->geom.cyl * vhdm->geom.heads * vhdm->geom.spt * VHD_SECTOR_SZ))
                return status = VHD_ERR_GEOM_SIZE_MISMATCH;
        else if (vhdm->geom.spt > 63)
                return status = VHD_WARN_SPT_SZ;
        else if (chksum_status == VHD_ERR_BAD_DYN_CHECKSUM)
                return status = VHD_ERR_BAD_DYN_CHECKSUM;
        else if (chksum_status == VHD_WARN_BAD_CHECKSUM)
                return status = VHD_WARN_BAD_CHECKSUM;
        else
                return status = VHD_VALID;
}
VHDError vhd_read_file(FILE *f, VHDMeta *vhdm)
{
        VHDError ret = VHD_RET_OK;
        memset(vhdm->raw_footer, 0, VHD_FOOTER_SZ);
        fseeko64(f, -VHD_FOOTER_SZ, SEEK_END);
        fread(vhdm->raw_footer, 1, VHD_FOOTER_SZ, f);
        // Check for valid cookie
        if (strncmp((char*)VFT_CONECTIX_COOKIE, (char*)vhdm->raw_footer, 8) == 0)
        {
                /* Don't want a pointer to who knows where... */
                vhdm->sparse_bat_arr = NULL;
                vhd_raw_foot_to_meta(vhdm);
                if (vhdm->type == VHD_DYNAMIC)
                {
                        memset(vhdm->raw_sparse_header, 0, VHD_SPARSE_HEAD_SZ);
                        fseeko64(f, vhdm->sparse_header_offset, SEEK_SET);
                        fread(vhdm->raw_sparse_header, 1, VHD_SPARSE_HEAD_SZ, f);
                        vhd_sparse_head_to_meta(vhdm);
                        if (!vhd_bat_from_file(vhdm, f))
                                ret = VHD_RET_MALLOC_ERROR;
                }
        }
        else
                ret = VHD_RET_NOT_VHD;
        return ret;
}
/* Convenience function to create VHD file by specifiying size in MB */
void vhd_create_file_sz(FILE *f, VHDMeta *vhdm, int sz_mb, VHDType type)
{
        VHDGeom chs = vhd_calc_chs((uint32_t)sz_mb);
        vhd_create_file(f, vhdm, chs.cyl, chs.heads, chs.spt, type);
}
/* Create VHD file from CHS geometry. */
void vhd_create_file(FILE *f, VHDMeta *vhdm, int cyl, int heads, int spt, VHDType type)
{
        uint64_t vhd_sz = (uint64_t)cyl * heads * spt * VHD_SECTOR_SZ;
        vhdm->curr_size = vhd_sz;
        vhdm->geom.cyl = (uint16_t)cyl;
        vhdm->geom.heads = (uint8_t)heads;
        vhdm->geom.spt = (uint8_t)spt;
        vhdm->type = type;
        vhdm->sparse_header_offset = VHD_FOOTER_SZ;
        vhdm->sparse_bat_offset = VHD_FOOTER_SZ + VHD_SPARSE_HEAD_SZ;
        vhdm->sparse_block_sz = VHD_DEF_BLOCK_SZ;
        vhdm->sparse_max_bat = vhdm->curr_size / vhdm->sparse_block_sz;
        if (vhdm->curr_size % vhdm->sparse_block_sz != 0)
                vhdm->sparse_max_bat += 1;
        vhd_new_raw(vhdm);
        if (type == VHD_DYNAMIC)
        {
                uint8_t zero_padding[VHD_BLK_PADDING];
                uint8_t bat_buff[VHD_MAX_BAT_SIZE_BYTES];
                memset(bat_buff, 255, sizeof(bat_buff));
                memset(zero_padding, 0, sizeof(zero_padding));
                fseeko64(f, 0, SEEK_SET);
                fwrite(vhdm->raw_footer, VHD_FOOTER_SZ, 1, f);
                fseeko64(f, vhdm->sparse_header_offset, SEEK_SET);
                fwrite(vhdm->raw_sparse_header, VHD_SPARSE_HEAD_SZ, 1, f);
                fseeko64(f, vhdm->sparse_bat_offset, SEEK_SET);
                fwrite(bat_buff, sizeof(bat_buff), 1, f);
                fwrite(zero_padding, sizeof(zero_padding), 1, f);
                fwrite(vhdm->raw_footer, VHD_FOOTER_SZ, 1, f);
        }
        else
        {
                uint8_t zero_buff[VHD_SECTOR_SZ];
                memset(zero_buff, 0, sizeof(zero_buff));
                uint32_t vhd_sect_sz = vhdm->curr_size / VHD_SECTOR_SZ;
                uint32_t i;
                fseeko64(f, 0, SEEK_SET);
                for (i = 0; i < vhd_sect_sz; i++)
                {
                        fwrite(zero_buff, sizeof(zero_buff), 1, f);
                }
                fwrite(vhdm->raw_footer, VHD_FOOTER_SZ, 1, f);
        }
}
static void vhd_raw_foot_to_meta(VHDMeta *vhdm)
{
        memcpy(&vhdm->type, vhdm->raw_footer + VHD_FOFF_TYPE, sizeof(vhdm->type));
        vhdm->type = be32_to_cpu(vhdm->type);
        memcpy(&vhdm->curr_size, vhdm->raw_footer + VHD_FOFF_CU_SZ, sizeof(vhdm->curr_size));
        vhdm->curr_size = be64_to_cpu(vhdm->curr_size);
        memcpy(&vhdm->geom.cyl, vhdm->raw_footer + VHD_FOFF_CYL, sizeof(vhdm->geom.cyl));
        vhdm->geom.cyl = be16_to_cpu(vhdm->geom.cyl);
        memcpy(&vhdm->geom.heads, vhdm->raw_footer + VHD_FOFF_HEAD, sizeof(vhdm->geom.heads));
        memcpy(&vhdm->geom.spt, vhdm->raw_footer + VHD_FOFF_SPT, sizeof(vhdm->geom.spt));
        memcpy(&vhdm->sparse_header_offset, vhdm->raw_footer + VHD_FOFF_DAT_OFF, sizeof(vhdm->sparse_header_offset));
        vhdm->sparse_header_offset = be64_to_cpu(vhdm->sparse_header_offset);
}
static void vhd_sparse_head_to_meta(VHDMeta *vhdm)
{
        memcpy(&vhdm->sparse_bat_offset, vhdm->raw_sparse_header + VHD_SOFF_BAT_OFF, sizeof(vhdm->sparse_bat_offset));
        vhdm->sparse_bat_offset = be64_to_cpu(vhdm->sparse_bat_offset);
        memcpy(&vhdm->sparse_max_bat, vhdm->raw_sparse_header + VHD_SOFF_MAX_BAT, sizeof(vhdm->sparse_max_bat));
        vhdm->sparse_max_bat = be32_to_cpu(vhdm->sparse_max_bat);
        memcpy(&vhdm->sparse_block_sz, vhdm->raw_sparse_header + VHD_SOFF_BLK_SZ, sizeof(vhdm->sparse_block_sz));
        vhdm->sparse_block_sz = be32_to_cpu(vhdm->sparse_block_sz);
        vhdm->sparse_spb = vhdm->sparse_block_sz / VHD_SECTOR_SZ;
        vhdm->sparse_sb_sz = vhdm->sparse_spb / 8;
        if (vhdm->sparse_sb_sz % VHD_SECTOR_SZ != 0)
                vhdm->sparse_sb_sz += (vhdm->sparse_sb_sz % VHD_SECTOR_SZ);
}
static void vhd_new_raw(VHDMeta *vhdm)
{
        /* Zero buffers */
        memset(vhdm->raw_footer, 0, VHD_FOOTER_SZ);
        memset(vhdm->raw_sparse_header, 0, VHD_SPARSE_HEAD_SZ);
        /* Write to footer buffer. */
        memcpy(vhdm->raw_footer + VHD_FOFF_COOKIE, VFT_CONECTIX_COOKIE, sizeof(VFT_CONECTIX_COOKIE));
        uint32_t features = cpu_to_be32(0x00000002);
        memcpy(vhdm->raw_footer + VHD_FOFF_FEATURES, &features, sizeof(features));
        uint32_t file_fmt_vers = cpu_to_be32(0x00010000);
        memcpy(vhdm->raw_footer + VHD_FOFF_VER, &file_fmt_vers, sizeof(file_fmt_vers));
        uint64_t sparse_dat_offset;
        if (vhdm->type == VHD_DYNAMIC)
                sparse_dat_offset = cpu_to_be64(vhdm->sparse_header_offset);
        else
                sparse_dat_offset = 0xffffffffffffffff;
        memcpy(vhdm->raw_footer + VHD_FOFF_DAT_OFF, &sparse_dat_offset, sizeof(sparse_dat_offset));
        uint32_t timestamp = cpu_to_be32(vhd_calc_timestamp());
        memcpy(vhdm->raw_footer + VHD_FOFF_TS, &timestamp, sizeof(timestamp));
        memcpy(vhdm->raw_footer + VHD_FOFF_CR, VFT_CREATOR, sizeof(VFT_CREATOR));
        uint32_t creator_vers = cpu_to_be32(0x000e0000);
        memcpy(vhdm->raw_footer + VHD_FOFF_CR_VER, &creator_vers, sizeof(creator_vers));
        memcpy(vhdm->raw_footer + VHD_FOFF_CR_HST, VFT_CREATOR_HOST_OS, sizeof(VFT_CREATOR_HOST_OS));
        uint64_t sz = cpu_to_be64(vhdm->curr_size);
        memcpy(vhdm->raw_footer + VHD_FOFF_OG_SZ, &sz, sizeof(sz));
        memcpy(vhdm->raw_footer + VHD_FOFF_CU_SZ, &sz, sizeof(sz));
        uint16_t cyl = cpu_to_be16(vhdm->geom.cyl);
        memcpy(vhdm->raw_footer + VHD_FOFF_CYL, &cyl, sizeof(cyl));
        vhdm->raw_footer[VHD_FOFF_HEAD] = vhdm->geom.heads;
        vhdm->raw_footer[VHD_FOFF_SPT] = vhdm->geom.spt;
        uint32_t disk_type = cpu_to_be32(vhdm->type);
        memcpy(vhdm->raw_footer + VHD_FOFF_TYPE, &disk_type, sizeof(disk_type));
        uint8_t uuid[16];
        mk_guid(uuid);
        memcpy(vhdm->raw_footer + VHD_FOFF_UUID, uuid, sizeof(uuid));
        uint32_t chk = vhd_generate_be_checksum(vhdm, VHD_FIXED);
        memcpy(vhdm->raw_footer + VHD_FOFF_CHK, &chk, sizeof(chk));
        /* Write to sparse header buffer */
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_COOKIE, VHD_CXSPARSE_COOKIE, sizeof(VHD_CXSPARSE_COOKIE));
        uint64_t sparse_data_offset = 0xffffffffffffffff;
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_DAT_OFF, &sparse_data_offset, sizeof(sparse_data_offset));
        uint64_t bat_ofst = cpu_to_be64(vhdm->sparse_bat_offset);
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_BAT_OFF, &bat_ofst, sizeof(bat_ofst));
        uint32_t sp_head_vers = cpu_to_be32(0x00010000);
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_VERS, &sp_head_vers, sizeof(sp_head_vers));
        uint32_t bat_ent = cpu_to_be32(vhdm->sparse_max_bat);
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_MAX_BAT, &bat_ent, sizeof(bat_ent));
        uint32_t bs = cpu_to_be32(vhdm->sparse_block_sz);
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_BLK_SZ, &bs, sizeof(bs));
        chk = vhd_generate_be_checksum(vhdm, VHD_DYNAMIC);
        memcpy(vhdm->raw_sparse_header + VHD_SOFF_CHK, &chk, sizeof(chk));
}
/* Create a dynamic array of the Block Allocation Table as stored in the file. */
static int vhd_bat_from_file(VHDMeta *vhdm, FILE *f)
{
        if (!vhdm->sparse_bat_arr)
        {
                int ba_sz = sizeof(uint32_t) * vhdm->sparse_max_bat;
                vhdm->sparse_bat_arr = malloc(ba_sz);
                if (vhdm->sparse_bat_arr)
                        memset(vhdm->sparse_bat_arr, 255, ba_sz);
                else
                        return 0;
        }
        int b;
        for (b = 0; b < vhdm->sparse_max_bat; b++)
        {
                uint32_t curr_entry;
                uint64_t file_offset = vhdm->sparse_bat_offset + (b * 4);
                fseeko64(f, file_offset, SEEK_SET);
                fread(&curr_entry, 4, 1, f);
                vhdm->sparse_bat_arr[b] = be32_to_cpu(curr_entry);
        }
        return 1;
}
/* Updates the Block Allocation Table in the file with the new offset for a block. */
static void vhd_update_bat(VHDMeta *vhdm, FILE *f, int blk)
{
        uint64_t blk_file_offset = vhdm->sparse_bat_offset + (blk * 4);
        uint32_t blk_offset = cpu_to_be32(vhdm->sparse_bat_arr[blk]);
        fseeko64(f, blk_file_offset, SEEK_SET);
        fwrite(&blk_offset, 4, 1, f);
}
/* Calculates the checksum for a footer or header */
static uint32_t vhd_generate_be_checksum(VHDMeta *vhdm, uint32_t type)
{
        uint32_t chk = 0;
        if (type == VHD_DYNAMIC)
        {
                int i;
                for (i = 0; i < VHD_SPARSE_HEAD_SZ; i++)
                {
                        if (i < VHD_SOFF_CHK || i >= VHD_SOFF_PAR_UUID)
                                chk += vhdm->raw_sparse_header[i];
                }
        }
        else
        {
                int i;
                for (i = 0; i < VHD_FOOTER_SZ; i++)
                {
                        if (i < VHD_FOFF_CHK || i >= VHD_FOFF_UUID)
                                chk += vhdm->raw_footer[i];
                }
        }
        chk = ~chk;
        return cpu_to_be32(chk);
}
/* Validates the checksums in the VHD file */
static VHDError vhd_validate_checksum(VHDMeta *vhdm)
{
        VHDError ret = VHD_VALID;
        uint32_t stored_chksum, calc_chksum;
        if (vhdm->type == VHD_DYNAMIC)
        {
                memcpy(&stored_chksum, vhdm->raw_sparse_header + VHD_SOFF_CHK, sizeof(stored_chksum));
                calc_chksum = vhd_generate_be_checksum(vhdm, VHD_DYNAMIC);
                if (stored_chksum != calc_chksum)
                {
                        ret = VHD_ERR_BAD_DYN_CHECKSUM;
                        return ret;
                }
        }
        memcpy(&stored_chksum, vhdm->raw_footer + VHD_FOFF_CHK, sizeof(stored_chksum));
        calc_chksum = vhd_generate_be_checksum(vhdm, VHD_FIXED);
        if (stored_chksum != calc_chksum)
                ret = VHD_WARN_BAD_CHECKSUM;
        return ret;
}
/* Calculate the geometry from size (in MB), using the algorithm provided in
   "Virtual Hard Disk Image Format Specification, Appendix: CHS Calculation" */
VHDGeom vhd_calc_chs(uint32_t sz_mb)
{
        VHDGeom chs;
        uint32_t ts = ((uint64_t)sz_mb * 1024 * 1024) / VHD_SECTOR_SZ;
        uint32_t spt, heads, cyl, cth;
        /* PCem does not currently support spt > 63 */
        // if (ts > 65535 * 16 * 255)
        //         ts = 65535 * 16 * 255;
        if (ts >= 65535 * 16 * 63)
        {
                ts = 65535 * 16 * 63;
                spt = 63;
                heads = 16;
                cth = ts / spt;
        }
        else
        {
                spt = 17;
                cth = ts / spt;
                heads = (cth +1023) / 1024;
                if (heads < 4)
                        heads = 4;
                if (cth >= (heads * 1024) || heads > 16)
                {
                        spt = 31;
                        heads = 16;
                        cth = ts / spt;
                }
                if (cth >= (heads * 1024))
                {
                        spt = 63;
                        heads = 16;
                        cth = ts / spt;
                }
        }
        cyl = cth / heads;
        chs.heads = heads;
        chs.spt = spt;
        chs.cyl = cyl;
        return chs;
}
/* Create new data block at the location of the existing footer.
   The footer gets replaced after the end of the new data block */
static void vhd_create_blk(VHDMeta *vhdm, FILE *f, int blk_num)
{
        uint8_t ftr[VHD_SECTOR_SZ];
        uint8_t zero_sect[VHD_SECTOR_SZ];
        uint8_t full_sect[VHD_SECTOR_SZ];
        uint8_t zero_padding[VHD_BLK_PADDING];
        memset(zero_sect, 0, VHD_SECTOR_SZ);
        memset(full_sect, 255, VHD_SECTOR_SZ);
        memset(zero_padding, 0, sizeof(zero_padding));
        uint32_t new_blk_offset;
        fseeko64(f, -512, SEEK_END);
        new_blk_offset = (uint64_t)ftello64(f) / VHD_SECTOR_SZ;
        fread(ftr, 1, 512, f);
        fseeko64(f, -512, SEEK_END);
        /* Let's be sure we are not potentially overwriting a data block for some reason. */
        if (strncmp((char*)VFT_CONECTIX_COOKIE, (char*)ftr, 8) == 0)
        {
                uint32_t sb_sz = vhdm->sparse_sb_sz / VHD_SECTOR_SZ;
                uint32_t sect_to_write = sb_sz + vhdm->sparse_spb;
                int s;
                for (s = 0; s < sect_to_write; s++)
                {
                        if (s < sb_sz)
                                fwrite(full_sect, VHD_SECTOR_SZ, 1, f);
                        else
                                fwrite(zero_sect, VHD_SECTOR_SZ, 1, f);
                }
                fwrite(zero_padding, sizeof(zero_padding), 1, f);
                fwrite(ftr, VHD_FOOTER_SZ, 1, f);
                vhdm->sparse_bat_arr[blk_num] = new_blk_offset;
                vhd_update_bat(vhdm, f, blk_num);
        }
}

int vhd_read_sectors(VHDMeta *vhdm, FILE *f, int offset, int nr_sectors, void *buffer)
{
        int transfer_sectors = nr_sectors;
        uint32_t total_sectors = vhdm->geom.cyl * vhdm->geom.heads * vhdm->geom.spt;
        /* This check comes from PCem */
        if ((total_sectors - offset) < transfer_sectors)
                transfer_sectors = total_sectors - offset;
        if (vhdm->type == VHD_DYNAMIC)
        {
                int start_blk = offset / vhdm->sparse_spb;
                int end_blk = (offset + (transfer_sectors - 1)) / vhdm->sparse_spb;
                int sbsz = vhdm->sparse_sb_sz / VHD_SECTOR_SZ;
                /* Most common case. No need to access multiple data blocks. */
                if (start_blk == end_blk)
                {
                        uint32_t sib = offset % vhdm->sparse_spb;
                        /* If the data block doesn't yet exist, fill the buffer with zero data */
                        if (vhdm->sparse_bat_arr[start_blk] == VHD_SPARSE_BLK)
                                memset(buffer, 0, (transfer_sectors * VHD_SECTOR_SZ));
                        else
                        {
                                uint32_t file_sect_offs = vhdm->sparse_bat_arr[start_blk] + sbsz + sib;
                                fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                                fread(buffer, transfer_sectors * VHD_SECTOR_SZ, 1, f);
                        }
                }
                /* Sometimes reads cross data block boundries. We handle this case here. */
                else
                {
                        uint32_t s, ls;
                        ls = offset + (transfer_sectors - 1);
                        for (s = offset; s <= ls; s++)
                        {
                                int blk = s / vhdm->sparse_spb;
                                uint32_t sib = s % vhdm->sparse_spb;
                                /* If the data block doesn't yet exist, fill the buffer with zero data */
                                if (vhdm->sparse_bat_arr[blk] == VHD_SPARSE_BLK)
                                        memset(buffer, 0, VHD_SECTOR_SZ);
                                else
                                {
                                        uint32_t file_sect_offs = vhdm->sparse_bat_arr[blk] + sbsz + sib;
                                        fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                                        fread(buffer, VHD_SECTOR_SZ, 1, f);
                                }
                                buffer = (uint8_t*)buffer + VHD_SECTOR_SZ;
                        }
                }
        }
        else
        {
                /* Code from PCem */
                uint64_t addr = (uint64_t)offset * VHD_SECTOR_SZ;
                fseeko64(f, addr, SEEK_SET);
                fread(buffer, transfer_sectors * VHD_SECTOR_SZ, 1, f);
        }
        if (nr_sectors != transfer_sectors)
                return 1;
        return 0;
}
int vhd_write_sectors(VHDMeta *vhdm, FILE *f, int offset, int nr_sectors, void *buffer)
{
        int transfer_sectors = nr_sectors;
        uint32_t total_sectors = vhdm->geom.cyl * vhdm->geom.heads * vhdm->geom.spt;
        /* This check comes from PCem */
        if ((total_sectors - offset) < transfer_sectors)
                transfer_sectors = total_sectors - offset;
        if (vhdm->type == VHD_DYNAMIC)
        {
                int start_blk = offset / vhdm->sparse_spb;
                int end_blk = (offset + (transfer_sectors - 1)) / vhdm->sparse_spb;
                int sbsz = vhdm->sparse_sb_sz / VHD_SECTOR_SZ;
                /* Most common case. No need to access multiple data blocks. */
                if (start_blk == end_blk)
                {
                        uint32_t sib = offset % vhdm->sparse_spb;
                        /* We need to create a data block if it does not yet exist. */
                        if (vhdm->sparse_bat_arr[start_blk] == VHD_SPARSE_BLK)
                                vhd_create_blk(vhdm, f, start_blk);
                        uint32_t file_sect_offs = vhdm->sparse_bat_arr[start_blk] + sbsz + sib;
                        fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                        fwrite(buffer, transfer_sectors * VHD_SECTOR_SZ, 1, f);
                }
                /* Sometimes writes cross data block boundries. We handle this case here. */
                else
                {
                        uint32_t s, ls;
                        ls = offset + (transfer_sectors - 1);
                        for (s = offset; s <= ls; s++)
                        {
                                int blk = s / vhdm->sparse_spb;
                                uint32_t sib = s % vhdm->sparse_spb;
                                /* We need to create a data block if it does not yet exist. */
                                if (vhdm->sparse_bat_arr[blk] == VHD_SPARSE_BLK)
                                        vhd_create_blk(vhdm, f, blk);
                                uint32_t file_sect_offs = vhdm->sparse_bat_arr[blk] + sbsz + sib;
                                fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                                fwrite(buffer, VHD_SECTOR_SZ, 1, f);

                                buffer = (uint8_t*)buffer + VHD_SECTOR_SZ;
                        }
                }
        }
        else
        {
                /* Code from PCem */
                uint64_t addr = (uint64_t)offset * VHD_SECTOR_SZ;
                fseeko64(f, addr, SEEK_SET);
                fwrite(buffer, transfer_sectors * VHD_SECTOR_SZ, 1, f);
        }
        if (nr_sectors != transfer_sectors)
                return 1;
        return 0;
}
int vhd_format_sectors(VHDMeta *vhdm, FILE *f, int offset, int nr_sectors)
{
        uint8_t zero_buffer[VHD_SECTOR_SZ];
        memset(zero_buffer, 0, VHD_SECTOR_SZ);
        int transfer_sectors = nr_sectors;
        uint32_t total_sectors = vhdm->geom.cyl * vhdm->geom.heads * vhdm->geom.spt;
        /* This check comes from PCem */
        if ((total_sectors - offset) < transfer_sectors)
                transfer_sectors = total_sectors - offset;

        if (vhdm->type == VHD_DYNAMIC)
        {
                int start_blk = offset / vhdm->sparse_spb;
                int end_blk = (offset + (transfer_sectors - 1)) / vhdm->sparse_spb;
                int sbsz = vhdm->sparse_sb_sz / VHD_SECTOR_SZ;
                /* Most common case. No need to access multiple data blocks. */
                if (start_blk == end_blk)
                {
                        uint32_t sib = offset % vhdm->sparse_spb;
                        if (vhdm->sparse_bat_arr[start_blk] != VHD_SPARSE_BLK)
                        {
                                uint32_t file_sect_offs = vhdm->sparse_bat_arr[start_blk] + sbsz + sib;
                                fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                                fwrite(zero_buffer, transfer_sectors * VHD_SECTOR_SZ, 1, f);
                        }
                }
                else
                {
                        uint32_t s, ls;
                        ls = offset + (transfer_sectors - 1);
                        for (s = offset; s <= ls; s++)
                        {
                                int blk = s / vhdm->sparse_spb;
                                uint32_t sib = s % vhdm->sparse_spb;
                                if (vhdm->sparse_bat_arr[blk] != VHD_SPARSE_BLK)
                                {
                                        uint32_t file_sect_offs = vhdm->sparse_bat_arr[blk] + sbsz + sib;
                                        fseeko64(f, (uint64_t)file_sect_offs * VHD_SECTOR_SZ, SEEK_SET);
                                        fwrite(zero_buffer, VHD_SECTOR_SZ, 1, f);
                                }
                        }
                }
        }
        else
        {
                /* Code from PCem */
                off64_t addr;
                int c;
                uint8_t zero_buffer[VHD_SECTOR_SZ];
                memset(zero_buffer, 0, VHD_SECTOR_SZ);
                addr = (uint64_t)offset * VHD_SECTOR_SZ;
                fseeko64(f, addr, SEEK_SET);
                for (c = 0; c < transfer_sectors; c++)
                        fwrite(zero_buffer, VHD_SECTOR_SZ, 1, f);
        }
        if (nr_sectors != transfer_sectors)
                return 1;
        return 0;
}
void vhd_close(VHDMeta *vhdm)
{
        if (vhdm->sparse_bat_arr)
        {
                free(vhdm->sparse_bat_arr);
                vhdm->sparse_bat_arr = NULL;
        }
}