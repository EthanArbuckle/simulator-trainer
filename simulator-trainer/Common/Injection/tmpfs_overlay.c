//
//  tmpfs_overlay.c
//  simulator-trainer
//
//  Created by Ethan Arbuckle on 4/29/25.
//

#include <CoreFoundation/CoreFoundation.h>
#include "tmpfs_overlay.h"
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/sysctl.h>

#define OVERLAY_CONFIG_PATH "/var/jb/overlays/overlay_list.conf"
#define OVERLAY_STORE_PREFIX "/var/jb/overlays"

typedef struct {
    char target_path[PATH_MAX];
    char backing_store[PATH_MAX];
} overlay_info_t;

static kern_return_t ensure_directory_exists(const char *path);
static bool dir_exists_and_nonempty(const char *dir);
static kern_return_t copy_dir_recursive(const char *src, const char *dst);
static kern_return_t write_overlay_config(const overlay_info_t *overlay);
static kern_return_t read_overlay_config(overlay_info_t **overlays_out, int *count_out);
static kern_return_t remove_directory_recursive(const char *path);
static kern_return_t symlink_contents_of_dir(const char *store_path, const char *overlay_path);
static bool is_symlink_pointing_to_store(const char *item_path, const char *store_prefix);
static kern_return_t commit_item_recursive(const char *overlay_item, const char *store_item, const char *store_prefix);

int commit_overlay_changes(const char *overlay_path) {
    if (overlay_path == NULL) {
        fprintf(stderr, "Error: overlay_path is NULL\n");
        return -1;
    }
    
    char store_path[PATH_MAX];
    if (snprintf(store_path, sizeof(store_path), "%s%s", OVERLAY_STORE_PREFIX, overlay_path) >= sizeof(store_path)) {
        fprintf(stderr, "Error: Path too long: %s%s\n", OVERLAY_STORE_PREFIX, overlay_path);
        return -1;
    }
    
    if (!is_tmpfs_mount(overlay_path)) {
        fprintf(stderr, "Cannot commit %s: not a tmpfs overlay\n", overlay_path);
        return -1;
    }
    
    kern_return_t ret = commit_item_recursive(overlay_path, store_path, store_path);
    return (ret == 0) ? 0 : -1;
}

int reapply_all_overlays(void) {
    overlay_info_t *list = NULL;
    int count = 0;
    kern_return_t ret = read_overlay_config(&list, &count);
    if (ret != 0 || list == NULL) {
        return -1;
    }
    
    int success = 0;
    for (int i = 0; i < count; i++) {
        if (create_or_remount_overlay_symlinks(list[i].target_path) == 0) {
            success++;
        }
    }
    
    free(list);
    return (success == count) ? 0 : -1;
}

kern_return_t create_or_remount_overlay_symlinks(const char *path) {
    if (geteuid() != 0) {
        fprintf(stderr, "Must be root to create overlay on %s\n", path);
        return -1;
    }
    
    if (is_tmpfs_mount(path)) {
        fprintf(stdout, "Overlay already exists on %s. Nothing to do\n", path);
        return 0;
    }
    
    if (is_mount_point(path) && !is_tmpfs_mount(path)) {
        fprintf(stderr, "Path %s is already a mount point. Cannot override\n", path);
        return -1;
    }
    
    char store_path[PATH_MAX];
    if (snprintf(store_path, sizeof(store_path), "%s%s", OVERLAY_STORE_PREFIX, path) >= sizeof(store_path)) {
        fprintf(stderr, "Error: Path too long: %s%s\n", OVERLAY_STORE_PREFIX, path);
        return -1;
    }
    
    if (!dir_exists_and_nonempty(store_path)) {
        kern_return_t ret = copy_dir_recursive(path, store_path);
        if (ret != 0) {
            fprintf(stderr, "Failed initial copy to backing store\n");
            return ret;
        }
    }
    
    unmount_if_mounted(path);
    
    struct tmpfs_args {
        uint64_t max_pages;
        uint64_t max_nodes;
        uint64_t case_insensitive;
    } args;
    
    args.max_pages = 1024 * 1024 * 1024 / getpagesize();
    args.max_nodes = UINT16_MAX;
    args.case_insensitive = 0;
    if (mount("tmpfs", path, 0, &args) != 0) {
        fprintf(stderr, "Failed to mount tmpfs on %s: %s\n", path, strerror(errno));
        return -1;
    }
    
    kern_return_t ret = symlink_contents_of_dir(store_path, path);
    if (ret != 0) {
        fprintf(stderr, "Failed to symlink backing store contents\n");
        if (unmount(path, MNT_FORCE) != 0) {
            fprintf(stderr, "Warning: Failed to unmount after error: %s\n", strerror(errno));
        }
        
        return ret;
    }
    
    overlay_info_t ov;
    memset(&ov, 0, sizeof(ov));
    strncpy(ov.target_path, path, sizeof(ov.target_path) - 1);
    strncpy(ov.backing_store, store_path, sizeof(ov.backing_store) - 1);
    ret = write_overlay_config(&ov);
    if (ret != 0) {
        fprintf(stderr, "Warning: Failed to write overlay config, but overlay is mounted\n");
    }
    
    return 0;
}

bool is_tmpfs_mount(const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0) {
        return false;
    }
    
    return strstr(fs.f_fstypename, "tmpfs") != NULL;
}

bool is_mount_point(const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0) {
        return false;
    }
    
    struct stat path_stat, parent_stat;
    if (stat(path, &path_stat) != 0) {
        return false;
    }
    
    char parent_path[PATH_MAX];
    if (snprintf(parent_path, sizeof(parent_path), "%s/..", path) >= sizeof(parent_path)) {
        return false;
    }
    
    if (stat(parent_path, &parent_stat) != 0) {
        return false;
    }
    
    return path_stat.st_dev != parent_stat.st_dev;
}

kern_return_t unmount_if_mounted(const char *path) {
    if (is_mount_point(path)) {
        if (unmount(path, MNT_FORCE) != 0) {
            fprintf(stderr, "Failed to unmount %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    
    return 0;
}

static kern_return_t ensure_directory_exists(const char *path) {
    if (path == NULL) {
        return -1;
    }
    
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= sizeof(tmp)) {
        fprintf(stderr, "Path too long: %s\n", path);
        return -1;
    }
    
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to mkdir %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    
    return 0;
}

static bool dir_exists_and_nonempty(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return false;
    }
    
    bool has_entry = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            has_entry = true;
            break;
        }
    }
    
    closedir(d);
    return has_entry;
}

static kern_return_t copy_dir_recursive(const char *src, const char *dst) {
    if (src == NULL || dst == NULL) {
        return -1;
    }
    
    DIR *dir = opendir(src);
    if (dir == NULL) {
        fprintf(stderr, "Failed to opendir('%s'): %s\n", src, strerror(errno));
        return -1;
    }
    
    kern_return_t ret = ensure_directory_exists(dst);
    if (ret != 0) {
        closedir(dir);
        return ret;
    }
    
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        if (strcmp(ent->d_name, ".fseventsd") == 0) {
            continue;
        }
        
        char src_path[PATH_MAX];
        if (snprintf(src_path, sizeof(src_path), "%s/%s", src, ent->d_name) >= sizeof(src_path)) {
            fprintf(stderr, "Path too long: %s/%s\n", src, ent->d_name);
            closedir(dir);
            return -1;
        }
        
        char dst_path[PATH_MAX];
        if (snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, ent->d_name) >= sizeof(dst_path)) {
            fprintf(stderr, "Path too long: %s/%s\n", dst, ent->d_name);
            closedir(dir);
            return -1;
        }
        
        struct stat st;
        if (lstat(src_path, &st) != 0) {
            fprintf(stderr, "Failed to lstat('%s'): %s\n", src_path, strerror(errno));
            closedir(dir);
            return -1;
        }
        
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            ret = copy_dir_recursive(src_path, dst_path);
            if (ret != 0) {
                closedir(dir);
                return ret;
            }
        }
        else {
            copyfile_state_t cst = copyfile_state_alloc();
            if (cst == NULL) {
                fprintf(stderr, "Failed to allocate copyfile state\n");
                closedir(dir);
                return -1;
            }

            if (copyfile(src_path, dst_path, cst, COPYFILE_ALL) != 0) {
                if (errno == ENOENT) {
                    fprintf(stderr, "Warning: Source missing, skipping '%s'\n", src_path);
                    copyfile_state_free(cst);
                    continue;
                }
                else {
                    fprintf(stderr, "Failed copy '%s' -> '%s': %s\n", src_path, dst_path, strerror(errno));
                    copyfile_state_free(cst);
                    closedir(dir);
                    return -1;
                }
            }

            copyfile_state_free(cst);
        }
    }
    
    closedir(dir);
    return 0;
}

static kern_return_t write_overlay_config(const overlay_info_t *overlay) {
    if (overlay == NULL) {
        return -1;
    }
    
    kern_return_t ret = ensure_directory_exists("/var/jb/overlays");
    if (ret != 0) {
        return ret;
    }
    
    overlay_info_t *existing = NULL;
    int count = 0;
    ret = read_overlay_config(&existing, &count);
    if (ret == 0 && existing != NULL) {
        for (int i = 0; i < count; i++) {
            if (strcmp(existing[i].target_path, overlay->target_path) == 0) {
                free(existing);
                return 0;
            }
        }
        free(existing);
    }
    
    FILE *f = fopen(OVERLAY_CONFIG_PATH, "a");
    if (f == NULL) {
        fprintf(stderr, "Cannot open config '%s': %s\n", OVERLAY_CONFIG_PATH, strerror(errno));
        return -1;
    }
    
    fprintf(f, "%s|%s\n", overlay->target_path, overlay->backing_store);
    fclose(f);
    return 0;
}

static kern_return_t read_overlay_config(overlay_info_t **overlays_out, int *count_out) {
    if (overlays_out == NULL || count_out == NULL) {
        return -1;
    }
    
    *overlays_out = NULL;
    *count_out = 0;
    
    FILE *f = fopen(OVERLAY_CONFIG_PATH, "r");
    if (f == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        
        fprintf(stderr, "Cannot open config '%s': %s\n", OVERLAY_CONFIG_PATH, strerror(errno));
        return -1;
    }
    
    char line[2 * PATH_MAX];
    overlay_info_t *list = NULL;
    int capacity = 0;
    int count = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        
        char *sep = strchr(line, '|');
        if (sep == NULL) {
            continue;
        }
        
        *sep = '\0';
        const char *target = line;
        const char *store = sep + 1;
        
        if (count >= capacity) {
            capacity = (capacity == 0) ? 8 : capacity * 2;
            overlay_info_t *new_list = realloc(list, capacity * sizeof(overlay_info_t));
            if (new_list == NULL) {
                free(list);
                fclose(f);
                fprintf(stderr, "Memory allocation failure\n");
                return -1;
            }
            list = new_list;
        }
        
        memset(&list[count], 0, sizeof(overlay_info_t));
        strncpy(list[count].target_path, target, sizeof(list[count].target_path) - 1);
        strncpy(list[count].backing_store, store, sizeof(list[count].backing_store) - 1);
        count++;
    }
    fclose(f);
    
    *overlays_out = list;
    *count_out = count;
    return 0;
}

static kern_return_t remove_directory_recursive(const char *path) {
    if (path == NULL) {
        return -1;
    }
    
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        fprintf(stderr, "Could not lstat '%s': %s\n", path, strerror(errno));
        return -1;
    }
    
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir = opendir(path);
        if (dir == NULL) {
            fprintf(stderr, "remove_directory_recursive(), opendir('%s') failed: %s\n", path, strerror(errno));
            return -1;
        }
        
        struct dirent *ent;
        kern_return_t ret = 0;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            
            if (strcmp(ent->d_name, ".fseventsd") == 0) {
                continue;
            }
            
            char subpath[PATH_MAX];
            if (snprintf(subpath, sizeof(subpath), "%s/%s", path, ent->d_name) >= sizeof(subpath)) {
                fprintf(stderr, "Path too long: %s/%s\n", path, ent->d_name);
                ret = -1;
                break;
            }
            
            if (remove_directory_recursive(subpath) != 0) {
                ret = -1;
                break;
            }
        }
        
        closedir(dir);
        if (ret != 0) {
            return ret;
        }
        
        if (rmdir(path) != 0) {
            if (errno == EBUSY && is_mount_point(path)) {
                return 0;
            }
            
            fprintf(stderr, "rmdir('%s') failed: %s\n", path, strerror(errno));
            return -1;
        }
    }
    else {
        if (unlink(path) != 0) {
            fprintf(stderr, "unlink('%s') failed: %s\n", path, strerror(errno));
            return -1;
        }
    }
    
    return 0;
}

static kern_return_t symlink_contents_of_dir(const char *store_path, const char *overlay_path) {
    if (store_path == NULL || overlay_path == NULL) {
        return -1;
    }
    
    DIR *d = opendir(store_path);
    if (d == NULL) {
        fprintf(stderr, "opendir***('%s') failed: %s\n", store_path, strerror(errno));
        return -1;
    }
    
    kern_return_t ret = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        if (strcmp(ent->d_name, ".fseventsd") == 0) {
            continue;
        }
        
        char store_item[PATH_MAX];
        if (snprintf(store_item, sizeof(store_item), "%s/%s", store_path, ent->d_name) >= sizeof(store_item)) {
            fprintf(stderr, "Path too long: %s/%s\n", store_path, ent->d_name);
            ret = -1;
            break;
        }
        
        char overlay_item[PATH_MAX];
        if (snprintf(overlay_item, sizeof(overlay_item), "%s/%s", overlay_path, ent->d_name) >= sizeof(overlay_item)) {
            fprintf(stderr, "Path too long: %s/%s\n", overlay_path, ent->d_name);
            ret = -1;
            break;
        }
        
        if (symlink(store_item, overlay_item) != 0) {
            if (errno == EEXIST) {
                if (remove_directory_recursive(overlay_item) != 0) {
                    ret = -1;
                    break;
                }
                
                if (symlink(store_item, overlay_item) != 0) {
                    fprintf(stderr, "Failed symlink('%s','%s'): %s\n", store_item, overlay_item, strerror(errno));
                    ret = -1;
                    break;
                }
            }
            else {
                fprintf(stderr, "Failed symlink('%s','%s'): %s\n", store_item, overlay_item, strerror(errno));
                ret = -1;
                break;
            }
        }
    }
    
    closedir(d);
    return ret;
}

static bool is_symlink_pointing_to_store(const char *item_path, const char *store_prefix) {
    if (item_path == NULL || store_prefix == NULL) {
        return false;
    }
    
    char buf[PATH_MAX];
    ssize_t len = readlink(item_path, buf, sizeof(buf) - 1);
    if (len < 0) {
        return false;
    }
    buf[len] = '\0';
    
    if (strncmp(buf, store_prefix, strlen(store_prefix)) == 0) {
        return true;
    }
    
    return false;
}

static kern_return_t commit_item_recursive(const char *overlay_item, const char *store_item, const char *store_prefix) {
    if (overlay_item == NULL || store_item == NULL || store_prefix == NULL) {
        return -1;
    }
    
    struct stat st;
    if (lstat(overlay_item, &st) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        
        fprintf(stderr, "lstat('%s') failed: %s\n", overlay_item, strerror(errno));
        return -1;
    }
    
    if (S_ISLNK(st.st_mode) && is_symlink_pointing_to_store(overlay_item, store_prefix)) {
        return 0;
    }
    
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        kern_return_t ret = ensure_directory_exists(store_item);
        if (ret != 0) {
            return ret;
        }
        
        DIR *d = opendir(overlay_item);
        if (d == NULL) {
            fprintf(stderr, "opendir*('%s') failed: %s\n", overlay_item, strerror(errno));
            return -1;
        }
        
        struct dirent *ent;
        kern_return_t ret_dir = 0;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            
            if (strcmp(ent->d_name, ".fseventsd") == 0) {
                continue;
            }
            
            char child_overlay[PATH_MAX];
            if (snprintf(child_overlay, sizeof(child_overlay), "%s/%s", overlay_item, ent->d_name) >= sizeof(child_overlay)) {
                fprintf(stderr, "Path too long: %s/%s\n", overlay_item, ent->d_name);
                ret_dir = -1;
                break;
            }
            
            char child_store[PATH_MAX];
            if (snprintf(child_store, sizeof(child_store), "%s/%s", store_item, ent->d_name) >= sizeof(child_store)) {
                fprintf(stderr, "Path too long: %s/%s\n", store_item, ent->d_name);
                ret_dir = -1;
                break;
            }
            
            if (commit_item_recursive(child_overlay, child_store, store_prefix) != 0) {
                ret_dir = -1;
                break;
            }
        }
        
        closedir(d);
        if (ret_dir != 0) {
            return ret_dir;
        }
        
        struct stat overlay_stat, parent_stat;
        char parent_path[PATH_MAX];
        snprintf(parent_path, sizeof(parent_path), "%s/..", overlay_item);
        if (stat(overlay_item, &overlay_stat) == 0 && stat(parent_path, &parent_stat) == 0 && overlay_stat.st_dev != parent_stat.st_dev) {
            return 0;
        }
        
        if (remove_directory_recursive(overlay_item) != 0) {
            return -1;
        }
        
        if (symlink(store_item, overlay_item) != 0) {
            fprintf(stderr, "symlink('%s','%s') failed: %s\n", store_item, overlay_item, strerror(errno));
            return -1;
        }
        
        return 0;
    }
    
    {
        char store_dir[PATH_MAX];
        strncpy(store_dir, store_item, sizeof(store_dir) - 1);
        store_dir[sizeof(store_dir) - 1] = '\0';
        
        char *slash = strrchr(store_dir, '/');
        if (slash) {
            *slash = '\0';
            kern_return_t ret = ensure_directory_exists(store_dir);
            if (ret != 0) {
                return ret;
            }
        }
        
        struct stat st2;
        if (lstat(store_item, &st2) == 0) {
            if (remove_directory_recursive(store_item) != 0) {
                return -1;
            }
        }
        
        copyfile_state_t cst = copyfile_state_alloc();
        if (cst == NULL) {
            fprintf(stderr, "Failed to allocate copyfile state\n");
            return -1;
        }
        
        if (copyfile(overlay_item, store_item, cst, COPYFILE_ALL) != 0) {
            fprintf(stderr, "Failed to copy '%s' -> '%s': %s\n", overlay_item, store_item, strerror(errno));
            copyfile_state_free(cst);
            return -1;
        }
        copyfile_state_free(cst);
        
        if (remove_directory_recursive(overlay_item) != 0) {
            return -1;
        }
        
        if (symlink(store_item, overlay_item) != 0) {
            fprintf(stderr, "symlink('%s','%s') failed: %s\n", store_item, overlay_item, strerror(errno));
            return -1;
        }
        
        return 0;
    }
}
