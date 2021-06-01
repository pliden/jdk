/*
 * Copyright (c) 2015, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/z/zDirector.hpp"
#include "gc/z/zDriver.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zHeuristics.hpp"
#include "gc/z/zStat.hpp"
#include "logging/log.hpp"

constexpr double one_in_1000 = 3.290527;
constexpr double sample_interval = 1.0 / ZStatAllocRate::sample_hz;

ZDirector::ZDirector(ZDriver* driver) :
    _driver(driver),
    _metronome(ZStatAllocRate::sample_hz) {
  set_name("ZDirector");
  create_and_start();
}

static void sample_allocation_rate() {
  // Sample allocation rate. This is needed by rule_allocation_rate()
  // below to estimate the time we have until we run out of memory.
  const double bytes_per_second = ZStatAllocRate::sample_and_reset();

  log_debug(gc, alloc)("Allocation Rate: %.1fMB/s, Avg: %.1f(+/-%.1f)MB/s",
                       bytes_per_second / M,
                       ZStatAllocRate::avg() / M,
                       ZStatAllocRate::sd() / M);
}

static ZDriverRequest rule_allocation_stall() {
  // Perform GC if we've observed at least one allocation stall since
  // the last GC started.
  const bool stall_since_last_gc = ZHeap::heap()->has_alloc_stalled();

  log_debug(gc, director)("Rule: Allocation Stall Since Last GC: %s",
                          stall_since_last_gc ? "Yes" : "No");

  if (!stall_since_last_gc) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_allocation_stall;
}

static ZDriverRequest rule_warmup() {
  if (ZStatCycle::is_warm()) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  // Perform GC if heap usage passes 10/20/30% and no other GC has been
  // performed yet. This allows us to get some early samples of the GC
  // duration, which is needed by the other rules.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const double used_threshold_percent = (ZStatCycle::nwarmup_cycles() + 1) * 0.1;
  const size_t used_threshold = soft_max_capacity * used_threshold_percent;

  log_debug(gc, director)("Rule: Warmup %.0f%%, Used: " SIZE_FORMAT "MB, UsedThreshold: " SIZE_FORMAT "MB",
                          used_threshold_percent * 100, used / M, used_threshold / M);

  if (used < used_threshold) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_warmup;
}

static ZDriverRequest rule_timer() {
  if (ZCollectionInterval <= 0) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  // Perform GC if timer has expired.
  const double time_since_last_gc = ZStatCycle::time_since_last();
  const double time_until_gc = ZCollectionInterval - time_since_last_gc;

  log_debug(gc, director)("Rule: Timer, Interval: %.3fs, TimeUntilGC: %.3fs",
                          ZCollectionInterval, time_until_gc);

  if (time_until_gc > 0) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_timer;
}

static double estimated_gc_workers(double serial_gc_time, double parallelizable_gc_time, double time_until_deadline) {
  const double parallelizable_time_until_deadline = MAX2(time_until_deadline - serial_gc_time, 0.001);
  return parallelizable_gc_time / parallelizable_time_until_deadline;
}

static uint discrete_gc_workers(double gc_workers) {
  return clamp<uint>(ceil(gc_workers), 1, ConcGCThreads);
}

static double select_gc_workers(double serial_gc_time, double parallelizable_gc_time, double time_until_oom) {
  // Calculate number of GC workers needed to avoid a long GC cycle and to avoid OOM.
  const double avoid_long_gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, 10 /* seconds */);
  const double avoid_oom_gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, time_until_oom);
  const double gc_workers = MAX2(avoid_long_gc_workers, avoid_oom_gc_workers);
  const uint actual_gc_workers = discrete_gc_workers(gc_workers);
  const uint last_gc_workers = ZStatCycle::last_active_workers();

  if (actual_gc_workers < last_gc_workers) {
    // Before decreasing number of GC workers compared to the previous GC cycle, check if the
    // next GC cycle will need to increase it again. If so, use the same number of GC workers
    // that will be needed in the next cycle.
    const double gc_duration_delta = (parallelizable_gc_time / actual_gc_workers) - (parallelizable_gc_time / last_gc_workers);
    const double additional_time_for_allocations = ZStatCycle::time_since_last() - gc_duration_delta - sample_interval;
    const double next_time_until_oom = time_until_oom + additional_time_for_allocations;
    const double next_avoid_oom_gc_workers = estimated_gc_workers(serial_gc_time, parallelizable_gc_time, next_time_until_oom);
    const double next_gc_workers = MAX2(avoid_long_gc_workers, next_avoid_oom_gc_workers);

    // Add 0.5 to increase friction and avoid lowering too eagerly
    return MIN2<double>(ceil(next_gc_workers + 0.50), last_gc_workers);
  }

  return gc_workers;
}

ZDriverRequest rule_allocation_rate_dynamic() {
  if (!ZStatCycle::is_time_trustable()) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, ZHeuristics::relocation_headroom());

  // Calculate time until OOM given the max allocation rate and the amount
  // of free memory. The allocation rate is a moving average and we multiply
  // that with an allocation spike tolerance factor to guard against unforeseen
  // phase changes in the allocate rate. We then add ~3.3 sigma to account for
  // the allocation rate variance, which means the probability is 1 in 1000
  // that a sample is outside of the confidence interval.
  const double alloc_rate_avg = ZStatAllocRate::avg();
  const double alloc_rate_sd = ZStatAllocRate::sd();
  const double alloc_rate_sd_percent = alloc_rate_sd / (alloc_rate_avg + 1.0);
  const bool alloc_rate_steady = alloc_rate_sd_percent < 0.15; // 15%
  const double alloc_rate = (alloc_rate_avg * ZAllocationSpikeTolerance) + (alloc_rate_sd * one_in_1000) + 1.0;
  double time_until_oom = free / alloc_rate;

  if (!alloc_rate_steady) {
    time_until_oom /= (1.0 + alloc_rate_sd_percent);
  }

  // Calculate max serial/parallel times of a GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double serial_gc_time = ZStatCycle::serial_time().davg() + (ZStatCycle::serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = ZStatCycle::parallelizable_time().davg() + (ZStatCycle::parallelizable_time().dsd() * one_in_1000);

  // Calculate number of GC workers needed to avoid OOM.
  double gc_workers = select_gc_workers(serial_gc_time, parallelizable_gc_time, time_until_oom);

  if (!alloc_rate_steady) {
    gc_workers = MAX2<double>(gc_workers, ZStatCycle::last_active_workers());
  }

  // Convert to a discrete number of GC workers within limits.
  const uint actual_gc_workers = discrete_gc_workers(gc_workers);

  // Calculate GC duration given number of GC workers needed.
  const double actual_gc_duration = serial_gc_time + (parallelizable_gc_time / actual_gc_workers);
  const uint last_gc_workers = ZStatCycle::last_active_workers();

  // Calculate time until GC given the time until OOM and GC duration.
  // We also subtract the sample interval, so that we don't overshoot the
  // target time and end up starting the GC too late in the next interval.
  const double more_safety_for_fewer_workers = (ConcGCThreads - actual_gc_workers) * sample_interval;
  const double time_until_gc = time_until_oom - actual_gc_duration - sample_interval - more_safety_for_fewer_workers;

  log_info(gc)("Rule: Allocation Rate (Dynamic GC Threads  New), MaxAllocRate: %.1fMB/s (+/-%.1f%%), Free: " SIZE_FORMAT "MB, GCCPUTime: %.3f, GCDuration: %.3fs, TimeUntilOOM: %.3fs, TimeUntilGC: %.3fs, GCWorkers: %.3f (%u -> %u)",
               alloc_rate / M,
               alloc_rate_sd_percent * 100,
               free / M,
               serial_gc_time + parallelizable_gc_time,
               serial_gc_time + (parallelizable_gc_time / actual_gc_workers),
               time_until_oom,
               time_until_gc,
               gc_workers,
               last_gc_workers,
               actual_gc_workers);

  if (actual_gc_workers <= last_gc_workers && time_until_gc > 0) {
    return ZDriverRequest(GCCause::_no_gc, actual_gc_workers);
  }

  return ZDriverRequest(GCCause::_z_allocation_rate, actual_gc_workers);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

ZDriverRequest rule_allocation_rate_dynamic_orig() {
  if (!ZStatCycle::is_time_trustable()) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  bool ret;
  uint suggested_n;
  double n_ideal = 0.0;
  double n_ideal_next_gc = 0.0;

  // 0.1%
  constexpr double sd_factor = 3.290527;
  const double alloc_rate = (ZStatAllocRate::avg() * ZAllocationSpikeTolerance) +
                            (ZStatAllocRate::sd() * sd_factor) +
                            1.0; // avoid division by zero

  const size_t mutator_max = ZHeap::heap()->soft_max_capacity() - ZHeuristics::relocation_headroom();

  // `margin` measures the closest distance to oom since previous stw1 in
  // seconds, negative value means potential alloc stall.
  const double watermark = (double)ZHeap::heap()->used_high() / mutator_max;
  const double margin = mutator_max * (1-watermark) / alloc_rate;
  // pliden: margin == worst_time_until_oom seen so far

  const double alloc_rate_sd_percent = ZStatAllocRate::sd() / (ZStatAllocRate::avg() + 1.0);

  const size_t used_bytes = ZHeap::heap()->used();
  const double used_percent = (double) used_bytes / ZStatHeap::max_capacity();
  const size_t free_bytes = (mutator_max > used_bytes) ? mutator_max - used_bytes : 0;

  // Calculate how much time left before hitting oom giving the current free
  // bytes and the predicted alloc rate. Bound by 1ms to avoid division by zero.
  double time_till_oom = MAX2(free_bytes/alloc_rate - sample_interval, 0.001);

  const double serial_gc_time = ZStatCycle::serial_time().davg() + (ZStatCycle::serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = ZStatCycle::parallelizable_time().davg() + (ZStatCycle::parallelizable_time().dsd() * one_in_1000);
  const double cputime_total = serial_gc_time + parallelizable_gc_time;

  // avoiding boost
  const uint previous_n = ZStatCycle::last_active_workers();

  // No adaptation once a gc cycle is initiated, so each cycle needs to be
  // short enough to handle emergencies.
  // TODO: not sure about absolute number or sth depending on cputime_per_worker
  constexpr double target_max_walltime = 10;

  // It seems in steady state, the sd is < 5%, using the following magic threshold.
  constexpr double alloc_rate_sd_threshold = 0.15; // 5% x 3 error margin

  uint min_n = clamp((uint)ceil(cputime_total / target_max_walltime), 1u, ConcGCThreads);

  // An accurate prediction requires steady alloc rate and gc duration (heap usage as the predicator)
  const bool is_env_steady = (alloc_rate_sd_percent <= alloc_rate_sd_threshold); /*&&
                             (used_percent - ZStatCycle::prev_used_percent <= 0.10); */

  if (false && !is_env_steady) {
    min_n = MAX2(min_n, (uint)ceil(ConcGCThreads / 2.0));
  }

  uint n;
  if (alloc_rate_sd_percent >= alloc_rate_sd_threshold) {
    // pliden: allocation rate varies a lot
    // Since time_till_oom is calculated based on the currently observed
    // alloc rate, when the alloc rate is volatile (reflected as large sd),
    // such calculation could be unreliable. In order to incorporate such
    // volatility, we artificially deflate the oom time to react promptly for
    // the potential imminent high alloc rate.
    time_till_oom = time_till_oom / (1.0 + alloc_rate_sd_percent);
    n_ideal = cputime_total / time_till_oom;
    // not reducing n when alloc rate is too volatile
    n = clamp((uint)ceil(n_ideal), MAX2(min_n, previous_n), ConcGCThreads);
  } else {
    // pliden: we can rely on the allocation rate metrics
    n_ideal = cputime_total / time_till_oom;
    n = clamp((uint)ceil(n_ideal), min_n, ConcGCThreads);
    // more stringent calculation on trying to reduce n
    if (n < previous_n) {
      // after reducing n, gc duration will increase, affecting the
      // calculation for next gc cycle. Therefore, we use the next
      // time_till_oom (deducting the gc duration delta) to derive n
      const double gc_duration_delta = cputime_total * (1.0/n - 1.0/previous_n);
      const double additional_time_for_allocations_to_happen = ZStatCycle::time_since_last() - gc_duration_delta - sample_interval;
      const double next_time_till_oom = time_till_oom + additional_time_for_allocations_to_happen;

      // pliden: n_dev_idle == predicted threads to use next GC cycle
      n_ideal_next_gc = cputime_total / MAX2(next_time_till_oom, 0.001); // in case it's negative

      // pliden: 0.5 is just random friction, might or might not be needed
      // some friction on reducing n
      n = clamp((uint) ceil(n_ideal_next_gc + 0.50), min_n, previous_n);
    }
  }

  suggested_n = n;

  // some head start for not running at full-speed and some negative feedback for too small margin
  const double extra = sample_interval +
                       ((ConcGCThreads - n) * sample_interval); /* +
                       MAX2(2*sample_interval - margin, 0.0) * 10; */

  const double time_till_gc = time_till_oom - ((cputime_total / n) + extra);

  ret = n > previous_n || time_till_gc <= 0;

  log_info(gc)("Rule: Allocation Rate (Dynamic GC Threads Orig), MaxAllocRate: %.1fMB/s (+/-%.1f%%), Free: " SIZE_FORMAT "MB, GCCPUTime: %.3f, GCDuration: %.3fs, TimeUntilOOM: %.3fs, TimeUntilGC: %.3fs, GCWorkers: %.3f (%u -> %u)",
               alloc_rate / M,
               alloc_rate_sd_percent * 100,
               free_bytes / M,
               serial_gc_time + parallelizable_gc_time,
               serial_gc_time + (parallelizable_gc_time / suggested_n),
               time_till_oom,
               time_till_gc,
               n_ideal,
               ZStatCycle::last_active_workers(),
               suggested_n);

  bool print_log = false;
  if (print_log) {
    log_info(gc)(
        "high: %.1f%%; "
        "min_n: %d; "
        "gc: %.3f, "
        "oom: %.3f, "
        "margin: %.3f, "
        "rate: %.3f + %.3f M/s (%.1f%%), "
        "n: %d -> %d (%.3f, %.3f), "
        "",
        watermark * 100,
        min_n,
        cputime_total,
        time_till_oom,
        margin,
        (ZStatAllocRate::avg())/(1024*1024), (ZStatAllocRate::sd() * 1)/(1024*1024),
        (alloc_rate_sd_percent * 100),
        previous_n,
        suggested_n,
        n_ideal,
        n_ideal_next_gc);
  }

  if (ret) {
    return ZDriverRequest(GCCause::_z_allocation_rate, suggested_n);
  }

  return ZDriverRequest(GCCause::_no_gc, suggested_n);
}

/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////

static ZDriverRequest rule_allocation_rate_static() {
  if (!ZStatCycle::is_time_trustable()) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  // Perform GC if the estimated max allocation rate indicates that we
  // will run out of memory. The estimated max allocation rate is based
  // on the moving average of the sampled allocation rate plus a safety
  // margin based on variations in the allocation rate and unforeseen
  // allocation spikes.

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, ZHeuristics::relocation_headroom());

  // Calculate time until OOM given the max allocation rate and the amount
  // of free memory. The allocation rate is a moving average and we multiply
  // that with an allocation spike tolerance factor to guard against unforeseen
  // phase changes in the allocate rate. We then add ~3.3 sigma to account for
  // the allocation rate variance, which means the probability is 1 in 1000
  // that a sample is outside of the confidence interval.
  const double max_alloc_rate = (ZStatAllocRate::avg() * ZAllocationSpikeTolerance) + (ZStatAllocRate::sd() * one_in_1000);
  const double time_until_oom = free / (max_alloc_rate + 1.0); // Plus 1.0B/s to avoid division by zero

  // Calculate max serial/parallel times of a GC cycle. The times are
  // moving averages, we add ~3.3 sigma to account for the variance.
  const double serial_gc_time = ZStatCycle::serial_time().davg() + (ZStatCycle::serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = ZStatCycle::parallelizable_time().davg() + (ZStatCycle::parallelizable_time().dsd() * one_in_1000);

  // Calculate GC duration given number of GC workers needed.
  const double gc_duration = serial_gc_time + (parallelizable_gc_time / ConcGCThreads);

  // Calculate time until GC given the time until OOM and max duration of GC.
  // We also deduct the sample interval, so that we don't overshoot the target
  // time and end up starting the GC too late in the next interval.
  const double time_until_gc = time_until_oom - gc_duration - sample_interval;

  log_debug(gc, director)("Rule: Allocation Rate (Static GC Threads), MaxAllocRate: %.1fMB/s, Free: " SIZE_FORMAT "MB, GCDuration: %.3fs, TimeUntilGC: %.3fs",
                          max_alloc_rate / M, free / M, gc_duration, time_until_gc);

  if (time_until_gc > 0) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_allocation_rate;
}

static ZDriverRequest rule_allocation_rate() {
  if (UseDynamicNumberOfGCThreads) {
#if 1
    ZDriverRequest a = rule_allocation_rate_dynamic_orig();
    ZDriverRequest b = rule_allocation_rate_dynamic();

    if (a.cause() != b.cause() || a.nworkers() != b.nworkers()) {
      log_info(gc)("DIFF: Orig: %s (%u) vs. New: %s (%u)",
                   GCCause::to_string(a.cause()),
                   a.nworkers(),
                   GCCause::to_string(b.cause()),
                   b.nworkers());
    }

    return UseNewCode ? b : a;
#else
    return rule_allocation_rate_dynamic();
#endif
  } else {
    return rule_allocation_rate_static();
  }
}

static ZDriverRequest rule_high_usage() {
  // Perform GC if the amount of free memory is 5% or less. This is a preventive
  // meassure in the case where the application has a very low allocation rate,
  // such that the allocation rate rule doesn't trigger, but the amount of free
  // memory is still slowly but surely heading towards zero. In this situation,
  // we start a GC cycle to avoid a potential allocation stall later.

  // Calculate amount of free memory available. Note that we take the
  // relocation headroom into account to avoid in-place relocation.
  const size_t soft_max_capacity = ZHeap::heap()->soft_max_capacity();
  const size_t used = ZHeap::heap()->used();
  const size_t free_including_headroom = soft_max_capacity - MIN2(soft_max_capacity, used);
  const size_t free = free_including_headroom - MIN2(free_including_headroom, ZHeuristics::relocation_headroom());
  const double free_percent = percent_of(free, soft_max_capacity);

  log_debug(gc, director)("Rule: High Usage, Free: " SIZE_FORMAT "MB(%.1f%%)",
                          free / M, free_percent);

  if (free_percent > 5.0) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_high_usage;
}

static ZDriverRequest rule_proactive() {
  if (!ZProactive || !ZStatCycle::is_warm()) {
    // Rule disabled
    return GCCause::_no_gc;
  }

  // Perform GC if the impact of doing so, in terms of application throughput
  // reduction, is considered acceptable. This rule allows us to keep the heap
  // size down and allow reference processing to happen even when we have a lot
  // of free space on the heap.

  // Only consider doing a proactive GC if the heap usage has grown by at least
  // 10% of the max capacity since the previous GC, or more than 5 minutes has
  // passed since the previous GC. This helps avoid superfluous GCs when running
  // applications with very low allocation rate.
  const size_t used_after_last_gc = ZStatHeap::used_at_relocate_end();
  const size_t used_increase_threshold = ZHeap::heap()->soft_max_capacity() * 0.10; // 10%
  const size_t used_threshold = used_after_last_gc + used_increase_threshold;
  const size_t used = ZHeap::heap()->used();
  const double time_since_last_gc = ZStatCycle::time_since_last();
  const double time_since_last_gc_threshold = 5 * 60; // 5 minutes
  if (used < used_threshold && time_since_last_gc < time_since_last_gc_threshold) {
    // Don't even consider doing a proactive GC
    log_debug(gc, director)("Rule: Proactive, UsedUntilEnabled: " SIZE_FORMAT "MB, TimeUntilEnabled: %.3fs",
                            (used_threshold - used) / M,
                            time_since_last_gc_threshold - time_since_last_gc);
    return GCCause::_no_gc;
  }

  const double assumed_throughput_drop_during_gc = 0.50; // 50%
  const double acceptable_throughput_drop = 0.01;        // 1%
  const double serial_gc_time = ZStatCycle::serial_time().davg() + (ZStatCycle::serial_time().dsd() * one_in_1000);
  const double parallelizable_gc_time = ZStatCycle::parallelizable_time().davg() + (ZStatCycle::parallelizable_time().dsd() * one_in_1000);
  const double gc_duration = serial_gc_time + (parallelizable_gc_time / ConcGCThreads);
  const double acceptable_gc_interval = gc_duration * ((assumed_throughput_drop_during_gc / acceptable_throughput_drop) - 1.0);
  const double time_until_gc = acceptable_gc_interval - time_since_last_gc;

  log_debug(gc, director)("Rule: Proactive, AcceptableGCInterval: %.3fs, TimeSinceLastGC: %.3fs, TimeUntilGC: %.3fs",
                          acceptable_gc_interval, time_since_last_gc, time_until_gc);

  if (time_until_gc > 0) {
    return GCCause::_no_gc;
  }

  return GCCause::_z_proactive;
}

static ZDriverRequest make_gc_decision() {
  // List of rules
  using ZDirectorRule = ZDriverRequest (*)();
  const ZDirectorRule rules[] = {
    rule_allocation_stall,
    rule_warmup,
    rule_timer,
    rule_allocation_rate,
    rule_high_usage,
    rule_proactive,
  };

  // Execute rules
  for (size_t i = 0; i < ARRAY_SIZE(rules); i++) {
    const ZDriverRequest request = rules[i]();
    if (request.cause() != GCCause::_no_gc) {
      return request;
    }
  }

  return GCCause::_no_gc;
}

void ZDirector::run_service() {
  // Main loop
  while (_metronome.wait_for_tick()) {
    sample_allocation_rate();
    if (!_driver->is_busy()) {
      const ZDriverRequest request = make_gc_decision();
      if (request.cause() != GCCause::_no_gc) {
        _driver->collect(request);
      }
    }
  }
}

void ZDirector::stop_service() {
  _metronome.stop();
}
