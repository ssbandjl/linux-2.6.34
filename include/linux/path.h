#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

struct path {
	struct vfsmount *mnt;    //文件系统信息
	struct dentry *dentry;  // 文件名与inode之间的关联
};

extern void path_get(struct path *);
extern void path_put(struct path *);

#endif  /* _LINUX_PATH_H */
