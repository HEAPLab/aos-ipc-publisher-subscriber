#include <linux/cdev.h> 
#include <linux/delay.h> 
#include <linux/device.h> 
#include <linux/fs.h> 
#include <linux/init.h> 
#include <linux/irq.h> 
#include <linux/kernel.h> 
#include <linux/module.h> 
#include <linux/poll.h> 
#include <linux/string.h>
#include <linux/list.h>

//new_topic functions
static int new_topic_open(struct inode *, struct file *); 
static int new_topic_release(struct inode *, struct file *); 
static ssize_t new_topic_read(struct file *, char __user *, size_t, loff_t *); 
static ssize_t new_topic_write(struct file *, const char __user *, size_t, loff_t *); 

//subscribe functions
static int subscribe_open(struct inode*, struct file*);
static int subscribe_release(struct inode*, struct file*); 
static ssize_t subscribe_write(struct file*, const char __user *, size_t, loff_t*);

//subscribers_list functions
static int subs_list_open(struct inode*, struct file*);
static int subs_list_release(struct inode*, struct file*);
static ssize_t subs_list_read(struct file*, char __user *, size_t, loff_t*);

//signal_nr functions
static int signal_nr_open(struct inode*, struct file*);
static int signal_nr_release(struct inode*, struct file*);
static ssize_t signal_nr_write(struct file*, const char __user *, size_t, loff_t*);

//endpoint functions
static int endpoint_open(struct inode*, struct file*);
static int endpoint_release(struct inode*, struct file*);
static ssize_t endpoint_write(struct file*, const char __user *, size_t, loff_t*);

//other functions
static void release_files(void); 
static void display_list(void);
static void display_pid_list(struct list_head*);
static struct topic_node* search_node(char*);  
static int pro_atoi(char*);
 
#define SUCCESS 0 
#define NEW_TOPIC_PATH "psipc/new_topic" /* Device name as it appears in /proc/devices */ 
#define TOPICS_PATH "psipc/topics/"
#define BUF_LEN 100 /* Max length of the message from the device */ 
#define NUM_SPECIAL_FILES 4 /* Number of device files for every topic */
#define MAX_SIZE_PID 10 /* On a 64-bit system the the max pid value is 4194304 */
 
static struct file_operations new_topic_dev_fops = { 
	.owner = THIS_MODULE,
    .read = new_topic_read, 
    .write = new_topic_write, 
    .open = new_topic_open, 
    .release = new_topic_release, 
};

static struct file_operations subscribe_fops = {
    .owner = THIS_MODULE,
    .write = subscribe_write,
    .open = subscribe_open,
    .release = subscribe_release,
};

static struct file_operations subscribers_list_fops = {
    .owner = THIS_MODULE,
    .read = subs_list_read,
    .open = subs_list_open,
    .release = subs_list_release,
}; 

static struct file_operations signal_nr_fops = {
    .owner = THIS_MODULE,
	.write = signal_nr_write,
	.open = signal_nr_open,
	.release = signal_nr_release,
};

static struct file_operations endpoint_fops = {
    .owner = THIS_MODULE,
	.write = endpoint_write,
	.open = endpoint_open,
	.release = endpoint_release,
};

static struct topic_node{
    char *dir_name; /* path name of the topic */
	struct class file_dev_cls[NUM_SPECIAL_FILES]; /* array of struct class, one for every device files */
    dev_t devices[NUM_SPECIAL_FILES]; /* array of dev_t, one for every device files, used to store device numbers*/
    struct list_head pidListHead; /* head of list of struct pid_node */
    int nr_signal; /* type of signal to send to all the topic subscribers */
    struct list_head list;
};

static struct pid_node{
    int pid;
    struct list_head list;
};

static struct list_head topicListHead; /* head of list of struct topic_node */

enum { 
    CDEV_NOT_USED = 0, 
    CDEV_EXCLUSIVE_OPEN = 1, 
}; 
 
/* Is device open? Used to prevent multiple access to device */ 
static atomic_t new_topic_already_open = ATOMIC_INIT(CDEV_NOT_USED); 
static atomic_t subscribers_list_already_open = ATOMIC_INIT(CDEV_NOT_USED);

static int major;
static int flag = 0;
static char msg[BUF_LEN]; /* The msg the device will give when asked */ 
 
static struct class *new_topic_cls; 

const char* files[] = {"/subscribe", "/subscribers_list", "/signal_nr", "/endpoint"};

const struct file_operations* fops[] = {&subscribe_fops, &subscribers_list_fops, &signal_nr_fops, &endpoint_fops};

/*
* It sets read and write permission to the device file
*/
static char *cls_set_readAndWrite_permission(struct device *dev, umode_t *mode){
    if(mode!=NULL){
        *mode = (umode_t)0666;
    }
    return NULL;
}

/*
* It sets write-only permission to the device file
*/
static char *cls_set_writeOnly_permission(struct device *dev, umode_t *mode){
    if(mode!=NULL){
        *mode = (umode_t)0622;
    }
    return NULL;
}

/*
* It sets read-only permission to the device file
*/
static char *cls_set_readOnly_permission(struct device *dev, umode_t *mode){
    if(mode!=NULL){
        *mode = (umode_t)0644;
    }
    return NULL;
}

/*
* Called when the module is loaded with insmod.
* It creates the /psipc directory and the new_topic character device.
*/
static int __init chardev_init(void)
{ 
    // Initialize the list of topics
    INIT_LIST_HEAD(&topicListHead);
    major = register_chrdev(0, NEW_TOPIC_PATH, &new_topic_dev_fops); 
 
    if (major < 0) { 
        pr_alert("Registering char device failed with %d\n", major); 
        return major; 
    } 
 
    pr_info("I was assigned major number %d.\n", major); 
 
    new_topic_cls = class_create(THIS_MODULE, NEW_TOPIC_PATH);
    if(IS_ERR(new_topic_cls)){

    }
    new_topic_cls->devnode = cls_set_readAndWrite_permission; 
    device_create(new_topic_cls, NULL, MKDEV(major, 0), NULL, NEW_TOPIC_PATH); 
 
    pr_info("Device created on /dev/%s\n", NEW_TOPIC_PATH); 
 
    return SUCCESS; 
} 
 
/*
* Called when the module is unloaded with rmmmod.
* It deletes all the device files for every topic and finally delete the /psipc directory.
*/
static void __exit chardev_exit(void) 
{ 
    release_files();
    device_destroy(new_topic_cls, MKDEV(major, 0)); 
    class_destroy(new_topic_cls); 
    unregister_chrdev(major, NEW_TOPIC_PATH); 
    pr_info("Device /dev/%s has been unregistered.\n", NEW_TOPIC_PATH);
} 
 
/*
* Called whenever a process attempts to open the new_topic device file.
*/
static int new_topic_open(struct inode *inode, struct file *file) 
{ 
    static int counter = 0; 
    
    /*
    * Read the 32-bit value of new_topic_already_open through its address.
    * Compute (new_topic_already_open == CDEV_NOT_USED) ?  CDEV_EXCLUSIVE_OPEN : old and store result in new_topic_already_open. 
    * The function returns old version of new_topic_already_open.
    */
    if (atomic_cmpxchg(&new_topic_already_open, CDEV_NOT_USED, CDEV_EXCLUSIVE_OPEN)) 
        return -EBUSY; 
 
    try_module_get(THIS_MODULE); 
 
    return SUCCESS; 
} 
 
/* 
* Called when a process closes the new_topic device file.
*/
static int new_topic_release(struct inode *inode, struct file *file) 
{  
    atomic_set(&new_topic_already_open, CDEV_NOT_USED); 
 
    module_put(THIS_MODULE); 
 
    return SUCCESS; 
} 

/*
* Called when a process, which already opened the new_topic device file, attempts to read from it.
*/
static ssize_t new_topic_read(struct file *filp,
                           char __user *buffer, /* user buffer to fill with data */ 
                           size_t length, /* length of the buffer */ 
                           loff_t *offset) 
{ 
    /* Number of bytes actually written to the buffer */ 
    int bytes_read = 0; 
    const char *msg_ptr = msg; 
 
    if (!*(msg_ptr + *offset)) { /* we are at the end of message */ 
        *offset = 0; /* reset the offset */ 
        return 0; /* signify end of file */ 
    } 
 
    msg_ptr += *offset; 
 
    /* Actually put the data into the buffer */ 
    while (length && *msg_ptr) { 
        put_user(*(msg_ptr++), buffer++); 
        length--; 
        bytes_read++; 
    } 
 
    *offset += bytes_read; 
 
    /* Return the number of bytes put into the buffer. */ 
    return bytes_read; 
} 
 
/* 
* Called when a process writes to the new_topic device file
* echo "test" > /dev/psipc/new_topic
*/
static ssize_t new_topic_write(struct file *filp, const char __user *buff, size_t len, loff_t *off) 
{ 
    int i, created_sub, path_len=0, buf_size, bytes_written; 
    char *dir;
    struct topic_node *elem;
    struct class *cls;
 
    for (i = 0; i < len && i < BUF_LEN; i++) 
        get_user(msg[i], buff + i);

    bytes_written = i;
    msg[i-1] = '\0';

    path_len += strlen(TOPICS_PATH);
    path_len += strlen(msg);

    if(!(dir = (char*)kmalloc(path_len, GFP_KERNEL))){
        pr_alert("Kmalloc error: cannot allocate memory for %s\n", msg);
        return -ENOMEM;
    }

    strcpy(dir, TOPICS_PATH);
    strcat(dir, msg);

    elem = (struct topic_node*)kmalloc(sizeof(*elem), GFP_KERNEL);
    if(elem==NULL){
        pr_alert("Kmalloc error: cannot allocate memory for the topic_node\n");
        return -ENOMEM;
    }
    elem->dir_name = dir;
    
    /* Create four device files in a topic */
    for(i = 0; i < NUM_SPECIAL_FILES; i++){
        char *path = (char*)kmalloc(path_len + strlen(files[i]), GFP_KERNEL);
        strcpy(path, dir);
        strcat(path, files[i]);

        created_sub = register_chrdev(0, path, fops[i]);
        if(created_sub<0){
            pr_alert("Register_chrdev error: cannot create directory /dev/%s\n", path);
        }

        cls = class_create(THIS_MODULE, path);
        if(IS_ERR(cls)){
            pr_alert("Class_create error: cannot create class for /dev/%s\n", path);
        }

        if(i==0 || i==2){//subscribe + nr_signal files
            cls->devnode = cls_set_writeOnly_permission;
        }else if(i==1){ //subscribers_list files
            cls->devnode = cls_set_readOnly_permission;
        }else{
            cls->devnode = cls_set_readAndWrite_permission; 
        }
        elem->devices[i] = MKDEV(created_sub, 0);
        device_create(cls, NULL, elem->devices[i], NULL, path); 
        elem->file_dev_cls[i] = *cls;
    }

    /* initialize list of pids */
    INIT_LIST_HEAD(&(elem->pidListHead));
    list_add(&(elem->list), &topicListHead);

    msg[0] = '\0';

    pr_info("Device files created on /dev/%s\n", dir);
    display_list();
    
    return bytes_written; 
} 

/*
* Called whenever a process attempts to open the subscribe device file.
*/
static int subscribe_open(struct inode *inode, struct file *file){
    //spin_lock(&(file->spinlock)); //best not using spinlocks, semaphore are better coz it's okay if the execution is preempted
    try_module_get(THIS_MODULE);

    return SUCCESS;
}

/* 
* Called when a process closes the subscribe device file.
*/
static int subscribe_release(struct inode *inode, struct file *file){
    //spin_unlock(&(file->spinlock));//not use spinlocks, use semaphore
    module_put(THIS_MODULE);
    return SUCCESS;
}  

/* 
* Called when a process writes to the subscribe device file
* echo "test" > /dev/psipc/subscribe
*/
static ssize_t subscribe_write(struct file *filp, const char __user *buff, size_t len, loff_t *off){
    int i, bytes_written;
    char *dentry;
    struct topic_node *node;
    struct pid_node* pid_node;
 
    for (i = 0; i < len && i < BUF_LEN; i++) 
        get_user(msg[i], buff + i);

    bytes_written = i;
    msg[i-1] = '\0';

    pid_node = (struct pid_node*)kmalloc(sizeof(struct pid_node), GFP_KERNEL);
    if(pid_node==NULL){
        pr_alert("Kmalloc error: cannot allocate memory for the pid_node\n");
        return -ENOMEM;
    }
    
    pid_node->pid = pro_atoi(msg);
    if(pid_node->pid < 0){
        pr_alert("ERROR: %s is not a pid\n", msg);
        return bytes_written;
    }

    dentry = filp->f_path.dentry->d_parent->d_iname;
    node = search_node(dentry);
    if(node==NULL){
        pr_alert("Search_node error: cannot find the node\n");
        return bytes_written;
    }
    list_add(&(pid_node->list), &(node->pidListHead));
    pr_info("Added pid node to list\n");

    display_pid_list(&(node->pidListHead));

    msg[0] = '\0';
    
    return bytes_written;
}

/*
* Called whenever a process attempts to open the subscribers_list device file.
*/
static int subs_list_open(struct inode *inode, struct file *file){
    if (atomic_cmpxchg(&subscribers_list_already_open, CDEV_NOT_USED, CDEV_EXCLUSIVE_OPEN)) 
        return -EBUSY; 
    try_module_get(THIS_MODULE);
    return SUCCESS;
}

/* 
* Called when a process closes the subscribers_list device file.
*/
static int subs_list_release(struct inode *inode, struct file *file){
    atomic_set(&subscribers_list_already_open, CDEV_NOT_USED);
    module_put(THIS_MODULE);
    return SUCCESS;
}

/*
* Called when a process, which already opened the subscribers_list device file, attempts to read from it.
* It prints the list of pid of subscribed processes.
* cat /dev/psipc/subscribers_list
*/
static ssize_t subs_list_read(struct file *filp, char __user *buffer, size_t length, loff_t *offset){
    int bytes_read = 0; 
    struct pid_node *ptr;
    struct topic_node *node;
    char *dentry;

    if (flag) { //we are at the end of message 
        pr_info("Exit.\n");
        flag = 0; //reset the flag
        *offset = 0; //reset the offset 
        return 0; // signify end of file
    } 

    dentry = filp->f_path.dentry->d_parent->d_iname;
    node = search_node(dentry);

    list_for_each_entry(ptr, &(node->pidListHead), list){
        int len, i = 0;
        char *str;
        str = (char*)kmalloc(MAX_SIZE_PID, GFP_KERNEL);
        snprintf(str, MAX_SIZE_PID, "%d", ptr->pid);
        const char *msg_ptr = str; 
    
        len = strlen(str);

        /* Actually put the data into the buffer */ 
        while (len > 0) { 
            put_user(msg_ptr[i++], buffer++);
            len--; 
            bytes_read++;
        } 
        put_user(' ', buffer++);
        bytes_read++;
    }

    put_user('\n', buffer++);
    bytes_read++;

    *offset += bytes_read; 
    flag = 1;
    
    return bytes_read;
}

/*
* Called whenever a process attempts to open the signal_nr device file.
*/
static int signal_nr_open(struct inode *inode, struct file *file){return -1;}

/* 
* Called when a process closes the signal_nr device file.
*/
static int signal_nr_release(struct inode *inode, struct file *file){return -1;}

/* 
* Called when a process writes to the signal_nr device file
* echo "test" > /dev/psipc/signal_nr
*/
static ssize_t signal_nr_write(struct file *filp, const char __user *buff, size_t len, loff_t *off){return 0;}


/*
* Called whenever a process attempts to open the endpoint device file.
*/
static int endpoint_open(struct inode *inode, struct file *file){return -1;}

/* 
* Called when a process closes the endpoint device file.
*/
static int endpoint_release(struct inode *inode, struct file *file){return -1;}

/* 
* Called when a process writes to the endpoint device file
* echo "test" > /dev/psipc/endpoint
*/
static ssize_t endpoint_write(struct file *filp, const char __user *buff, size_t len, loff_t *off){return 0;}

/*
* It releases all the device files for every topic.
*/
static void release_files(void){
    int n_topics, n_files;
    struct list_head *ptr1, *ptr2, *temp1, *temp2;
    struct topic_node *entry, *entry_temp;

    if(!list_empty(&topicListHead)){
        entry = list_first_entry_or_null(&topicListHead, struct topic_node, list);

        if(entry == NULL){
            pr_alert("No topic to delete!\n");
            return;
        }

        list_for_each_safe(ptr1, temp1, &topicListHead){
            entry_temp = list_entry(ptr1, struct topic_node, list);
            if(entry_temp!=NULL){
                if(!list_empty(&(entry_temp->pidListHead))){
                    list_for_each_safe(ptr2, temp2, &(entry_temp->pidListHead)){
                        list_del(ptr2);
                    }
                }else{
                    pr_alert("No pid list to free\n");
                }
                pr_info("All pid freed\n");

                for(n_files=0; n_files < NUM_SPECIAL_FILES; n_files++){
                    char *str = (char*)kmalloc(strlen(entry_temp->dir_name) + strlen(files[n_files]), GFP_KERNEL);
                    strcpy(str, entry_temp->dir_name);
                    strcat(str, files[n_files]);
                    device_destroy(&(entry_temp->file_dev_cls[n_files]), entry_temp->devices[n_files]);
                    class_destroy(&(entry_temp->file_dev_cls[n_files]));
                    pr_info("DESTROY: %s/%s\n", str, files[n_files]);
                    unregister_chrdev(major, str);
                    kfree(str);
                }
                if(ptr1!=NULL){
                    list_del(ptr1);
                }else
                    pr_alert("ERROR_RELEASE: no pointer to free\n");
            }else
                pr_alert("ALERT:null entry\n");
        }
    }
}

/*
* It displays the list of topics.
*/
static void display_list(void){
    int i=0;
    struct list_head *ptr;
    struct topic_node *entry, *entry_ptr;

    entry = list_first_entry_or_null(&topicListHead, struct topic_node, list);
    if(entry==NULL){
        pr_alert("ERROR: list is empty\n");
        return;
    }else{
    pr_info("-------BEGIN_TOPIC_LIST--------\n");
    list_for_each_entry(entry_ptr, &topicListHead, list){
        pr_info("Topic[%d]\n\tDir: %s\n", i++, entry_ptr->dir_name);
    }
    pr_info("------------END_TOPIC_LIST-------\n\n");
    }
}

/*
* It displays the list of pid of subscribed processes to a topic.
*/
static void display_pid_list(struct list_head *head){
    struct pid_node *ptr;
    int i=0;

    pr_info("-------BEGIN_PID_LIST------\n");
    list_for_each_entry(ptr, head, list){
        pr_info("Pid[%d]: %d\n", i, ptr->pid);
    }
    pr_info("---------END_PID_LIST------\n\n");
}

/*
* It searches in the list the correct topic_node given the topic path.
*/
static struct topic_node* search_node(char *dir){
    char *path;
    struct topic_node *ptr;

    path = (char*)kmalloc(strlen(TOPICS_PATH) + strlen(dir), GFP_KERNEL);
    if(path==NULL){
        pr_alert("Kmalloc error: cannot allocate memory for the path\n");
        return NULL;;
    }

    strcpy(path, TOPICS_PATH);
    strcat(path, dir);

    list_for_each_entry(ptr, &topicListHead, list){
        if(strcmp(ptr->dir_name, path)==0){
            pr_info("Node found!\n");
            return ptr;
        }
    }
    return NULL;
}

/*
* Custom atoi function that converts a string to int.
*/
static int pro_atoi(char *s){
    int n=0, i;

    for(i=0; s[i]!='\0'; i++){
        if(s[i]<'0'  || s[i]>'9'){
            pr_alert("%s is not a pid\n", s);
            return -1;
        }
        n = n*10 + (s[i] - '0');
    }
    return n;
}
 
module_init(chardev_init); 
module_exit(chardev_exit); 
 
MODULE_LICENSE("GPL");
