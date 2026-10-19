# CPU denoising scheduling benchmark

Run `remix-forward-lightmap-denoise-test --cpu-benchmark` to measure the production
filter on 512 independent charts at each of two image sizes, 32 and 64 pixels.
The regular test invocation still runs the chart isolation, directional SH,
metadata and cancellation checks.

The benchmark explicitly requests the CPU device, prepares the model before
timing, and queues eight chart packets before each synchronization. Every chart
starts with the same deterministic nine-field input. It reports elapsed time,
progress-monitored execution count, and a digest of all filtered values. The
digest helps detect output changes when comparing scheduling experiments; it
does not replace the regular quality tests. Timings include packet copies,
validation and digest calculation.

OIDN's `OIDN_NUM_THREADS` environment override can test thread counts without
changing production policy. Keep the machine uncontended, compare repeated runs,
and retain the environment and build identity alongside the results. The OIDN
runtime libraries must be on `PATH` when running this executable directly.

This synthetic benchmark cannot establish a map bake-time improvement. Verify
promising changes on identical captured map inputs and measure the whole bake.
Do not combine charts spatially or reduce directional fields to improve this
benchmark: those change the filtering problem and its isolation guarantees.
