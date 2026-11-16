/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! Benchmarks for `request_context_tokio` hook overhead.
//!
//! Run locally:
//!   buck run @mode/opt fbsource//xplat/folly/rust/request_context:request_context_tokio_bench -- --bench

use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use criterion::Criterion;
use criterion::Throughput;
use criterion::criterion_group;
use criterion::criterion_main;
use request_context::RequestContext;
use request_context_tokio::BuilderExt as _;

fn build_runtime(workers: usize, hooks: bool) -> tokio::runtime::Runtime {
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    builder.worker_threads(workers).enable_all();
    if hooks {
        builder.install_request_context_hooks();
    }
    builder.build().unwrap()
}

/// Pure spawn throughput: 100k tasks, no yields.
/// Stresses map insert/remove and context capture on every spawn.
fn bench_spawn_throughput(c: &mut Criterion) {
    let mut group = c.benchmark_group("spawn_throughput");
    group.sample_size(10);
    let n = 100_000u64;
    group.throughput(Throughput::Elements(n));

    group.bench_function("no_hooks", |b| {
        let rt = build_runtime(4, false);
        b.iter(|| {
            rt.block_on(async {
                let handles: Vec<_> = (0..n)
                    .map(|_| tokio::spawn(async { std::hint::black_box(()) }))
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.bench_function("hooks_with_ctx", |b| {
        let rt = build_runtime(4, true);
        b.iter(|| {
            rt.block_on(async {
                RequestContext::create();
                let handles: Vec<_> = (0..n)
                    .map(|_| tokio::spawn(async { std::hint::black_box(()) }))
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.finish();
}

/// Poll-heavy workload: 100K tasks x 50 yields each = 5M+ poll events.
/// Stresses map get + clone + RequestContext swap on every poll.
fn bench_poll_throughput(c: &mut Criterion) {
    let mut group = c.benchmark_group("poll_throughput");
    group.sample_size(10);
    let tasks = 100_000u64;
    let yields_per_task = 10u64;
    group.throughput(Throughput::Elements(tasks));

    group.bench_function("no_hooks", |b| {
        let rt = build_runtime(4, false);
        b.iter(|| {
            rt.block_on(async {
                let handles: Vec<_> = (0..tasks)
                    .map(|_| {
                        tokio::spawn(async move {
                            for _ in 0..yields_per_task {
                                tokio::task::yield_now().await;
                            }
                            std::hint::black_box(())
                        })
                    })
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.bench_function("hooks_with_ctx", |b| {
        let rt = build_runtime(4, true);
        b.iter(|| {
            rt.block_on(async {
                RequestContext::create();
                let handles: Vec<_> = (0..tasks)
                    .map(|_| {
                        tokio::spawn(async move {
                            for _ in 0..yields_per_task {
                                tokio::task::yield_now().await;
                            }
                            std::hint::black_box(())
                        })
                    })
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.finish();
}

/// Contention stress test: 100K tasks with a yield, 8 worker threads.
/// Stresses map shard lock contention across many concurrent workers.
fn bench_contention_throughput(c: &mut Criterion) {
    let mut group = c.benchmark_group("contention_throughput");
    group.sample_size(10);
    let n = 100_000u64;
    group.throughput(Throughput::Elements(n));

    group.bench_function("no_hooks", |b| {
        let rt = build_runtime(8, false);
        b.iter(|| {
            rt.block_on(async {
                let counter = std::sync::Arc::new(AtomicUsize::new(0));
                let handles: Vec<_> = (0..n)
                    .map(|_| {
                        let counter = counter.clone();
                        tokio::spawn(async move {
                            tokio::task::yield_now().await;
                            counter.fetch_add(1, Ordering::Relaxed);
                        })
                    })
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.bench_function("hooks_with_ctx", |b| {
        let rt = build_runtime(8, true);
        b.iter(|| {
            rt.block_on(async {
                RequestContext::create();
                let counter = std::sync::Arc::new(AtomicUsize::new(0));
                let handles: Vec<_> = (0..n)
                    .map(|_| {
                        let counter = counter.clone();
                        tokio::spawn(async move {
                            tokio::task::yield_now().await;
                            counter.fetch_add(1, Ordering::Relaxed);
                        })
                    })
                    .collect();
                futures::future::join_all(handles).await
            })
        });
    });

    group.finish();
}

criterion_group!(
    benches,
    bench_spawn_throughput,
    bench_poll_throughput,
    bench_contention_throughput
);
criterion_main!(benches);
