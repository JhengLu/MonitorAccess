#include "monitor.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <linux/perf_event.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <tuple>

Monitor monitor = Monitor();
std::vector<int> cores_g;

// Takeaways (for uncore monitoring):
// (1) For perf_event_attr.type, specify the integer value for individual PMU instead of PERF_TYPE_RAW
// (2) Do not enable perf_event_attr.exclude_kernel
// (3) In the hardware I use, I need to set precise_ip = 0 (i.e., PEBS won't work).
//     Newer hardware may support PEBS for uncore monitoring.
// (4) uncore monitoring is per-socket (i.e., no per-core or per-process monitoring)

static int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
    int ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
                    group_fd, flags);
    return ret;
}

ApplicationInfo::ApplicationInfo(std::string app_name) {
    pid = -1;
    process_exists = false;
    name = app_name;
}

ApplicationInfo::~ApplicationInfo() {
}

LatencyInfoPerCore::LatencyInfoPerCore(int cpu_id) {
    cpu_id = cpu_id;
    fd_l1d_pend_miss = -1;
    fd_retired_l3_miss = -1;
    curr_count_l1d_pend_miss = 0;
    curr_count_retired_l3_miss = 0;
}

LatencyInfoPerCore::~LatencyInfoPerCore() {
}

LatencyInfoPerProcess::LatencyInfoPerProcess() {
    pid = -1;
    fd_l1d_pend_miss = -1;
    fd_retired_l3_miss = -1;
    curr_count_l1d_pend_miss = 0;
    curr_count_retired_l3_miss = 0;
}

LatencyInfoPerProcess::LatencyInfoPerProcess(int pid) {
    pid = pid;
    fd_l1d_pend_miss = -1;
    fd_retired_l3_miss = -1;
    curr_count_l1d_pend_miss = 0;
    curr_count_retired_l3_miss = 0;
}

LatencyInfoPerProcess::~LatencyInfoPerProcess() {
}

BWInfoPerCore::BWInfoPerCore(int cpu_id) {
    cpu_id = cpu_id;
    fd_offcore_all_reqs = -1;
    curr_count_offcore_all_reqs = 0;
    curr_bw = 0;
}

BWInfoPerCore::~BWInfoPerCore() {
}

PageTempInfoPerCore::PageTempInfoPerCore(int cpu_id, int num_events) {
    cpu_id = cpu_id;
    fds.resize(num_events, -1);
    perf_m_pages.resize(num_events, NULL);
}

// TODO: consider calling munmap for perf pages
PageTempInfoPerCore::~PageTempInfoPerCore() {
}

template<typename A, typename B>
std::pair<B,A> flip_pair(const std::pair<A,B> &p)
{
    return std::pair<B,A>(p.second, p.first);
}

template<typename A, typename B>
std::multimap<B,A> flip_map(const std::map<A,B> &src)
{
    std::multimap<B,A> dst;
    std::transform(src.begin(), src.end(), std::inserter(dst, dst.begin()), 
                   flip_pair<A,B>);
    return dst;
}

Monitor::Monitor() {
    num_sockets_ = NUM_SOCKETS;
    sampling_period_ms_ = SAMPLING_PERIOD_MS;
    sampling_period_event_ = SAMPLING_PERIOD_EVENT;
    ewma_alpha_ = EWMA_ALPHA;
    num_cpu_throttle_ = 0;
    num_cpu_unthrottle_ = 0;
    num_local_access_ = 0;
    num_remote_access_ = 0;
    for (const auto &x : PMU_CHA_TYPE) {
        pmu_cha_type_.push_back(x);
    }
    for (const auto &x : PMU_IMC_TYPE) {
        pmu_imc_type_.push_back(x);
    }
    fd_rxc_occ_.resize(NUM_SOCKETS);
    fd_rxc_ins_.resize(NUM_SOCKETS);
    fd_cas_rd_.resize(NUM_SOCKETS);
    fd_cas_wr_.resize(NUM_SOCKETS);
    fd_cas_all_.resize(NUM_SOCKETS);
    curr_count_occ_ = std::vector<std::vector<uint64_t>>(num_sockets_,
        std::vector<uint64_t>(pmu_cha_type_.size(), 0));
    curr_count_ins_ = std::vector<std::vector<uint64_t>>(num_sockets_,
        std::vector<uint64_t>(pmu_cha_type_.size(), 0));
    curr_count_rd_ = std::vector<std::vector<uint64_t>>(num_sockets_,
        std::vector<uint64_t>(pmu_imc_type_.size(), 0));
    curr_count_wr_ = std::vector<std::vector<uint64_t>>(num_sockets_,
        std::vector<uint64_t>(pmu_imc_type_.size(), 0));
    bw_read_ = std::vector<std::vector<double>>(num_sockets_,
        std::vector<double>(pmu_imc_type_.size(), 0));
    bw_write_ = std::vector<std::vector<double>>(num_sockets_,
        std::vector<double>(pmu_imc_type_.size(), 0));

    for (int i = 0; i < NUM_CORES; i++) {
        lat_info_cpu_.emplace_back(LatencyInfoPerCore(i));
    }

    for (int i = 0; i < NUM_CORES; i++) {
        bw_info_cpu_.emplace_back(BWInfoPerCore(i));
    }

    page_temp_events_ = {EVENT_MEM_LOAD_L3_MISS_RETIRED_LOCAL_DRAM, EVENT_MEM_LOAD_L3_MISS_RETIRED_REMOTE_DRAM};
    for (int i = 0; i < NUM_CORES; i++) {
        page_temp_info_.emplace_back(PageTempInfoPerCore(i, page_temp_events_.size()));
    }

}

Monitor::~Monitor() {
    // TODO: delete ApplicationInfo *
}

void Monitor::add_application(ApplicationInfo *app_info) {
    // find pid by app name
    int pid = -1;
    bool found_pid = false;
    while (!found_pid) {
        pid = get_pid_from_proc_name(app_info->name);
        if (pid != -1) {
            found_pid = true;
        }
    }
    app_info->pid = pid;
    app_info->process_exists = true;

    application_info_[app_info->pid] = app_info;

    // update the core list to monitor b/w
    for (const auto &c : app_info->bw_cores) {
        if (bw_core_list_.contains(c)) {
            std::cout << "[Error] add_application: bw core (" << c 
                << ") already exists in monitor's bw core list" << std::endl;
        }
        bw_core_list_.insert(c);
    }
}

void Monitor::perf_event_reset(int fd) {
    int ret = ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    if (ret < 0) {
        std::cout << "[Error] perf_event_reset: " << strerror(errno) << std::endl;
    }
}

void Monitor::perf_event_enable(int fd) {
    int ret = ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    if (ret < 0) {
        std::cout << "[Error] perf_event_enable: " << strerror(errno) << std::endl;
    }
}

void Monitor::perf_event_disable(int fd) {
    int ret = ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    if (ret < 0) {
        std::cout << "[Error] perf_event_disable: " << strerror(errno) << std::endl;
    }
}

//int perf_event_setup(int cpu, int pid, int group_fd, int sampling_period) {
int Monitor::perf_event_setup(int pid, int cpu, int group_fd, uint32_t type, uint64_t event_id) {
    struct perf_event_attr event_attr;
    memset(&event_attr, 0, sizeof(event_attr));
    event_attr.type = type;
    event_attr.size = sizeof(event_attr);
    event_attr.config = event_id;
    event_attr.disabled = 1;
    event_attr.inherit = 1;     // includes child process
    event_attr.precise_ip = 0;

    int ret = perf_event_open(&event_attr, pid, cpu, group_fd, 0);
    if (ret < 0) {
        std::cout << "[Error] perf_event_open: " << strerror(errno) << std::endl;
    }
    return ret;
}

double Monitor::sleep_ms(int time) {
    auto start = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(sampling_period_ms_));
    std::chrono::duration<double, std::milli> elapsed = std::chrono::high_resolution_clock::now() - start;
    return elapsed.count();
}

int Monitor::get_pid_from_proc_name(std::string proc_name) {
    std::string cmd = "pidof " + proc_name;
    char pidline[1024] = "";
    FILE *fp = popen(cmd.c_str(), "r");
    fgets(pidline, 1024, fp);

    if (pidline && !pidline[0]) {   // check empty c string
        pclose(fp);
        return -1;
    }

    int pid = strtoul(pidline, NULL, 10);
    pclose(fp);

    return pid;
}

void Monitor::measure_uncore_latency() {
    // setup fd for each event on each PMU per socket
    for (int i = 0; i < num_sockets_; i++) {
        for (int j = 0; j < pmu_cha_type_.size(); j++) {
            int fd = perf_event_setup(-1, i, -1, pmu_cha_type_[j], EVENT_RxC_OCCUPANCY_IRQ);
            fd_rxc_occ_[i].push_back(fd);
            perf_event_reset(fd);

            fd = perf_event_setup(-1, i, -1, pmu_cha_type_[j], EVENT_RxC_INSERTS_IRQ);
            fd_rxc_ins_[i].push_back(fd);
            perf_event_reset(fd);
        }
    }

    // monitor all PMUs
    //std::vector<uint64_t> count_sum(2, 0);
    for (;;) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_cha_type_.size(); j++) {
                perf_event_enable(fd_rxc_occ_[i][j]);
                perf_event_enable(fd_rxc_ins_[i][j]);
            }
        }

        sleep_ms(sampling_period_ms_);

        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_cha_type_.size(); j++) {
                perf_event_disable(fd_rxc_occ_[i][j]);
                perf_event_disable(fd_rxc_ins_[i][j]);
            }
        }

        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_cha_type_.size(); j++) {
                uint64_t count_occ = 0, count_ins = 0;     // TODO: need to construct a custom struct when we specify more read format later
                read(fd_rxc_occ_[i][j], &count_occ, sizeof(count_occ));
                read(fd_rxc_ins_[i][j], &count_ins, sizeof(count_ins));
                double latency_cycles = (double) (count_occ - curr_count_occ_[i][j]) / (count_ins - curr_count_ins_[i][j]);
                curr_count_occ_[i][j] = count_occ;
                curr_count_ins_[i][j] = count_ins;
                double latency_ns = latency_cycles / PROCESSOR_GHZ;

                std::cout << "socket[" << i << "]: cha" << j << "  \tRxC_OCCUPANCY.IRQ = " << count_occ
                    << ", RxC_INSERTS.IRQ = " << count_ins << ", latency = " << latency_ns << " ns" << std::endl;
                //count_sum[i] += count;
            }
            //std::cout << "socket[" << i << "]: total count = " << count_sum[i] << std::endl;
        }
        std::cout << std::endl;
    }
}

void Monitor::perf_event_setup_cores_latency(const std::set<int> &cpu_ids) {
    assert(!cpu_ids.empty());

    for (auto it = cpu_ids.begin(); it != cpu_ids.end(); ++it) {
        int cpu_id = *it;
        lat_info_cpu_[cpu_id].fd_l1d_pend_miss = perf_event_setup(
            -1, cpu_id, -1, PERF_TYPE_RAW, EVENT_L1D_PEND_MISS_PENDING);
        perf_event_reset(lat_info_cpu_[cpu_id].fd_l1d_pend_miss);
    }

    for (auto it = cpu_ids.begin(); it != cpu_ids.end(); ++it) {
        int cpu_id = *it;
        lat_info_cpu_[cpu_id].fd_retired_l3_miss = perf_event_setup(
            -1, cpu_id, -1, PERF_TYPE_RAW, EVENT_MEM_LOAD_RETIRED_L3_MISS);
        perf_event_reset(lat_info_cpu_[cpu_id].fd_retired_l3_miss);
    }
}

void Monitor::perf_event_enable_cores_latency(const std::set<int> &cpu_ids) {
    for (const auto &cpu_id : cpu_ids) {
        perf_event_enable(lat_info_cpu_[cpu_id].fd_l1d_pend_miss);
        perf_event_enable(lat_info_cpu_[cpu_id].fd_retired_l3_miss);
    }
}

void Monitor::perf_event_disable_cores_latency(const std::set<int> &cpu_ids) {
    for (const auto &cpu_id : cpu_ids) {
        perf_event_disable(lat_info_cpu_[cpu_id].fd_l1d_pend_miss);
        perf_event_disable(lat_info_cpu_[cpu_id].fd_retired_l3_miss);
    }
}

void Monitor::perf_event_read_cores_latency(const std::set<int> &cpu_ids) {
    uint64_t count_l1d_pend_misses = 0, count_retired_l3_misses = 0;
    uint64_t curr_count_l1d_pend_misses = 0, curr_count_retired_l3_misses = 0;

    for (const auto &cpu_id : cpu_ids) {
        uint64_t count_l1d_pend_miss = 0, count_retired_l3_miss = 0;
        read(lat_info_cpu_[cpu_id].fd_l1d_pend_miss, &count_l1d_pend_miss,
             sizeof(count_l1d_pend_miss));
        read(lat_info_cpu_[cpu_id].fd_retired_l3_miss, &count_retired_l3_miss,
             sizeof(count_retired_l3_miss));
        count_l1d_pend_misses += count_l1d_pend_miss;
        count_retired_l3_misses += count_retired_l3_miss;
        curr_count_l1d_pend_misses +=
            lat_info_cpu_[cpu_id].curr_count_l1d_pend_miss;
        curr_count_retired_l3_misses +=
            lat_info_cpu_[cpu_id].curr_count_retired_l3_miss;

        lat_info_cpu_[cpu_id].curr_count_l1d_pend_miss = count_l1d_pend_miss;
        lat_info_cpu_[cpu_id].curr_count_retired_l3_miss =
            count_retired_l3_miss;
    }

    double latency_cycles =
        (double)(count_l1d_pend_misses - curr_count_l1d_pend_misses) /
        (count_retired_l3_misses - curr_count_retired_l3_misses);

    double latency_ns = latency_cycles / PROCESSOR_GHZ;
    std::string cpu_ids_str = "";
    for (size_t i = 0; i < cpu_ids.size(); ++i) {
        cpu_ids_str += std::to_string(*std::next(cpu_ids.begin(), i));
        if (i != cpu_ids.size() - 1) {
            cpu_ids_str += ",";
        }
    }
    std::cout << "cpus[" << cpu_ids_str << "]: latency = " << latency_ns
              << " ns" << std::endl;
}

void Monitor::measure_cores_latency(const std::set<int> &cpu_ids) {
    perf_event_setup_cores_latency(cpu_ids);
    for (;;) {
        perf_event_enable_cores_latency(cpu_ids);

        sleep_ms(sampling_period_ms_);

        perf_event_disable_cores_latency(cpu_ids);
        perf_event_read_cores_latency(cpu_ids);
    }
}

// TODO: group multiple latency-related events together
void Monitor::perf_event_setup_core_latency(int cpu_id) {
    int fd = perf_event_setup(-1, cpu_id, -1, PERF_TYPE_RAW, EVENT_L1D_PEND_MISS_PENDING);
    lat_info_cpu_[cpu_id].fd_l1d_pend_miss = fd;
    perf_event_reset(fd);

    fd = perf_event_setup(-1, cpu_id, -1, PERF_TYPE_RAW, EVENT_MEM_LOAD_RETIRED_L3_MISS);
    lat_info_cpu_[cpu_id].fd_retired_l3_miss = fd;
    perf_event_reset(fd);
}

void Monitor::perf_event_enable_core_latency(int cpu_id) {
    perf_event_enable(lat_info_cpu_[cpu_id].fd_l1d_pend_miss);
    perf_event_enable(lat_info_cpu_[cpu_id].fd_retired_l3_miss);
}

void Monitor::perf_event_disable_core_latency(int cpu_id) {
    perf_event_disable(lat_info_cpu_[cpu_id].fd_l1d_pend_miss);
    perf_event_disable(lat_info_cpu_[cpu_id].fd_retired_l3_miss);
}

void Monitor::perf_event_read_core_latency(int cpu_id) {
    uint64_t count_l1d_pend_miss = 0, count_retired_l3_miss = 0;
    read(lat_info_cpu_[cpu_id].fd_l1d_pend_miss, &count_l1d_pend_miss, sizeof(count_l1d_pend_miss));
    read(lat_info_cpu_[cpu_id].fd_retired_l3_miss, &count_retired_l3_miss, sizeof(count_retired_l3_miss));
    double latency_cycles = (double) (count_l1d_pend_miss - lat_info_cpu_[cpu_id].curr_count_l1d_pend_miss)
        / (count_retired_l3_miss - lat_info_cpu_[cpu_id].curr_count_retired_l3_miss);
    lat_info_cpu_[cpu_id].curr_count_l1d_pend_miss = count_l1d_pend_miss;
    lat_info_cpu_[cpu_id].curr_count_retired_l3_miss = count_retired_l3_miss;
    double latency_ns = latency_cycles / PROCESSOR_GHZ;
    std::cout << "cpu[" << cpu_id << "]: latency = " << latency_ns << " ns" << std::endl;
}

void Monitor::measure_core_latency(int cpu_id) {
    perf_event_setup_core_latency(cpu_id);
    for (;;) {
        perf_event_enable_core_latency(cpu_id);

        sleep_ms(sampling_period_ms_);

        perf_event_disable_core_latency(cpu_id);
        perf_event_read_core_latency(cpu_id);
    }
}

void Monitor::perf_event_setup_process_latency(int pid) {
    auto latinfo = LatencyInfoPerProcess(pid);
    lat_info_process_[pid] = latinfo;

    int fd = perf_event_setup(pid, -1, -1, PERF_TYPE_RAW, EVENT_L1D_PEND_MISS_PENDING);
    lat_info_process_[pid].fd_l1d_pend_miss = fd;
    perf_event_reset(fd);

    fd = perf_event_setup(pid, -1, -1, PERF_TYPE_RAW, EVENT_MEM_LOAD_RETIRED_L3_MISS);
    lat_info_process_[pid].fd_retired_l3_miss = fd;
    perf_event_reset(fd);
}

void Monitor::perf_event_enable_process_latency(int pid) {
    perf_event_enable(lat_info_process_[pid].fd_l1d_pend_miss);
    perf_event_enable(lat_info_process_[pid].fd_retired_l3_miss);
}

void Monitor::perf_event_disable_process_latency(int pid) {
    perf_event_disable(lat_info_process_[pid].fd_l1d_pend_miss);
    perf_event_disable(lat_info_process_[pid].fd_retired_l3_miss);
}

void Monitor::perf_event_read_process_latency(int pid, bool log_latency, ApplicationInfo *app_info) {
    uint64_t count_l1d_pend_miss = 0, count_retired_l3_miss = 0;
    read(lat_info_process_[pid].fd_l1d_pend_miss, &count_l1d_pend_miss, sizeof(count_l1d_pend_miss));
    read(lat_info_process_[pid].fd_retired_l3_miss, &count_retired_l3_miss, sizeof(count_retired_l3_miss));
    double latency_cycles = (double) (count_l1d_pend_miss - lat_info_process_[pid].curr_count_l1d_pend_miss)
        / (count_retired_l3_miss - lat_info_process_[pid].curr_count_retired_l3_miss);
    lat_info_process_[pid].curr_count_l1d_pend_miss = count_l1d_pend_miss;
    lat_info_process_[pid].curr_count_retired_l3_miss = count_retired_l3_miss;
    double latency_ns = latency_cycles / PROCESSOR_GHZ;
    if (log_latency) {
        sampled_process_lat_.push_back(latency_ns);
    }
    if (app_info) {
        std::cout << "App (\"" << app_info->name <<  "\"): latency = " << latency_ns << " ns" << std::endl;
    } else {
        std::cout << "process [" << pid << "]: latency = " << latency_ns << " ns" << std::endl;
    }
}

void Monitor::measure_process_latency(int pid) {
    perf_event_setup_process_latency(pid);

    for (;;) {
        perf_event_enable_process_latency(pid);

        sleep_ms(sampling_period_ms_);

        perf_event_disable_process_latency(pid);
        perf_event_read_process_latency(pid);
    }
}

void Monitor::measure_process_latency(std::string proc_name) {
    // get pid first via process name
    int pid = -1;
    bool found_pid = false;
    while (!found_pid) {
        pid = get_pid_from_proc_name(proc_name);
        if (pid != -1) {
            std::cout << "Start measuring latency for " << proc_name << "(" << pid << ") ..." << std::endl;
            found_pid = true;
        }
    }

    // measure latency given the pid; always check if the process still exists
    bool process_exists = true;

    perf_event_setup_process_latency(pid);

    while (process_exists) {
        perf_event_enable_process_latency(pid);

        sleep_ms(sampling_period_ms_);

        perf_event_disable_process_latency(pid);
        perf_event_read_process_latency(pid, true);

        if (get_pid_from_proc_name(proc_name) == -1) {
            process_exists = false;
        }
    }

    if (!sampled_process_lat_.empty()) {
        double sum_lat = 0;
        std::cout << "sampled latency = [";
        int i = 0;
        for (; i < sampled_process_lat_.size() - 1; i++) {
            std::cout << int(sampled_process_lat_[i]) << ",";
            sum_lat += sampled_process_lat_[i];
        }
        std::cout << int(sampled_process_lat_[i]) << "]" << std::endl;
        sum_lat += sampled_process_lat_[i];
        std::sort(sampled_process_lat_.begin() + sampled_process_lat_.size() * 0.1, sampled_process_lat_.begin() + sampled_process_lat_.size() * 0.9);
        double avg_lat = sum_lat / sampled_process_lat_.size();
        int medium = sampled_process_lat_[sampled_process_lat_.size() * 0.5];
        std::cout << "avg sampled latency = " << avg_lat << std::endl;
        std::cout << "Medium Sampled Latency = " << medium << std::endl;
    }

    std::cout << proc_name << " no longer exists. Stop measuring." << std::endl;
}

void Monitor::measure_application_latency() {
    for (const auto &[pid, app] : application_info_) {
        perf_event_setup_process_latency(pid);
    }

    for (;;) {
        for (const auto &[pid, app] : application_info_) {
            if (app->process_exists) {
                perf_event_enable_process_latency(pid);
            }
        }

        sleep_ms(sampling_period_ms_);

        for (const auto &[pid, app] : application_info_) {
            if (app->process_exists) {
                perf_event_disable_process_latency(pid);
                perf_event_read_process_latency(pid, false, app);
            }
        }

        for (const auto &[pid, app] : application_info_) {
            if (get_pid_from_proc_name(app->name) == -1) {
                app->process_exists = false;
            }
            //} else {
            //    app->process_exists = true;     // shall we do this?
            //}
        }
    }
}

// opcode - 0: read, 1: write, 2: both
void Monitor::perf_event_setup_uncore_mem_bw(int opcode) {
    if (opcode == 0) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                int fd = perf_event_setup(-1, i, -1, pmu_imc_type_[j], EVENT_CAS_COUNT_RD);
                fd_cas_rd_[i].push_back(fd);
                perf_event_reset(fd);
            }
        }
    } else if (opcode == 1) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                int fd = perf_event_setup(-1, i, -1, pmu_imc_type_[j], EVENT_CAS_COUNT_WR);
                fd_cas_wr_[i].push_back(fd);
                perf_event_reset(fd);
            }
        }
    } else if (opcode == 2) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                int fd = perf_event_setup(-1, i, -1, pmu_imc_type_[j], EVENT_CAS_COUNT_RD);
                fd_cas_rd_[i].push_back(fd);
                perf_event_reset(fd);
                fd = perf_event_setup(-1, i, -1, pmu_imc_type_[j], EVENT_CAS_COUNT_WR);
                fd_cas_wr_[i].push_back(fd);
                perf_event_reset(fd);
            }
        }
    } else {
        assert(false);
    }
}

// opcode - 0: read, 1: write, 2: both
void Monitor::perf_event_enable_uncore_mem_bw(int opcode) {
    if (opcode == 0) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_enable(fd_cas_rd_[i][j]);
            }
        }
    } else if (opcode == 1) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_enable(fd_cas_wr_[i][j]);
            }
        }
    } else if (opcode == 2) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_enable(fd_cas_rd_[i][j]);
                perf_event_enable(fd_cas_wr_[i][j]);
            }
        }
    } else {
        assert(false);
    }
}

// opcode - 0: read, 1: write, 2: both
void Monitor::perf_event_disable_uncore_mem_bw(int opcode) {
    if (opcode == 0) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_disable(fd_cas_rd_[i][j]);
            }
        }
    } else if (opcode == 1) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_disable(fd_cas_wr_[i][j]);
            }
        }
    } else if (opcode == 2) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                perf_event_disable(fd_cas_rd_[i][j]);
                perf_event_disable(fd_cas_wr_[i][j]);
            }
        }
    } else {
        assert(false);
    }
}

// opcode - 0: read, 1: write, 2: both
void Monitor::perf_event_read_uncore_mem_bw(int opcode, double elapsed_ms) {
    if (opcode == 0) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                uint64_t count_rd = 0;     // TODO: need to construct a custom struct when we specify more read format later
                read(fd_cas_rd_[i][j], &count_rd, sizeof(count_rd));
                double curr_bw_rd = (count_rd - curr_count_rd_[i][j]) * 64 / 1024 / 1024 / (elapsed_ms / 1000);     // in MBps
                curr_count_rd_[i][j] = count_rd;
                bw_read_[i][j] = ewma_alpha_ * curr_bw_rd + (1 - ewma_alpha_) * bw_read_[i][j];
                std::cout << "socket[" << i << "]: imc" << j << "  \t BW_read = " << bw_read_[i][j] << " MBps" << std::endl;
            }
        }
        std::cout << std::endl;
    } else if (opcode == 1) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                uint64_t count_wr = 0;     // TODO: need to construct a custom struct when we specify more read format later
                read(fd_cas_wr_[i][j], &count_wr, sizeof(count_wr));
                double curr_bw_wr = (count_wr - curr_count_wr_[i][j]) * 64 / 1024 / 1024 / (elapsed_ms / 1000);     // in MBps
                curr_count_wr_[i][j] = count_wr;
                bw_write_[i][j] = ewma_alpha_ * curr_bw_wr + (1 - ewma_alpha_) * bw_write_[i][j];

                std::cout << "socket[" << i << "]: imc" << j << "  \t BW_write = " << bw_write_[i][j] << " MBps" << std::endl;
            }
        }
        std::cout << std::endl;
    } else if (opcode == 2) {
        for (int i = 0; i < num_sockets_; i++) {
            for (int j = 0; j < pmu_imc_type_.size(); j++) {
                uint64_t count_rd = 0, count_wr = 0, count_all = 0;     // TODO: need to construct a custom struct when we specify more read format later
                read(fd_cas_rd_[i][j], &count_rd, sizeof(count_rd));
                double curr_bw_rd = (count_rd - curr_count_rd_[i][j]) * 64 / 1024 / 1024 / (elapsed_ms / 1000);     // in MBps
                curr_count_rd_[i][j] = count_rd;
                bw_read_[i][j] = ewma_alpha_ * curr_bw_rd + (1 - ewma_alpha_) * bw_read_[i][j];

                read(fd_cas_wr_[i][j], &count_wr, sizeof(count_wr));
                double curr_bw_wr = (count_wr - curr_count_wr_[i][j]) * 64 / 1024 / 1024 / (elapsed_ms / 1000);     // in MBps
                curr_count_wr_[i][j] = count_wr;
                bw_write_[i][j] = ewma_alpha_ * curr_bw_wr + (1 - ewma_alpha_) * bw_write_[i][j];

                std::cout << "socket[" << i << "]: imc" << j << "  \t BW_read = " << bw_read_[i][j]
                    << "MBps, BW_write = " << bw_write_[i][j] << "MBps, BW_all = " << bw_read_[i][j] + bw_write_[i][j] << " MBps" << std::endl;
            }
        }
        std::cout << std::endl;
    } else {
        assert(false);
    }
}

// opcode - 0: read, 1: write, 2: both
void Monitor::measure_uncore_bandwidth(int opcode) {
    // setup fd for each event on each PMU per socket
    perf_event_setup_uncore_mem_bw(opcode);

    // monitor all PMUs
    for (;;) {
        perf_event_enable_uncore_mem_bw(opcode);

        double elapsed = sleep_ms(sampling_period_ms_);

        perf_event_disable_uncore_mem_bw(opcode);
        perf_event_read_uncore_mem_bw(opcode, elapsed);
    }
}

void Monitor::measure_uncore_bandwidth_read() {
    measure_uncore_bandwidth(0);
}

void Monitor::measure_uncore_bandwidth_write() {
    measure_uncore_bandwidth(1);
}

void Monitor::measure_uncore_bandwidth_all() {
    measure_uncore_bandwidth(2);
}

void Monitor::perf_event_setup_offcore_mem_bw(int cpu_id) {
    int fd = perf_event_setup(-1, cpu_id, -1, PERF_TYPE_RAW, EVENT_OFFCORE_REQUESTS_ALL_REQUESTS);
    bw_info_cpu_[cpu_id].fd_offcore_all_reqs = fd;
    perf_event_reset(fd);
}

//void Monitor::perf_event_setup_offcore_mem_bw_l3_load(int cpu_id) {
//    int fd = perf_event_setup(-1, cpu_id, -1, PERF_TYPE_RAW, EVENT_OFFCORE_REQUESTS_L3_MISS_DEMAND_DATA_RD);
//    bw_info_cpu_[cpu_id].fd_offcore_all_reqs = fd;
//    perf_event_reset(fd);
//}

void Monitor::perf_event_enable_offcore_mem_bw(int cpu_id) {
    perf_event_enable(bw_info_cpu_[cpu_id].fd_offcore_all_reqs);
}

void Monitor::perf_event_disable_offcore_mem_bw(int cpu_id) {
    perf_event_disable(bw_info_cpu_[cpu_id].fd_offcore_all_reqs);
}

void Monitor::perf_event_read_offcore_mem_bw(int cpu_id, double elapsed_ms) {
    uint64_t count_bw = 0;     // TODO: need to construct a custom struct when we specify more read format later
    BWInfoPerCore *info = &bw_info_cpu_[cpu_id];
    read(info->fd_offcore_all_reqs, &count_bw, sizeof(count_bw));
    double curr_bw = (count_bw - info->curr_count_offcore_all_reqs) * 64 / 1000 / 1000 / (elapsed_ms / 1000);     // in MBps
    info->curr_count_offcore_all_reqs = count_bw;
    info->curr_bw = ewma_alpha_ * curr_bw + (1 - ewma_alpha_) * info->curr_bw;
    std::cout << "cpu[" << cpu_id << "]: memory BW = " << info->curr_bw << " MBps" << std::endl;
}

//TODO: clear some counters values that are too small to keep the readings clean.
// This is the total bandwidth
void Monitor::measure_offcore_bandwidth(const std::vector<int> &cores) {
    for (const auto &c : cores) {
        perf_event_setup_offcore_mem_bw(c);
    }

    for (;;) {
        for (const auto &c: cores) {
            perf_event_enable_offcore_mem_bw(c);
        }

        double elapsed = sleep_ms(sampling_period_ms_);

        for (const auto &c: cores) {
            perf_event_disable_offcore_mem_bw(c);
        }

        double total_bw = 0;
        for (const auto &c: cores) {
            perf_event_read_offcore_mem_bw(c, elapsed);
            total_bw += bw_info_cpu_[c].curr_bw;
        }
        //std::cout << "TOTAL memory BW: " << total_bw << " MBps" << std::endl;
        std::cout << "TOTAL memory BW: " << total_bw / 1000 << " GBps" << std::endl;
    }
}

void Monitor::measure_total_bandwidth_per_socket() {
    for (int i = 0; i < NUM_CORES; i++) {
        perf_event_setup_offcore_mem_bw(i);
        //perf_event_setup_offcore_mem_bw_l3_load(i);
    }

    for (;;) {
        for (int i = 0; i < NUM_CORES; i++) {
            perf_event_enable_offcore_mem_bw(i);
        }

        double elapsed = sleep_ms(sampling_period_ms_);

        for (int i = 0; i < NUM_CORES; i++) {
            perf_event_disable_offcore_mem_bw(i);
        }

        std::vector<double> total_bw_per_socket = std::vector<double>(NUM_SOCKETS, 0);
        for (int i = 0; i < NUM_CORES; i++) {
            perf_event_read_offcore_mem_bw(i, elapsed);
            total_bw_per_socket[i % NUM_SOCKETS] += bw_info_cpu_[i].curr_bw;
        }
        double total_bw = 0;
        for (int i = 0; i < NUM_SOCKETS; i++) {
            //std::cout << "Total memory BW on Socket " << i << ": " << total_bw_per_socket[i] << " MBps" << std::endl;
            std::cout << "Total memory BW on Socket " << i << ": " << total_bw_per_socket[i] / 1000 << " GBps" << std::endl;
            total_bw += total_bw_per_socket[i];
        }
        //std::cout << "Total memory BW all sockets: " << total_bw << " MBps" << std::endl << std::endl;;
        std::cout << "Total memory BW all sockets: " << total_bw / 1000 << " GBps" << std::endl << std::endl;;
    }
}

void Monitor::measure_application_bandwidth() {
    for (const auto &c : bw_core_list_) {
        perf_event_setup_offcore_mem_bw(c);
    }

    for (;;) {
        for (const auto &c: bw_core_list_) {
            perf_event_enable_offcore_mem_bw(c);
        }

        double elapsed = sleep_ms(sampling_period_ms_);

        for (const auto &c: bw_core_list_) {
            perf_event_disable_offcore_mem_bw(c);
        }

        for (const auto &[pid, app] : application_info_) {
            double total_bw = 0;
            for (const auto &c : app->bw_cores) {
                perf_event_read_offcore_mem_bw(c, elapsed);
                total_bw += bw_info_cpu_[c].curr_bw;
            }
            std::cout << "App (\"" << app->name <<  "\") BW: " << total_bw << " MBps" << std::endl << std::endl;
        }
    }
}

int Monitor::perf_event_setup_pebs(int pid, int cpu, int group_fd, uint32_t type, uint64_t event_id) {
    struct perf_event_attr event_attr;
    memset(&event_attr, 0, sizeof(event_attr));
    event_attr.type = type;
    event_attr.size = sizeof(event_attr);
    event_attr.config = event_id;
    event_attr.sample_period = sampling_period_event_;
    event_attr.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU;
    event_attr.disabled = 1;
    event_attr.exclude_kernel = 1;      // TODO: add this for other core and offcore event
    //event_attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    event_attr.precise_ip = 2;

    int ret = perf_event_open(&event_attr, pid, cpu, group_fd, 0);
    if (ret < 0) {
        std::cout << "[Error] perf_event_open: " << strerror(errno) << std::endl;
    }
    return ret;
}

// allocate metadata page for a sampled event
struct perf_event_mmap_page *Monitor::perf_event_setup_mmap_page(int fd) {
    struct perf_event_mmap_page *m_page = (struct perf_event_mmap_page *) mmap(NULL,
        PAGE_SIZE * (NUM_PERF_EVENT_MMAP_PAGES + 1), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    return m_page;
}

void Monitor::perf_event_setup_page_temp(const std::vector<int> &cores) {
    for (const auto &c : cores) {
        for (int i = 0; i < page_temp_events_.size(); i++) {
            int fd = -1;
            struct perf_event_mmap_page *m_page = NULL;
            fd = perf_event_setup_pebs(-1, c, -1, PERF_TYPE_RAW, page_temp_events_[i]);
            m_page = perf_event_setup_mmap_page(fd);
            page_temp_info_[c].fds[i] = fd;
            page_temp_info_[c].perf_m_pages[i] = m_page;
            perf_event_reset(fd);
        }
    }
}

void Monitor::perf_event_enable_page_temp(const std::vector<int> &cores) {
    for (const auto &c : cores) {
        for (int i = 0; i < page_temp_events_.size(); i++) {
            perf_event_enable(page_temp_info_[c].fds[i]);
        }
    }
}

void Monitor::sample_page_access(const std::vector<int> &cores) {
    for (;;) {
    ////int n = 20000;
    ////while (n > 0) {          // for quick test purposes
        for (const auto &c : cores) {
            for (int i = 0; i < page_temp_events_.size(); i++) {
                struct perf_event_mmap_page *p = page_temp_info_[c].perf_m_pages[i];
                uint64_t data_head = p->data_head;
                uint64_t data_tail = p->data_tail;
                uint64_t data_offset = p->data_offset;
                uint64_t data_size = p->data_size;

                if (data_head == data_tail) {
                    continue;   // wait for more samples to read
                }

                __sync_synchronize();

                PerfSample *sample = (PerfSample *)((char *)p + data_offset + (data_tail % data_size));     // need manual wrapping for head & tail
                
                // TODO: count throttle and untrottle events if CPU throttling becomes an issue in the future
                // For now, ignore all events except PERF_RECORD_SAMPLE
                if (sample->header.type == PERF_RECORD_SAMPLE) {
                    uint64_t page_addr = sample->addr & PAGE_MASK;
                    page_access_map_[page_addr]++;
                } else if (sample->header.type == PERF_RECORD_THROTTLE) {
                    num_cpu_throttle_++;
                } else if (sample->header.type == PERF_RECORD_UNTHROTTLE) {
                    num_cpu_unthrottle_++;
                }
                p->data_tail += sample->header.size;    // manually update data tail
                if (i == 0) {           // assuming EVENT_MEM_LOAD_L3_MISS_RETIRED_LOCAL_DRAM
                    num_local_access_++;
                } else if (i == 1) {    // assuming EVENT_MEM_LOAD_L3_MISS_RETIRED_REMOTE_DRAM
                    num_remote_access_++;
                }
                //std::cout << "page_access_map_.size() = " << page_access_map_.size() << std::endl;
                ////n -= 1;
            }
        }
    }
}

// currently used for test purposes
// might extend to a separate long-runing thread in the future as an actual design component
void Monitor::measure_hot_page_pctg(const std::vector<int> &cores) {
    if (page_access_map_.size() == 0) {
        return;
    }
    std::vector<std::pair<uint64_t, uint64_t>> temp_vec(page_access_map_.begin(), page_access_map_.end());
    std::sort(temp_vec.begin(), temp_vec.end(), [](auto &left, auto &right) {
        return left.second < right.second;
    });
    
    int num_percentile = 100;
    int step = temp_vec.size() / num_percentile;
    std::cout << "hot page access pdf (" << num_percentile << " pctl):" << std::endl;
    std::cout << "[";
    for (int i = 0; i < num_percentile; i++) {
        uint64_t num_acc = temp_vec[step * i].second;
        std::cout << num_acc;
        if (i < num_percentile - 1) {
            std::cout << ",";
        } else {
            std::cout << "]" << std::endl;
        }
        //std::cout << i << "th: \t" << num_acc << std::endl;
    }
    std::cout << "num cpu throttle: " << num_cpu_throttle_ << "; num cpu untrottle: " << num_cpu_unthrottle_ << std::endl;

    double remote_access_ratio = (double) num_remote_access_ / (num_local_access_ + num_remote_access_);
    std::cout << "remote access ratio = " << remote_access_ratio << std::endl;

    //std::multimap<uint64_t, uint64_t> sorted_map = flip_map(page_access_map_);
    //for (const auto &[num_acc, addr] : sorted_map) {
    //}
}

void Monitor::measure_page_temp(const std::vector<int> &cores) {
    perf_event_setup_page_temp(cores);

    perf_event_enable_page_temp(cores);

    sample_page_access(cores);
    
}

// for test purposes
void signal_handler(int s) {
    //std::cout << "receive signal " << s << std::endl;
    monitor.measure_hot_page_pctg(cores_g);
    exit(1);
}

int main (int argc, char *argv[]) {
    ////Monitor monitor = Monitor();        // moved to global to make signal handler work
    ////std::vector<int> cores = {0};       // moved to global to make signal handler work

    for (int i = 0; i < NUM_CORES; i++) {
        cores_g.push_back(i);
    }

    //// for easy test purposes
    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = signal_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction(SIGINT, &sigIntHandler, NULL);
    ////

    //monitor.measure_uncore_latency();
    //monitor.measure_uncore_bandwidth_all();
    //monitor.measure_core_latency(1);
    //int pid = atoi(argv[1]);
    //monitor.measure_process_latency(pid);

    //monitor.measure_offcore_bandwidth(cores_g);
    //monitor.measure_total_bandwidth_per_socket();

    /*
    ApplicationInfo *app_info_1 = new ApplicationInfo();
    app_info_1->pid = 1;
    app_info_1->name = "local thread";
    ////app_info_1->bw_cores = {1,3,5,7,9,11,13,15};
    app_info_1->bw_cores = {1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31};
    ////app_info_1->bw_cores = {1,3,5,7,9,11,13,15,49,51,53,55,57,59,61,63};
    //app_info_1->bw_cores = {1,3,5,7,9,11,13,15,33,35,37,39,41,43,45,47};
    monitor.add_application(app_info_1);

    ApplicationInfo *app_info_2 = new ApplicationInfo();
    app_info_2->pid = 2;
    app_info_2->name = "remote threads";
    //app_info_2->bw_cores = {17,19,21,23,25,27,29,31};
    //app_info_2->bw_cores = {33,35,37,39,41,43,45,47};
    app_info_2->bw_cores = {33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63};
    monitor.add_application(app_info_2);

    monitor.measure_application_bandwidth();
    */

    //monitor.measure_page_temp(cores_g);

    //monitor.measure_process_latency("memtier_benchmark");
     monitor.measure_process_latency("redis-server");
    //monitor.measure_process_latency("bc");
    //monitor.measure_process_latency("memcached");

    std::set<int> node1_cores = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61};
    //monitor.measure_cores_latency(node1_cores);

    //ApplicationInfo *app_info_1 = new ApplicationInfo("test_page_freq");
    //monitor.add_application(app_info_1);

    //ApplicationInfo *app_info_1 = new ApplicationInfo("test_page_freq_local");
    //ApplicationInfo *app_info_2 = new ApplicationInfo("test_page_freq_remote");
    //monitor.add_application(app_info_1);
    //monitor.add_application(app_info_2);

    //monitor.measure_application_latency();

}
