#include "procsmpl.h"
#include "mmap_processor.h"

#include <poll.h>
#include <fcntl.h>
#include <errno.h>

static __thread threadsmpl tsmp;

pid_t gettid(void)
{
	return (pid_t)syscall(__NR_gettid);
}

int perf_event_open(struct perf_event_attr *attr,
                    pid_t pid, int cpu, int group_fd,
                    unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

void thread_sighandler(int sig, siginfo_t *info, void *extra)
{
    int i;
    int fd = info->si_fd;

    for(i=0; i<tsmp.num_events; i++)
    {
        if(tsmp.events[i].fd == fd)
        {
            process_sample_buffer(&tsmp.pes,
                                  tsmp.events[i].attr.sample_type,
                                  tsmp.proc_parent->handler_fn,
                                  tsmp.proc_parent->handler_fn_args,
                                  tsmp.events[i].mmap_buf,
                                  tsmp.proc_parent->pgmsk);
        }
    }

    ioctl(fd, PERF_EVENT_IOC_REFRESH, 1);
}

procsmpl::procsmpl()
{
    // Defaults
    mmap_pages = 1;
    use_frequency = 1;
    sample_frequency = 4000;
    sample_latency_threshold = 8;
    pgsz = sysconf(_SC_PAGESIZE);
    mmap_size = (mmap_pages+1)*pgsz;
    pgmsk = mmap_pages*pgsz-1;

    handler_fn = NULL;

    first_time = true;
}

procsmpl::~procsmpl()
{
}

void procsmpl::init_attrs()
{
    num_attrs = 2;
    attrs = (struct perf_event_attr*)malloc(num_attrs*sizeof(struct perf_event_attr));

    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(struct perf_event_attr));
    attr.size = sizeof(struct perf_event_attr);

    attr.mmap = 1;
    attr.mmap_data = 1;
    attr.comm = 1;
    attr.exclude_user = 0;
    attr.exclude_kernel = 0;
    attr.exclude_hv = 0;
    attr.exclude_idle = 0;
    attr.exclude_host = 0;
    attr.exclude_guest = 1;
    attr.exclusive = 0;
    attr.pinned = 0;
    attr.sample_id_all = 0;
    attr.wakeup_events = 1;

    if(use_frequency)
    {
        attr.sample_freq = sample_frequency;
        attr.freq = 1;
    }
    else
    {
        attr.sample_period = sample_period;
        attr.freq = 0;
    }

    attr.sample_type =
        PERF_SAMPLE_IP |
        //PERF_SAMPLE_CALLCHAIN |
        //PERF_SAMPLE_ID |
        PERF_SAMPLE_STREAM_ID |
        PERF_SAMPLE_TIME |
        PERF_SAMPLE_TID |
        //PERF_SAMPLE_PERIOD |
        PERF_SAMPLE_CPU |
        PERF_SAMPLE_ADDR |
        PERF_SAMPLE_WEIGHT |
        PERF_SAMPLE_TRANSACTION |
        PERF_SAMPLE_DATA_SRC;

    attr.type = PERF_TYPE_RAW;

    // Set up load sampling
    attr.type = PERF_TYPE_RAW;
    attr.config = 0x5101cd;          // MEM_TRANS_RETIRED:LATENCY_THRESHOLD
    attr.config1 = sample_latency_threshold;
    //attr.config = 0x5341d0;         // MEM_UOPS_RETIRED:SPLIT_LOADS
    //attr.config1 = 0;
    attr.precise_ip = 2;
    attr.disabled = 1;              // Event group leader starts disabled

    attrs[0] = attr;

    // Set up store sampling
    attr.config = 0x5382d0;         // MEM_UOPS_RETIRED:ALL_STORES
    attr.config1 = 0;
    //attr.config = 0x5342d0;         // MEM_UOPS_RETIRED:SPLIT_STORES
    //attr.config1 = 0;
    attr.precise_ip = 2;
    attr.disabled = 0;              // Event group follower starts enabled

    attrs[1] = attr;
}

int procsmpl::begin_sampling()
{
    if(first_time)
        init_attrs();

    first_time = false;

    int ret = tsmp.init(this);
    if(ret)
        return ret;

    return tsmp.begin_sampling();
}

void procsmpl::end_sampling()
{
    tsmp.end_sampling();
}

int threadsmpl::init(procsmpl *parent)
{
    int ret;
    
    ready = 0;

    proc_parent = parent;

    ret = init_perf_events(proc_parent->attrs, proc_parent->num_attrs, proc_parent->mmap_size);
    if(ret)
        return ret;

    ret = init_thread_sighandler();
    if(ret)
        return ret;

    // Success
    ready = 1;

    return 0;
}

int threadsmpl::init_perf_events(struct perf_event_attr *attrs, int num_attrs, size_t mmap_size)
{
    int i;

    num_events = num_attrs;
    events = (struct perf_event_container*)malloc(num_events*sizeof(struct perf_event_container));

    events[0].fd = -1;
    
    for(i=0; i<num_events; i++)
    {
        events[i].attr = attrs[i];

        // Create attr according to sample mode
        events[i].fd = perf_event_open(&events[i].attr, gettid(), -1, events[0].fd, 0);

        //fprintf(stderr, "i: %d : thread: %d : events[0].fd: %d : returned %d\n", i, gettid(), events[0].fd, errno);

        if(events[i].fd == -1)
        {
            perror("perf_event_open");
            return 1;
        }

        // Create mmap buffer for samples
        events[i].mmap_buf = (struct perf_event_mmap_page*)
            mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, events[i].fd, 0);

        if(events[i].mmap_buf == MAP_FAILED)
        {
            perror("mmap");
            return 1;
        }
    }

    return 0;
}

int threadsmpl::init_thread_sighandler()
{
    int i, ret;
    struct f_owner_ex fown_ex;
    struct sigaction sact;

    // Set up signal handler
    memset(&sact, 0, sizeof(sact));
    sact.sa_sigaction = &thread_sighandler;
    sact.sa_flags = SA_SIGINFO;

    ret = sigaction(SIGIO, &sact, NULL);
    if(ret)
    {
        perror("sigaction");
        return ret;
    }

    // Unblock SIGIO signal if necessary
    sigset_t sold, snew;
    sigemptyset(&sold);
    sigemptyset(&snew);
    sigaddset(&snew, SIGIO);

    ret = sigprocmask(SIG_SETMASK, NULL, &sold);
    if(ret)
    {
        perror("sigaction");
        return 1;
    }

	if(sigismember(&sold, SIGIO))
    {
		ret = sigprocmask(SIG_UNBLOCK, &snew, NULL);
        if(ret)
        {
            perror("sigaction");
            return 1;
        }
    } 

    for(i=0; i<num_events; i++)
    {
        // Set perf event events[i].fd to signal
        ret = fcntl(events[i].fd, F_SETSIG, SIGIO);
        if(ret)
        {
            perror("fcntl");
            return 1;
        }
        ret = fcntl(events[i].fd, F_SETFL, O_NONBLOCK | O_ASYNC);
        if(ret)
        {
            perror("fcntl");
            return 1;
        }
        // Set owner to current thread
        fown_ex.type = F_OWNER_TID;
        fown_ex.pid = gettid();
        ret = fcntl(events[i].fd, F_SETOWN_EX, (unsigned long)&fown_ex);
        if(ret)
        {
            perror("fcntl");
            return 1;
        } 
    }

    return 0;
}


int threadsmpl::begin_sampling()
{
    int i, ret;

    if(!ready)
    {
        fprintf(stderr, "Not ready to begin sampling!\n");
        return 1;
    }

    ret = ioctl(events[0].fd, PERF_EVENT_IOC_RESET, 0);
    if(ret)
        perror("ioctl");

    ret = ioctl(events[0].fd, PERF_EVENT_IOC_ENABLE, 0);
    if(ret)
        perror("ioctl");

    return ret;
}

void threadsmpl::end_sampling()
{
    int i, ret;

    ret = ioctl(events[0].fd, PERF_EVENT_IOC_DISABLE, 0);
    if(ret)
        perror("ioctl");

    for(i=0; i<num_events; i++)
    {
        // Flush out remaining samples
        process_sample_buffer(&pes, 
                              events[i].attr.type,
                              proc_parent->handler_fn,
                              proc_parent->handler_fn_args,
                              events[i].mmap_buf, 
                              proc_parent->pgmsk); 
    }
}

const char* Mitos_hit_type(struct perf_event_sample *s)
{
    uint64_t lvl_bits = s->data_src >> PERF_MEM_LVL_SHIFT;

    if(lvl_bits & PERF_MEM_LVL_NA)
        return "Not Available";
    else if(lvl_bits & PERF_MEM_LVL_HIT)
        return "Hit";
    else if(lvl_bits & PERF_MEM_LVL_MISS)
        return "Miss";

    return "Invalid Data Source";
}

const char* Mitos_memory_level(struct perf_event_sample *s)
{
    uint64_t lvl_bits = s->data_src >> PERF_MEM_LVL_SHIFT;

    if(lvl_bits & PERF_MEM_LVL_NA)
        return "Not Available";
    else if(lvl_bits & PERF_MEM_LVL_L1)
        return "L1";
    else if(lvl_bits & PERF_MEM_LVL_LFB)
        return "LFB";
    else if(lvl_bits & PERF_MEM_LVL_L2)
        return "L2";
    else if(lvl_bits & PERF_MEM_LVL_L3)
        return "L3";
    else if(lvl_bits & PERF_MEM_LVL_LOC_RAM)
        return "Local RAM";
    else if(lvl_bits & PERF_MEM_LVL_REM_RAM1)
        return "Remote RAM 1 Hop";
    else if(lvl_bits & PERF_MEM_LVL_REM_RAM2)
        return "Remote RAM 2 Hops";
    else if(lvl_bits & PERF_MEM_LVL_REM_CCE1)
        return "Remote Cache 1 Hops";
    else if(lvl_bits & PERF_MEM_LVL_REM_CCE2)
        return "Remote Cache 2 Hops";
    else if(lvl_bits & PERF_MEM_LVL_IO)
        return "I/O Memory";
    else if(lvl_bits & PERF_MEM_LVL_UNC)
        return "Uncached Memory";

    return "Invalid Data Source";
}

const char* Mitos_op_type(struct perf_event_sample *s)
{
    uint64_t op_bits = s->data_src >> PERF_MEM_OP_SHIFT;

    if(op_bits & PERF_MEM_OP_NA)
        return "Not Available";
    else if(op_bits & PERF_MEM_OP_LOAD)
        return "Load";
    else if(op_bits & PERF_MEM_OP_STORE)
        return "Store";
    else if(op_bits & PERF_MEM_OP_PFETCH)
        return "Prefetch";
    else if(op_bits & PERF_MEM_OP_EXEC)
        return "Exec";

    return "Invalid Data Source";
}

const char* Mitos_snoop_mode(struct perf_event_sample *s)
{
    uint64_t snoop_bits = s->data_src >> PERF_MEM_SNOOP_SHIFT;

    if(snoop_bits & PERF_MEM_SNOOP_NA)
        return "Not Available";
    else if(snoop_bits & PERF_MEM_SNOOP_NONE)
        return "Snoop None";
    else if(snoop_bits & PERF_MEM_SNOOP_HIT)
        return "Snoop Hit";
    else if(snoop_bits & PERF_MEM_SNOOP_MISS)
        return "Snoop Miss";
    else if(snoop_bits & PERF_MEM_SNOOP_HITM)
        return "Snoop Hit Modified";

    return "Invalid Data Source";
}

const char* Mitos_tlb_access(struct perf_event_sample *s)
{
    uint64_t tlb_bits = s->data_src >> PERF_MEM_TLB_SHIFT;

    if(tlb_bits & PERF_MEM_TLB_NA)
        return "Not Available";
    else if(tlb_bits & PERF_MEM_TLB_HIT)
        return "TLB Hit";
    else if(tlb_bits & PERF_MEM_TLB_MISS)
        return "TLB Miss";
    else if(tlb_bits & PERF_MEM_TLB_L1)
        return "TLB L1";
    else if(tlb_bits & PERF_MEM_TLB_L2)
        return "TLB L2";
    else if(tlb_bits & PERF_MEM_TLB_WK)
        return "TLB Hardware Walker";
    else if(tlb_bits & PERF_MEM_TLB_OS)
        return "TLB OS Fault Handler";

    return "Invalid Data Source";
}
