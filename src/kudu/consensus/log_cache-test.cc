// Copyright (c) 2014, Cloudera, inc.

#include <gtest/gtest.h>
#include <string>
#include <tr1/memory>

#include "kudu/consensus/consensus-test-util.h"
#include "kudu/consensus/log_cache.h"
#include "kudu/util/mem_tracker.h"
#include "kudu/util/metrics.h"
#include "kudu/util/test_util.h"

using std::tr1::shared_ptr;

DECLARE_int32(consensus_max_batch_size_bytes);
DECLARE_int32(log_cache_size_soft_limit_mb);
DECLARE_int32(log_cache_size_hard_limit_mb);
DECLARE_int32(global_log_cache_size_soft_limit_mb);
DECLARE_int32(global_log_cache_size_hard_limit_mb);

namespace kudu {
namespace consensus {

class LogCacheTest : public KuduTest {
 public:
  LogCacheTest()
    : metric_context_(&metric_registry_, "LogCacheTest"),
      cache_(new LogCache(metric_context_, "TestMemTracker")) {
  }

 protected:
  bool AppendReplicateMessagesToCache(
    int first,
    int count,
    int payload_size = 0) {

    for (int i = first; i < first + count; i++) {
      int term = i / 7;
      int index = i;
      gscoped_ptr<ReplicateMsg> msg = CreateDummyReplicate(term, index, payload_size);
      if (!cache_->AppendOperation(&msg)) {
        return false;
      }
    }
    return true;
  }

  MetricRegistry metric_registry_;
  MetricContext metric_context_;
  gscoped_ptr<LogCache> cache_;
};


TEST_F(LogCacheTest, TestGetMessages) {
  ASSERT_EQ(0, cache_->metrics_.log_cache_total_num_ops->value());
  ASSERT_EQ(0, cache_->metrics_.log_cache_size_bytes->value());
  ASSERT_TRUE(AppendReplicateMessagesToCache(1, 100));
  ASSERT_EQ(100, cache_->metrics_.log_cache_total_num_ops->value());
  ASSERT_GE(cache_->metrics_.log_cache_size_bytes->value(), 500);

  vector<ReplicateMsg*> messages;
  OpId preceding;
  ASSERT_OK(cache_->ReadOps(MakeOpId(0, 0), 8 * 1024 * 1024, &messages, &preceding));
  EXPECT_EQ(100, messages.size());
  EXPECT_EQ("0.0", OpIdToString(preceding));

  // Get starting in the middle of the cache.
  messages.clear();
  ASSERT_OK(cache_->ReadOps(MakeOpId(0, 70), 8 * 1024 * 1024, &messages, &preceding));
  EXPECT_EQ(30, messages.size());
  EXPECT_EQ("10.70", OpIdToString(preceding));
  EXPECT_EQ("10.71", OpIdToString(messages[0]->id()));

  // Get at the end of the cache
  messages.clear();
  ASSERT_OK(cache_->ReadOps(MakeOpId(0, 100), 8 * 1024 * 1024, &messages, &preceding));
  EXPECT_EQ(0, messages.size());
  EXPECT_EQ("14.100", OpIdToString(preceding));
}


// Ensure that the cache always yields at least one message,
// even if that message is larger than the batch size. This ensures
// that we don't get "stuck" in the case that a large message enters
// the cache.
TEST_F(LogCacheTest, TestAlwaysYieldsAtLeastOneMessage) {
  // generate a 2MB dummy payload
  const int kPayloadSize = 2 * 1024 * 1024;

  // Append several large ops to the cache
  ASSERT_TRUE(AppendReplicateMessagesToCache(1, 4, kPayloadSize));

  // We should get one of them, even though we only ask for 100 bytes
  vector<ReplicateMsg*> messages;
  OpId preceding;
  ASSERT_OK(cache_->ReadOps(MakeOpId(0, 0), 100, &messages, &preceding));
  ASSERT_EQ(1, messages.size());
}

// Test that, if the hard limit has been reached, requests are refused.
TEST_F(LogCacheTest, TestCacheRefusesRequestWhenFilled) {
  FLAGS_log_cache_size_soft_limit_mb = 0;
  FLAGS_log_cache_size_hard_limit_mb = 1;

  cache_.reset(new LogCache(metric_context_, "TestCacheRefusesRequestWhenFilled"));

  // generate a 128Kb dummy payload
  const int kPayloadSize = 128 * 1024;

  // append 8 messages to the cache, these should be allowed
  ASSERT_TRUE(AppendReplicateMessagesToCache(1, 7, kPayloadSize));

  // should fail because the cache is full
  ASSERT_FALSE(AppendReplicateMessagesToCache(8, 1, kPayloadSize));


  // Move the pin past the first two ops
  cache_->SetPinnedOp(2);

  // And try again -- should now succeed
  ASSERT_TRUE(AppendReplicateMessagesToCache(8, 1, kPayloadSize));
}


TEST_F(LogCacheTest, TestHardAndSoftLimit) {
  FLAGS_log_cache_size_soft_limit_mb = 1;
  FLAGS_log_cache_size_hard_limit_mb = 2;

  cache_.reset(new LogCache(metric_context_, "TestHardAndSoftLimit"));

  const int kPayloadSize = 768 * 1024;
  // Soft limit should not be violated.
  ASSERT_TRUE(AppendReplicateMessagesToCache(1, 1, kPayloadSize));

  int size_with_one_msg = cache_->BytesUsed();
  ASSERT_LT(size_with_one_msg, 1 * 1024 * 1024);

  // Violating a soft limit, but not a hard limit should still allow
  // the operation to be added.
  ASSERT_TRUE(AppendReplicateMessagesToCache(2, 1, kPayloadSize));

  // Since the first operation is not yet done, we can't trim.
  int size_with_two_msgs = cache_->BytesUsed();
  ASSERT_GE(size_with_two_msgs, 2 * 768 * 1024);
  ASSERT_LT(size_with_two_msgs, 2 * 1024 * 1024);

  cache_->SetPinnedOp(2);

  // Verify that we have trimmed by appending a message that would
  // otherwise be rejected, since the cache max size limit is 2MB.
  ASSERT_TRUE(AppendReplicateMessagesToCache(3, 1, kPayloadSize));

  // The cache should be trimmed down to two messages.
  ASSERT_EQ(size_with_two_msgs, cache_->BytesUsed());

  // Ack indexes 2 and 3
  cache_->SetPinnedOp(4);
  ASSERT_TRUE(AppendReplicateMessagesToCache(4, 1, kPayloadSize));

  // Verify that the cache is trimmed down to just one message.
  ASSERT_EQ(size_with_one_msg, cache_->BytesUsed());

  cache_->SetPinnedOp(5);

  // Add a small message such that soft limit is not violated.
  const int kSmallPayloadSize = 128 * 1024;
  ASSERT_TRUE(AppendReplicateMessagesToCache(5, 1, kSmallPayloadSize));

  // Verify that the cache is not trimmed.
  ASSERT_GT(cache_->BytesUsed(), 0);
}

TEST_F(LogCacheTest, TestGlobalHardLimit) {
  FLAGS_log_cache_size_soft_limit_mb = 1;
  FLAGS_global_log_cache_size_soft_limit_mb = 4;

  FLAGS_log_cache_size_hard_limit_mb = 2;
  FLAGS_global_log_cache_size_hard_limit_mb = 5;

  const string kParentTrackerId = "TestGlobalHardLimit";
  shared_ptr<MemTracker> parent_tracker = MemTracker::CreateTracker(
    FLAGS_log_cache_size_soft_limit_mb * 1024 * 1024,
    kParentTrackerId,
    NULL);

  ASSERT_TRUE(parent_tracker.get() != NULL);

  // Exceed the global hard limit.
  parent_tracker->Consume(6 * 1024 * 1024);

  cache_.reset(new LogCache(metric_context_, kParentTrackerId));

  const int kPayloadSize = 768 * 1024;

  // Should fail because the cache has exceeded hard limit.
  ASSERT_FALSE(AppendReplicateMessagesToCache(1, 1, kPayloadSize));

  // Now release the memory.
  parent_tracker->Release(2 * 1024 * 1024);

  ASSERT_TRUE(AppendReplicateMessagesToCache(1, 1, kPayloadSize));
}

TEST_F(LogCacheTest, TestEvictWhenGlobalSoftLimitExceeded) {
  FLAGS_log_cache_size_soft_limit_mb = 1;
  FLAGS_global_log_cache_size_soft_limit_mb = 4;

  FLAGS_log_cache_size_hard_limit_mb = 2;
  FLAGS_global_log_cache_size_hard_limit_mb = 5;

  const string kParentTrackerId = "TestGlobalSoftLimit";

  shared_ptr<MemTracker> parent_tracker = MemTracker::CreateTracker(
     FLAGS_log_cache_size_soft_limit_mb * 1024 * 1024,
     kParentTrackerId,
     NULL);

 ASSERT_TRUE(parent_tracker.get() != NULL);

 // Exceed the global soft limit.
 parent_tracker->Consume(4 * 1024 * 1024);
 parent_tracker->Consume(1024);

 cache_.reset(new LogCache(metric_context_, kParentTrackerId));

 const int kPayloadSize = 768 * 1024;
 ASSERT_TRUE(AppendReplicateMessagesToCache(1, 1, kPayloadSize));

 int size_with_one_msg = cache_->BytesUsed();

 cache_->SetPinnedOp(2);

 // If this goes through, that means the queue has been trimmed, otherwise
 // the hard limit would be violated and false would be returnedl.
 ASSERT_TRUE(AppendReplicateMessagesToCache(2, 1, kPayloadSize));

 // Verify that there is only one message in the queue.
 ASSERT_EQ(size_with_one_msg, cache_->BytesUsed());
}

} // namespace consensus
} // namespace kudu