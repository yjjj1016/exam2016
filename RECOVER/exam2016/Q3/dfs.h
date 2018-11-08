#define BLOCKSIZE 512
#define BLOCKSIZE_BITS 8
#define NUMBLOCKS 100
#define MAXNAME 15

#define MAXDATASIZE (BLOCKSIZE - 3*sizeof(int))

#define MIN(a,b) (((a)<(b))?(a):(b))


#define true 1
#define false 0


struct dfs_inode {
  int is_empty;
  int is_directory;
  int size;
  char data[MAXDATASIZE];
};

struct dfs_dir_entry {
  char name[MAXNAME+1];
  int inode_number;
};
