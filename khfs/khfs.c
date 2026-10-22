/*
  khfs - User Level File System with Lazy RAM Buffering
  Inspired by Big Brother File System
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <pthread.h>

// --- Lazy Loading RAM Cache Data Structures ---

typedef struct cache_node {
    char path[PATH_MAX];
    char *data;
    size_t size;
    int dirty; // 1 if modified in RAM, 0 if identical to disk
    struct cache_node *next;
} cache_node_t;

cache_node_t *cache_head = NULL;
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

struct khfs_state {
    char *rootdir;
};
#define KHFS_DATA ((struct khfs_state *) fuse_get_context()->private_data)

// --- Helper Functions ---

// Constructs the real physical path from the virtual FUSE path
static void khfs_fullpath(char fpath[PATH_MAX], const char *path) {
	snprintf(fpath, PATH_MAX, "%s%s", KHFS_DATA->rootdir,path);
}

// --- RAM Visualizer ---
static void print_ram_state() {
    printf("\n=== CURRENT RAM CACHE STATE (MEMORY MAP) ===\n");
    pthread_mutex_lock(&cache_mutex);
    cache_node_t *curr = cache_head;
    
    if (!curr) {
        printf("  [ RAM is currently empty ]\n");
    }
    
    while (curr) {
        // We cast to (void *) to prevent any compiler warnings about pointer printing
        printf("  -> File: %-15s | Node Addr: %p | Data Addr: %p | Next: %p | Size: %4zu B | %s\n", 
               curr->path, 
               (void *)curr, 
               (void *)curr->data, 
               (void *)curr->next,
               curr->size, 
               curr->dirty ? "[DIRTY]" : "[CLEAN]");
        curr = curr->next;
    }
    pthread_mutex_unlock(&cache_mutex);
    printf("============================================\n\n");
    fflush(stdout);
}

// Writes a single cached file back to the physical disk
static void flush_cache_node_to_disk(cache_node_t *node) {
    if (!node->dirty) return;

    char fpath[PATH_MAX];
    khfs_fullpath(fpath, node->path);

    int fd = open(fpath, O_WRONLY | O_TRUNC | O_CREAT, 0644);
    if (fd != -1) {
        pwrite(fd, node->data, node->size, 0);
        close(fd);
        node->dirty = 0;
        printf("[khfs] Flushed to disk: %s\n", node->path);
    } else {
        fprintf(stderr, "[khfs] ERROR: Could not flush %s to disk!\n", node->path);
    }
}

// Retrieves a file from cache, or loads it from disk if not present
static cache_node_t* get_or_load_cache(const char *path) {
    // 1. Search in cache
    cache_node_t *curr = cache_head;
    while (curr) {
        if (strcmp(curr->path, path) == 0) {
            return curr; 
        }
        curr = curr->next;
    }

    // 2. Not in cache, load from disk
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);
    
    struct stat st;
    if (lstat(fpath, &st) == -1 || !S_ISREG(st.st_mode)) {
        return NULL; // Doesn't exist or is not a regular file
    }

    cache_node_t *new_node = malloc(sizeof(cache_node_t));
    strcpy(new_node->path, path);
    new_node->size = st.st_size;
    new_node->data = malloc(st.st_size > 0 ? st.st_size : 1);
    new_node->dirty = 0;

    if (st.st_size > 0) {
        int fd = open(fpath, O_RDONLY);
        if (fd != -1) {
            pread(fd, new_node->data, st.st_size, 0);
            close(fd);
        }
    }

    // Add to linked list
    new_node->next = cache_head;
    cache_head = new_node;
    
    printf("[khfs] Lazy Loaded into RAM: %s\n", path);
    return new_node;
}

// Removes a file from the RAM cache (used when a file is deleted/renamed)
static void remove_from_cache(const char *path) {
    cache_node_t *curr = cache_head;
    cache_node_t *prev = NULL;

    while (curr) {
        if (strcmp(curr->path, path) == 0) {
            if (prev) prev->next = curr->next;
            else cache_head = curr->next;
            
            free(curr->data);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// --- FUSE Operations ---

static int khfs_getattr(const char *path, struct stat *stbuf) {
    int res;
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);

    res = lstat(fpath, stbuf);
    if (res == -1)
        return -errno;

    // If it's a file, override the disk size with the RAM cache size
    if (S_ISREG(stbuf->st_mode)) {
        pthread_mutex_lock(&cache_mutex);
        cache_node_t *node = get_or_load_cache(path);
        if (node) {
            stbuf->st_size = node->size;
        }
        pthread_mutex_unlock(&cache_mutex);
    }
    return 0;
}

static int khfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi) {
    DIR *dp;
    struct dirent *de;
    char fpath[PATH_MAX];

    (void) offset;
    (void) fi;

    khfs_fullpath(fpath, path);
    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

    while ((de = readdir(dp)) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        if (filler(buf, de->d_name, &st, 0))
            break;
    }

    closedir(dp);
    return 0;
}

static int khfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void) fi;
    int bytes_read = 0;

    pthread_mutex_lock(&cache_mutex);
    cache_node_t *node = get_or_load_cache(path);
    
    if (node) {
        if (offset < node->size) {
            if (offset + size > node->size)
                size = node->size - offset;
            memcpy(buf, node->data + offset, size);
            bytes_read = size;
        }
    }
    pthread_mutex_unlock(&cache_mutex);
    
    return bytes_read;
}

static int khfs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    (void) fi;
    
    pthread_mutex_lock(&cache_mutex);
    cache_node_t *node = get_or_load_cache(path);
    
    if (node) {
        // Expand RAM buffer if writing past the current end of file
        if (offset + size > node->size) {
            size_t new_size = offset + size;
            char *new_data = realloc(node->data, new_size);
            if (!new_data) {
                pthread_mutex_unlock(&cache_mutex);
                return -ENOMEM;
            }
            // If writing far past end of file, zero out the gap
            if (offset > node->size) {
                memset(new_data + node->size, 0, offset - node->size);
            }
            node->data = new_data;
            node->size = new_size;
        }
        
        memcpy(node->data + offset, buf, size);
        node->dirty = 1;
    }
    pthread_mutex_unlock(&cache_mutex);

    printf("\n[khfs] Action: Wrote %zu bytes to %s\n", size, path);
    print_ram_state();

    return size;
}
static int khfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);
    int res;

    // Check if it is a regular file
    if (S_ISREG(mode)) {
        res = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
        if (res >= 0) res = close(res);
    } else {
        // If it's a special file (like a device node), pass it to physical disk
        res = mknod(fpath, mode, rdev);
    }
    
    if (res == -1) return -errno;

    // Add empty file to RAM cache
    pthread_mutex_lock(&cache_mutex);
    get_or_load_cache(path);
    pthread_mutex_unlock(&cache_mutex);
    
    printf("\n[khfs] Action: Created new file via mknod %s\n", path);
    print_ram_state();
    
    return 0;
}

static int khfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int res;
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);

    res = open(fpath, fi->flags, mode);
    if (res == -1)
        return -errno;
    close(res);

    // Add empty file to RAM cache
    pthread_mutex_lock(&cache_mutex);
    get_or_load_cache(path);
    pthread_mutex_unlock(&cache_mutex);
    
    print_ram_state();
    return 0;
}

static int khfs_utimens(const char *path, const struct timespec tv[2])
{
	char fpath[PATH_MAX];
	khfs_fullpath(fpath, path);

	//Pass the timestamp update down to the underlying file
	int res = utimensat(AT_FDCWD,fpath,tv,AT_SYMLINK_NOFOLLOW);
	if(res == -1)
		return -errno;
	return 0;
}

static int khfs_mkdir(const char *path, mode_t mode) {
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);
    int res = mkdir(fpath, mode);
    if (res == -1) return -errno;
    return 0;
}

static int khfs_unlink(const char *path) {
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);
    
    pthread_mutex_lock(&cache_mutex);
    remove_from_cache(path);
    pthread_mutex_unlock(&cache_mutex);

    printf("\n[khfs] Action: Deleted %s from RAM and Disk\n", path);
    print_ram_state();

    int res = unlink(fpath);
    if (res == -1) return -errno;
    return 0;
}

static int khfs_rmdir(const char *path) {
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);
    int res = rmdir(fpath);
    if (res == -1) return -errno;
    return 0;
}

static int khfs_rename(const char *from, const char *to) {
    char ffrom[PATH_MAX], fto[PATH_MAX];
    khfs_fullpath(ffrom, from);
    khfs_fullpath(fto, to);

    pthread_mutex_lock(&cache_mutex);

    // If a file with the newfile name already exist in RAM then delete it
    if(strcmp(from,to)!=0)
    {
	    remove_from_cache(to);
    }

    // If the file is in RAM, we must flush it, rename the physical file, 
    // and rename it in the cache so we don't lose data.
    cache_node_t *node = get_or_load_cache(from);
    if (node) {
        flush_cache_node_to_disk(node);
        strcpy(node->path, to); // Update cache name
    }
    pthread_mutex_unlock(&cache_mutex);

    int res = rename(ffrom, fto);
    if (res == -1) return -errno;

    print_ram_state();
    
    return 0;
}

static int khfs_truncate(const char *path, off_t size) {
    char fpath[PATH_MAX];
    khfs_fullpath(fpath, path);

    pthread_mutex_lock(&cache_mutex);
    cache_node_t *node = get_or_load_cache(path);
    if (node) {
        char *new_data = realloc(node->data, size);
        if (size > node->size && new_data) {
            memset(new_data + node->size, 0, size - node->size);
        }
        if (new_data || size == 0) {
            node->data = new_data;
        }
        node->size = size;
        node->dirty = 1;
    }
    pthread_mutex_unlock(&cache_mutex);

    printf("\n[khfs] Action: Truncated %s to %ld bytes\n", path, size);
    print_ram_state();

    int res = truncate(fpath, size);
    if (res == -1) return -errno;
    return 0;
}

static void khfs_destroy(void *userdata) {
    (void) userdata;
    printf("\n[khfs] Unmounting. Flushing RAM cache to disk...\n");

    pthread_mutex_lock(&cache_mutex);
    cache_node_t *curr = cache_head;
    while (curr) {
        flush_cache_node_to_disk(curr);
        cache_node_t *next = curr->next;
        free(curr->data);
        free(curr);
        curr = next;
    }
    cache_head = NULL;
    pthread_mutex_unlock(&cache_mutex);

    print_ram_state();
    
    printf("[khfs] Cache flushed. Goodbye!\n");
}

static struct fuse_operations khfs_oper = {
    .getattr  = khfs_getattr,
    .readdir  = khfs_readdir,
    .read     = khfs_read,
    .write    = khfs_write,
    .create   = khfs_create,
    .mknod    = khfs_mknod,
    .mkdir    = khfs_mkdir,
    .unlink   = khfs_unlink,
    .rmdir    = khfs_rmdir,
    .rename   = khfs_rename,
    .truncate = khfs_truncate,
    .destroy  = khfs_destroy,
    .utimens = khfs_utimens,
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s [working_directory] [mount_directory]\n", argv[0]);
        exit(1);
    }

    struct khfs_state *khfs_data = malloc(sizeof(struct khfs_state));
    
    // Resolve the absolute path of the working directory
    khfs_data->rootdir = realpath(argv[argc-2], NULL);


    if (khfs_data->rootdir == NULL) {
        perror("realpath working_directory");
        exit(1);
    }

    // Shift arguments to hide the working directory from FUSE
    argv[argc-2] = argv[argc-1];
    argv[argc-1] = NULL;
    argc--;

    printf("[khfs] Mirroring %s to %s with Lazy RAM Caching...\n", khfs_data->rootdir, argv[argc-1]);
    
    return fuse_main(argc, argv, &khfs_oper, khfs_data);
}
