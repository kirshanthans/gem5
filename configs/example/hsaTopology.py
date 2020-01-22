import m5

import operator
from os import mkdir, makedirs, getpid, listdir, fsync
from os.path import join as joinpath
from os.path import isdir
from shutil import rmtree, copyfile

def file_append(path, contents):
    with open(joinpath(*path), 'a') as f:
        f.write(str(contents))
        f.flush()
        fsync(f.fileno())

def remake_dir(path):
    if isdir(path):
        rmtree(path)
    makedirs(path)

def createHsaTopology(options):
    topology_dir = joinpath(m5.options.outdir, \
        'fs/sys/devices/virtual/kfd/kfd/topology')
    remake_dir(topology_dir)

    # Ripped from real Kaveri platform to appease kmt version checks
    # Set up generation_id
    file_append((topology_dir, 'generation_id'), 1)

    # Set up system properties
    sys_prop = 'platform_oem 2314885673410447169\n' + \
               'platform_id 35322352389441\n'       + \
               'platform_rev 1\n'
    file_append((topology_dir, 'system_properties'), sys_prop)

    # Populate the topology tree
    # TODO: Just the bare minimum to pass for now
    node_dir = joinpath(topology_dir, 'nodes/0')
    remake_dir(node_dir)

    # must show valid kaveri gpu id or massive meltdown
    file_append((node_dir, 'gpu_id'), 2765)

    # must have marketing name
    file_append((node_dir, 'name'), 'Carrizo\n')

    # populate global node properties
    # NOTE: SIMD count triggers a valid GPU agent creation
    # TODO: Really need to parse these from options
    node_prop = 'cpu_cores_count %s\n' % options.num_cpus   + \
                'simd_count 32\n'                           + \
                'mem_banks_count 0\n'                       + \
                'caches_count 0\n'                          + \
                'io_links_count 0\n'                        + \
                'cpu_core_id_base 16\n'                     + \
                'simd_id_base 2147483648\n'                 + \
                'max_waves_per_simd 40\n'                   + \
                'lds_size_in_kb 64\n'                       + \
                'gds_size_in_kb 0\n'                        + \
                'wave_front_size 64\n'                      + \
                'array_count 1\n'                           + \
                'simd_arrays_per_engine 1\n'                + \
                'cu_per_simd_array 10\n'                    + \
                'simd_per_cu 4\n'                           + \
                'max_slots_scratch_cu 32\n'                 + \
                'vendor_id 4098\n'                          + \
                'device_id 39028\n'                         + \
                'location_id 8\n'                           + \
                'max_engine_clk_fcompute 800\n'             + \
                'local_mem_size 0\n'                        + \
                'fw_version 699\n'                          + \
                'capability 4738\n'                         + \
                'max_engine_clk_ccompute 2100\n'

    file_append((node_dir, 'properties'), node_prop)
