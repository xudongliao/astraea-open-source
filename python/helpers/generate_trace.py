import argparse
import numpy as np
from os import path

MTU = 1500 * 8

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bandwidth', metavar='Mbps', required=True,
                        help='constant bandwidth (Mbps)')
    parser.add_argument('--output-dir', metavar='DIR', required=True,
                        help='directory to output trace')
    args = parser.parse_args()

    # number of packets in 60*5 seconds
    # num_packets = int(float(args.bandwidth) * 5000 * 5)
    num_segments = int(float(args.bandwidth) * 1e6 / MTU * 60)
    ts_list = np.linspace(0, 60000, num=num_segments, endpoint=False)

    # trace path
    # make_sure_path_exists(args.output_dir)
    trace_path = path.join(args.output_dir, '%smbps.trace' % args.bandwidth)

    # write timestamps to trace
    with open(trace_path, 'w') as trace:
        for ts in ts_list:
            trace.write('%d\n' % ts)


if __name__ == '__main__':
    main()
