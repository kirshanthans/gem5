#
# Copyright (c) 2018 Advanced Micro Devices, Inc.
# All rights reserved.
#
# For use for simulation and test purposes only
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Tuan Ta, Xianwei Zhang
#

import m5
from m5.objects import *
from m5.defines import buildEnv
from m5.util import addToPath
import os, optparse, sys

addToPath('../')

from common import Options
from ruby import Ruby

# Get paths we might need.  It's expected this file is in m5/configs/example.
config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)
m5_root = os.path.dirname(config_root)

parser = optparse.OptionParser()
Options.addNoISAOptions(parser)

# GPU Ruby tester options
parser.add_option("--cache-size", type="int", default=0,
                  help="Cache sizes to use. Small encourages races between \
                        requests and writebacks. Large stresses write-through \
                        and/or write-back GPU caches. Range [0..1]")
parser.add_option("--system-size", type="int", default=0,
                  help="This option defines how many CUs, CPUs and cache \
                        components in the test system. Range[0..2]")
parser.add_option("--address-range", type="int", default=0,
                  help="This option defines the number of atomic \
                        locations that affects the working set's size. \
                        A small number of atomic locations encourage more \
                        races among threads. The large option stresses cache \
                        resources. Range [0..1]")
parser.add_option("--episode-length", type="int", default=0,
                  help="This option defines the number of LDs and \
                        STs in an episode. The small option encourages races \
                        between the start and end of an episode. The long \
                        option encourages races between LDs and STs in the \
                        same episode. Range [0..2]")
parser.add_option("--test-length", type="int", default=1,
                  help="The number of episodes to be executed by each \
                        wavefront. This determines the maximum number, i.e., \
                        val X #WFs, of episodes to be executed in the test.")
parser.add_option("--debug-tester", action='store_true',
                  help="This option will turn on DRF checker")
parser.add_option("--random-seed", type="int", default=0,
                  help="Random seed number. Default value (i.e., 0) means \
                        using runtime-specific value")
parser.add_option("--log-file", type="string", default="gpu-ruby-test.log")

# GPU configurations
parser.add_option("--wf-size", type="int", default=64, help="wavefront size")

parser.add_option("-w", "--wavefronts-per-cu", type="int", default=1,
                  help="Number of wavefronts per cu")

parser.add_option("--cu-per-sqc", type="int", default=4,
                  help="number of CUs sharing an SQC")

parser.add_option("--cu-per-scalar-cache", type="int", default=4,
                  help="number of CUs sharing an scalar cache")

parser.add_option("--cu-per-sa", type="int", default=4,
                  help="number of CUs per shader array \
                        This must be a multiple of options.cu-per-sqc and \
                        options.cu-per-scalar")
#
# Add the ruby specific and protocol specific options
#
Ruby.define_options(parser)

execfile(os.path.join(config_root, "common", "Options.py"))

(options, args) = parser.parse_args()

#
# Set the default cache size and associativity to be very small to encourage
# races between requests and writebacks.
#
options.l1d_size="256B"
options.l1i_size="256B"
options.l2_size="512B"
options.l3_size="1kB"
options.l1d_assoc=2
options.l1i_assoc=2
options.l2_assoc=2
options.l3_assoc=2

#
# Set up cache size - 2 options
#   0: small cache
#   1: large cache
#
if (options.cache_size == 0):
    options.tcp_size="256B"
    options.tcp_assoc=2
    options.tcc_size="1kB"
    options.tcc_assoc=2
elif (options.cache_size == 1):
    options.tcp_size="256kB"
    options.tcp_assoc=16
    options.tcc_size="1024kB"
    options.tcc_assoc=16
else:
     print("Error: option cache_size '%s' not recognized", options.cache_size)
     sys.exit(1)

#
# Set up system size - 3 options
#
if (options.system_size == 0):
    # 1 CU, 1 CPU, 1 SQC, 1 Scalar
    options.wf_size = 1
    options.wavefronts_per_cu = 1
    options.num_cpus = 1
    options.cu_per_sqc = 1
    options.cu_per_scalar_cache = 1
    options.num_compute_units = 1
elif (options.system_size == 1):
    # 4 CUs, 4 CPUs, 1 SQCs, 1 Scalars
    options.wf_size = 16
    options.wavefronts_per_cu = 4
    options.num_cpus = 4
    options.cu_per_sqc = 4
    options.cu_per_scalar_cache = 4
    options.num_compute_units = 4
elif (options.system_size == 2):
    # 8 CUs, 4 CPUs, 1 SQCs, 1 Scalars
    options.wf_size = 32
    options.wavefronts_per_cu = 4
    options.num_cpus = 4
    options.cu_per_sqc = 4
    options.cu_per_scalar_cache = 4
    options.num_compute_units = 8
else:
    print("Error: option system size '%s' not recognized", options.system_size)
    sys.exit(1)

#
# set address range - 2 options
#   level 0: small
#   level 1: large
# each location corresponds to a 4-byte piece of data
#
options.mem_size = '1024MB'
num_atomic_locs = 10
num_regular_locs_per_atomic_loc = 10000
if (options.address_range == 1):
    num_atomic_locs = 100
    num_regular_locs_per_atomic_loc = 100000
elif (options.address_range != 0):
    print("Error: option address_range '%s' not recognized", \
              options.address_range)
    sys.exit(1)

#
# set episode length (# of actions per episode) - 3 options
#   0: 10 actions
#   1: 100 actions
#   2: 500 actions
#
eps_length = 10
if (options.episode_length == 1):
    eps_length = 100
elif (options.episode_length == 2):
    eps_length = 500
elif (options.episode_length != 0):
    print("Error: option episode_length '%s' not recognized",
              options.episode_length)
    sys.exit(1)

# set the Ruby's and tester's deadlock thresholds
# the Ruby's deadlock detection is the primary check for deadlock.
# the tester's deadlock threshold detection is a secondary check for deadlock
# if there is a bug in RubyPort that causes a packet not to return to the
# tester properly, the tester will throw a deadlock exception.
# we set cache_deadlock_threshold < tester_deadlock_threshold to detect
# deadlock caused by Ruby protocol first before one caused by the coalescer
options.cache_deadlock_threshold = 100000000
tester_deadlock_threshold = 1000000000

# for now, we're testing only GPU protocol, so we set num_cpus to 0
options.num_cpus = 0
# number of CPUs and CUs
n_CPUs = options.num_cpus
n_CUs = options.num_compute_units
# set test length, i.e., number of episodes per wavefront * #WFs
# test length can be 1x#WFs, 10x#WFs, 100x#WFs, ...
n_WFs = n_CUs * options.wavefronts_per_cu
max_episodes = options.test_length * n_WFs
# number of SQC and Scalar caches
assert(n_CUs % options.cu_per_sqc == 0)
n_SQCs = int(n_CUs/options.cu_per_sqc)
options.num_sqc = n_SQCs
assert(n_CUs % options.cu_per_scalar_cache == 0)
n_Scalars = int(n_CUs/options.cu_per_scalar_cache)

# for now, we only set CUs and SQCs
# TODO: add scalars if necessary
n_Scalars = 0
options.num_scalar_cache = n_Scalars
if n_Scalars == 0:
    options.cu_per_scalar_cache = 0

if args:
     print("Error: script doesn't take any positional arguments")
     sys.exit(1)

#
# Create GPU Ruby random tester
#
tester = ProtocolTester(cus_per_sqc = options.cu_per_sqc,
                        cus_per_scalar = options.cu_per_scalar_cache,
                        wavefronts_per_cu = options.wavefronts_per_cu,
                        workitems_per_wavefront = options.wf_size,
                        num_atomic_locations = num_atomic_locs,
                        num_normal_locs_per_atomic = \
                                          num_regular_locs_per_atomic_loc,
                        max_num_episodes = max_episodes,
                        episode_length = eps_length,
                        debug_tester = options.debug_tester,
                        random_seed = options.random_seed,
                        log_file = options.log_file)

#
# Create the M5 system. Note that the memory object isn't actually
# used by the vitester, but is included to support
# the M5 memory size == Ruby memory size checks
#
# The system doesn't have real CPUs or CUs.
# It just has a tester that has physical ports to be connected to Ruby
#
system = System(cpu = tester,
                mem_ranges = [AddrRange(options.mem_size)],
                cache_line_size = options.cacheline_size,
                mem_mode = 'timing')

system.voltage_domain = VoltageDomain(voltage = options.sys_voltage)
system.clk_domain = SrcClockDomain(clock = options.sys_clock,
                                   voltage_domain = system.voltage_domain)

options.num_cp = 0

#
# Create the Ruby system
#
Ruby.create_system(options, False, system)

#
# The tester is most effective when randomization is turned on and
# artifical delay is randomly inserted on messages
#
system.ruby.randomization = True

# assert that we got the right number of Ruby ports
assert(len(system.ruby._cpu_ports) == n_CPUs + n_CUs + n_SQCs + n_Scalars)

#
# attach Ruby ports to the tester
# in the order: cpu_sequencers,
#               vector_coalescers,
#               sqc_sequencers,
#               scalar_sequencers
#
print("Attaching ruby ports to the tester")
i = 0
for ruby_port in system.ruby._cpu_ports:
    ruby_port.no_retry_on_stall = True
    ruby_port.using_ruby_tester = True

    if i < n_CPUs:
        tester.cpu_ports = ruby_port.slave
    elif i < (n_CPUs + n_CUs):
        tester.cu_vector_ports = ruby_port.slave
    elif i < (n_CPUs + n_CUs + n_SQCs):
        tester.cu_sqc_ports = ruby_port.slave
    else:
        tester.cu_scalar_ports = ruby_port.slave

    i += 1

#
# Create CPU threads
#
thread_clock = SrcClockDomain(clock = '1GHz',
                              voltage_domain = system.voltage_domain)

cpu_threads = []
print("Creating %i CpuThreads" % n_CPUs)
for cpu_idx in range(n_CPUs):
    cpu_threads.append(CpuThread(thread_id = cpu_idx,
                                 num_lanes = 1,     # CPU thread is scalar
                                 clk_domain = thread_clock,
                                 deadlock_threshold = \
                                        tester_deadlock_threshold))
tester.cpu_threads = cpu_threads

#
# Create GPU wavefronts
#
wavefronts = []
g_thread_idx = n_CPUs
print("Creating %i WFs attached to %i CUs" % \
                (n_CUs * tester.wavefronts_per_cu, n_CUs))
for cu_idx in range(n_CUs):
    for wf_idx in range(tester.wavefronts_per_cu):
        wavefronts.append(GpuWavefront(thread_id = g_thread_idx,
                                         cu_id = cu_idx,
                                         num_lanes = options.wf_size,
                                         clk_domain = thread_clock,
                                         deadlock_threshold = \
                                                tester_deadlock_threshold))
        g_thread_idx += 1
tester.wavefronts = wavefronts

# -----------------------
# run simulation
# -----------------------

root = Root( full_system = False, system = system )

# Not much point in this being higher than the L1 latency
m5.ticks.setGlobalFrequency('1ns')

# instantiate configuration
m5.instantiate()

# simulate until program terminates
exit_event = m5.simulate(options.abs_max_tick)

print('Exiting tick: ', m5.curTick())
print('Exiting because ', exit_event.getCause())
