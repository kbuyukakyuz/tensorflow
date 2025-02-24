/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/convert/xplane_to_op_stats.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/convert/multi_xplanes_to_op_stats.h"
#include "tensorflow/core/profiler/convert/repository.h"
#include "tensorflow/core/profiler/convert/step_events_to_steps_db.h"
#include "tensorflow/core/profiler/protobuf/diagnostics.pb.h"
#include "tensorflow/core/profiler/protobuf/op_metrics.pb.h"
#include "tensorflow/core/profiler/protobuf/op_stats.pb.h"
#include "tensorflow/core/profiler/protobuf/steps_db.pb.h"
#include "tensorflow/core/profiler/protobuf/tf_function.pb.h"
#include "tensorflow/core/profiler/protobuf/xplane.pb.h"
#include "tensorflow/core/profiler/utils/group_events.h"
#include "tensorflow/core/profiler/utils/xplane_builder.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_test_utils.h"

namespace tensorflow {
namespace profiler {
namespace {

using ::testing::Property;
using ::testing::UnorderedElementsAre;

TEST(ConvertXPlaneToOpStats, GpuPerfEnv) {
  XSpace space;
  constexpr double kMaxError = 0.01;
  constexpr int kClockRateKHz = 1530000;
  constexpr int kCoreCount = 80;
  constexpr uint64 kMemoryBandwidthBytesPerSecond =
      uint64{900} * 1000 * 1000 * 1000;
  // Volta.
  constexpr int kComputeCapMajor = 7;
  constexpr int kComputeCapMinor = 0;

  XPlaneBuilder device_plane(
      GetOrCreateGpuXPlane(&space, /*device_ordinal=*/0));
  device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata(
                                GetStatTypeStr(StatType::kDevVendor)),
                            kDeviceVendorNvidia);
  device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata("clock_rate"),
                            kClockRateKHz);
  device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata("core_count"),
                            kCoreCount);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("memory_bandwidth"),
      kMemoryBandwidthBytesPerSecond);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("compute_cap_major"),
      kComputeCapMajor);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("compute_cap_minor"),
      kComputeCapMinor);

  GroupTfEvents(&space);
  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  const PerfEnv& perf_env = op_stats.perf_env();
  EXPECT_NEAR(141, perf_env.peak_tera_flops_per_second(), kMaxError);
  EXPECT_NEAR(900, perf_env.peak_hbm_bw_giga_bytes_per_second(), kMaxError);
  EXPECT_NEAR(156.67, perf_env.ridge_point(), kMaxError);
}

TEST(ConvertXPlaneToOpStats, GpuRunEnvironment) {
  XSpace space;
  XPlaneBuilder device_plane1(
      GetOrCreateGpuXPlane(&space, /*device_ordinal=*/0));
  device_plane1.AddStatValue(*device_plane1.GetOrCreateStatMetadata(
                                 GetStatTypeStr(StatType::kDevVendor)),
                             kDeviceVendorNvidia);
  XPlaneBuilder device_plane2(
      GetOrCreateGpuXPlane(&space, /*device_ordinal=*/1));
  device_plane2.AddStatValue(*device_plane2.GetOrCreateStatMetadata(
                                 GetStatTypeStr(StatType::kDevVendor)),
                             kDeviceVendorNvidia);

  GroupTfEvents(&space);
  OpStats op_stats = ConvertXSpaceToOpStats(space, OpStatsOptions());
  const RunEnvironment& run_env = op_stats.run_environment();

  EXPECT_EQ("Nvidia GPU", run_env.device_type());
  EXPECT_EQ(1, run_env.host_count());
  EXPECT_EQ(1, run_env.task_count());
  EXPECT_EQ(2, run_env.device_core_count());
}

TEST(ConvertXPlaneToOpStats, CpuOnlyStepDbTest) {
  constexpr int64_t kStepNum = 123;
  constexpr int64_t kStepId = 0;

  XSpace space;
  XPlaneBuilder host_plane_builder(GetOrCreateHostXPlane(&space));
  host_plane_builder.ReserveLines(2);

  auto main_thread = host_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kTraceContext,
               0, 100, {{StatType::kStepNum, kStepNum}});
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kFunctionRun,
               10, 90, {{StatType::kStepId, kStepId}});

  auto tf_executor_thread = host_plane_builder.GetOrCreateLine(1);
  CreateXEvent(&host_plane_builder, &tf_executor_thread,
               HostEventType::kExecutorStateProcess, 20, 80,
               {{StatType::kStepId, kStepId}});
  CreateXEvent(&host_plane_builder, &tf_executor_thread, "matmul", 30, 70);

  GroupTfEvents(&space);
  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  options.generate_step_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  const StepDatabaseResult& step_db = op_stats.step_db();

  EXPECT_EQ(step_db.step_sequence_size(), 1);
}

TEST(ConvertXPlaneToOpStats, GpuStepDbTest) {
  constexpr int64_t kStepNum = 123;
  constexpr int64_t kStepId = 0;
  constexpr int64_t kCorrelationId = 100;

  XSpace space;
  XPlaneBuilder host_plane_builder(GetOrCreateHostXPlane(&space));
  host_plane_builder.ReserveLines(2);

  auto main_thread = host_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kTraceContext,
               0, 100, {{StatType::kStepNum, kStepNum}});
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kFunctionRun,
               10, 90, {{StatType::kStepId, kStepId}});

  auto tf_executor_thread = host_plane_builder.GetOrCreateLine(1);
  CreateXEvent(&host_plane_builder, &tf_executor_thread,
               HostEventType::kExecutorStateProcess, 20, 20,
               {{StatType::kStepId, kStepId}});
  CreateXEvent(&host_plane_builder, &tf_executor_thread, "matmul", 30, 10,
               {{StatType::kCorrelationId, kCorrelationId}});

  XPlaneBuilder device_plane_builder(
      GetOrCreateGpuXPlane(&space, /*device_ordinal=*/0));
  device_plane_builder.ReserveLines(1);

  auto stream = device_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&device_plane_builder, &stream, "matmul", 50, 40,
               {{StatType::kCorrelationId, kCorrelationId}});

  GroupTfEvents(&space);
  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  options.generate_step_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  const StepDatabaseResult& step_db = op_stats.step_db();

  EXPECT_EQ(step_db.step_sequence_size(), 1);

  PrecisionStats precision_stats =
      op_stats.device_op_metrics_db().precision_stats();
  EXPECT_EQ(precision_stats.compute_16bit_ps(), 0);
  EXPECT_EQ(precision_stats.compute_32bit_ps(), 40);
}

TEST(ConvertXPlaneToOpStats, PropagateAndDedupErrors) {
  XSpace space;
  static constexpr char kError[] = "host: error";
  *space.add_errors() = kError;
  *space.add_errors() = kError;

  OpStats op_stats = ConvertXSpaceToOpStats(space, OpStatsOptions());

  EXPECT_EQ(1, op_stats.diagnostics().errors_size());
  EXPECT_EQ(kError, op_stats.diagnostics().errors(/*index=*/0));
}

TEST(ConvertXPlaneToOpStats, Hostnames) {
  XSpace space;
  static constexpr char kHost[] = "host1";
  *space.add_hostnames() = kHost;

  OpStats op_stats = ConvertXSpaceToOpStats(space, OpStatsOptions());
  EXPECT_EQ(
      kHost,
      op_stats.core_id_to_details().at(kDefaultGpuLocalCoreId).hostname());
}

void BuildXSpaceForTest(XSpace& xspace, absl::string_view hostname) {
  constexpr int64_t kStepNum = 123;
  constexpr int64_t kStepId = 456;
  // Create a host only XSpace for test.
  XPlaneBuilder host_plane_builder(GetOrCreateHostXPlane(&xspace));
  host_plane_builder.ReserveLines(2);

  auto main_thread = host_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kTraceContext,
               0, 100, {{StatType::kStepNum, kStepNum}});
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kFunctionRun,
               10, 90, {{StatType::kStepId, kStepId}});

  auto executor_thread = host_plane_builder.GetOrCreateLine(1);
  CreateXEvent(&host_plane_builder, &executor_thread,
               HostEventType::kExecutorStateProcess, 20, 80,
               {{StatType::kStepId, kStepId}});
  // Create a TensorFlow op that runs for 70 ps.
  CreateXEvent(&host_plane_builder, &executor_thread, "aaa:bbb", 30, 70);
  GroupTfEvents(&xspace);
  xspace.add_hostnames(std::string(hostname));
}

TEST(ConvertXPlaneToOpStats, TestConvertMultiXSpacesToCombinedOpStats) {
  static constexpr char kHost1[] = "host1";
  static constexpr char kHost2[] = "host2";

  auto xspace1 = std::make_unique<XSpace>();
  auto xspace2 = std::make_unique<XSpace>();

  BuildXSpaceForTest(*xspace1, kHost1);
  BuildXSpaceForTest(*xspace2, kHost2);

  std::vector<std::string> xspace_paths;
  xspace_paths.push_back("xspace_path1");
  xspace_paths.push_back("xspace_path2");

  std::vector<std::unique_ptr<XSpace>> xspaces;
  xspaces.push_back(std::move(xspace1));
  xspaces.push_back(std::move(xspace2));

  auto session_snapshot_or =
      SessionSnapshot::Create(std::move(xspace_paths), std::move(xspaces));
  TF_CHECK_OK(session_snapshot_or.status());

  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  options.generate_step_db = true;
  OpStats combined_op_stats;

  TF_CHECK_OK(ConvertMultiXSpacesToCombinedOpStats(session_snapshot_or.value(),
                                                   options, &combined_op_stats))
      << "Failed to convert multi XSpace to OpStats";

  // Result OpStats has 2 Host Ops, "IDLE" and "aaa:bbb".
  ASSERT_EQ(combined_op_stats.host_op_metrics_db().metrics_db_size(), 2);
  const auto& metric = combined_op_stats.host_op_metrics_db().metrics_db(1);
  EXPECT_EQ(metric.name(), "aaa");
  EXPECT_EQ(metric.category(), "bbb");
  // Each host has the HostOp "aaa:bbb" running for 70 ps, so the combined
  // OpStats has "aaa:bbb" running for 140 ps in total.
  EXPECT_EQ(metric.self_time_ps(), 140);

  // Result OpStats has 1 step, 2 cores.
  ASSERT_EQ(combined_op_stats.step_db().step_sequence_size(), 1);
  ASSERT_EQ(
      combined_op_stats.step_db().step_sequence(0).step_info_per_core_size(),
      2);
  const auto& step_info_per_core =
      combined_op_stats.step_db().step_sequence(0).step_info_per_core();
  // global_core_id is computed using: 1000 * host_id + local_core_id.
  EXPECT_TRUE(step_info_per_core.contains(kDefaultGpuLocalCoreId));
  EXPECT_TRUE(step_info_per_core.contains(1000 + kDefaultGpuLocalCoreId));

  const auto& core_details_map = combined_op_stats.core_id_to_details();
  EXPECT_EQ(kHost1, core_details_map.at(kDefaultGpuLocalCoreId).hostname());
  EXPECT_EQ(kHost2,
            core_details_map.at(1000 + kDefaultGpuLocalCoreId).hostname());
}

TEST(ConvertXPlaneToOpStats, RunEnvironmentExtractedFromTpuPlane) {
  XSpace xspace;
  for (int i : {0, 1, 2, 3}) {
    GetOrCreateTpuXPlane(&xspace, i, "TPU V4", 0, 0);
  }

  OpStats op_stats = ConvertXSpaceToOpStats(xspace, OpStatsOptions());

  EXPECT_EQ(op_stats.run_environment().device_type(), "TPU V4");
  EXPECT_EQ(op_stats.run_environment().device_core_count(), 4);
}

TEST(ConvertXPlaneToOpStats, TpuPerfEnv) {
  XSpace space;
  constexpr double kMaxError = 0.01;
  constexpr int kClockRateKHz = 1530000;
  constexpr int kCoreCount = 80;
  constexpr uint64 kMemoryBandwidthBytesPerSecond =
      uint64{900} * 1000 * 1000 * 1000;
  // Volta.
  constexpr int kComputeCapMajor = 7;
  constexpr int kComputeCapMinor = 0;
  constexpr double kDevCapPeakTeraflopsPerSecond = 141.0;
  constexpr double kDevCapPeakHbmBwGigabytesPerSecond = 900.0;

  XPlaneBuilder device_plane(GetOrCreateTpuXPlane(
      &space, /*device_ordinal=*/0, "TPU V4", kDevCapPeakTeraflopsPerSecond,
      kDevCapPeakHbmBwGigabytesPerSecond));
  /*device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata(
                            GetStatTypeStr(StatType::kDevVendor)),
                        kDeviceVendorNvidia); // "Google, Inc.");*/
  device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata("clock_rate"),
                            kClockRateKHz);
  device_plane.AddStatValue(*device_plane.GetOrCreateStatMetadata("core_count"),
                            kCoreCount);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("memory_bandwidth"),
      kMemoryBandwidthBytesPerSecond);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("compute_cap_major"),
      kComputeCapMajor);
  device_plane.AddStatValue(
      *device_plane.GetOrCreateStatMetadata("compute_cap_minor"),
      kComputeCapMinor);

  GroupTfEvents(&space);
  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  const PerfEnv& perf_env = op_stats.perf_env();
  EXPECT_NEAR(141, perf_env.peak_tera_flops_per_second(), kMaxError);
  EXPECT_NEAR(900, perf_env.peak_hbm_bw_giga_bytes_per_second(), kMaxError);
  EXPECT_NEAR(156.67, perf_env.ridge_point(), kMaxError);
}

TEST(ConvertXPlaneToOpStats, TpuRunEnvironment) {
  XSpace space;
  XPlaneBuilder device_plane1(
      GetOrCreateTpuXPlane(&space, /*device_ordinal=*/0, "TPU V4", 0, 0));
  XPlaneBuilder device_plane2(
      GetOrCreateTpuXPlane(&space, /*device_ordinal=*/1, "TPU V4", 0, 0));

  GroupTfEvents(&space);
  OpStats op_stats = ConvertXSpaceToOpStats(space, OpStatsOptions());
  const RunEnvironment& run_env = op_stats.run_environment();

  EXPECT_EQ("TPU V4", run_env.device_type());
  EXPECT_EQ(1, run_env.host_count());
  EXPECT_EQ(1, run_env.task_count());
  EXPECT_EQ(2, run_env.device_core_count());
}

TEST(ConvertXPlaneToOpStats, TpuStepDbTest) {
  constexpr int64_t kStepNum = 123;
  constexpr int64_t kStepId = 0;
  constexpr int64_t kCorrelationId = 100;
  constexpr int kCoreCount = 80;
  constexpr double kDevCapPeakTeraflopsPerSecond = 141.0;
  constexpr double kDevCapPeakHbmBwGigabytesPerSecond = 900.0;

  XSpace space;
  XPlaneBuilder host_plane_builder(GetOrCreateHostXPlane(&space));
  host_plane_builder.ReserveLines(2);

  auto main_thread = host_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kTraceContext,
               0, 100, {{StatType::kStepNum, kStepNum}});
  CreateXEvent(&host_plane_builder, &main_thread, HostEventType::kFunctionRun,
               10, 90, {{StatType::kStepId, kStepId}});

  auto tf_executor_thread = host_plane_builder.GetOrCreateLine(1);
  CreateXEvent(&host_plane_builder, &tf_executor_thread,
               HostEventType::kExecutorStateProcess, 20, 20,
               {{StatType::kStepId, kStepId}});
  CreateXEvent(&host_plane_builder, &tf_executor_thread, "matmul", 30, 10,
               {{StatType::kCorrelationId, kCorrelationId}});

  XPlaneBuilder device_plane_builder(GetOrCreateTpuXPlane(
      &space, /*device_ordinal=*/0, "TPU V4", kDevCapPeakTeraflopsPerSecond,
      kDevCapPeakHbmBwGigabytesPerSecond));
  device_plane_builder.ReserveLines(1);
  device_plane_builder.AddStatValue(
      *device_plane_builder.GetOrCreateStatMetadata("core_count"), kCoreCount);

  auto stream = device_plane_builder.GetOrCreateLine(0);
  CreateXEvent(&device_plane_builder, &stream, "matmul", 50, 40,
               {{StatType::kCorrelationId, kCorrelationId}});

  GroupTfEvents(&space);
  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  options.generate_step_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  const StepDatabaseResult& step_db = op_stats.step_db();

  EXPECT_EQ(step_db.step_sequence_size(), 1);

  PrecisionStats precision_stats =
      op_stats.device_op_metrics_db().precision_stats();
  EXPECT_EQ(precision_stats.compute_16bit_ps(), 0);
  EXPECT_EQ(precision_stats.compute_32bit_ps(), 40);
}

TEST(ConvertXPlaneToOpStats, TpuDeviceTraceToStepDb) {
  XSpace space;
  constexpr double kDevCapPeakTeraflopsPerSecond = 141.0;
  constexpr double kDevCapPeakHbmBwGigabytesPerSecond = 900.0;
  XPlaneBuilder xplane_builder(GetOrCreateTpuXPlane(
      &space, /*device_ordinal=*/0, "TPU V4", kDevCapPeakTeraflopsPerSecond,
      kDevCapPeakHbmBwGigabytesPerSecond));

  XEventMetadata* event_metadata = xplane_builder.GetOrCreateEventMetadata(1);
  event_metadata->set_name("op_name");
  XStatsBuilder<XEventMetadata> stats(event_metadata, &xplane_builder);

  stats.AddStatValue(*xplane_builder.GetOrCreateStatMetadata(
                         GetStatTypeStr(StatType::kProgramId)),
                     1);
  stats.AddStatValue(*xplane_builder.GetOrCreateStatMetadata(
                         GetStatTypeStr(StatType::kSymbolId)),
                     1);
  stats.AddStatValue(*xplane_builder.GetOrCreateStatMetadata(
                         GetStatTypeStr(StatType::kSelfDurationPs)),
                     10);
  stats.AddStatValue(
      *xplane_builder.GetOrCreateStatMetadata(GetStatTypeStr(StatType::kTfOp)),
      "tf_op_name");
  stats.AddStatValue(*xplane_builder.GetOrCreateStatMetadata(
                         GetStatTypeStr(StatType::kHloCategory)),
                     "category");
  XLineBuilder line = xplane_builder.GetOrCreateLine(1);
  line.SetName(kTensorFlowOpLineName);
  XEventBuilder event = line.AddEvent(*event_metadata);
  event.SetOffsetNs(0);
  event.SetDurationNs(10);

  OpStatsOptions options;
  options.generate_op_metrics_db = true;
  OpStats op_stats = ConvertXSpaceToOpStats(space, options);
  EXPECT_THAT(op_stats.device_op_metrics_db().metrics_db(),
              UnorderedElementsAre(Property(&OpMetrics::name, "op_name"),
                                   Property(&OpMetrics::name, "IDLE")));
}

}  // namespace
}  // namespace profiler
}  // namespace tensorflow
