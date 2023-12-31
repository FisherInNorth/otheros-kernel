#include <fs/buf.h>
#include <fs/cluster.h>
#include <fs/dirent.h>
#include <fs/fat32.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/error.h>
#include <lib/log.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <lib/wchar.h>
#include <lock/mutex.h>

extern mutex_t mtx_file;

/**
 * 概要：专门管理目录相关的底层事务，交互的对象是raw_dirent
 * 功能：读取、写入和分配FAT32目录项
 * TODO：需要引入删除目录项的内容
 */

/**
 * @brief 从offset偏移开始，查询一个目录项
 * @param offset 开始查询的位置偏移
 * @param next_offset 下一个dirent的开始位置
 * @note 目前设计为仅在初始化时使用，因此使用file_read读取，无需外部加锁
 * @return 读取的内容长度。若为0，表示读到末尾
 */
int dirGetDentFrom(Dirent *dir, u64 offset, struct Dirent **file, int *next_offset,
		   longEntSet *longSet) {
	mtx_lock_sleep(&mtx_file);

	char direntBuf[DIRENT_SIZE];
	assert(offset % DIR_SIZE == 0);

	FileSystem *fs = dir->file_system;
	u32 j;
	FAT32Directory *f;
	FAT32LongDirectory *longEnt;
	int clusSize = CLUS_SIZE(fs);

	char tmpName[MAX_NAME_LEN];
	char tmpBuf[32] __attribute__((aligned(2)));
	wchar fullName[MAX_NAME_LEN]; // 以wchar形式储存的文件名
	fullName[0] = 0;	      // 初始化为空字符串

	if (longSet)
		longSet->cnt = 0; // 初始化longSet有0个元素

	// 1. 跳过dir中的无效项目
	for (j = offset; j < dir->file_size; j += DIR_SIZE) {
		file_read(dir, 0, (u64)direntBuf, j, DIR_SIZE);
		f = ((FAT32Directory *)direntBuf);

		// 跳过空项（FAT32_INVALID_ENTRY表示已删除）
		if (f->DIR_Name[0] == 0 || f->DIR_Name[0] == FAT32_INVALID_ENTRY)
			continue;

		// 跳过"."和".." (因为我们在解析路径时使用字符串匹配，不使用FAT32内置的.和..机制)
		if (strncmp((const char *)f->DIR_Name, ".          ", 11) == 0
			|| strncmp((const char *)f->DIR_Name, "..         ", 11) == 0)
			continue;

		// 是长文件名项（可能属于文件也可能属于目录）
		if (f->DIR_Attr == ATTR_LONG_NAME_MASK) {
			longEnt = (FAT32LongDirectory *)f;
			// 是第一项
			if (longEnt->LDIR_Ord & LAST_LONG_ENTRY) {
				tmpName[0] = 0;
				if (longSet)
					longSet->cnt = 0;
			}

			// 向longSet里面存放长文件名项的指针
			if (longSet)
				longSet->longEnt[longSet->cnt++] = longEnt;
			memcpy(tmpBuf, (char *)longEnt->LDIR_Name1, 10);
			memcpy(tmpBuf + 10, (char *)longEnt->LDIR_Name2, 12);
			memcpy(tmpBuf + 22, (char *)longEnt->LDIR_Name3, 4);
			wstrnins(fullName, (const wchar *)tmpBuf, 13);
		} else {
			if (wstrlen(fullName) != 0) {
				wstr2str(tmpName, fullName);
			} else {
				strncpy(tmpName, (const char *)f->DIR_Name, 11);
				tmpName[11] = 0;
			}

			log(FS_MODULE, "find: \"%s\"\n", tmpName);

			// 2. 设置找出的dirent的信息（为NULL的无需设置）
			Dirent *dirent = dirent_alloc();
			strncpy(dirent->name, tmpName, MAX_NAME_LEN);

			extern struct FileDev file_dev_file;
			dirent->dev = &file_dev_file; // 赋值设备指针

			dirent->raw_dirent = *f; // 指向根dirent
			dirent->file_system = fs;
			dirent->first_clus = f->DIR_FstClusHI * 65536 + f->DIR_FstClusLO;
			dirent->file_size = f->DIR_FileSize;
			dirent->parent_dir_off = j; // 父亲目录内偏移
			dirent->type = IS_DIRECTORY(f) ? DIRENT_DIR : DIRENT_FILE;
			dirent->parent_dirent = dir; // 设置父级目录项
			LIST_INIT(&dirent->child_list);
			dirent->linkcnt = 1;

			// 对于目录文件的大小，我们将其重置为其簇数乘以簇大小，不再是0
			if (dirent->raw_dirent.DIR_Attr & ATTR_DIRECTORY) {
				dirent->file_size = countClusters(dirent) * clusSize;
			}

			*file = dirent;
			*next_offset = j + DIR_SIZE;

			mtx_unlock_sleep(&mtx_file);
			return DIR_SIZE;
		}
	}

	warn("no more dents in dir: %s\n", dir->name);
	*next_offset = dir->file_size;

	mtx_unlock_sleep(&mtx_file);
	return 0; // 读到结尾
}

/**
 * @brief 将Dirent结构体里的有效数据同步到dirent中，并写回(无需加锁)
 */
void sync_dirent_rawdata_back(Dirent *dirent) {
	// first_clus, file_size
	// name不需要，因为在文件创建阶段就已经固定了
	dirent->raw_dirent.DIR_FstClusHI = dirent->first_clus / 65536;
	dirent->raw_dirent.DIR_FstClusLO = dirent->first_clus % 65536;
	dirent->raw_dirent.DIR_FileSize = dirent->file_size;

	// 将目录项写回父级目录中
	Dirent *parentDir = dirent->parent_dirent;

	if (parentDir == NULL) {
		// Note: 本目录是root，所以无需将目录大小写回上级目录
		return;
	}

	file_write(parentDir, 0, (u64)&dirent->raw_dirent, dirent->parent_dir_off, DIRENT_SIZE);
}

/**
 * @brief 在某个目录中分配cnt个连续的目录项。需要携带父亲Dirent的锁
 * @param dir 要分配文件的目录
 * @param ent 返回的分配了的目录项。如果要求多个，Dirent->parent_dir_off设为最后一项的偏移
 * @param cnt 需要分配的连续目录项数
 * @update 从clusterRead/clusterWrite的写法改为file_read/file_write，以减少代码量，提高复用度
 */
static int dir_alloc_entry(Dirent *dir, Dirent **ent, int cnt) {
	assert(cnt >= 1); // 要求分配的个数不能小于1
	mtx_lock_sleep(&mtx_file);

	u32 offset = 0;
	int curN = 0;
	int start_off = 0, end_off = 0; // 分配的终止目录项的偏移
	FAT32Directory fat32_dirent;
	int prev_size = dir->file_size;

	// 1. 分配若干连续的目录项
	while (offset < prev_size) {
		file_read(dir, 0, (u64)&fat32_dirent, offset, DIR_SIZE);
		if (fat32_dirent.DIR_Name[0] == 0) {
			// 表示为空闲目录项
			curN += 1;

			// 表示积攒的连续目录项已足够cnt个
			if (curN == cnt) {
				start_off = offset - (cnt - 1) * DIR_SIZE;
				end_off = offset;
				break;
			}
		} else {
			curN = 0;
		}
		offset += DIR_SIZE;
	}

	// 2. 若目录现存磁盘块中无连续目录项，则在文件尾部分配
	assert(prev_size % DIR_SIZE == 0);
	if (curN != cnt) {
		start_off = prev_size;
		end_off = prev_size + DIR_SIZE * (cnt - 1);
	}

	// 3. 将分配的目录项染黑，防止后面的分配使用到该块
	memset(&fat32_dirent, 0, DIR_SIZE);
	fat32_dirent.DIR_Name[0] = FAT32_INVALID_ENTRY;
	for (offset = start_off; offset <= end_off; offset += DIR_SIZE) {
		file_write(dir, 0, (u64)&fat32_dirent, offset, DIR_SIZE);
	}

	// 4. 记录尾部目录项（即记录文件元信息的目录项）的偏移
	Dirent *dirent = dirent_alloc();
	dirent->parent_dir_off = offset + DIR_SIZE * (cnt - 1);
	*ent = dirent;

	mtx_unlock_sleep(&mtx_file);
	return 0;
}

/**
 * @brief 填写长文件名项，返回应该继续填的位置。如果已经填完，返回NULL
 */
static char *fill_long_entry(FAT32LongDirectory *longDir, char *raw_name) {
	wchar _name[MAX_NAME_LEN];
	wchar *name = _name;
	str2wstr(name, raw_name);
	int len = strlen(raw_name) + 1;

	longDir->LDIR_Attr = ATTR_LONG_NAME_MASK;
	memcpy((char *)longDir->LDIR_Name1, (char *)name, 10);
	len -= 5, name += 5;
	if (len <= 0) {
		return NULL;
	}

	memcpy((char *)longDir->LDIR_Name2, (char *)name, 12);
	len -= 6, name += 6;
	if (len <= 0) {
		return NULL;
	}

	memcpy((char *)longDir->LDIR_Name3, (char *)name, 4);
	len -= 2, name += 2;
	if (len <= 0) {
		return NULL;
	} else {
		return raw_name + (name - _name);
	}
}

/**
 * @brief
 * 分配一个目录项，其中填入文件名（已支持长文件名）。无需获取dir的锁，因为程序内将自动获取锁。出函数将携带file的锁
 */
int dir_alloc_file(Dirent *dir, Dirent **file, char *name) {
	Dirent *dirent = dirent_alloc();

	log(FS_GLOBAL, "create a file using long Name! name is %s\n", name);
	int cnt = get_entry_count_by_name(name);

	unwrap(dir_alloc_entry(dir, &dirent, cnt));
	strncpy(dirent->name, name, MAX_NAME_LEN);
	strncpy((char *)dirent->raw_dirent.DIR_Name, name, 10);
	dirent->raw_dirent.DIR_Name[10] = 0;
	dget(dirent);

	// 倒序填写长文件名
	for (int i = 1; i <= cnt - 1; i++) {
		FAT32LongDirectory longDir;

		memset(&longDir, 0, sizeof(FAT32LongDirectory));
		name = fill_long_entry(&longDir, name);
		longDir.LDIR_Ord = i;
		if (i == 1)
			longDir.LDIR_Ord = LAST_LONG_ENTRY;

		// 写入到目录中
		file_write(dir, 0, (u64)&longDir, dirent->parent_dir_off - i * DIR_SIZE, DIR_SIZE);

		if (name == NULL) {
			break;
		}
	}

	*file = dirent;
	return 0;
}
