/*
 * This benchmark is designed to stress THP allocation and compaction. It does
 * not guarantee that THP allocations take place and it's up to the user to
 * monitor system activity and check that the relevant paths are used.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <numaif.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define PAGESIZE page_size
#define HPAGESIZE hpage_size
#define __ALIGN_MASK(x, mask)    (((x) + (mask)) & ~(mask))
#define ALIGN(x, a)            __ALIGN_MASK(x, (typeof(x))(a) - 1)
#define PTR_ALIGN(p, a)         ((typeof(p))ALIGN((unsigned long)(p), (a)))
#define HPAGE_ALIGN(p)          PTR_ALIGN(p, HPAGESIZE)

size_t total_size;
size_t thread_size;
unsigned long *anon_init;
int nr_hpages;
int madvise_huge;
size_t page_size;
size_t hpage_size;

/* barrier for all threads to finish initialisation on */
static pthread_barrier_t init_barrier;

static inline uint64_t timeval_to_us(struct timeval *tv)
{
	return ((uint64_t)tv->tv_sec * 1000000) + tv->tv_usec;
}

static inline int current_nid() {
#ifdef SYS_getcpu
	int cpu, nid, ret;

	ret = syscall(SYS_getcpu, &cpu, &nid, NULL);
	return ret == -1 ? -1 : nid;
#else
	return -1;
#endif
}

static void setup_page_sizes()
{
#ifdef __powerpc64__
	char line[128];
	FILE *f;
	int rc;

	f = fopen("/proc/cpuinfo", "r");
	if (!f) {
		perror("CPU info");
		exit(EXIT_FAILURE);
	}

	hpage_size = 1048576 * 2;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!strcmp(line, "MMU		: Hash\n")) {
			hpage_size = 1048576 * 16;
			break;
		}
	}

	fclose(f);
#else
	hpage_size = 1048576 * 2;
#endif
	page_size = getpagesize();
}

struct fault_timing {
	bool hugepage;
	struct timeval tv;
	uint64_t latency;
	int locality;
};

static struct fault_timing **timings;

static void *worker(void *data)
{
	int thread_idx = (unsigned long *)data - anon_init;
	size_t i, offset;
	char *mapping;
	char *aligned;
	struct timeval tv_start, tv_end;
	int task_nid, memory_nid;

	gettimeofday(&tv_start, NULL);

	/* Allocate mapping but do not fault it */
	mapping = mmap(NULL, thread_size + HPAGESIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
	if (mapping == MAP_FAILED) {
		perror("mapping");
		exit(EXIT_FAILURE);
	}
	aligned = HPAGE_ALIGN(mapping);
	offset = aligned - mapping;

	if (madvise_huge) {
		printf("Using madv_hugepage 0x%lu %lu gb\n", (unsigned long)mapping, (thread_size + HPAGESIZE)/1048576/1024);
		madvise(mapping, thread_size + HPAGESIZE, MADV_HUGEPAGE);
	} else {
		printf("Using default advice\n");
	}

	/* Record anon init timings */
	gettimeofday(&tv_end, NULL);
	anon_init[thread_idx] = timeval_to_us(&tv_end) - timeval_to_us(&tv_start);

	/* Wait for all threads to init */
	pthread_barrier_wait(&init_barrier);

	/* Fault the mapping and record timings */
	for (i = 0; i < nr_hpages; i++) {
		unsigned char vec;
		size_t arridx = offset + i * HPAGESIZE;
		int ret;

		gettimeofday(&tv_start, NULL);
		mapping[arridx] = 1;

		/* Check if the fault is THP or not */
		mincore(&mapping[arridx + PAGESIZE*64], PAGESIZE, &vec);
		timings[thread_idx][i].hugepage = vec;

		/* Measure time to fill a THPs worth of memory */
		memset(&mapping[arridx], 2, HPAGESIZE);

		/* Total latency is time to fault and write */
		gettimeofday(&timings[thread_idx][i].tv, NULL);

		/*
		 * Check locality, this is approximate as task could have
		 * migrated during the fault.
		 */
		task_nid = current_nid();
		ret = get_mempolicy(&memory_nid, NULL, 0, &mapping[arridx], MPOL_F_NODE|MPOL_F_ADDR);
		if (ret == -1)
			timings[thread_idx][i].locality = -1;
		else
			timings[thread_idx][i].locality = (memory_nid == task_nid) ? 1 : 0;

		/* Record the latency */
		timings[thread_idx][i].latency = timeval_to_us(&timings[thread_idx][i].tv) - timeval_to_us(&tv_start);
	}

	/* Cleanup */
	munmap(mapping, thread_size);

	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t *th;
	int nr_threads, i, j;
	if (argc != 4) {
		printf("Usage: thpfioscale [nr_threads] [total_size] [madvise_hugepage]\n");
		exit(EXIT_FAILURE);
	}

	setup_page_sizes();
	nr_threads = atoi(argv[1]);
	total_size = atol(argv[2]);
	madvise_huge = atoi(argv[3]);
	printf("Running with %d thread%c\n", nr_threads, nr_threads > 1 ? 's' : ' ');
	anon_init = malloc(nr_threads * sizeof(unsigned long));
	if (anon_init == NULL) {
		printf("Unable to allocate anon_init\n");
		exit(EXIT_FAILURE);
	}

	nr_hpages = total_size / nr_threads / HPAGESIZE / 2;
	thread_size = PTR_ALIGN(total_size / nr_threads, HPAGESIZE*4);

	th = malloc(nr_threads * sizeof(pthread_t));
	if (th == NULL) {
		printf("Unable to allocate thread structures\n");
		exit(EXIT_FAILURE);
	}

	timings = malloc(nr_threads * sizeof(struct fault_timing *));
	if (timings == NULL) {
		printf("Unable to allocate timings structure\n");
		exit(EXIT_FAILURE);
	}

	pthread_barrier_init(&init_barrier, NULL, nr_threads);
	for (i = 0; i < nr_threads; i++) {
		timings[i] = malloc(nr_hpages * sizeof(struct fault_timing));
		if (timings[i] == NULL) {
			printf("Unable to allocate timing for thread %d\n", i);
			exit(EXIT_FAILURE);
		}
		if (pthread_create(&th[i], NULL, worker, &anon_init[i])) {
			perror("Creating thread");
			exit(EXIT_FAILURE);
		}
	}

	for (i = 0; i < nr_threads; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&init_barrier);

	printf("\n");
	for (i = 0; i < nr_threads; i++)
		printf("anoninit %d %12lu\n", i, anon_init[i]);

	for (i = 0; i < nr_threads; i++)
		for (j = 0; j < nr_hpages; j++)
			printf("fault %d %s %12lu %lu.%lu %d\n", i,
				timings[i][j].hugepage ? "huge" : "base",
				timings[i][j].latency,
				timings[i][j].tv.tv_sec,
				timings[i][j].tv.tv_usec,
				timings[i][j].locality);

	return 0;
}

