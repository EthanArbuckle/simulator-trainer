//
//  tmpfs_overlay.h
//  simulator-trainer
//
//  Created by Ethan Arbuckle on 4/29/25.
//

int create_or_remount_overlay_symlinks(const char *path);
int commit_overlay_changes(const char *overlay_path);
int reapply_all_overlays(void);

bool is_tmpfs_mount(const char *path);
bool is_mount_point(const char *path);
kern_return_t unmount_if_mounted(const char *path);
