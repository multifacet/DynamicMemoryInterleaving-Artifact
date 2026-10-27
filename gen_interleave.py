#!/usr/bin/env python3
# Copyright (c) 2023-2025 SK hynix, Inc.
# Copyright (c) 2025-2026 Bijan Tabatabai
# SPDX-License-Identifier: BSD-2-Clause
#
# Derived from gen_migpol.py in HMSDK:
# https://github.com/skhynix/hmsdk/blob/main/tools/gen_migpol.py

import argparse
import json
import os
import subprocess as sp
import sys
import yaml

def parse_argument():
	parser = argparse.ArgumentParser()
	parser.add_argument(
		"-p",
		"--pid",
		dest="pid",
		default=None,
		type=int,
		action="append",
		help="The PIDs of the processes to track.",
		required=True
	)
	parser.add_argument(
		"-o",
		"--output",
		dest="output",
		default=None,
		help="The output yaml file."
	)
	parser.add_argument(
		"-d",
		"--damo_path",
		dest="damo_path",
		default="damo", # Assumes damo is in PATH
		help="Path to the damo utility"
	)

	return parser.parse_args()

def run_command(cmd):
	with sp.Popen(cmd.split(), stdout=sp.PIPE, stderr=sp.PIPE) as p:
		stdout,stderr = p.communicate()
		stdout_lines = stdout.decode(errors="ignore")
		stderr_lines = stderr.decode(errors="ignore")
		if len(stderr_lines) > 0:
			print(stderr_lines)
		return stdout_lines

def interleave_action():
	# node 0->weight 100, node 1->weight 0
	damos_action = "--damos_action migrate_hot 0 100 1 0"
	damos_access_rate = "--damos_access_rate 40% 100%"
	damos_age = "--damos_age 1s max"
	damos_quotas = "--damos_quotas 0s 0G 0s 0 0 0"
	damos_wmark = "--damos_wmarks none 0 0 0 0"
	damos_apply_int = "--damos_apply_interval 1s"
	cmd = (
		f"{damos_action} {damos_access_rate} {damos_age} {damos_quotas} "
		f"{damos_wmark} {damos_apply_int} "
	)

	return cmd

def demote_action():
	damos_action = "--damos_action migrate_cold 1"
	damos_access_rate = "--damos_access_rate 0% 0%"
	damos_age = "--damos_age 5s max"
	damos_quota = "--damos_quotas 1s 10G 0s 0 0 0"
	damos_quota_goal = "--damos_quota_goal node_mem_free_bp 1% 0"
	damos_filter = "--damos_filter reject young"
	damos_apply_int = "--damos_apply_interval 1s"
	cmd = (
		f"{damos_action} {damos_access_rate} {damos_age} {damos_quota} "
		f"{damos_quota_goal} {damos_filter} {damos_apply_int} "
	)

	return cmd


def main():
	args = parse_argument()

	if os.geteuid() != 0:
		print("error: Run as root")
		sys.exit(1)

	damo = args.damo_path

	monitoring_nr_regions_range = "--monitoring_nr_regions_range 1000 100000"
	monitoring_intervals = "--monitoring_intervals 200ms 1s 1s"
	node_jsons = []

	pid = ""
	for p in args.pid:
		pid += f"--target_pid {p} "

	cmd = f"{damo} args damon --format json {pid} --ops vaddr {monitoring_intervals} {monitoring_nr_regions_range} "
	cmd += "--damos_nr_filters 0 1 --damos_nr_quota_goals 0 1 "
	cmd += interleave_action()
	cmd += demote_action()

	json_str = run_command(cmd)
	node_json = json.loads(json_str)

	config = yaml.dump(node_json, default_flow_style=False, sort_keys=False)
	if args.output:
			with open(args.output, "w") as f:
				f.write(config+"\n")
	else:
		print(config)

	return 0

if __name__ == "__main__":
	sys.exit(main())
