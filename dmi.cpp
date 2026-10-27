#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <signal.h>
#include <stdlib.h>
#include <fftw3.h>

#include "perf.h"

/*
 * FFT related global variables
 */
// The window length in ms with which we calculate the FFT of BW utilization
constexpr int FFT_WINDOW_LEN = 90000;
constexpr int MAX_ADJUST_INTERVAL = 30000;

constexpr int BW_PERCENTILE = 100;
constexpr int MAX_STEP = 8;
constexpr int MIN_STEP = 2;
constexpr int MIN_THROTTLE_STEP = MAX_STEP / 2;
constexpr int THROTTLE_THRESHOLD = 90;

// Related to bandwidth sampling
std::queue<int64_t> bw_sample_queue;
std::mutex bw_sample_mutex;
std::condition_variable bw_sample_cv;

std::chrono::high_resolution_clock::time_point start_time;
std::atomic<bool> continue_running(true);

void signal_handler(int s)
{
    continue_running = false;
}

#ifdef TOTALBW
uint64_t cur_max_events = 0;
uint64_t cur_max_loc_events = 0;
uint64_t cur_max_rem_events = 0;
#endif
int64_t get_bw(int sample_int, std::vector<std::vector<int>> &rd_fds, std::vector<std::vector<int>> wr_fds)
{
    uint64_t count;
    int64_t rd_bw, wr_bw;
    uint64_t rd_count = 0;
    uint64_t wr_count = 0;
#ifdef TOTALBW
    uint64_t loc_count = 0;
    uint64_t rem_count = 0;
#endif

    for (uint64_t i = 0; i < rd_fds.size(); i++) {
        apply_ioctl(PERF_EVENT_IOC_RESET, rd_fds[i]);
        apply_ioctl(PERF_EVENT_IOC_RESET, wr_fds[i]);
        apply_ioctl(PERF_EVENT_IOC_ENABLE, rd_fds[i]);
        apply_ioctl(PERF_EVENT_IOC_ENABLE, wr_fds[i]);
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(sample_int));

    for (uint64_t i = 0; i < rd_fds.size(); i++) {
        apply_ioctl(PERF_EVENT_IOC_DISABLE, rd_fds[i]);
        apply_ioctl(PERF_EVENT_IOC_DISABLE, wr_fds[i]);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    for (uint64_t i = 0; i < rd_fds.size(); i++) {
        uint64_t node_counts = 0;
        for (int fd : rd_fds[i]) {
            read(fd, &count, sizeof(count));

            node_counts += count;
            rd_count += count;
        }
        for (int fd : wr_fds[i]) {
            read(fd, &count, sizeof(count));

            node_counts += count;
            wr_count += count;
        }

#ifdef TOTALBW
        if (i == 0)
	    loc_count = node_counts;
        else
	    rem_count = node_counts;
#endif
    }

#ifdef TOTALBW
    if (cur_max_events < rd_count + wr_count) {
        cur_max_events = rd_count + wr_count;
        cur_max_loc_events = loc_count;
        cur_max_rem_events = rem_count;
    }
#endif

    // bw in MB/s = (bytes read / 1,000,000) / (time in seconds)
    //            = (bytes read / 1,000,000) / (time in us / 1,000,000)
    //            = (bytes read) / (time in us)
    rd_bw = (rd_count * 64) / duration.count();
    wr_bw = (wr_count * 64) / duration.count();

    return rd_bw + wr_bw;
}

void bw_sample_thread(int sample_int, std::vector<std::vector<int>> &rd_fds, std::vector<std::vector<int>> wr_fds)
{
    while (true) {
        int64_t bw = get_bw(sample_int, rd_fds, wr_fds);
        std::lock_guard<std::mutex> lock(bw_sample_mutex);
        bw_sample_queue.push(bw);
        bw_sample_cv.notify_one();
    }
}

/*
 * If we try to send a DAMON command while some other process is doing the
 * same, we might get a busy error and the command will fail. This retry
 * function helps avoid that.
 */
void try_system_cmd(std::string cmd, int tries, int delay_ms)
{
    int ret;

    for (int i = 0; i < tries; i++) {
        ret = system(cmd.c_str());
        if (ret == 0)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

int adjust_interleave_ratio(std::list<int64_t> &bw_history, int adjust_interval, int ratio, int64_t bw_cutoff, bool weighted_interleave)
{
    const std::string wi_path = "/sys/kernel/mm/mempolicy/weighted_interleave/";
    const std::string damon_path = "/sys/kernel/mm/damon/admin/kdamonds/0/";
    const std::string damon_dest_path = damon_path + "contexts/0/schemes/0/dests/";
    static int64_t last_bw = 1;
    static int last_ratio = 100;
    static int last_step = -MAX_STEP * 2;
    int64_t cur_bw;
    int nth_percentile_index;
    int bw_change, interleave_change;
    int cur_step;
    bool good_step;
    bool big_bw_change;
    bool touchup_update = false;
    std::stringstream shell_cmd;
    std::multiset<uint64_t> sorted_bw;
#ifdef TOTALBW
    static int ratio_mismatch_count = 0;
    int bw_ratio;
#endif

    // Sort the bandwidths to get the Nth percentile
    int i = 0;
    for (auto it = bw_history.rbegin(); it != bw_history.rend() && i < adjust_interval; it++, i++)
        sorted_bw.insert(*it);
    nth_percentile_index = (sorted_bw.size() * BW_PERCENTILE / 100) - 1;
    cur_bw = *std::next(sorted_bw.begin(), nth_percentile_index);
    if (cur_bw == 0)
        cur_bw = 1;

    // Calculate the relative change in BW and interleave ratio
    if (last_bw == 0)
        last_bw = cur_bw;
    if (last_ratio == 0)
        last_ratio = 1;
    // Multiple by 10000 instead of 100 to get more resolution
    bw_change = (10000 * (last_bw - cur_bw)) / last_bw;
    interleave_change = last_step * -100;
    big_bw_change = (interleave_change != 0) && (std::abs(bw_change) > 5 * std::abs(interleave_change));


#ifdef TOTALBW
    // If the measured BW ratio is not what we expect, wait
    bw_ratio = (cur_max_loc_events * 100) / cur_max_events;
    if (abs(bw_ratio - ratio) > 4 && cur_bw >= bw_cutoff) {
        std::cout << "Expected BW ratio: " << ratio << " Got: " << bw_ratio << " Cur BW: " << cur_bw << std::endl;
        ratio_mismatch_count++;
        if (ratio_mismatch_count >= 2 && abs(bw_ratio - ratio) > 4 * MAX_STEP) {
            std::cout << "Ratio mismatch count exceeded, adjusting ratio anyway" << std::endl;
            cur_step = (ratio - bw_ratio > 0) ? MAX_STEP : -MAX_STEP;
            ratio = bw_ratio + cur_step;
            ratio_mismatch_count = 0;
            goto commit_ratio;
        } else {
            return ratio;
        }
    } else {
        ratio_mismatch_count = 0;
    }
#endif

#if defined(TOTALBW)
    // When measuring the total bandwidth, whether or not a step is good or not is simple:
    // does the bandwidth go up?
    good_step = cur_bw > last_bw;
#elif defined(LOCALBW)
    // Step is good is the change is bw is less than the change in interleave ratio
    // However, if the bw change is positive (bw going down) when the interleave change
    // is negative (more in local), we probably want to stay in the same direction
    good_step = (bw_change < interleave_change) || (bw_change > 0 && interleave_change < 0);
#endif

    // Adjust the interleave ratio
    if (cur_bw < bw_cutoff) {
        // The bandwidth is clearly unsaturated, so increase the local ratio
        if (last_ratio == 100) {
            cur_step = 0;
        } else if (last_step == 0 && bw_change > 0) {
            cur_step = (ratio * (bw_change / 100)) / 100;
            if (abs(cur_step) < MIN_STEP)
                cur_step = MIN_STEP;
            else if (abs(cur_step) > MAX_STEP / 2)
                cur_step = (cur_step > 0) ? MAX_STEP / 2 : -MAX_STEP / 2;
        } else if (last_step <= 0) {
            cur_step = std::max(abs(last_step) / 2, MIN_STEP);
        } else {
            cur_step = last_step;
        }
    } else if (last_step == 0) {
        // If we have stopped moving, see if the bandwidth has changed
        // enough due to application changes to search again.
        // Divide by 100 because bw_change is in houndreths of a percent
        cur_step = (ratio * (bw_change / 100)) / 100;
        if (abs(cur_step) < 4)
            cur_step = 0;
        else if (abs(cur_step) > MAX_STEP)
            cur_step = (cur_step > 0) ? MAX_STEP : -MAX_STEP;
    } else if (last_ratio == 100) {
        // Probe downward to see if we can make use of more bandwidth
        cur_step = -abs(last_step) / 2;
    } else if (good_step) {
        int throttle_step;
        int bw_int_ratio = abs((bw_change * 100) / interleave_change);
#if defined(TOTALBW)
        bool do_throttle = bw_int_ratio < THROTTLE_THRESHOLD;
        throttle_step = bw_int_ratio * last_step / 100;
#else
        bool do_throttle = bw_change > 0 && interleave_change > 0;
        // When only looking at local bandwidth, we want to throttle when
        // we start having noticeable changes in the local BW as we decrease
        // the interleave ratio
        do_throttle = do_throttle && bw_int_ratio > 100 - THROTTLE_THRESHOLD;
        throttle_step = (100 - bw_int_ratio) * last_step / 100;
#endif
        do_throttle = do_throttle && abs(last_step) > MIN_THROTTLE_STEP;

        if (do_throttle) {
            cur_step = throttle_step;
            if (abs(cur_step) < MIN_THROTTLE_STEP)
                cur_step = (last_step > 0) ? MIN_THROTTLE_STEP : -MIN_THROTTLE_STEP;
        } else {
            // The last step was good, keep going
            cur_step = last_step;
        }
    } else {
        // The last step was bad, reverse
        cur_step = -last_step / 2;
    }

    // Make sure the step stays in bounds
    if (big_bw_change && abs(cur_step) < MAX_STEP / 2) {
        cur_step = cur_step < 0 ? -MAX_STEP : MAX_STEP;
    } else if (abs(cur_step) < MIN_STEP) {
        cur_step = 0;
    } else if (abs(cur_step) > MAX_STEP) {
        cur_step = cur_step < 0 ? -MAX_STEP : MAX_STEP;
    }

    // If this is the first step we've gone to a step size
    // of 0, steady ourselves at the better option
#ifdef TOTALBW
    if (cur_step == 0 && last_bw > cur_bw) {
#else
    if (cur_step == 0) {
#endif
        ratio -= last_step;
        touchup_update = last_step != 0;
    } else {
        ratio += cur_step;
    }

// This label is only used in total bandwidth mode
#ifdef TOTALBW
commit_ratio:
#endif
    // If we are in steady state, don't update last_bw so we can detect small
    // changes in BW utilization that build over time
    if (last_step != 0 || cur_step != 0)
        last_bw = cur_bw;

    if (ratio > 100)
        ratio = 100;
    else if (ratio < 0)
        ratio = 0;

    last_step = cur_step;
    last_ratio = ratio;

    // Actually change the ratio
    if (cur_step != 0 || touchup_update) {
        // Change the ratio for static weighted interleaving
        if (weighted_interleave) {
            shell_cmd << "echo " << ratio << " | tee " << wi_path << "node0";
            system(shell_cmd.str().c_str());
            shell_cmd.str(std::string());
            shell_cmd << "echo " << 100 - ratio << " | tee " << wi_path << "node1";
            system(shell_cmd.str().c_str());
            shell_cmd.str(std::string());
        }
        // Change the ratio used by DAMON
        shell_cmd << "echo " << ratio << " | tee " << damon_dest_path << "0/weight";
        system(shell_cmd.str().c_str());
        shell_cmd.str(std::string());
        shell_cmd << "echo " << 100 - ratio << " | tee " << damon_dest_path << "1/weight";
        system(shell_cmd.str().c_str());
        shell_cmd.str(std::string());
        shell_cmd << "echo commit | tee " << damon_path << "state";
        try_system_cmd(shell_cmd.str(), 1000, 500);
        shell_cmd.str(std::string());
        // The below command is designed to update the <damon stuff>/total_bytes
        // file, but we will use it as a way to wait until the new weights have
        // been applied
        shell_cmd << "echo update_schemes_tried_bytes | tee " << damon_path << "state";
        try_system_cmd(shell_cmd.str(), 1000, 500);
    }

    auto cur_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(cur_time - start_time);

    std::cout << "Target ratio: " << ratio << " "
              << "BW Change: " << bw_change << " "
              << "Int Change: " << interleave_change << " "
              << "BW: " << cur_bw << " "
              << "Adj Period: " << adjust_interval << " "
              << "Time: " << duration.count() << "s" << std::endl;

    return ratio;
}

// Convert signal from MB/s to GB/s, remove the DC component, and optionally
// apply a hamming window.
std::vector<double> process_signal(std::list<int64_t> &signal, int window_size, bool hamming_window)
{
    std::vector<double> windowed_signal(window_size);
    double signal_sum = 0;
    double signal_avg;
    int i;

    i = window_size - 1;
    // The most recent bandwidth samples are at the end of the list, so iterate
    // in reverse to fill the signal array.
    for (auto it = signal.rbegin(); it != signal.rend() && i >= 0; it++, i--) {
        // Convert from MB/s to GB/s to limit the magnitudes
        windowed_signal[i] = *it / 1000.0;
        signal_sum +=  windowed_signal[i];
    }
    signal_avg = signal_sum / window_size;
    for (i = 0; i < window_size; i++) {
        // Remove the DC component then apply the hamming window
        windowed_signal[i] -= signal_avg;
        if (hamming_window)
            windowed_signal[i] *= 0.54 - (0.46 * std::cos((2 * M_PI * i) / (window_size - 1)));
    }

    return windowed_signal;
}

double goertzel_magnitude(std::vector<double> &signal, double target_freq)
{
    double s0 = 0;
    double s1 = 0;
    double s2 = 0;
    double coef = 2 * std::cos(2 * M_PI * target_freq);

    for (unsigned long i = 0; i < signal.size(); i++) {
        s0 = signal[i] + (coef * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt((s1 * s1) - (coef * s1 * s2) + (s2 * s2));
}

// Return array of size periods.size() representing the magnitudes of the
// goertzel algorithm for frequencies corresponding to the periods passed
// in the periods vector.
std::unique_ptr<double[]> do_goertzel(std::list<int64_t> &bw_history, std::vector<double> &periods, int window_size)
{
    // Aribitrarily chosen threshold.
    constexpr int HAMMING_WINDOW_THRESH = 64;
    int num_periods = (int)periods.size();
    std::unique_ptr<double[]> magnitudes(new double[num_periods]);
    std::vector<double> signal;
    int i;
    bool use_hamming_window = window_size >= HAMMING_WINDOW_THRESH;

    // Don't apply a hamming window to the signal when the window length is
    // small, since it increases the width of main lobes in the frequency
    // domain, which is a problem because we are looking in between DFT bins.
    signal = process_signal(bw_history, window_size, use_hamming_window);

    for (i = 0; i < num_periods; i++) {
        double target_freq = 1.0 / periods[i];
        magnitudes[i] = goertzel_magnitude(signal, target_freq);
    }
    return magnitudes;
}

// Returns array of size (window_size / 2) + 1 representing the magnitudes of
// the fft.
std::unique_ptr<double[]> do_fft(std::list<int64_t> &bw_history, int window_size)
{
    static int last_window_size = 0;
    static double *fft_in = NULL;
    static fftw_complex *fft_out = NULL;
    static fftw_plan plan = NULL;
    int output_list_size = (window_size / 2) + 1;
    std::unique_ptr<double[]> magnitude_out(new double[output_list_size]);
    std::vector<double> signal;

    if (window_size != last_window_size) {
        if (fft_in)
            fftw_free(fft_in);
        if (fft_out)
            fftw_free(fft_out);
        if (plan)
            fftw_destroy_plan(plan);

        fft_in = fftw_alloc_real(window_size);
        fft_out = fftw_alloc_complex(output_list_size);
        plan = fftw_plan_dft_r2c_1d(window_size, fft_in, fft_out, FFTW_ESTIMATE);

        last_window_size = window_size;
    }

    signal = process_signal(bw_history, window_size, true);
    for (int i = 0; i < window_size; i++)
        fft_in[i] = signal[i];

    fftw_execute(plan);
    // Get the magnitudes from the complex FFT outputs
    for (int i = 0; i < output_list_size; i++)
        magnitude_out[i] = std::sqrt((fft_out[i][0] * fft_out[i][0]) + (fft_out[i][1] * fft_out[i][1]));

    return magnitude_out;
}

int choose_adjust_interval(std::list<int64_t> &bw_history, int cur_int, int min_int, int max_int)
{
    // We may not just want the max magnitude, but the lowest frequency whose
    // magnitude is within some factor of the magnitude. This is that factor
    constexpr double MAGNITUDE_FACTOR = 5.0 / 4.0;
    constexpr double MIN_MAGNITUDE = 5;
    std::vector<double> periods;
    std::unique_ptr<double[]> magnitudes;
    int dominant_index;
    int output_list_size;
    int cur_int_index;
    int max_int_index;
    int new_int;
    int goertzel_step = 1;
    double max_magnitude = 0;

    // Choosing the new adjustment interval is composed of two stages.
    // The first is computing the FFT of the bandwidth measurements only since
    // the last adjustment, since older measurements may be from a different
    // interleave ratio, throwing of the DFT. However, due to the number of
    // samples, this is only useful for determining if the adjustment interval
    // can be shortened.
    // If the dominant period is the same as the current adjustment interval,
    // we might have to increase the adjustment interval, which leads to the
    // second stage. In the second stage, we look at a larger history (up to
    // FFT_WINDOW_LEN) to get a better picture of the bandwidth trends. Then,
    // we pick the period with the greatnest magnitude between the current
    // period and the max period, not including any spikes at the lowest
    // frequencies. This way, we can pick out lower frequency patterns, without
    // getting trapped by high magnitude low frequency measurements caused by
    // phase changes or our systems changes to the interleave ratio.

    // First stage: look at the bandwidth history since the last adjustment
    // Check the periods corresponding to the FFT bins and the periods in between
    // the first and second FFT bins, with a step size of goertzel_step.
    for (int i = 1; i <= cur_int / 2; i++) {
        periods.push_back(cur_int / (double)i);
        for (int j = (cur_int / i) - goertzel_step; i <= 2 && j > (cur_int / (i + 1)); j -= goertzel_step) {
            periods.push_back(j);
        }
    }
    output_list_size = (int)periods.size();
    magnitudes = do_goertzel(bw_history, periods, cur_int);

    // Start with the dominant period set to the max value instead of 0, so if
    // is only a DC component, we will use the minimum adjustment interval
    dominant_index = output_list_size - 1;
    // First, find the max magnitude.
    std::cout << "[";
    for (int i = 0; i < output_list_size; i++) {
        std::cout << periods[i] << ":" << magnitudes[i] << (i == output_list_size - 1 ? "]\n" : ", ");
        if (magnitudes[i] > max_magnitude) {
            max_magnitude = magnitudes[i];
            dominant_index = i;
        }
    }
    // If the max magnitude is very small, just consider it DC
    if (max_magnitude < MIN_MAGNITUDE)
        return min_int;
    // Now, find the lowest frequency whose magnitude is within
    // MAGNITUDE_FACTOR of the max magnitude.
    for (int i = 0; i < dominant_index; i++) {
        if (magnitudes[i] * MAGNITUDE_FACTOR > max_magnitude) {
            // If the index corresponding to cur_int (i = 0) is within the
            // magnitude factor, we probably don't want to search the broader
            // bandwidth history, so just return the cur_int
            if (i == 0) {
                new_int = cur_int;
                goto clamp_int;
            }
            dominant_index = i;
            break;
        }
    }

    new_int = periods[dominant_index];
    if (new_int < cur_int)
        goto clamp_int;

    std::cout << cur_int << " " << bw_history.size() << std::endl;
    // If the cur interval is the same as the bandwidth history, we don't have
    // enough samples to look at a larger history.
    if ((unsigned long)cur_int == bw_history.size()) {
        new_int = cur_int;
        goto clamp_int;
    }

    // Second stage: look at the larger bandwidth history to see if we should
    // increase the adjustment interval.
    // We don't want to be thrown off by low frequency spectra from our
    // adjustments or changes in phase, so find the max magnitude of the
    // frequencies between the current adjustment interval and the lowest frequency
    // that has a magnitude greater than the frequency before it.
    output_list_size = (bw_history.size() / 2) + 1;
    cur_int_index = std::ceil(((double)bw_history.size()) / cur_int);
    // Not the smallest interval we can use, but the lowest we will check
    // in the second phase.
    max_int_index = bw_history.size() / max_int;
    if (max_int_index < 2)
        max_int_index = 2;
    magnitudes = do_fft(bw_history, bw_history.size());
    max_magnitude = 0;
    dominant_index = cur_int_index;
    std::cout << "[";
    for (int i = 1; i < max_int_index; i++) {
        std::cout << bw_history.size() / i << ":" << magnitudes[i] << (i == cur_int_index ? "]\n" : ", ");
    }
    for (int i = max_int_index; i <= cur_int_index; i++) {
        if (magnitudes[i] > magnitudes[i - 1] * MAGNITUDE_FACTOR) {
            max_magnitude = magnitudes[i];
            dominant_index = i;
            break;
        }
        std::cout << bw_history.size() / i << ":" << magnitudes[i] << (i == cur_int_index ? "]\n" : ", ");
    }
    for (int i = dominant_index; i <= cur_int_index; i++) {
        std::cout << bw_history.size() / i << ":" << magnitudes[i] << (i == cur_int_index ? "]\n" : ", ");
        if (magnitudes[i] > max_magnitude) {
            max_magnitude = magnitudes[i];
            dominant_index = i;
        }
    }
    // The frequency that an index to the FFT corresponds to is (i/N)*f_s, where
    // i is the DFT index, N is the number of samples sent to the DFT, and f_s
    // is the sampling frequency. In this case, we consider f_s to be 1 because
    // we care about frequencies/periods in terms of the bandwidth sampling
    // frequency/period. Thus, the perspective new adjustment interval is
    // computed as (N / i).
    new_int = bw_history.size() / dominant_index;

clamp_int:
    // Add a bit of a buffer to ensure it can see its own frequency next time.
    new_int += std::max(std::ceil(new_int * 0.1), 1.0);
    if (new_int < min_int)
        new_int = min_int;
    else if (new_int > max_int)
        new_int = max_int;

    return new_int;
}

int main(int argc, char *argv[])
{
    std::vector<uint32_t> types;
#if defined(GNR) || defined(SPR)
    std::vector<uint32_t> cxl_types;
#endif
    std::vector<int> cpus;
    std::vector<uint64_t> rd_configs;
    std::vector<uint64_t> wr_configs;
    std::vector<std::vector<int>> rd_fds;
    std::vector<std::vector<int>> wr_fds;
    std::list<int64_t> bw_history;
    std::ofstream bw_out_file("/tmp/dmi_bw_history");
    int sample_interval_ms;
    int bw_read_time_ms;
    int adjust_interval;
    int min_adj_int;
    int max_adj_int;
    int max_fft_window_size;
    int64_t cur_bw;
    int64_t bw_saturation_cutoff;
    bool weighted_interleave = false;
    bool fixed_adjust_interval = false;
    int interleave_ratio = 100;

    if (argc < 4) {
        std::cout << "Usage: " << argv[0] << " <sample int (ms)> <adjust int (ms)> <bw saturation cutoff (MB/s)> [wi]" << std::endl;
        return -1;
    }

    sample_interval_ms = std::stoi(argv[1]);
    if (argv[2][0] == 'f') {
        fixed_adjust_interval = true;
        bw_read_time_ms = std::stoi(&argv[2][1]);
    } else {
        bw_read_time_ms = std::stoi(argv[2]);
    }
    bw_saturation_cutoff = std::stoul(argv[3]);
    if (argc >= 5) {
        weighted_interleave = strcmp(argv[4], "wi") == 0;
    }

    max_fft_window_size = FFT_WINDOW_LEN / sample_interval_ms;
    adjust_interval = bw_read_time_ms / sample_interval_ms;
    min_adj_int = adjust_interval / 2;
    max_adj_int = MAX_ADJUST_INTERVAL / sample_interval_ms;

    std::cout << "Running with " << std::endl
        << "\tSample interval: " << sample_interval_ms << " ms" << std::endl
        << "\tBW Read Time: " << bw_read_time_ms << " ms" << std::endl
        << "\tBandwidth saturation cutoff: " << bw_saturation_cutoff << " MB/s" << std::endl;

    get_perf_uncore_info(types, cpus, rd_configs, wr_configs);

#if defined(TOTALBW)
    rd_fds.resize(cpus.size());
    wr_fds.resize(cpus.size());
    for (long unsigned int i = 0; i < cpus.size(); i++) {
        open_perf_events(cpus[i], types, rd_configs, rd_fds[i]);
        open_perf_events(cpus[i], types, wr_configs, wr_fds[i]);
    }

#if defined(GNR) || defined(SPR)
    // Manually add CXL BW events
    rd_fds.resize(cpus.size() + 1);
    wr_fds.resize(cpus.size() + 1);
    get_perf_cxlcm_types(cxl_types);
    open_perf_events(0, cxl_types, cxl_read_configs, rd_fds[cpus.size()]);
    open_perf_events(0, cxl_types, cxl_write_configs, wr_fds[cpus.size()]);
#endif

#elif defined(LOCALBW)
    rd_fds.resize(1);
    wr_fds.resize(1);
    // What to put in the cpu to read from each socket can be found by reading
    // /sys/devices/uncore_imc_0/cpumask - we only care about socket 0, which
    // is represented by CPU 0
    open_perf_events(0, types, rd_configs, rd_fds[0]);
    open_perf_events(0, types, wr_configs, wr_fds[0]);
#else
    #error "Must define either TOTALBW or LOCALBW"
#endif

    start_time = std::chrono::high_resolution_clock::now();

    signal(SIGINT, signal_handler);
    std::thread bw_thread(bw_sample_thread, sample_interval_ms, std::ref(rd_fds), std::ref(wr_fds));
    int num_samples_made = 0;
    while (continue_running) {
        {
            std::unique_lock<std::mutex> lock(bw_sample_mutex);
            bw_sample_cv.wait(lock, []{ return !bw_sample_queue.empty(); });
            // Multiple samples might be added while we were adjusting the interleave ratio, so add all of them to the history
            while (!bw_sample_queue.empty()) {
                cur_bw = bw_sample_queue.front();
                bw_sample_queue.pop();
                if (cur_bw < bw_saturation_cutoff)
                    cur_bw = bw_saturation_cutoff - 1;
                bw_history.push_back(cur_bw);

                bw_out_file << cur_bw << std::endl;
            }
        }
        while (bw_history.size() > (uint64_t)max_fft_window_size) {
            bw_history.pop_front();
        }
        // Only count the latest sample towards the adjustment interval, since
        // the other samples may have been made while adjusting the interleave ratio.
        num_samples_made++;

        // Have we reached an adjustment interval?
        if (num_samples_made >= adjust_interval) {
            interleave_ratio = adjust_interleave_ratio(bw_history, adjust_interval,
                    interleave_ratio, bw_saturation_cutoff, weighted_interleave);
            if (!fixed_adjust_interval) {
                adjust_interval = choose_adjust_interval(bw_history, adjust_interval,
                        min_adj_int, max_adj_int);
                std::cout << "New adjust period: " << adjust_interval << std::endl;
            }
#ifdef TOTALBW
            cur_max_events = 0;
#endif
            num_samples_made = 0;
        }
    }
}
