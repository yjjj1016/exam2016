
/* hello world module - Eric McCreath 2005,2006,2008,2010,2012,2016 */
/* to compile use:
    make -C  /usr/src/linux-headers-`uname -r` SUBDIRS=$PWD modules
   to install into the kernel use :
    insmod hello.ko
   to test :
    cat /proc/hello
   to remove :
    rmmod hello
*/

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>

static int hello_proc_show(struct seq_file *m, void *v)
{
    printk("proc hello show\n");
    seq_printf(m, "Hello World!\n");
    return 0;
}

static int hello_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, hello_proc_show, NULL);
}

static const struct file_operations hello_proc_fops = {
	.open		= hello_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_hello_init(void)
{
    printk("init proc hello\n");
    proc_create("hello", 0, NULL, &hello_proc_fops);
    return 0;
}
static void __exit cleanup_hello_module(void)
{
    printk("cleanup proc hello\n");
    remove_proc_entry("hello",NULL);
}


module_init(proc_hello_init);
module_exit(cleanup_hello_module);



