# Dynamic Memory Interleaving
This repository contains the userspace code for [Dynamic Memory Interleaving (DMI)](https://dl.acm.org/doi/abs/10.1145/3814942.3816137).
DMI is a tiered memory system that monitors the bandwidth utilization of the system
and dynamically adjusts the interleave ratio used by opted-in applications to
maximize bandwidth utilization and migrates pages to match that ratio.
The userspace component in this repo is what measures the bandwidth and decides
what interleave ratio to use.
The kernel component utilizes DAMON to migrate hot pages to achieve the interleave
ratio, and also "demotes" cold pages from local memory to remote memory when the
local memory is at capacity.
These kernel components have been integrated into DAMON since Linux kernel 6.17.

This work was a collaboration with Micron.
Work is being done by Micron to port the DMI algorithm into `damo`, the DAMON userspace tool: https://github.com/ravis-opensrc/damo/tree/damo/auto-tier-bw-2026-10-04

## The tools in this repo
This repo contains the main DMI userspace utility and a couple of other useful utilities described below.

* `dmi <Sample Interval (ms)> <BW Read Time (ms)> <BW Saturation Cutoff (MB/s)> [wi]`: The DMI userspace utility. Note that this saves a log of the measured bandwidth to `/tmp/dmi_bw_history`
	* `Sample Interval (ms)`: How often, in ms, to measure the system bandwidth utilization
	* `BW Read Time (ms)`: How long to read the system BW before changing the interleave ratio. Will read (Adjust Interval) / (Sample Interval) samples.
	* `BW Saturation Cutoff (MB/s)`: Consider the system bandwidth to be obviously unsaturated when the measured bandwidth is under the cutoff. This is to prevent the system from being fooled by noise when the bandwidth utilization is low. Half the maximum local bandwidth is usually fine.
	* `wi`: If exactly "`wi`" is passed for this parameter, the DMI utility will update the global weighted interleave weights (at `/sys/kernel/mm/mempolicy/weighted_interleave/`) in addition to the interleave weights used by DAMON.
* `bwmon <Sample Interval (ms)> <out file> <pid>`: Utility that simply reads and outputs the system BW utilization
	* `Sample Interval (ms)`: How often, in ms, to measure system bandwidth utilization
	* `out file` (Optional): If used, save the output to the provided file instead of stdout
	* `pid` (Optional): If used, terminate `bwmon` when the process with the provided PID ends
* `memlat <Remote PFN Start> <Aggregation Interval (ms)> <out file>`: Utility that measures the local and remote access latency for [Colloid](https://dl.acm.org/doi/abs/10.1145/3694715.3695968) and outputs the measurement periodically. Might still work without Colloid loaded, but if it does, it will probably include many warning messages. Only works on Intel CPUs
	* `Remote PFN Start`: The PFN at the beginning of remote memory. Can be found by reading `/proc/zoneinfo`
	* `Aggregation Interval (ms)`: How often, in ms, to compute the average access latency in the period
	* `out file` (Optional): If used, save the output to the provided file instead of stdout

### Compiling
Compiling `dmi` requires having `libfftw3` installed. This can be done in Ubuntu with the following command:
`sudo apt install libfftw3-dev`

To compile run the following:
`make ARCH=<arch> BWMODE=<mode>`
* `ARCH` is the architecture to compile for. The options are
	* `haswell` for Intel Haswell CPUs
	* `gnr` (Default) for Intel Granite Rapids CPUs
	* `spr` for Intel Sapphire Rapids CPUs
	* `zen5` for AMD Zen5 CPUs
* `BWMODE` is the operating mode to use for DMI
	* `totalbw` (Default): Use the total system bandwidth utilization to determine the interleave ratio. When using CXL memory, this is only supported by GNR. When using a remote NUMA node as a proxy for CXL, all architectures should work
	* `localbw`: Only use measurements of the local bandwidth utilization to determine the interleave ratio.

## Running DMI
DMI requires Linux kernel version 6.17 or above with the following configuration options enabled:
* `CONFIG_DAMON`
* `CONFIG_DAMON_VADDR`
* `CONFIG_DAMON_PADDR`
* `CONFIG_DAMON_SYSFS`

To run DMI, take the following steps:
* Start DAMON:
	* Generate the yaml file to configure DAMON, where `pid` is the PID of either the single process you want to control, or the shell that will spawn the processes to control, `damo_path` is the path to the [damo userspace tool](https://github.com/damonitor/damo.git), and `yaml_file` is the file to write the yaml output to. I have included a submodule of the version of `damo` I used for my experiments, but newer versions of `damo` should work as well.

			sudo ./gen_interleave.py -p <pid> -d <damo_path> -o <yaml_file>
	* Start DAMON. With the `include_child_tasks` option, the damo userspace tool will add child tasks of the tracked processes to the list of targets. If you are only tracking a single process, omit the `--include_child_tasks` option:

			sudo <damo_path> start <yaml_file> --include_child_tasks &
	* Optionally pin the DAMON daemon to a core (recommended):

			sudo taskset -cp <core> $(pgrep kdamond)
* Start DMI:

		sudo taskset -c <core> ./dmi <Sample Interval (ms)> <BW Read Time (ms)> <BW Saturation Cutoff (MB/s)>