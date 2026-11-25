# Dispatch benchmarks

`engine_bench` measures synchronous `Engine::execute`. Use the same compiler,
Release flags, book layout and workload on both revisions. Run on an otherwise
idle machine. The comparison script checks measured command counts, trade counts
and final state fingerprints before reporting per-trial medians.

From the repository root, export the pre-change revision into a separate build
directory and use the current benchmark and regression drivers with both versions.

```sh
before=build-dispatch-reference
after=build-dispatch-current
mkdir -p "$before/source"
git archive 4da18ba308bc736f163b110c08fca2983ce6b322 exchange-lab | tar -x -C "$before/source"
reference="$before/source/exchange-lab"
cp exchange-lab/bench/engine_bench.cpp "$reference/bench/"
cmake -S "$reference" -B "$before" -DCMAKE_BUILD_TYPE=Release
cmake --build "$before" --target engine_bench
python3 exchange-lab/bench/compare_dispatch.py capture "$before/engine_bench" "$before/core.jsonl"

cmake -S exchange-lab -B "$after" -DCMAKE_BUILD_TYPE=Release
cmake --build "$after"
python3 exchange-lab/bench/compare_dispatch.py capture "$after/engine_bench" "$after/core.jsonl"
python3 exchange-lab/bench/compare_dispatch.py compare "$before/core.jsonl" "$after/core.jsonl"
ctest --test-dir "$after" --output-on-failure
```

The matrix varies active order depth, unrelated users and orders, and sweep
size. `mix` cycles through passive place, reduce, move and cancel. `sweep`
times only the crossing order and replenishes liquidity outside the measurement.
A sweep with 1,000 fills has few samples and serves as a throughput and
scaling check. Use the `ops` field and sample counts when interpreting
percentiles. The empty-book mix has one million latency samples across five
trials. Single-operation modes include clock reads on the throughput path.
Use `engine_bench` with an invalid argument to show the positional argument list.

For exact replay comparison, compile the current regression driver against
the old engine and compare its results and state snapshots with the new engine.
Use fresh output paths because the driver appends binary snapshots.

```sh
c++ -std=c++20 -O3 -DNDEBUG -I "$reference/include" \
    exchange-lab/tests/dispatch_transaction_test.cpp "$before/libexchange_lab.a" \
    -o "$before/dispatch_transaction_tests"
"$before/dispatch_transaction_tests" trace "$before/states.bin" > "$before/trace.txt"
"$after/dispatch_transaction_tests" trace "$after/states.bin" > "$after/trace.txt"
cmp "$before/trace.txt" "$after/trace.txt"
cmp "$before/states.bin" "$after/states.bin"
```

`dispatch_pipeline_bench` submits the same zero-fill IOC repeatedly, with
base liquidity outside the order's limit. Rate zero counts calls to scalar
`operator new` and requested bytes around synchronous execution. A nonzero
rate schedules commands against a fixed arrival clock and measures through
future delivery, including queueing and scheduling. Queue-full submissions
are counted separately from latency percentiles. An optional output path
enables the existing journal write and stream flush for each accepted command.

```sh
c++ -std=c++20 -O3 -DNDEBUG -pthread -I "$reference/include" \
    exchange-lab/bench/dispatch_pipeline_bench.cpp "$before/libexchange_lab.a" \
    -o "$before/dispatch_pipeline_bench"
for directory in "$before" "$after"; do
    "$directory/dispatch_pipeline_bench" 1000 0 5000
    "$directory/dispatch_pipeline_bench" 100 100000 50000
    "$directory/dispatch_pipeline_bench" 100 500000 250000
    "$directory/dispatch_pipeline_bench" 100 100000 50000 "$directory/benchmark.journal"
done
```

For pipeline statistics, repeat each case five times and alternate which
revision runs first. Report arrival rate, queue-full count and latency together.
