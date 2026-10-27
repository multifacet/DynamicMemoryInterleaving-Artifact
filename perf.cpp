#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "perf.h"

#if defined(GNR) || defined(SPR)
// TODO: Don't hard code these.
// These values are found in https://github.com/torvalds/linux/blob/master/tools/perf/pmu-events/arch/x86/sapphirerapids/uncore-cxl.json
// Under: UNC_CXLCM_RxC_AGF_INSERTS.MEM_DATA and UNC_CXLCM_RxC_AGF_INSERTS.MEM_REQ
std::vector<uint64_t> cxl_read_configs = {0x2043};
std::vector<uint64_t> cxl_write_configs = {0x1043};
#endif

int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
    int group_fd, unsigned long flags)
{
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

uint64_t read_perf_event(std::ifstream &file)
{
    std::string input;
    std::string event_str, umask_str;
    size_t start_pos, end_pos;
    uint64_t event;
    uint64_t umask;

    // Format: event=<event>,umask=<umask>
    file >> input;

    // Read the event str
    start_pos = input.find("=") + 1;
    end_pos = input.find(",");
    event_str = input.substr(start_pos, end_pos - start_pos);
    input.erase(0, end_pos + 1);

    // Read the umask string
    start_pos = input.find("=") + 1;
    umask_str = input.substr(start_pos);

    event = stol(event_str, nullptr, 16);
    umask = stol(umask_str, nullptr, 16);

    return (umask << 8) | event;
}

static int get_memory_controller_types(std::vector<uint32_t> &types, std::string base_dir)
{
    uint32_t type;
    int valid_mc;

    // From what I've seen, the numbers can get up to 18. Go to 32 for a bit
    // more safety.
    // A better method would be to do a regex of the directories in /sys/devices
    for (int i = 0; i < 32; i++) {
        std::stringstream type_path;
        std::ifstream type_file;

        type_path << base_dir << i << "/type";
        type_file.open(type_path.str().c_str());

        if (!type_file.is_open())
            continue;

        // The type file is easy to parse - just the decimal value
        type_file >> type;

        types.push_back(type);
        valid_mc = i;
    }

    return valid_mc;
}

static void parse_cpumask_file(std::vector<int> &cpus, std::string cpumask_str)
{
    size_t pos = 0;

    // The format of cpumask is CPU indices delimited by "," or "-"
    while (pos != std::string::npos) {
        pos = cpumask_str.find(",");
        // TODO: Add code to actually parse the range when the - delimiter is used
        if (pos == std::string::npos)
            pos = cpumask_str.find("-");
        std::string token = cpumask_str.substr(0, pos);
        int cpu = std::stoi(token);
        cpus.push_back(cpu);
        // + 1 for the length of the delimiter
        cpumask_str.erase(0, pos + 1);
    }
}


#ifdef ZEN5
void get_perf_uncore_info(std::vector<uint32_t> &types, std::vector<int> &cpus,
    std::vector<uint64_t> &rd_configs, std::vector<uint64_t> &wr_configs)
{
    const std::string BASE_DIR = "/sys/devices/amd_umc_";
    int valid_mc;

    valid_mc = get_memory_controller_types(types, BASE_DIR);

    // Get what "cpu" values correspond to different NUMA nodes
    if (valid_mc != -1) {
        std::stringstream cpumask_path;
        std::ifstream cpumask_file;
        std::string cpumask_str;

        cpumask_path << BASE_DIR << valid_mc << "/cpumask";
        cpumask_file.open(cpumask_path.str().c_str());

        if (!cpumask_file.is_open())
            return;

        cpumask_file >> cpumask_str;
        parse_cpumask_file(cpus, cpumask_str);
    }

    // Config values taking from https://github.com/torvalds/linux/blob/master/tools/perf/pmu-events/arch/x86/amdzen5/memory-controller.json
    // and format data in /sys/devices/amd_umc_<i>/format/
    // umc_cas_cmd.rd
    rd_configs.push_back(0x10a);
    // umc_cas_cmd.wr
    wr_configs.push_back(0x20a);
}
#else // Intel
void get_perf_uncore_info(std::vector<uint32_t> &types, std::vector<int> &cpus,
    std::vector<uint64_t> &rd_configs, std::vector<uint64_t> &wr_configs)
{
    const std::string BASE_DIR = "/sys/devices/uncore_imc_";
    uint64_t rd_config;
    uint64_t wr_config;
    int valid_mc;

    valid_mc = get_memory_controller_types(types, BASE_DIR);

    // Get what "cpu" values correspond to different NUMA nodes
    // and the event codes for rd and write events
    if (valid_mc != -1) {
        std::stringstream cpumask_path;
        std::stringstream read_event_path;
        std::stringstream write_event_path;
        std::ifstream cpumask_file;
        std::ifstream read_event_file;
        std::ifstream write_event_file;
        std::string cpumask_str;

        // All these values should be the same regardless of the memory controller
        // number, so it's fine to use an arbitrary valid one.
        cpumask_path << BASE_DIR << valid_mc << "/cpumask";
        read_event_path << BASE_DIR << valid_mc << "/events/cas_count_read";
        write_event_path << BASE_DIR << valid_mc << "/events/cas_count_write";

        cpumask_file.open(cpumask_path.str().c_str());
        read_event_file.open(read_event_path.str().c_str());
        write_event_file.open(write_event_path.str().c_str());

        if (!cpumask_file.is_open() || !read_event_file.is_open() || !write_event_file.is_open())
            return;

        cpumask_file >> cpumask_str;
        parse_cpumask_file(cpus, cpumask_str);

        rd_config = read_perf_event(read_event_file);
        wr_config = read_perf_event(write_event_file);
        rd_configs.push_back(rd_config);
        wr_configs.push_back(wr_config);
#if defined(GNR) || defined(SPR)
        // Quick hack: In SPR and GNR, there are two channels for reads and writes
        // SCH0 and SCH1. SCH0 is found in the file. The event for SCH1 is just
        // one larger than SCH0
        rd_configs.push_back(rd_config + 1);
        wr_configs.push_back(wr_config + 1);
#endif

    }
}
#endif

#if defined(GNR) || defined(SPR)
void get_perf_cxlcm_types(std::vector<uint32_t> &types)
{
    const std::string BASE_DIR = "/sys/devices/uncore_cxlcm_";

    get_memory_controller_types(types, BASE_DIR);
}
#endif

void open_perf_events(int cpu, std::vector<uint32_t> types,
    std::vector<uint64_t> &configs, std::vector<int> &fds)
{
    int fd;
    struct perf_event_attr pe;

    for (unsigned long i = 0; i < types.size(); i++) {
        memset(&pe, 0, sizeof(pe));
        pe.type = types[i];
        pe.size = sizeof(pe);
        pe.disabled = 1;
        pe.inherit = 1;

        for (unsigned long j = 0; j < configs.size(); j++) {
                pe.config = configs[j];
                fd = perf_event_open(&pe, -1, cpu, -1, 0);
                if (fd == -1) {
                    std::cerr << "Error opening type: " << pe.type << " config: " <<
                        std::hex << pe.config << std::dec << std::endl;
                    continue;
                }

                fds.push_back(fd);
        }
    }
}

void apply_ioctl(int cmd, std::vector<int> fds)
{
    for (int fd : fds)
        ioctl(fd, cmd, 0);
}

int perf_sample_open(pid_t pid, int cpu, int group_fd, uint64_t type, uint64_t config,
        uint64_t config1, uint64_t sample_type, uint64_t sample_period) {
    struct perf_event_attr attr;
    int fd;

    memset(&attr, 0, sizeof(perf_event_attr));

    attr.type = type;
    attr.size = sizeof(perf_event_attr);
    attr.config = config;
    attr.config1 = config1;
    attr.sample_period = sample_period;
    attr.sample_type = sample_type;
    attr.read_format = PERF_FORMAT_ID | PERF_FORMAT_LOST;
    attr.pinned = 0;
    attr.disabled = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.exclude_callchain_kernel = 1;
    attr.exclude_callchain_user = 1;
    attr.precise_ip = 1;
    attr.freq = 1;

    fd = perf_event_open(&attr, pid, cpu, group_fd, 0);
    if (fd == -1) {
        std::cerr << "perf_event_mmap_page: Failed perf_event_open " << errno << 
		" " << cpu << std::endl;
        return fd;
    }

    return fd;
}
