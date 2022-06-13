/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME "ssd_file"
/*
    lba  = page  = low 16 bits
    nand = block = high 16 bits
*/
#define PCA_IDX(pca) ((pca & 0xffff) + ((pca >> 16) * PAGE_PER_BLOCK))

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int lba : 16;
        unsigned int nand: 16;
    } fields;
};

PCA_RULE curr_pca;
static unsigned int get_next_pca();

unsigned int* L2P,* P2L,* valid_count, free_block_number, gc_block_idx;

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size > NAND_SIZE_KB * 1024)
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }
}

static int ssd_expand(size_t new_size)
{
    //logic must less logic limit
    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //read
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}

static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.nand);

    //write
    if ((fptr = fopen(nand_name, "r+")))
    {
        fseek(fptr, my_pca.fields.lba * 512, SEEK_SET);
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
        physic_size++;
        valid_count[my_pca.fields.nand]++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block_index)
{
    char nand_name[100];
    FILE* fptr;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block_index);
    fptr = fopen(nand_name, "w");
    if (fptr == NULL)
    {
        printf("erase nand_%d fail", block_index);
        return 0;
    }
    fclose(fptr);
    valid_count[block_index] = FREE_BLOCK;
    free_block_number++;
    return 1;
}

static unsigned int get_next_block()
{
    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (valid_count[(curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM] == FREE_BLOCK)
        {
            curr_pca.fields.nand = (curr_pca.fields.nand + i) % PHYSICAL_NAND_NUM;
            curr_pca.fields.lba = 0;
            free_block_number--;
            valid_count[curr_pca.fields.nand] = 0;
            return curr_pca.pca;
        }
    }
    return OUT_OF_BLOCK;
}

void garbage_collection()
{
    int erase_block_idx, min_valid;
    char *buf;
    PCA_RULE my_pca;

    erase_block_idx = -1;
    min_valid = PAGE_PER_BLOCK + 1;

    for (int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if (valid_count[i] < min_valid)
        {
            min_valid = valid_count[i];
            erase_block_idx = i;
        }
    }

    if (min_valid == PAGE_PER_BLOCK)
    {
        printf("[WARNING] NO MORE SPACE in garbage_collection\n");
        return;
    }

    // set gc_block_idx to curr_block
    curr_pca.fields.nand = gc_block_idx;
    curr_pca.fields.lba = 0;
    free_block_number--;
    valid_count[curr_pca.fields.nand] = 0;

    buf = calloc(512, sizeof(char));
    my_pca.fields.nand = erase_block_idx;

    for (int i = 0; i < PAGE_PER_BLOCK; i++)
    {
        // Not Sure ?
        my_pca.fields.lba = i;
        int my_lba = P2L[my_pca.fields.lba + my_pca.fields.nand * PAGE_PER_BLOCK];
        // set INVALID_PCA
        L2P[my_lba] = INVALID_PCA;
        // if this LBA is valid
        if (my_lba != INVALID_LBA)
        {
            nand_read(buf, my_pca.pca);
            nand_write(buf, curr_pca.pca);
            L2P[my_lba] = curr_pca.pca;
            P2L[PCA_IDX(curr_pca.pca)] = my_lba;
            P2L[PCA_IDX(my_pca.pca)] = INVALID_LBA;
            curr_pca.fields.lba += 1;
        }
    }

    free(buf);
    nand_erase(erase_block_idx);
    gc_block_idx = erase_block_idx;
    return;
}

static unsigned int get_next_pca()
{
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.pca = 0;
        valid_count[0] = 0;
        free_block_number--;
        return curr_pca.pca;
    }

    if(curr_pca.fields.lba == 9)
    {
        // if curr_block is full & free_block_number == 1 => GC
        if (free_block_number == 1)
        {
            garbage_collection();
            return curr_pca.pca;
        }

        int temp = get_next_block();
        if (temp == OUT_OF_BLOCK)
        {
            return OUT_OF_BLOCK;
        }
        else if(temp == -EINVAL)
        {
            return -EINVAL;
        }
        else
        {
            return temp;
        }
    }
    else
    {
        curr_pca.fields.lba += 1;
    }
    return curr_pca.pca;
}

static int ftl_read(char* buf, size_t lba)
{
    int ret;
    PCA_RULE my_pca;

    // check L2P to get PCA
    my_pca.pca = L2P[lba];

    // RMW==
    if (my_pca.pca == INVALID_PCA)
    {
        printf("[WARNING] INVALID_PCA in ftl_read\n");
        return 512;
    }

    ret = nand_read(buf, my_pca.pca);
    return ret;
}

static int ftl_write(const char* buf, size_t lba_range, size_t lba)
{
    int ret, pca;

    pca = get_next_pca();

    if (pca == OUT_OF_BLOCK || pca == -EINVAL)
    {
        printf("[ERROR] FAIL TO get_next_pca() in ftl_write\n");
        return 0;
    }

    ret = nand_write(buf, pca);
    L2P[lba] = pca;
    P2L[PCA_IDX(pca)] = lba;

    return ret;
}

static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}

static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, ret;
    char* tmp_buf;

    //off limit
    if ((offset) >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        ret = ftl_read(&tmp_buf[i * 512], tmp_lba + i);
        if (ret <= 0)
        {
            printf("[ERROR] FAIL TO ftl_read() in ssd_do_read\n");
            return -1;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}

static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range;
    int idx, ret;
    char* tmp_buf;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / 512;
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    // prev => remember prev_len
    int prev = 0;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        // size = 512, last page may < 512
        int tmp_size = 512;
        if (idx == tmp_lba_range - 1 && (size + (offset % 512)) % 512 != 0)
        {
            tmp_size = (size + offset) % 512;
        }
        // prev_len = 0, first page may > 0
        int prev_len = 0;
        if (idx == 0)
        {
            prev_len = offset % 512;
            prev = prev_len;
        }
        // alloc tmp_buf, 512 * null bytes
        tmp_buf = calloc(512, sizeof(char));
        // check L2P to get PCA
        int tmp_pca = L2P[tmp_lba + idx];

        // if page is valid => RMW
        if (tmp_pca != INVALID_PCA)
        {
            // read
            ret = ftl_read(tmp_buf, tmp_lba + idx);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO ftl_read() in ssd_do_write\n");
                free(tmp_buf);
                return 0;
            }
            // modify
            // special case (idx > 0)
            if (prev != prev_len)
            {
                memcpy(&tmp_buf[prev_len], &buf[idx * 512 - prev], tmp_size - prev_len);
            }
            // nomral case (idx = 0)
            else
            {
                memcpy(&tmp_buf[prev_len], &buf[idx * 512], tmp_size - prev_len);
            }
            // write
            ret = ftl_write(tmp_buf, tmp_lba_range - idx, tmp_lba + idx);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO ftl_write() in ssd_do_write (valid page)\n");
                free(tmp_buf);
                return 0;
            }
            // valid count of this block - 1
            valid_count[tmp_pca >> 16]--;
            P2L[PCA_IDX(tmp_pca)] = INVALID_LBA;
        }
        // if page is free
        else
        {
            // special case (idx > 0)
            if (prev != prev_len)
            {
                memcpy(&tmp_buf[prev_len], &buf[idx * 512 - prev], tmp_size - prev_len);
            }
            // nomral case (idx = 0)
            else
            {
                memcpy(&tmp_buf[prev_len], &buf[idx * 512], tmp_size - prev_len);
            }
            ret = ftl_write(tmp_buf, tmp_lba_range - idx, tmp_lba + idx);
            if (ret <= 0)
            {
                printf("[ERROR] FAIL TO ftl_write() in ssd_do_write (free page)\n");
                free(tmp_buf);
                return 0;
            }
        }
        free(tmp_buf);
    }
    return size;
}

static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{

    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    // init gc_block index
    gc_block_idx = PHYSICAL_NAND_NUM - 1;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    curr_pca.pca = INVALID_PCA;
    free_block_number = PHYSICAL_NAND_NUM;
    // init gc_block index
    gc_block_idx = PHYSICAL_NAND_NUM - 1;

    L2P = malloc(LOGICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * PAGE_PER_BLOCK);
    P2L = malloc(PHYSICAL_NAND_NUM * PAGE_PER_BLOCK * sizeof(int));
    memset(P2L, INVALID_LBA, sizeof(int) * PHYSICAL_NAND_NUM * PAGE_PER_BLOCK);
    valid_count = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(valid_count, FREE_BLOCK, sizeof(int) * PHYSICAL_NAND_NUM);

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}