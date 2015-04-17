#include <linux/migrate.h>
#include <linux/memory_hotplug.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <asm/pgtable.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <asm-generic/pgtable.h>

//[DCSLAB]
#include <linux/lockdep.h>
#include <asm/tlbflush.h>
#include <linux/time.h>
#include <linux/mmu_notifier.h>
#include <linux/preempt.h>
#include "../kernel/sched/sched.h"
#include "../mm/internal.h"
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/migrate_mode.h>
#include <linux/hugetlb.h>
#include <linux/random.h>


#define DEBUG_FILE 1
#ifdef DEBUG_FILE
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/syscalls.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>
#endif

//#define DEBUG_TIME
#define DEBUG 1
#define WORKING_SET_TOO_LARGE 1
#define LOCALITY_JUMPING 1

//#define NR_CORES_PER_NODE 2
#define NR_CORES_PER_NODE 4 
#define NR_NODES_IN_SYSTEM 1
#define NR_COLORS_IN_CACHE NUM_OF_COLORS
//#define DEBUG_DURATION 128
#define SCAN_TIME_WINDOW_US 500
#define SCAN_FREQUENCY_MS 200
//#define SCAN_FREQUENCY_MS 200
#define MIGRATION_DELAY_UNIT 1000
//#define MIGRATION_DELAY_UNIT 500
//#define MIGRATION_DELAY_UNIT 200
#define WORKER_TIMEOUT 5
#define MANAGER_TIMEOUT 30

#define MIGRATION_INTERVAL 16
#define MIGRATION_THRESHOLD 40
#define JUST_BEFORE_DECISION (MIGRATION_INTERVAL - 1)

#define NR_PAGES_PER_COLOR_IN_MEMORY 32768
#define NR_WAYS_IN_CACHE 24
//#define NR_MIGRATE_PAGES_AT_ONCE (256)
#define NR_MIGRATE_PAGES_AT_ONCE (65536)
#define NO_COLOR (-1)
#define PRCP_NO_LIMIT (-1)

#define JAIL_NUM 0xF 
#define NOT_JAIL_NUM 0xFFFFFFFFFFFFFFF0 
#define JAIL_COLOR 16

#define WINDOW_NUM 3
enum WORKER_STATE
{
	WORKER_WAIT,
	WORKER_START,
	WORKER_EXIT,
	WORKER_TERMINATED
};

enum PRCP_FLAG
{
	FLAG_DEFAULT,
	FLAG_NOT_MOVABLE,
	FLAG_NOT_FIT_IN_CACHE_CHECK,
	FLAG_NOT_FIT_IN_CACHE,
	FLAG_NORMAL,
	FLAG_POLLUTE
};

enum PRCP_STAGE
{
	STAGE_DEFAULT,
	STAGE_PREPARE_DECISION,
	STAGE_PAGE_MIGRATION
};

extern unsigned long (*_generate_color_bitmap)(void);
extern struct task_struct *get_rq_task(int cpu);
extern struct list_head *get_rq_tasks(int cpu);
extern struct task_struct *get_rq_next_task(int cpu);
extern struct task_struct *get_rq_last_task(int cpu);
extern struct cfs_rq *get_cfs_rq(int cpu);
extern struct page *prcp_alloc_migrate_target(struct page *page, unsigned long prefered_color, int **resultp);
extern int prcp_do_migrate_range(struct task_struct *tsk, struct vm_area_struct *vma, unsigned long start_pfn, unsigned long end_pfn);
extern int prcp_try_isolate_lru_page(struct vm_area_struct *vma, unsigned long pfn);
extern void free_global_color_list(unsigned int cpu);
extern void drain_pages_color(unsigned int cpu);
extern unsigned int NR_POLLUTE_COLORS;
extern unsigned int NR_NORMAL_COLORS; // == POLLUTE_COLOR
struct task_struct *prcp_manager;

static char *output;
unsigned int prcp_stage = STAGE_DEFAULT;
unsigned int DELAY_WEIGHT = 1;

int HIGH_THRESHOLD = MIGRATION_INTERVAL / 2;
int LOW_THRESHOLD = 2;
int TOO_LARGE_THRESHOLD = MIGRATION_INTERVAL / 2;
int local_highest_access_cnt[NR_CORES_PER_NODE];
int local_migration_flag[NR_CORES_PER_NODE];
int local_sum_access_cnt[NR_CORES_PER_NODE];
unsigned long local_spatial_locality_cnt[NR_CORES_PER_NODE];
int group_predict_sum[NR_CORES_PER_NODE][WINDOW_NUM];
int group_predict_comb[NR_CORES_PER_NODE][WINDOW_NUM][WINDOW_NUM];
double local_spatial_prop[NR_CORES_PER_NODE];
int local_nr_pages[NR_CORES_PER_NODE];
int local_nr_normal_pages[NR_CORES_PER_NODE];
int local_nr_pollute_pages[NR_CORES_PER_NODE];
int local_nr_migrated_pages[NR_CORES_PER_NODE];
unsigned long local_scanning_cnt[NR_CORES_PER_NODE];
int migration_flag;
int xalancbmk_live;
int libquantum_live;
int bzip_live;

static inline int get_access_cnt(struct page *page)
{
	return atomic_read(&page->access_cnt);
}

static inline int get_difference_between_access_cnts(int access_cnt1, int access_cnt2)
{
	int difference = (access_cnt1 > access_cnt2)? (access_cnt1 - access_cnt2) : (access_cnt2 - access_cnt1);
	return difference;
}

static inline int get_prcp_flags(struct page *page)
{
	return page->prcp_flags;
}

static inline void init_access_cnt(struct page *page)
{
	atomic_set(&page->access_cnt, 0);
}

static inline void increase_access_cnt(struct page *page)
{
	atomic_inc(&page->access_cnt);
}

static inline void init_prcp_flags(struct page *page)
{
	page->prcp_flags = 0;
}

static inline void set_prcp_flags(struct page *page, int flags)
{
	page->prcp_flags = flags;
}

static inline int get_total_nr_migrated_pages(void)
{
	int i;
	int total = 0;
	for (i=0; i<NR_CORES_PER_NODE; i++) {
		total += local_nr_migrated_pages[i];
	}
	return total;
}

static inline void update_nr_colors(unsigned int new_nr_pollute_colors)
{
	NR_POLLUTE_COLORS = new_nr_pollute_colors;
	NR_NORMAL_COLORS = NR_COLORS_IN_CACHE - new_nr_pollute_colors;
}

static inline void grow_pollute_buffer(void)
{
	if (NR_POLLUTE_COLORS < NR_COLORS_IN_CACHE - 1)
		update_nr_colors(NR_POLLUTE_COLORS + 1);
}

static inline void shrink_pollute_buffer(void)
{
	if (NR_POLLUTE_COLORS > 1)
		update_nr_colors(NR_POLLUTE_COLORS - 1);
}

unsigned long my_generate_color_bitmap(void)
{
        char pname[TASK_COMM_LEN];  /* [DCSLAB] for saving process's name */ 
        struct task_struct *tsk = current;
        struct mm_struct *mm = tsk->mm;

        if(mm) {
            get_task_comm(pname,tsk);       /* copy process's name into pname */ 	
	if(strncmp("xalancbmk", tsk->comm, sizeof("xalancbmk") -1) == 0){
		if(xalancbmk_live == 0){
			printk("xalancbmk live == 0\n");
			xalancbmk_live = 1;
			return 0UL;
		}else{
			mm->is_tiger = 1;
			printk("xalancbmk live == 1\n");
			return NOT_JAIL_NUM;
		}			
	}
	if(strncmp("libquantum", tsk->comm, sizeof("libquantum") -1) == 0){
		if(libquantum_live == 0){
			printk("libquantum liive == 0\n");
			libquantum_live = 1;
			return 0UL;
		}else{
			printk("libquantum live == 1\n");
			mm->is_tiger = 1;
			//return NOT_JAIL_NUM;
			return JAIL_NUM;
		}			
	}
	if(strncmp("bzip", tsk->comm, sizeof("bzip") -1) == 0){
		if(bzip_live == 0){
			printk("bzip live == 0\n");
			bzip_live = 1;
			return 0UL;
		}else{
			printk("bzip live == 1\n");
			mm->is_tiger = -1;
			return NOT_JAIL_NUM;
		}			
	}
	}
	return 0UL;
}
static inline void adjust_pollute_buffer(void)
{
	int core;
	int total_nr_pollute_pages = 0;
	int total_nr_normal_pages = 0;
	for (core = 0; core < NR_CORES_PER_NODE; core++) {
		total_nr_pollute_pages += local_nr_pollute_pages[core];
		total_nr_normal_pages += local_nr_normal_pages[core];
	}
	if (total_nr_pollute_pages > ((NR_POLLUTE_COLORS * NR_PAGES_PER_COLOR_IN_MEMORY) / 2)) {
#if DEBUG > 0
		printk("[adjust_pollute_buffer] grow_pollute_buffer: %d -> ", NR_POLLUTE_COLORS);
#endif	
		grow_pollute_buffer();
#if DEBUG > 0
		printk("%d\n", NR_POLLUTE_COLORS);
#endif	
	} else if (total_nr_normal_pages > ((NR_NORMAL_COLORS * NR_PAGES_PER_COLOR_IN_MEMORY) / 2)) {
#if DEBUG > 0
		printk("[adjust_pollute_buffer] shrink_pollute_buffer: %d -> ", NR_POLLUTE_COLORS);
#endif	
		shrink_pollute_buffer();
#if DEBUG > 0
		printk("%d\n", NR_POLLUTE_COLORS);
#endif	
	}
}

static inline void update_threshold(void)
{
	int core;
	int total_highest = 0;
	int total_sum = 0;
	int total_nr_pages = 0;
	int total_average = 0;
	int temp;
//	int weight;
	for (core = 0; core < NR_CORES_PER_NODE; core++) {
		if (total_highest < local_highest_access_cnt[core]) {
			total_highest = local_highest_access_cnt[core];
		}
		total_sum += local_sum_access_cnt[core];
		total_nr_pages += local_nr_pages[core];
	}
	if (total_nr_pages != 0) {
		total_average = total_sum / total_nr_pages;
		temp = (total_highest + total_highest + total_average) / 3;
		//HIGH_THRESHOLD = (temp < 10)? 10 : temp;
		HIGH_THRESHOLD = temp;
		LOW_THRESHOLD = (total_average > 4)? (total_average / 2) : 2;
		TOO_LARGE_THRESHOLD = total_average;
		//LOW_THRESHOLD = (total_average > 1)? total_average : 2;
#if 0
		weight = DELAY_WEIGHT / 2;	
		HIGH_THRESHOLD += weight;
		LOW_THRESHOLD += weight;
#endif
		if (HIGH_THRESHOLD <= LOW_THRESHOLD)
			HIGH_THRESHOLD = LOW_THRESHOLD;
	}
#if DEBUG > 0
	printk("[update_threshold] HIGH_THRESHOLD: %d, LOW_THRESHOLD: %d, TOO_LARGE_THRESHOLD: %d\n", HIGH_THRESHOLD, LOW_THRESHOLD, TOO_LARGE_THRESHOLD);
#endif
}

#ifdef DEBUG_FILE
struct file* fp;
loff_t offset = 0;
char buf[1024];
#endif

module_param(output, charp, 0);

#ifdef DEBUG_FILE
static struct file* file_open(const char *path, int flags, int rights)
{
	struct file* filp = NULL;
	mm_segment_t oldfs;
	int err = 0;

	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, flags, rights);
	set_fs(oldfs);
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		return NULL;
	}
	return filp;
}

static void file_close(struct file* file)
{
	filp_close(file, NULL);
}

static int file_write(struct file* file, unsigned char* data, unsigned int size, loff_t* off)
{
	mm_segment_t oldfs;
	int ret;
	oldfs = get_fs();
	set_fs(get_ds());

	ret = vfs_write(file, data, size, off);

	set_fs(oldfs);
	return ret;
}
#endif

struct prcp_global_data {
	atomic_t thread_count;
	wait_queue_head_t manager_wq;
};

struct prcp_private_data {
	unsigned int worker_state;
	wait_queue_head_t worker_wq;
};

struct prcp_worker_args {
	struct prcp_global_data *global;
	struct prcp_private_data *private;
	struct task_struct *target_tsk;
};

static inline void prcp_init_private_date(struct prcp_private_data *private)
{
	private->worker_state = WORKER_WAIT;
	init_waitqueue_head(&private->worker_wq);
}

static inline void prcp_init_global_data(struct prcp_global_data *global)
{
	atomic_set(&global->thread_count, NR_CORES_PER_NODE);
	init_waitqueue_head(&global->manager_wq);
}

static inline void prcp_init_worker_args(struct prcp_worker_args *data, struct prcp_global_data *global, struct prcp_private_data *private, int nr_cores_per_node)
{
	int i;
	prcp_init_global_data(global);
	for (i=0; i<nr_cores_per_node; i++) {
		prcp_init_private_date(&private[i]);
		data[i].global = global;
		data[i].private = &private[i];
		data[i].target_tsk = NULL;
	}
}

static inline int prcp_check_all_worker_terminated(struct prcp_private_data *private)
{
	int i;
	for (i=0; i<NR_CORES_PER_NODE; i++) {
		if (private[i].worker_state != WORKER_TERMINATED) {
			return 0;
		}
	}
	return 1;
}

static inline signed long long get_time(void)
{
	struct timespec ts;
	getnstimeofday(&ts);
	return timespec_to_ns(&ts);
}

static inline unsigned int prcp_get_color(unsigned long pfn)
{
	return pfn & (NR_COLORS_IN_CACHE - 1);
}

static inline pte_t* get_pte_from_vpa(struct mm_struct *target_mm, unsigned long target_address)
{
	struct mm_struct *mm = target_mm;
	unsigned long address = target_address;
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return NULL;
	pud = pud_offset(pgd, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return NULL;
	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
		return NULL;
	return pte_offset_map(pmd, address);
}

static inline pte_t* get_pte_from_vpa_lock(struct mm_struct *target_mm, unsigned long target_address, spinlock_t **ptl)
{
	struct mm_struct *mm = target_mm;
	unsigned long address = target_address;
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return NULL;
	pud = pud_offset(pgd, address);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return NULL;
	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd) || unlikely(pmd_bad(*pmd)))
		return NULL;
	return pte_offset_map_lock(mm, pmd, address, ptl);
}

static inline int is_pollute_color(unsigned int color)
{
	unsigned int i;
	for (i = NR_NORMAL_COLORS; i < NR_COLORS_IN_CACHE; i++) {
		if (color == i) {
			return 1;
		}
	}
	return 0;
}

static inline void INIT_LIST_HEADS(struct list_head *list, unsigned int nr_lists)
{
	unsigned int i;
	for (i = 0; i < nr_lists; i++) {
		INIT_LIST_HEAD(&list[i]);
	}
}

static inline void init_working_set_too_large_detector(unsigned long *start_addr, unsigned long *end_addr, int *working_set_size, int *start_access_cnt)
{
	*start_addr = 0;
	*end_addr = 0;
	*working_set_size = 0;
	*start_access_cnt = 0;
}

static inline int check_vma_skippable(struct vm_area_struct *vma)
{
#if 0
		if ((vma->vm_flags & (VM_WRITE|VM_EXEC)) == (VM_EXEC)) {// skip text_area
#if DEBUG > 1
			printk("prcp_page_migration: %s skip text area\n", tsk->comm);
#endif
			return 1;
		}
		if (vma->vm_flags & VM_SHARED) {
#if DEBUG > 1
			printk("prcp_page_migration: %s skip shared area\n", tsk->comm);
#endif
			return 1;
		}
		if (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)) {// skip read-only file-backed mappings
#if DEBUG > 1
			printk("prcp_page_migration: %s skip read-only file-backed mappings\n", tsk->comm);
#endif
			return 1;
		}
#if DEBUG > 1
		printk("prcp_page_migration: %s vma selected for migration\n", tsk->comm);
#endif
#endif
		return 0;
}

static inline int prcp_detect_working_set_too_large(struct mm_struct *mm, struct task_struct *tsk, struct vm_area_struct *vma, int working_set_size, unsigned long start_addr, unsigned long end_addr, struct list_head *migration_list)
{
	unsigned long pfn;
	unsigned long address;
	unsigned int color_idx;
	int nr_migrated_pollute_pages = 0;
	struct page *page;
	pte_t *ptep;
	spinlock_t *ptl;
	if (working_set_size > NR_MIGRATE_PAGES_AT_ONCE) 
		return 0;
	if (working_set_size > NR_WAYS_IN_CACHE * NR_NORMAL_COLORS) {
#if DEBUG > 0
		printk("[%s]prcp_detect_working_set_too_large: try to isolate %d Not-Fit-In-Cache pages from %lu to %lu\n", tsk->comm, working_set_size, start_addr, end_addr);
#endif
#if DEBUG > 0
		printk("core[0]'s highest access cnt: %d, core[1]'s highest access cnt: %d\n", local_highest_access_cnt[0], local_highest_access_cnt[1]);
#endif
		for (address = start_addr; address < end_addr; address+=PAGE_SIZE) {
			//ptep = get_pte_from_vpa(mm, address);
			ptep = get_pte_from_vpa_lock(mm, address, &ptl);
			if (ptep == NULL) 
				break;
			pfn = pte_pfn(*ptep);
			page = pfn_to_page(pfn);
			if (get_prcp_flags(page) == FLAG_NOT_FIT_IN_CACHE_CHECK) {
				if (prcp_try_isolate_lru_page(vma, pfn)) {
					color_idx = NR_NORMAL_COLORS + (nr_migrated_pollute_pages % NR_POLLUTE_COLORS);
					nr_migrated_pollute_pages++;
					set_prcp_flags(page, FLAG_NOT_FIT_IN_CACHE);
					page->prefered_color = color_idx;
					list_add_tail(&page->lru, migration_list);
#if DEBUG > 0
					if (address == start_addr) {
						printk("[%s]prcp_detect_working_set_too_large: ", tsk->comm);
					}
					printk("(%lu, %lu, %d, %d) ", address, pfn, get_access_cnt(page), nr_migrated_pollute_pages);
					if (address == (end_addr - PAGE_SIZE)) {
						printk("\n");
					}
#endif
				}
			} else {
				set_prcp_flags(page, FLAG_NOT_FIT_IN_CACHE_CHECK);
			}
		}	
	}
	return nr_migrated_pollute_pages;
}

static inline unsigned int prcp_page_migration(struct task_struct *tsk, int core)
{
	unsigned long target_address;
	struct mm_struct *mm = get_task_mm(tsk);
	struct vm_area_struct *vma = mm->mmap;
    	pte_t *ptep;
	pte_t pte;
	spinlock_t *ptl;
	struct list_head migration_list;
	struct page *page;
	unsigned long pfn = 0;
	unsigned int color;

	INIT_LIST_HEAD(&migration_list);

	for (; vma; vma = vma->vm_next) {
		if (check_vma_skippable(vma)) {
			continue;
		}
		for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
			ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
			if (ptep == NULL) {
				break;
			}
			pte = *ptep;
			if (pte_present(pte)){
				pfn = pte_pfn(pte);
				page = pfn_to_page(pfn);
				color = prcp_get_color(pfn);
				
				if (prcp_try_isolate_lru_page(vma, pfn)) {
					//to be tiger
					if(mm->is_tiger == 1){
					//	printk("mm->is_tiger == 1 %s\n", tsk->comm);
						if(color >= JAIL_COLOR){
							int rand = 0;
							get_random_bytes(&rand,1);
							rand = rand % JAIL_COLOR;
							page->prefered_color = rand;
							//printk("page->prefered_color %lX\n",page->prefered_color);
			        			list_add_tail(&page->lru, &migration_list);
						}
					//to be rabbit
					}else{
				//		printk("mm->is_tiger == 0 %s\n", tsk->comm);
						if(color < JAIL_COLOR){
							int rand = 0;
							get_random_bytes(&rand,1);
							rand = rand % 48;
							page->prefered_color = color + rand;
							list_add_tail(&page->lru, &migration_list);
						}
					}	
				}
			}
			pte_unmap_unlock(ptep, ptl);
		}
	}
	if (!list_empty(&migration_list)) {
		prcp_migrate_pages(tsk, &migration_list, prcp_alloc_migrate_target, 0, MIGRATE_SYNC, MR_NUMA_MISPLACED);
		flush_tlb_mm(mm);
	}
	//local_nr_migrated_pages[core] = nr_migrated_cold_pages;
#if DEBUG > 0
	//printk("[prcp_page_migration] For [%s] nr_normal_too_large_pages: %d, nr_normal_low_pages: %d, nr_pollute_high_pages: %d,  nr_skipped_pages: %d, nr_migrated_normal_pages: %d, nr_migrated_pollute_pages: %d, nr_migrated_cold_pages: %d\n", tsk->comm, nr_migrated_pollute_pages, nr_normal_low_pages, nr_pollute_high_pages, nr_skipped_pages, nr_migrated_normal_pages, nr_migrated_pollute_pages, nr_migrated_cold_pages);
#endif
	mmput(mm);
	return 0;
}
static inline unsigned long get_jumping_variance(unsigned long jumping_weight)
{
	unsigned long var = 0;
	unsigned long my_weight = (jumping_weight > 32)? 32 : jumping_weight;
	get_random_bytes(&var, sizeof(unsigned long));
	var = (var % ((my_weight/4)+1));
	return var;
}

static inline int pte_accessed(pte_t pte)
{
	if (pte_present(pte))
		if (pte_young(pte))
			return 1;
	return 0;
}

static inline int prcp_prepare_scan(struct mm_struct *mm)
{
	unsigned long target_address;
	struct vm_area_struct *vma = mm->mmap; 
	//int my_core = smp_processor_id();
	//struct page *page;
    	pte_t *ptep;
	pte_t pte;
	spinlock_t *ptl;
#if 0 
	if(local_scanning_cnt[my_core] > 1000){
	for (; vma; vma = vma->vm_next) {
		if (check_vma_skippable(vma))
			continue;
			for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
				ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
				if (ptep == NULL) {
					break;
				}
				pte = *ptep;
				if (pte_present(pte)){
					page = pte_page(pte);
					init_access_cnt(page);
					if (pte_young(pte)) {
						pte = pte_mkold(pte);
						set_pte(ptep, pte);
					}
				}
				pte_unmap_unlock(ptep, ptl);
			}
		}
		local_scanning_cnt[my_core] = 0;
	}else{
#endif
		for (; vma; vma = vma->vm_next) {
			if (check_vma_skippable(vma))
				continue;
			for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
				ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
				if (ptep == NULL) {
					break;
				}
				pte = *ptep;
				if (pte_present(pte)){
					if (pte_young(pte)) {
						pte = pte_mkold(pte);
						set_pte(ptep, pte);
					}
				}
				pte_unmap_unlock(ptep, ptl);
			}
		}
//	}
    return 0;
}

int catch_tiger_process(int window_cnt)
{
	int total_group_comb_sum;
	int my_core = smp_processor_id();
	int total_group_sum;
	if(window_cnt > 2){
		int temp = window_cnt % 3;
		if(temp == 0)
		{
			total_group_comb_sum = group_predict_comb[my_core][1][0]+
						group_predict_comb[my_core][2][1]+
						group_predict_comb[my_core][0][2];
			//printk("1/ comb 1 %d comb 2 %d com3 %d sum 1 %d sum 2 %d sum3 %d\n",group_predict_comb[my_core][1][0],group_predict_comb[my_core][2][1],group_predict_comb[my_core][0][2],group_predict_sum[my_core][0],group_predict_sum[my_core][1],group_predict_sum[my_core][2]);
			group_predict_comb[my_core][1][0] = 0;
			group_predict_comb[my_core][1][1] = 0;
			group_predict_comb[my_core][1][2] = 0;
			total_group_sum = group_predict_sum[my_core][0] + group_predict_sum[my_core][1] + group_predict_sum[my_core][2];
			group_predict_sum[my_core][1] = 0;
		}else if(temp == 1)
		{
                        total_group_comb_sum = group_predict_comb[my_core][2][0]+
                                                group_predict_comb[my_core][0][1]+
                                                group_predict_comb[my_core][1][2];
			//printk("1/ comb 1 %d comb 2 %d com3 %d sum 1 %d sum 2 %d sum3 %d\n",group_predict_comb[my_core][2][0],group_predict_comb[my_core][0][1],group_predict_comb[my_core][1][2],group_predict_sum[my_core][0],group_predict_sum[my_core][1],group_predict_sum[my_core][2]);
			group_predict_comb[my_core][2][0] = 0;
			group_predict_comb[my_core][2][1] = 0;
			group_predict_comb[my_core][2][2] = 0;
			total_group_sum = group_predict_sum[my_core][0] + group_predict_sum[my_core][1] + group_predict_sum[my_core][2];
			group_predict_sum[my_core][2] = 0;

		}else
		{
	                total_group_comb_sum = group_predict_comb[my_core][0][0]+
                                                group_predict_comb[my_core][1][1]+
                                                group_predict_comb[my_core][2][2];
			//printk("1/ comb 1 %d comb 2 %d com3 %d sum 1 %d sum 2 %d sum3 %d\n",group_predict_comb[my_core][0][0],group_predict_comb[my_core][1][1],group_predict_comb[my_core][2][2],group_predict_sum[my_core][0],group_predict_sum[my_core][1],group_predict_sum[my_core][2]);
			group_predict_comb[my_core][0][0] = 0;
			group_predict_comb[my_core][0][1] = 0;
			group_predict_comb[my_core][0][2] = 0;
			total_group_sum = group_predict_sum[my_core][0] + group_predict_sum[my_core][1] + group_predict_sum[my_core][2];
			group_predict_sum[my_core][0] = 0;

		}
	
		//int s1 = 281338;
		int s1 = 5962613;
		
	
		//printk("total group sum %d total group_comb_sum %d group_predict_sum0 %d group_predict_sum1 %d group_predict_sum2 %d\n",total_group_sum,total_group_comb_sum,group_predict_sum[my_core][0],group_predict_sum[my_core][1],group_predict_sum[my_core][2]);
		int s2 = total_group_comb_sum - ((11325 * total_group_sum)/150);

		//printk("s1 %d, s2 comb %d sum %d, \n",s1, total_group_comb_sum, ((11325 * total_group_sum)/150));
		//int b1 = s2/s1;

		int temp_b1 = (s2 * 10000)/s1;
		int b0 = (total_group_sum / 150) - ((temp_b1 * 75)/10000);

		int temp_predict_next_value = (175 * temp_b1)/10000;
		int predict_next_value = b0 + temp_predict_next_value;

	
		//printk("total_group_sum_avg %d, next %d\n",(total_group_sum / 150), ((temp_b1 *75)/100));
		//printk("s1 %d s2 %d total_group_comb_sum %d total_group_sum %d ((11325 * total_group_sum)/150) %d\n",s1,s2,total_group_comb_sum, total_group_sum,((11325 * total_group_sum)/150));
		//printk("[%s]predict_next_value %d avg %d tilt %d\n",current->comm,predict_next_value,(total_group_sum / 150),temp_b1);
		return predict_next_value;	
	}
	if(window_cnt == 2)
	{
		group_predict_comb[my_core][0][0] = 0;
		group_predict_comb[my_core][0][1] = 0;
		group_predict_comb[my_core][0][2] = 0;
		group_predict_sum[my_core][0] = 0;
		return 1000;

	}
		//printk("window_cnt %d\n",window_cnt);
	return 1000;
}
int process_migration(struct task_struct *tsk,int my_core)
{
	struct mm_struct *mm;
	mm = tsk->mm;
#if 0
	if((end_time - start_time) > 100000000000 && (mm->color_bitmap == 0)){
	        //printk("prcp_page_migration [%s]\n",tsk->comm);
                if(strncmp("libquantum", tsk->comm, sizeof("libquantum") -1) == 0){
        	        mm->is_tiger = 1;
                        mm->color_bitmap = JAIL_NUM;
                        prcp_page_migration(tsk, my_core);
                        libquantum_check = 1;
               	}
                if(strncmp("sphinx", tsk->comm, sizeof("sphinx") -1) == 0){
                	mm->color_bitmap = NOT_JAIL_NUM;
                       	prcp_page_migration(tsk, my_core);
                        sphinx_check = 1;
                }
                                
                if((libquantum_check == 1) && (sphinx_check == 1))
                	start_time = get_time();
	}
#endif
	//to be tiger, will go to jail
	if(mm->is_tiger == 1)
	{
		mm->color_bitmap = JAIL_NUM;
		prcp_page_migration(tsk, my_core);
		printk("is_tiger = 1 migration %s\n",tsk->comm); 
	}
	
	//to be rabbit, will be free
	if(mm->is_tiger == -1)
	{
		mm->color_bitmap = NOT_JAIL_NUM;
		prcp_page_migration(tsk, my_core);
		printk("is_tiger = -1 migration %s\n",tsk->comm); 
	}

	return 0;
	//mm->color_bitmap = JAIL_NUM;
	//prcp_page_migration(tsk, my_core);
}
int prcp_page_table_scan(void *args)
{
	struct prcp_worker_args *data = args;
	unsigned long target_address;
	unsigned long old_target_address;
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct page *page;
	pte_t *ptep;
	pte_t pte;
	spinlock_t *ptl;
	int my_core = smp_processor_id();
	int cur_access_cnt;
	int my_old_access_cnt;
	int spatial_locality_cnt;
	int window_cnt;
	int sliding_cnt;
	int my_highest_access_cnt;
	int my_sum_access_cnt;
	int my_nr_pages;
	int my_nr_normal_pages;
	int my_nr_pollute_pages;

	int libquantum_check;
	int sphinx_check;
#if DEBUG > 0
	int average;
#endif
	signed long long start_time, end_time;

	start_time = get_time();
	window_cnt = 0;
	sliding_cnt = 0;
	migration_flag = 0;
	while(!kthread_should_stop()) {

		//end_time = get_time();
		wait_event_interruptible(data->private->worker_wq, (data->private->worker_state != WORKER_WAIT)); // wait here until all other threads are ready
		if (data->private->worker_state == WORKER_EXIT)
			goto worker_exit;
		else
			data->private->worker_state = WORKER_WAIT;

		tsk = data->target_tsk;
		if (tsk)
			get_task_struct(tsk);
		else
			goto no_mm_struct;

		mm = get_task_mm(tsk);
		if (!mm) {// if kernel thread
			goto no_mm_struct;
		}

		if(mm->is_tiger == 0)
		{
			//initialization
			window_cnt = 0;
			//mm->is_tiger 0: process has started now.
			//mm->is_tiger 10: process don't need to reset.
			mm->is_tiger = 10;
			printk("[%s] colormap %lx\n", mm->color_bitmap);
			group_predict_sum[my_core][0] = 0;
			group_predict_sum[my_core][1] = 0;
			group_predict_sum[my_core][2] = 0;

			group_predict_comb[my_core][0][0] = 0;
			group_predict_comb[my_core][0][1] = 0;
			group_predict_comb[my_core][0][2] = 0;

			group_predict_comb[my_core][1][0] = 0;
			group_predict_comb[my_core][1][1] = 0;
			group_predict_comb[my_core][1][2] = 0;

			group_predict_comb[my_core][2][0] = 0;
			group_predict_comb[my_core][2][1] = 0;
			group_predict_comb[my_core][2][2] = 0;

		}
#ifdef DEBUG_TIME
		start_time = get_time();
#endif

		//printk("mm's color_map %lX [%s]\n", mm->color_bitmap, tsk->comm);
		//if (unlikely(prcp_stage == STAGE_PAGE_MIGRATION))
//migration
#if 0
		if((end_time - start_time) > 100000000000 && (mm->color_bitmap == 0)){
			//printk("prcp_page_migration [%s]\n",tsk->comm);
			if(strncmp("libquantum", tsk->comm, sizeof("libquantum") -1) == 0){
				mm->is_tiger = 1;
				mm->color_bitmap = JAIL_NUM;
				prcp_page_migration(tsk, my_core);
				libquantum_check = 1;
			}
			if(strncmp("sphinx", tsk->comm, sizeof("sphinx") -1) == 0){
				mm->color_bitmap = NOT_JAIL_NUM;
				prcp_page_migration(tsk, my_core);
				sphinx_check = 1;
			}
				
			if((libquantum_check == 1) && (sphinx_check == 1))
				start_time = get_time();
		}
#endif
		local_scanning_cnt[my_core]++;
		prcp_prepare_scan(mm);

#ifdef DEBUG_TIME
		end_time = get_time();
		//printk("[%s] prcp_prepare_scan()'s running time: %lld\n", tsk->comm, end_time - start_time);
#endif
		usleep_range(SCAN_TIME_WINDOW_US, SCAN_TIME_WINDOW_US);

		

#ifdef DEBUG_TIME
		start_time = get_time();
#endif
		vma = mm->mmap;

//		if (unlikely(prcp_stage == STAGE_PREPARE_DECISION)) {
			//unsigned int color;
			unsigned long pfn;
			my_highest_access_cnt = 0;
			my_sum_access_cnt = 0;
			my_nr_pages = 0;
			my_nr_normal_pages = 0;
			my_nr_pollute_pages = 0;
			my_old_access_cnt = 0;
			spatial_locality_cnt = 0;
			//window_cnt = 0;
			//printk("8 scan starts\n");
			for (; vma; vma = vma->vm_next) {
				//my_old_access_cnt = 0;
				if (check_vma_skippable(vma))
					continue;
				for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
					ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
					if (ptep == NULL) {
						break;
					}
					pte = *ptep;
					if (pte_present(pte)) {
						pfn = pte_pfn(pte);
						page = pfn_to_page(pfn);
						if (pte_young(pte)) {
							increase_access_cnt(page);
							cur_access_cnt = get_access_cnt(page);
							//printk("%d;",cur_access_cnt);
							//sprintf(buf, "%d;", cur_access_cnt);
							//file_write(fp, buf, strlen(buf), &offset);
							//sprintf(buf, "%lx;",target_address);
							//file_write(fp, buf, strlen(buf), &offset);

							//my_sum_access_cnt += cur_access_cnt;
							//my_nr_pages++;
							my_nr_pages++;
							
							if(target_address == old_target_address + PAGE_SIZE){
								if(my_old_access_cnt == cur_access_cnt){
									spatial_locality_cnt++;
								}
							}
							

							old_target_address = target_address;
							my_old_access_cnt = cur_access_cnt;
							

						//cur_access_cnt = get_access_cnt(page);
						//if(cur_access_cnt != 0)
						//	printk("[%d]",cur_access_cnt);
						//color = prcp_get_color(pfn);
						//if (is_pollute_color(color)) {
						//	my_nr_pollute_pages++;
						//} else {
						//	my_nr_normal_pages++;
						//}

							//if(cur_access_cnt != 0){
#if 0
								if(my_old_access_cnt != cur_access_cnt){
									spatial_miss_cnt++;
									my_old_access_cnt = cur_access_cnt;
								}
#endif
							//}
						}
					}
					pte_unmap_unlock(ptep, ptl);
				}
			}
			//printk("\n");
			//sprintf(buf, "\n");
			//file_write(fp, buf, strlen(buf), &offset);
			//file_write(fp, buf, strlen(buf), &offset);
			//local_highest_access_cnt[my_core] = my_highest_access_cnt;
			//local_sum_access_cnt[my_core] = my_sum_access_cnt;
			local_nr_pages[my_core] = my_nr_pages;
			local_spatial_locality_cnt[my_core] = spatial_locality_cnt;

			sliding_cnt++;
			int predict_tiger;
			int spatial_locality;

			if(sliding_cnt < 51){
				if(my_nr_pages != 0 && spatial_locality_cnt != 0){
					spatial_locality = ((my_nr_pages - spatial_locality_cnt) * 100) / my_nr_pages;
					group_predict_sum[my_core][window_cnt % 3] += spatial_locality;
					group_predict_comb[my_core][window_cnt % 3][0] += sliding_cnt * spatial_locality;
					group_predict_comb[my_core][window_cnt % 3][1] += (sliding_cnt + 50) * spatial_locality;
					group_predict_comb[my_core][window_cnt % 3][2] += (sliding_cnt + 100) * spatial_locality;

					//printk("spatial_locality_cnt %d, my_nr_pages %d sliding_cnt %d [%d]\n",spatial_locality_cnt, my_nr_pages,sliding_cnt,(spatial_locality_cnt / my_nr_pages));
					//printk("[%s]prediction_sum[%d][%d] = %d\n",tsk->comm,my_core,window_cnt % 3, group_predict_sum[my_core][window_cnt % 3]);
					//if(strncmp("sphinx", tsk->comm, sizeof("sphinx") -1) == 0){
					//	printk("[%d]",temp);
				}
			}else{
				//-1 : to be rabbit
				// 1 : to be tiger
				//printk("\n");
				predict_tiger = catch_tiger_process(window_cnt);
				window_cnt++;
				sliding_cnt = 0;

				//printk("predit_tiger %d [%s]\n", predict_tiger, tsk->comm);
#if 0
				if(predict_tiger < 1000)
				{
					if(local_migration_flag[my_core] == 1)
					{
						if(predict_tiger < 40)
						{
							if((mm->is_tiger == 10) || (mm->is_tiger == -1))
							{
								mm->is_tiger = 1;
								process_migration(tsk, my_core);
								local_migration_flag[my_core] = 0;
							}else
							{
								local_migration_flag[my_core] = 0;
							}	
						}else
						{
							if((mm->is_tiger == 10) || (mm->is_tiger == 1))
							{
								mm->is_tiger = -1;
								process_migration(tsk, my_core);
								local_migration_flag[my_core];
							}else
							{
								local_migration_flag[my_core] = 0;
							}	
						}	
					}else
					{
						if(predict_tiger < 40)
						{
							if( (mm->is_tiger == 10) || (mm->is_tiger == -1))
							{
								mm->is_tiger = 1;
								process_migration(tsk,my_core);
								int i;
								for (i=0; i<NR_CORES_PER_NODE; i++) {
									local_migration_flag[i] = 1;
								}
								local_migration_flag[my_core] = 0;
							}
						}
					}

				}
#endif
#if 0
				if((mm->is_tiger != 1) && (predict_tiger < 40)){
					mm->is_tiger = 1;
					process_migration();
					migration_flag = 1;
				}
				if((mm->is_tiger != -1) && (migration_flag == 1))
				{
					mm->is_tiger = -1;	
				} 
#endif
			}
			sprintf(buf, "%d;", spatial_locality);
			file_write(fp, buf, strlen(buf), &offset);
			sprintf(buf, "\n", local_nr_pages[my_core]);
			file_write(fp, buf, strlen(buf), &offset);
			sprintf(buf, "%d;", predict_tiger);
			file_write(fp, buf, strlen(buf), &offset);


			//local_spatial_prop[my_core] = spatial_miss_cnt / my_nr_pages;
			//local_nr_pollute_pages[my_core] = my_nr_pollute_pages;
			//local_nr_normal_pages[my_core] = my_nr_normal_pages;
#if DEBUG > 0
			//average = (my_nr_pages > 0)? my_sum_access_cnt / my_nr_pages : -1;
			//printk("[prcp_prepare_decision] For [%s], local_highest_access_cnt: %d, local_average_access_cnt: %d, local_nr_total_pages: %d, local_nr_pollute_pages: %d, local_nr_normal_pages: %d\n", tsk->comm, my_highest_access_cnt, average, my_nr_pollute_pages+my_nr_normal_pages, my_nr_pollute_pages, my_nr_normal_pages);
			//sprintf(buf, "scanning For ");
			//sprintf(buf + strlen(buf), "[%s]", tsk->comm);
			//file_write(fp, buf, strlen(buf), &offset);
			//sprintf(buf, "%d;", local_spatial_locality_cnt[my_core]);
			//file_write(fp, buf, strlen(buf), &offset);
			//sprintf(buf, "%d;", local_nr_pages[my_core]);
			//file_write(fp, buf, strlen(buf), &offset);


			//printk("[prcp_prepare_decision 1] For [%s][%d], local_highest_access_cnt: %d, local_average_access_cnt: %d my_nr_pages: %d\n", tsk->comm,my_core, local_highest_access_cnt[my_core], average, my_nr_pages);
			//printk("[prcp_prepare_decision] For [%s][%d], local_highest_access_cnt: %d, local_average_access_cnt: %d\n", tsk->comm,my_core, local_highest_access_cnt[my_core], average);
#endif
//		}
#if 0	
		else if (unlikely(prcp_stage == STAGE_PAGE_MIGRATION)) {
			for (; vma; vma = vma->vm_next) {
				if (check_vma_skippable(vma))
					continue;
				for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
					ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
					if (ptep == NULL) {
						break;
					}
					pte = *ptep;
					if (pte_present(pte)) {
						page = pte_page(pte);
						if (get_access_cnt(page)) {
							init_access_cnt(page);
						}
					}
					pte_unmap_unlock(ptep, ptl);
				}
			}
		}
#if LOCALITY_JUMPING
		else {
			for (; vma; vma = vma->vm_next) {
				unsigned long jumping_weight = 1;
				unsigned long jumping_var = 0;
				unsigned long last_non_accessed_addr = -1;
				if (check_vma_skippable(vma))
					continue;
				for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=((jumping_weight-jumping_var)*PAGE_SIZE)) {
					ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
					if (ptep == NULL) {
						break;
					}
					pte = *ptep;
					if (pte_accessed(pte)) { // accessed
						if (jumping_weight != 1) {
							jumping_weight = 1;
							if (last_non_accessed_addr != -1) {
								target_address = last_non_accessed_addr;
								last_non_accessed_addr = -1;
							}
						}
						page = pte_page(pte);
						increase_access_cnt(page);
					} else { // not accessed
						if (jumping_weight < 64) {
							jumping_weight = jumping_weight << 1;
							last_non_accessed_addr = target_address;
						}
					}
					jumping_var = get_jumping_variance(jumping_weight);
					pte_unmap_unlock(ptep, ptl);
				}
			}
		}
#else
		else {
			for (; vma; vma = vma->vm_next) {
				if (check_vma_skippable(vma))
					continue;
				for (target_address = vma->vm_start; target_address < vma->vm_end; target_address+=PAGE_SIZE) {
					ptep = get_pte_from_vpa_lock(mm, target_address, &ptl);
					if (ptep == NULL) {
						break;
					}
					pte = *ptep;
					if (pte_accessed(pte)) { // accessed
						page = pte_page(pte);
						increase_access_cnt(page);
					}
					pte_unmap_unlock(ptep, ptl);
				}
			}
		}
#endif
#endif

		//printk("[%s] prcp_page_table_scan()'s running time: %lld\n", tsk->comm, end_time - start_time);
		mmput(mm);
		put_task_struct(tsk);

no_mm_struct:
		atomic_dec(&data->global->thread_count);
worker_exit:
		wake_up_interruptible(&data->global->manager_wq);
	}

	data->private->worker_state = WORKER_TERMINATED;
	printk("[prcp_worker_thread] the worker thread on <core %d> terminated gracefully\n", my_core);
	wake_up_interruptible(&data->global->manager_wq);
	return 0;
}

int prcp_manager_thread(void *args) 
{
	unsigned int i;
	//unsigned int cur_turn = 0;
	struct prcp_global_data global;
	struct prcp_private_data private[NR_CORES_PER_NODE];
	struct prcp_worker_args data[NR_CORES_PER_NODE];
	struct task_struct *worker[NR_CORES_PER_NODE];

#ifdef DEBUG_TIME
	signed long long start_time = 0;
	signed long long end_time = 0;
#endif
	printk("[DCSLAB]prcp_manager_thread start\n");
	prcp_init_worker_args(data, &global, &private[0], NR_CORES_PER_NODE);
	
	//create worker threads for each core
	for (i = 0; i < NR_CORES_PER_NODE; i++) {
		worker[i] = kthread_create(prcp_page_table_scan, &data[i], "prcp_worker[%d]", i);
		printk("worker[%d] \n",i);
		if (!IS_ERR(worker[i])) {
			kthread_bind(worker[i], i);
			wake_up_process(worker[i]);
		} else {
			printk("[prcp_manager_thread] creating worker thread[%d] failed\n", i);
		}
	}

	while(!kthread_should_stop()) {
		atomic_set(&global.thread_count, NR_CORES_PER_NODE);
#if 0
		if (unlikely(cur_turn == JUST_BEFORE_DECISION)) {
			prcp_stage = STAGE_PREPARE_DECISION;
		} else if (unlikely(prcp_stage == STAGE_PREPARE_DECISION)) {
			prcp_stage = STAGE_PAGE_MIGRATION;
		} else if (unlikely(prcp_stage == STAGE_PAGE_MIGRATION)) {
			prcp_stage = STAGE_DEFAULT;
			cur_turn = 0;
		}
#endif
		for (i = 0; i < NR_CORES_PER_NODE; i++) {
			if (data[i].private->worker_state == WORKER_WAIT) {
				if(i == 0)
				{
					struct list_head *rq_tasks;
					rq_tasks = get_rq_tasks(i);
					struct task_struct *next_task;
					int j;
					for(j = 0; j < 3; j++)
					{
						next_task = list_first_entry(rq_tasks, struct task_struct, se.group_node);
						list_move_tail(&next_task->se.group_node, rq_tasks);
					
						if(((next_task->state) == 0) && (next_task->comm != get_rq_task(i)->comm)){
							data[i].target_tsk = next_task;
							break;
						}else{
							data[i].target_tsk = get_rq_task(i);
						}
					}
				}else
				{
					data[i].target_tsk = get_rq_task(i);
				}

				data[i].private->worker_state = WORKER_START;
				//printk("current[%d]: %s\n",i, data[i].target_tsk->comm);
				wake_up_interruptible(&data[i].private->worker_wq);
			}
		}
		if (wait_event_interruptible_timeout(global.manager_wq, (atomic_read(&global.thread_count) == 0), MANAGER_TIMEOUT * HZ) == 0) {
			printk("[prcp_manager_thread] some of workers do not answer\n");
			goto preparation_for_exit;
		}
#if 0
		if (unlikely(prcp_stage == STAGE_PREPARE_DECISION)) {
			update_threshold();
			adjust_pollute_buffer();
		}
		cur_turn++;
#endif
#if 0
		if (unlikely(prcp_stage == STAGE_PAGE_MIGRATION)) {
			if (get_total_nr_migrated_pages() < MIGRATION_THRESHOLD) {
				DELAY_WEIGHT++;
			} else {
				DELAY_WEIGHT = 1;
			}
#if DEBUG > 0
			printk("[prcp_manager_thread] DELAY_WEIGHT = %u\n", DELAY_WEIGHT);
#endif
			msleep(MIGRATION_DELAY_UNIT * DELAY_WEIGHT);
		} else {
#endif
			msleep(SCAN_FREQUENCY_MS);
//		}
	}

preparation_for_exit:
	for (i = 0; i < NR_CORES_PER_NODE; i++) {
		if (worker[i]) {
			data[i].private->worker_state = WORKER_EXIT;
			kthread_stop(worker[i]);
			wake_up_interruptible(&data[i].private->worker_wq);
		}
	}
	printk("[prcp_manager_thread] waiting for all worker threads to stop gracefully\n");
	if (wait_event_interruptible_timeout(global.manager_wq, (prcp_check_all_worker_terminated(&private[0]) == 1), MANAGER_TIMEOUT * HZ) == 0) {

		printk("[prcp_manager_thread] failed to gracefully terminate all workers\n");
	}

	printk("[DCSLAB]prcp_manager_thread end\n");
	return 0;
}

static int scan_init(void)
{
	printk("scan_init\n");
#ifdef DEBUG_FILE
	if (output == NULL) {
		sprintf(buf, "cache_flooding_hmmer.csv");
	} else {
		sprintf(buf, output);
	}
	if (!(fp = file_open(buf, O_WRONLY|O_CREAT, 0644))) {
		printk("prcp_scan_init: fd open failed\n");
		return 0;
	} else {
		//int i;
		//sprintf(buf, "Name,Turn,");
		//for (i = 0; i < NR_COLORS_IN_CACHE; i++) {
		//	sprintf(buf + strlen(buf), "%d,",i);
		//}
		//sprintf(buf + strlen(buf), "\n");
		file_write(fp, buf, strlen(buf), &offset);
	}
#endif
#if USE_SPLIT_PTE_PTLOCKS
	printk(KERN_ALERT "[DCSLAB]USE_SPLIT_PTE_PTLOCKS\n");
#endif
#if ALLOC_SPLIT_PTLOCKS
	printk(KERN_ALERT "[DCSLAB]ALLOC_SPLIT_PTE_PTLOCKS\n");
#endif
	printk(KERN_ALERT "[DCSLAB]prcp start\n");
	//NR_POLLUTE_COLORS = (NR_COLORS_IN_CACHE >> 4);
	//NR_POLLUTE_COLORS = 1;
	//NR_NORMAL_COLORS = NR_COLORS_IN_CACHE - NR_POLLUTE_COLORS; // == POLLUTE_COLOR
	_generate_color_bitmap = my_generate_color_bitmap;
	prcp_manager = kthread_create(prcp_manager_thread, NULL, "prcp_manager");
	if (!IS_ERR(prcp_manager)) {
		//kthread_bind(prcp_manager, 1);
		kthread_bind(prcp_manager, 0);
		wake_up_process(prcp_manager);
	}
#ifdef CONFIG_FLATMEM
	printk("CONFIG_FLATMEM\n");
#endif
	return 0;
}

static void scan_exit(void)
{
#ifdef DEBUG_FILE
	if (fp) {
		file_close(fp);
	}
#endif
	_generate_color_bitmap = NULL;
	if (prcp_manager)
		kthread_stop(prcp_manager);
#if 0
	drain_pages_color(0);
	drain_pages_color(1);
	drain_pages_color(2);
	drain_pages_color(3);
	free_global_color_list(0);
#endif

	printk(KERN_ALERT "[DCSLAB]prcp stop\n");
}

module_init(scan_init);
module_exit(scan_exit);
MODULE_LICENSE("GPL");
