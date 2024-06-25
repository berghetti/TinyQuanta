## Tiny Quanta

TQ is a system that enables efficient blind scheduling of microsecond-scale workloads. It achieves both low tail latency and high throughput, by using two mechanisms: forced multitasking and two-level scheduling. It was presented at [ASPLOS 2024](https://dl.acm.org/doi/10.1145/3620665.3640381).


### Contact

For any questions about TQ, please email <zhluo@berkeley.edu>.

This repository includes the sever runtime of TQ, its compiler instrumentation pass, an instrumented RocksDB, and an implementation for artificial workloads. TQ uses [LLVM](https://github.com/llvm/llvm-project), [DPDK](https://github.com/DPDK/dpdk) (huge pages allocated), [RocksDB](https://github.com/facebook/rocksdb), [Boost](https://www.boost.org/). Their dependencies need to be properly installed. Then, one could simply build these compoennts and run a TQ server by:

```
./run.sh
```
