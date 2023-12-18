#ifndef __MONITOR_H__
#define __MONITOR_H__

#include <linux/perf_event.h>

#include <map>
#include <set>
#include <string>
#include <vector>

// Event ID: UMask + EventSel. (https://perfmon-events.intel.com/skylake_server.html)
#define EVENT_RxC_OCCUPANCY_IRQ 0x0111UL
#define EVENT_RxC_INSERTS_IRQ 0x0113UL
#define EVENT_CAS_COUNT_RD 0x0304UL
#define EVENT_CAS_COUNT_WR 0x0201UL
#define EVENT_L1D_PEND_MISS_PENDING 0x0148UL
#define EVENT_MEM_LOAD_RETIRED_L1_MISS 0x08D1UL
#define EVENT_MEM_LOAD_RETIRED_L3_MISS 0x20D1UL
#define EVENT_MEM_LOAD_L3_MISS_RETIRED_LOCAL_DRAM 0x01D3UL
#define EVENT_MEM_LOAD_L3_MISS_RETIRED_REMOTE_DRAM 0x02D3UL
#define EVENT_OFFCORE_REQUESTS_ALL_REQUESTS 0x80B0
#define EVENT_OFFCORE_REQUESTS_L3_MISS_DEMAND_DATA_RD 0x10B0

#define PAGE_SIZE 4096UL
#define NUM_PERF_EVENT_MMAP_PAGES 256UL
#define SAMPLING_PERIOD_EVENT 500UL         // in # of events
//#define SAMPLING_PERIOD_MS 50UL             // in ms
//#define SAMPLING_PERIOD_MS 200UL             // in ms
//#define SAMPLING_PERIOD_MS 500UL             // in ms
#define SAMPLING_PERIOD_MS 1000UL             // in ms
#define EWMA_ALPHA 0.5

#define PAGE_MASK ((PAGE_SIZE - 1) ^ UINT64_MAX)      // ~(PAGE_SIZE - 1)

#define NUM_SOCKETS 2
#define PROCESSOR_GHZ 2.4           // CloudLab c6420
#define NUM_CORES 64                // CloudLab c6420; include both sockets
#define NUM_CORES_PER_SOCKET 32     // CloudLab c6420
// TODO: make one of the core (local) exclusive for monitoring
#define NUM_TIERS 2                 // Fast and Slow

// TODO: read the numbers from path
// perf_event_attr.type value for each individual cha unit found in /sys/bus/event_source/devices/uncore_cha_*/type
const uint32_t PMU_CHA_TYPE[] = {25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40};   // CloudLab c6420

// perf_event_attr.type value for each individual imc unit found in /sys/bus/event_source/devices/uncore_imc_*/type
const uint32_t PMU_IMC_TYPE[] = {12, 13, 14, 15, 16, 17};   // CloudLab c6420

class ApplicationInfo {
  public:
  ApplicationInfo(std::string app_name);
  ~ApplicationInfo();
    int pid;
    bool process_exists;
    std::string name;
    std::vector<int> bw_cores;
    std::vector<int> lat_cores;
};

class LatencyInfoPerCore {
  public:
    LatencyInfoPerCore(int cpu_id);
    ~LatencyInfoPerCore();
    int cpu_id;
    int fd_l1d_pend_miss;
    int fd_retired_l3_miss;
    uint64_t curr_count_l1d_pend_miss;
    uint64_t curr_count_retired_l3_miss;
};

class LatencyInfoPerProcess {
  public:
    LatencyInfoPerProcess();    // std::map wants it for [] operator
    LatencyInfoPerProcess(int pid);
    ~LatencyInfoPerProcess();
    int pid;
    int fd_l1d_pend_miss;
    int fd_retired_l3_miss;
    uint64_t curr_count_l1d_pend_miss;
    uint64_t curr_count_retired_l3_miss;
};

class BWInfoPerCore {
  public:
    BWInfoPerCore(int cpu_id);
    ~BWInfoPerCore();
    int cpu_id;
    int fd_offcore_all_reqs;
    uint64_t curr_count_offcore_all_reqs;
    double curr_bw;
};

// https://manpages.ubuntu.com/manpages/xenial/man2/perf_event_open.2.html
// see "PERF_RECORD_SAMPLE"
struct PerfSample {
    struct perf_event_header header;
    uint32_t pid, tid;      // PERF_SAMPLE_TID
    uint64_t addr;          // PERF_SAMPLE_ADDR
    uint32_t cpu, res;      // PERF_SAMPLE_CPU
};

// assume 1 app can take 1 or more cores, and 1 core can't take more than 1 app
class PageTempInfoPerCore {
  public:
    PageTempInfoPerCore(int cpu_id, int num_events);
    ~PageTempInfoPerCore();
    int cpu_id;
    std::vector<int> fds;   // curently 2 events; fast and slow mem retired loads
    std::vector<struct perf_event_mmap_page *> perf_m_pages;
    // TODO: store page info
};

class Monitor {
  public:
    Monitor();
    ~Monitor();

    void add_application(ApplicationInfo *app_info);

    // TODO: move most functions to private
    void perf_event_reset(int fd);
    void perf_event_enable(int fd);
    void perf_event_disable(int fd);
    int perf_event_setup(int pid, int cpu, int group_fd, uint32_t type, uint64_t event_id);
    double sleep_ms(int time);
    
    int get_pid_from_proc_name(std::string proc_name);

    void measure_uncore_latency();

    void perf_event_setup_core_latency(int cpu_id);
    void perf_event_enable_core_latency(int cpu_id);
    void perf_event_disable_core_latency(int cpu_id);
    void perf_event_read_core_latency(int cpu_id);
    void measure_core_latency(int cpu_id);

    void perf_event_setup_cores_latency(const std::set<int> &cpu_ids);
    void perf_event_enable_cores_latency(const std::set<int> &cpu_ids);
    void perf_event_disable_cores_latency(const std::set<int> &cpu_ids);
    void perf_event_read_cores_latency(const std::set<int> &cpu_ids);
    void measure_cores_latency(const std::set<int> &cpu_ids);

    void perf_event_setup_process_latency(int pid);
    void perf_event_enable_process_latency(int pid);
    void perf_event_disable_process_latency(int pid);
    void perf_event_read_process_latency(int pid, bool log_latency = false, ApplicationInfo *app_info = NULL);
    void measure_process_latency(int pid);
    void measure_process_latency(std::string proc_name);
    void measure_application_latency();

    void perf_event_setup_uncore_mem_bw(int opcode);
    void perf_event_enable_uncore_mem_bw(int opcode);
    void perf_event_disable_uncore_mem_bw(int opcode);
    void perf_event_read_uncore_mem_bw(int opcode, double elapsed);
    void measure_uncore_bandwidth(int opcode);
    void measure_uncore_bandwidth_read();
    void measure_uncore_bandwidth_write();
    void measure_uncore_bandwidth_all();

    void perf_event_setup_offcore_mem_bw(int cpu_id);
    //void perf_event_setup_offcore_mem_bw_l3_load(int cpu_id);
    void perf_event_enable_offcore_mem_bw(int cpu_id);
    void perf_event_disable_offcore_mem_bw(int cpu_id);
    void perf_event_read_offcore_mem_bw(int cpu_id, double elapsed);
    void measure_offcore_bandwidth(const std::vector<int> &cores);
    void measure_total_bandwidth_per_socket();
    void measure_application_bandwidth();

    int perf_event_setup_pebs(int pid, int cpu, int group_fd, uint32_t type, uint64_t event_id);
    struct perf_event_mmap_page *perf_event_setup_mmap_page(int fd);
    void perf_event_setup_page_temp(const std::vector<int> &cores);
    void perf_event_enable_page_temp(const std::vector<int> &cores);
    void sample_page_access(const std::vector<int> &cores);
    void measure_hot_page_pctg(const std::vector<int> &cores);
    void measure_page_temp(const std::vector<int> &cores);


  private:
    uint32_t num_sockets_;
    int sampling_period_ms_;
    int sampling_period_event_;
    double ewma_alpha_;
    std::vector<uint32_t> pmu_cha_type_;
    std::vector<uint32_t> pmu_imc_type_;

    // for uncore perf measurements ([socket][cha])
    std::vector<std::vector<int>> fd_rxc_occ_;
    std::vector<std::vector<int>> fd_rxc_ins_;
    std::vector<std::vector<int>> fd_cas_rd_;
    std::vector<std::vector<int>> fd_cas_wr_;
    std::vector<std::vector<int>> fd_cas_all_;
    std::vector<std::vector<uint64_t>> curr_count_occ_;
    std::vector<std::vector<uint64_t>> curr_count_ins_;
    std::vector<std::vector<uint64_t>> curr_count_rd_;
    std::vector<std::vector<uint64_t>> curr_count_wr_;
    std::vector<std::vector<double>> bw_read_;
    std::vector<std::vector<double>> bw_write_;

    // for core lat measurements ([cpu])
    std::vector<LatencyInfoPerCore> lat_info_cpu_;

    // for per-thread lat measurements ({pid, ()})
    std::map<int, LatencyInfoPerProcess> lat_info_process_;
    std::vector<double> sampled_process_lat_;   // for plotting

    // for offcore bw measurements ([cpu])
    std::vector<BWInfoPerCore> bw_info_cpu_;

    // for page temperature monitoring ([cpu])
    std::vector<uint32_t> page_temp_events_;
    std::vector<PageTempInfoPerCore> page_temp_info_;
    std::map<uint64_t, uint64_t> page_access_map_;
    uint64_t num_cpu_throttle_;
    uint64_t num_cpu_unthrottle_;
    uint64_t num_local_access_;
    uint64_t num_remote_access_;
    // TODO: need a pid to core mapping

    std::map<int, ApplicationInfo *> application_info_;     // key: pid
    std::set<int> bw_core_list_;
    
};

#endif
