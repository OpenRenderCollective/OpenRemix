# CPU denoising scheduling benchmark

Run `remix-forward-lightmap-denoise-test --cpu-benchmark` to measure the production
filter on 512 independent charts at each of three image sizes, 16, 32 and 64 pixels.
The regular test invocation still runs the chart isolation, directional SH,
metadata and cancellation checks.

An optional final argument selects one, two or four independent queues, for
example `remix-forward-lightmap-denoise-test --cpu-benchmark 4`. Multiple queues
split the same 512 charts and use a nominal 32-thread budget through per-device
limits. OIDN environment overrides still take precedence. Models warm before a
shared start barrier. Reported time is the slowest queue's measured duration.

An optional queue-cap argument compares synchronization frequency, for example
`remix-forward-lightmap-denoise-test --cpu-benchmark 4 32`. The default cap is
eight; valid caps are 1 through 32. Actual packets are also limited by production's
shared byte budget, including each chart's original and filtered L0. Larger
images can therefore have a lower effective cap. The final partial packet group
still processes every chart exactly once.

The benchmark explicitly requests the CPU device and prepares the model before
timing. Every chart
starts with the same deterministic nine-field input. It reports elapsed time,
progress-monitored execution count, a per-worker digest, and a single-packet
digest. Each worker must produce the same fields and digest for its equal share
of the charts. Packet digests remain comparable across worker counts; whole-worker
digests change when each worker processes fewer charts. These checks
help detect output changes when comparing scheduling experiments; they
do not replace the regular quality tests. Timings include packet copies,
validation and digest calculation.

OIDN's `OIDN_NUM_THREADS` environment override can test thread counts without
changing production policy. Keep the machine uncontended, compare repeated runs,
and retain the environment and build identity alongside the results. The OIDN
runtime libraries must be on `PATH` when running this executable directly.

This synthetic benchmark cannot establish a map bake-time improvement. Verify
promising changes on identical captured map inputs and measure the whole bake.
Do not combine charts spatially or reduce directional fields to improve this
benchmark: those change the filtering problem and its isolation guarantees.
