/* Copyright 2013 Google Inc. All Rights Reserved.

   Distributed under MIT license.
   See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

/* Function to find backward reference copies. */

#include "../../brotli/enc/backward_references.h"

#include <math.h>  /* INFINITY */
#include <string.h>  /* memcpy, memset */

extern int brotlirep; // TurboBench
#include "../../brotli/common/constants.h"
#include "../../brotli/common/types.h"
#include "../../brotli/enc/command.h"
#include "../../brotli/enc/fast_log.h"
#include "../../brotli/enc/find_match_length.h"
#include "../../brotli/enc/literal_cost.h"
#include "../../brotli/enc/memory.h"
#include "../../brotli/enc/port.h"
#include "../../brotli/enc/prefix.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/* The maximum length for which the zopflification uses distinct distances. */
static const uint16_t kMaxZopfliLenQuality10 = 150;
static const uint16_t kMaxZopfliLenQuality11 = 325;

#ifdef INFINITY
static const float kInfinity = INFINITY;
#else
static const float kInfinity = 3.4028e38f;
#endif

void BrotliInitZopfliNodes(ZopfliNode* array, size_t length) {
  ZopfliNode stub;
  size_t i;
  stub.length = 1;
  stub.distance = 0;
  stub.insert_length = 0;
  stub.u.cost = kInfinity;
  for (i = 0; i < length; ++i) array[i] = stub;
}

static BROTLI_INLINE uint32_t ZopfliNodeCopyLength(const ZopfliNode* self) {
  return self->length & 0xffffff;
}

static BROTLI_INLINE uint32_t ZopfliNodeLengthCode(const ZopfliNode* self) {
  const uint32_t modifier = self->length >> 24;
  return ZopfliNodeCopyLength(self) + 9u - modifier;
}

static BROTLI_INLINE uint32_t ZopfliNodeCopyDistance(const ZopfliNode* self) {
  return self->distance & 0x1ffffff;
}

static BROTLI_INLINE uint32_t ZopfliNodeDistanceCode(const ZopfliNode* self) {
  const uint32_t short_code = self->distance >> 25;
  return short_code == 0 ? ZopfliNodeCopyDistance(self) + 15 : short_code - 1;
}

static BROTLI_INLINE uint32_t ZopfliNodeCommandLength(const ZopfliNode* self) {
  return ZopfliNodeCopyLength(self) + self->insert_length;
}

static BROTLI_INLINE size_t MaxZopfliLenForQuality(int quality) {
  return quality <= 10 ? kMaxZopfliLenQuality10 : kMaxZopfliLenQuality11;
}

/* Histogram based cost model for zopflification. */
typedef struct ZopfliCostModel {
  /* The insert and copy length symbols. */
  float cost_cmd_[BROTLI_NUM_COMMAND_SYMBOLS];
  float cost_dist_[BROTLI_NUM_DISTANCE_SYMBOLS];
  /* Cumulative costs of literals per position in the stream. */
  float* literal_costs_;
  float min_cost_cmd_;
  size_t num_bytes_;
} ZopfliCostModel;

static void InitZopfliCostModel(
    MemoryManager* m, ZopfliCostModel* self, size_t num_bytes) {
  self->num_bytes_ = num_bytes;
  self->literal_costs_ = BROTLI_ALLOC(m, float, num_bytes + 2);
  if (BROTLI_IS_OOM(m)) return;
}

static void CleanupZopfliCostModel(MemoryManager* m, ZopfliCostModel* self) {
  BROTLI_FREE(m, self->literal_costs_);
}

static void SetCost(const uint32_t* histogram, size_t histogram_size,
                    float* cost) {
  size_t sum = 0;
  float log2sum;
  size_t i;
  for (i = 0; i < histogram_size; i++) {
    sum += histogram[i];
  }
  log2sum = (float)FastLog2(sum);
  for (i = 0; i < histogram_size; i++) {
    if (histogram[i] == 0) {
      cost[i] = log2sum + 2;
      continue;
    }

    /* Shannon bits for this symbol. */
    cost[i] = log2sum - (float)FastLog2(histogram[i]);

    /* Cannot be coded with less than 1 bit */
    if (cost[i] < 1) cost[i] = 1;
  }
}

static void ZopfliCostModelSetFromCommands(ZopfliCostModel* self,
                                           size_t position,
                                           const uint8_t* ringbuffer,
                                           size_t ringbuffer_mask,
                                           const Command* commands,
                                           size_t num_commands,
                                           size_t last_insert_len) {
  uint32_t histogram_literal[BROTLI_NUM_LITERAL_SYMBOLS];
  uint32_t histogram_cmd[BROTLI_NUM_COMMAND_SYMBOLS];
  uint32_t histogram_dist[BROTLI_NUM_DISTANCE_SYMBOLS];
  float cost_literal[BROTLI_NUM_LITERAL_SYMBOLS];
  size_t pos = position - last_insert_len;
  float min_cost_cmd = kInfinity;
  size_t i;
  float* cost_cmd = self->cost_cmd_;

  memset(histogram_literal, 0, sizeof(histogram_literal));
  memset(histogram_cmd, 0, sizeof(histogram_cmd));
  memset(histogram_dist, 0, sizeof(histogram_dist));

  for (i = 0; i < num_commands; i++) {
    size_t inslength = commands[i].insert_len_;
    size_t copylength = CommandCopyLen(&commands[i]);
    size_t distcode = commands[i].dist_prefix_;
    size_t cmdcode = commands[i].cmd_prefix_;
    size_t j;

    histogram_cmd[cmdcode]++;
    if (cmdcode >= 128) histogram_dist[distcode]++;

    for (j = 0; j < inslength; j++) {
      histogram_literal[ringbuffer[(pos + j) & ringbuffer_mask]]++;
    }

    pos += inslength + copylength;
  }

  SetCost(histogram_literal, BROTLI_NUM_LITERAL_SYMBOLS, cost_literal);
  SetCost(histogram_cmd, BROTLI_NUM_COMMAND_SYMBOLS, cost_cmd);
  SetCost(histogram_dist, BROTLI_NUM_DISTANCE_SYMBOLS, self->cost_dist_);

  for (i = 0; i < BROTLI_NUM_COMMAND_SYMBOLS; ++i) {
    min_cost_cmd = BROTLI_MIN(float, min_cost_cmd, cost_cmd[i]);
  }
  self->min_cost_cmd_ = min_cost_cmd;

  {
    float* literal_costs = self->literal_costs_;
    size_t num_bytes = self->num_bytes_;
    literal_costs[0] = 0.0;
    for (i = 0; i < num_bytes; ++i) {
      literal_costs[i + 1] = literal_costs[i] +
          cost_literal[ringbuffer[(position + i) & ringbuffer_mask]];
    }
  }
}

static void ZopfliCostModelSetFromLiteralCosts(ZopfliCostModel* self,
                                               size_t position,
                                               const uint8_t* ringbuffer,
                                               size_t ringbuffer_mask) {
  float* literal_costs = self->literal_costs_;
  float* cost_dist = self->cost_dist_;
  float* cost_cmd = self->cost_cmd_;
  size_t num_bytes = self->num_bytes_;
  size_t i;
  BrotliEstimateBitCostsForLiterals(position, num_bytes, ringbuffer_mask,
                                    ringbuffer, &literal_costs[1]);
  literal_costs[0] = 0.0;
  for (i = 0; i < num_bytes; ++i) {
    literal_costs[i + 1] += literal_costs[i];
  }
  for (i = 0; i < BROTLI_NUM_COMMAND_SYMBOLS; ++i) {
    cost_cmd[i] = (float)FastLog2(11 + (uint32_t)i);
  }
  for (i = 0; i < BROTLI_NUM_DISTANCE_SYMBOLS; ++i) {
    cost_dist[i] = (float)FastLog2(20 + (uint32_t)i);
  }
  self->min_cost_cmd_ = (float)FastLog2(11);
}

static BROTLI_INLINE float ZopfliCostModelGetCommandCost(
    const ZopfliCostModel* self, uint16_t cmdcode) {
  return self->cost_cmd_[cmdcode];
}

static BROTLI_INLINE float ZopfliCostModelGetDistanceCost(
    const ZopfliCostModel* self, size_t distcode) {
  return self->cost_dist_[distcode];
}

static BROTLI_INLINE float ZopfliCostModelGetLiteralCosts(
    const ZopfliCostModel* self, size_t from, size_t to) {
  return self->literal_costs_[to] - self->literal_costs_[from];
}

static BROTLI_INLINE float ZopfliCostModelGetMinCostCmd(
    const ZopfliCostModel* self) {
  return self->min_cost_cmd_;
}

static BROTLI_INLINE size_t ComputeDistanceCode(size_t distance,
                                                size_t max_distance,
                                                int quality,
                                                const int* dist_cache) {
  if (brotlirep && distance <= max_distance) { // TurboBench
    if (distance == (size_t)dist_cache[0]) {
      return 0;
    } else if (distance == (size_t)dist_cache[1]) {
      return 1;
    } else if (distance == (size_t)dist_cache[2]) {
      return 2;
    } else if (distance == (size_t)dist_cache[3]) {
      return 3;
    } else if (quality > 3 && distance >= 6) {
      size_t k;
      for (k = 4; k < BROTLI_NUM_DISTANCE_SHORT_CODES; ++k) {
        size_t idx = kDistanceCacheIndex[k];
        size_t candidate = (size_t)(dist_cache[idx] + kDistanceCacheOffset[k]);
        static const size_t kLimits[16] = {  0,  0,  0,  0,
                                             6,  6, 11, 11,
                                            11, 11, 11, 11,
                                            12, 12, 12, 12 };
        if (distance == candidate && distance >= kLimits[k]) {
          return k;
        }
      }
    }
  }
  return distance + 15;
}

/* REQUIRES: len >= 2, start_pos <= pos */
/* REQUIRES: cost < kInfinity, nodes[start_pos].cost < kInfinity */
/* Maintains the "ZopfliNode array invariant". */
static BROTLI_INLINE void UpdateZopfliNode(ZopfliNode* nodes, size_t pos,
    size_t start_pos, size_t len, size_t len_code, size_t dist,
    size_t short_code, float cost) {
  ZopfliNode* next = &nodes[pos + len];
  next->length = (uint32_t)(len | ((len + 9u - len_code) << 24));
  next->distance = (uint32_t)(dist | (short_code << 25));
  next->insert_length = (uint32_t)(pos - start_pos);
  next->u.cost = cost;
}

typedef struct PosData {
  size_t pos;
  int distance_cache[4];
  float costdiff;
} PosData;

/* Maintains the smallest 8 cost difference together with their positions */
typedef struct StartPosQueue {
  PosData q_[8];
  size_t idx_;
} StartPosQueue;

static BROTLI_INLINE void InitStartPosQueue(StartPosQueue* self) {
  self->idx_ = 0;
}

static size_t StartPosQueueSize(const StartPosQueue* self) {
  return BROTLI_MIN(size_t, self->idx_, 8);
}

static void StartPosQueuePush(StartPosQueue* self, const PosData* posdata) {
  size_t offset = ~(self->idx_++) & 7;
  size_t len = StartPosQueueSize(self);
  size_t i;
  PosData* q = self->q_;
  q[offset] = *posdata;
  /* Restore the sorted order. In the list of |len| items at most |len - 1|
     adjacent element comparisons / swaps are required. */
  for (i = 1; i < len; ++i) {
    if (q[offset & 7].costdiff > q[(offset + 1) & 7].costdiff) {
      BROTLI_SWAP(PosData, q, offset & 7, (offset + 1) & 7);
    }
    ++offset;
  }
}

static const PosData* StartPosQueueAt(const StartPosQueue* self, size_t k) {
  return &self->q_[(k - self->idx_) & 7];
}

/* Returns the minimum possible copy length that can improve the cost of any */
/* future position. */
static size_t ComputeMinimumCopyLength(const StartPosQueue* queue,
                                       const ZopfliNode* nodes,
                                       const ZopfliCostModel* model,
                                       const size_t num_bytes,
                                       const size_t pos) {
  /* Compute the minimum possible cost of reaching any future position. */
  const size_t start0 = StartPosQueueAt(queue, 0)->pos;
  float min_cost = (nodes[start0].u.cost +
                    ZopfliCostModelGetLiteralCosts(model, start0, pos) +
                    ZopfliCostModelGetMinCostCmd(model));
  size_t len = 2;
  size_t next_len_bucket = 4;
  size_t next_len_offset = 10;
  while (pos + len <= num_bytes && nodes[pos + len].u.cost <= min_cost) {
    /* We already reached (pos + len) with no more cost than the minimum
       possible cost of reaching anything from this pos, so there is no point in
       looking for lengths <= len. */
    ++len;
    if (len == next_len_offset) {
      /* We reached the next copy length code bucket, so we add one more
         extra bit to the minimum cost. */
      min_cost += 1.0f;
      next_len_offset += next_len_bucket;
      next_len_bucket *= 2;
    }
  }
  return len;
}

/* Fills in dist_cache[0..3] with the last four distances (as defined by
   Section 4. of the Spec) that would be used at (block_start + pos) if we
   used the shortest path of commands from block_start, computed from
   nodes[0..pos]. The last four distances at block_start are in
   starting_dist_cach[0..3].
   REQUIRES: nodes[pos].cost < kInfinity
   REQUIRES: nodes[0..pos] satisfies that "ZopfliNode array invariant". */
static void ComputeDistanceCache(const size_t block_start,
                                 const size_t pos,
                                 const size_t max_backward,
                                 const int* starting_dist_cache,
                                 const ZopfliNode* nodes,
                                 int* dist_cache) {
  int idx = 0;
  size_t p = pos;
  /* Because of prerequisite, does at most (pos + 1) / 2 iterations. */
  while (idx < 4 && p > 0) {
    const size_t clen = ZopfliNodeCopyLength(&nodes[p]);
    const size_t ilen = nodes[p].insert_length;
    const size_t dist = ZopfliNodeCopyDistance(&nodes[p]);
    /* Since block_start + p is the end position of the command, the copy part
       starts from block_start + p - clen. Distances that are greater than this
       or greater than max_backward are static dictionary references, and do
       not update the last distances. Also distance code 0 (last distance)
       does not update the last distances. */
    if (dist + clen <= block_start + p && dist <= max_backward &&
        ZopfliNodeDistanceCode(&nodes[p]) > 0) {
      dist_cache[idx++] = (int)dist;
    }
    /* Because of prerequisite, p >= clen + ilen >= 2. */
    p -= clen + ilen;
  }
  for (; idx < 4; ++idx) {
    dist_cache[idx] = *starting_dist_cache++;
  }
}

static void UpdateNodes(const size_t num_bytes,
                        const size_t block_start,
                        const size_t pos,
                        const uint8_t* ringbuffer,
                        const size_t ringbuffer_mask,
                        const int quality,
                        const size_t max_backward_limit,
                        const int* starting_dist_cache,
                        const size_t num_matches,
                        const BackwardMatch* matches,
                        const ZopfliCostModel* model,
                        StartPosQueue* queue,
                        ZopfliNode* nodes) {
  const size_t cur_ix = block_start + pos;
  const size_t cur_ix_masked = cur_ix & ringbuffer_mask;
  const size_t max_distance = BROTLI_MIN(size_t, cur_ix, max_backward_limit);
  const size_t max_len = num_bytes - pos;
  const size_t max_zopfli_len = MaxZopfliLenForQuality(quality);
  const size_t max_iters = quality <= 10 ? 1 : 5;
  size_t min_len;
  size_t k;

  if (nodes[pos].u.cost <= ZopfliCostModelGetLiteralCosts(model, 0, pos)) {
    PosData posdata;
    posdata.pos = pos;
    posdata.costdiff = nodes[pos].u.cost -
        ZopfliCostModelGetLiteralCosts(model, 0, pos);
    ComputeDistanceCache(block_start, pos, max_backward_limit,
                         starting_dist_cache, nodes, posdata.distance_cache);
    StartPosQueuePush(queue, &posdata);
  }

  min_len = ComputeMinimumCopyLength(queue, nodes, model, num_bytes, pos);

  /* Go over the command starting positions in order of increasing cost
     difference. */
  for (k = 0; k < max_iters && k < StartPosQueueSize(queue); ++k) {
    const PosData* posdata = StartPosQueueAt(queue, k);
    const size_t start = posdata->pos;
    const uint16_t inscode = GetInsertLengthCode(pos - start);
    const float start_costdiff = posdata->costdiff;
    const float base_cost = start_costdiff + (float)GetInsertExtra(inscode) +
        ZopfliCostModelGetLiteralCosts(model, 0, pos);

    /* Look for last distance matches using the distance cache from this
       starting position. */
    size_t best_len = min_len - 1;
    size_t j = 0;
    for (; j < BROTLI_NUM_DISTANCE_SHORT_CODES && best_len < max_len; ++j) {
      const size_t idx = kDistanceCacheIndex[j];
      const size_t backward =
          (size_t)(posdata->distance_cache[idx] + kDistanceCacheOffset[j]);
      size_t prev_ix = cur_ix - backward;
      if (prev_ix >= cur_ix) {
        continue;
      }
      if (PREDICT_FALSE(backward > max_distance)) {
        continue;
      }
      prev_ix &= ringbuffer_mask;

      if (cur_ix_masked + best_len > ringbuffer_mask ||
          prev_ix + best_len > ringbuffer_mask ||
          ringbuffer[cur_ix_masked + best_len] !=
              ringbuffer[prev_ix + best_len]) {
        continue;
      }
      {
        const size_t len =
            FindMatchLengthWithLimit(&ringbuffer[prev_ix],
                                     &ringbuffer[cur_ix_masked],
                                     max_len);
        const float dist_cost = base_cost +
            ZopfliCostModelGetDistanceCost(model, j);
        size_t l;
        for (l = best_len + 1; l <= len; ++l) {
          const uint16_t copycode = GetCopyLengthCode(l);
          const uint16_t cmdcode =
              CombineLengthCodes(inscode, copycode, j == 0);
          const float cost = (cmdcode < 128 ? base_cost : dist_cost) +
              (float)GetCopyExtra(copycode) +
              ZopfliCostModelGetCommandCost(model, cmdcode);
          if (cost < nodes[pos + l].u.cost) {
            UpdateZopfliNode(nodes, pos, start, l, l, backward, j + 1, cost);
          }
          best_len = l;
        }
      }
    }

    /* At higher iterations look only for new last distance matches, since
       looking only for new command start positions with the same distances
       does not help much. */
    if (k >= 2) continue;

    {
      /* Loop through all possible copy lengths at this position. */
      size_t len = min_len;
      for (j = 0; j < num_matches; ++j) {
        BackwardMatch match = matches[j];
        size_t dist = match.distance;
        int is_dictionary_match = (dist > max_distance) ? 1 : 0;
        /* We already tried all possible last distance matches, so we can use
           normal distance code here. */
        size_t dist_code = dist + 15;
        uint16_t dist_symbol;
        uint32_t distextra;
        uint32_t distnumextra;
        float dist_cost;
        size_t max_match_len;
        PrefixEncodeCopyDistance(dist_code, 0, 0, &dist_symbol, &distextra);
        distnumextra = distextra >> 24;
        dist_cost = base_cost + (float)distnumextra +
            ZopfliCostModelGetDistanceCost(model, dist_symbol);

        /* Try all copy lengths up until the maximum copy length corresponding
           to this distance. If the distance refers to the static dictionary, or
           the maximum length is long enough, try only one maximum length. */
        max_match_len = BackwardMatchLength(&match);
        if (len < max_match_len &&
            (is_dictionary_match || max_match_len > max_zopfli_len)) {
          len = max_match_len;
        }
        for (; len <= max_match_len; ++len) {
          const size_t len_code =
              is_dictionary_match ? BackwardMatchLengthCode(&match) : len;
          const uint16_t copycode = GetCopyLengthCode(len_code);
          const uint16_t cmdcode = CombineLengthCodes(inscode, copycode, 0);
          const float cost = dist_cost + (float)GetCopyExtra(copycode) +
              ZopfliCostModelGetCommandCost(model, cmdcode);
          if (cost < nodes[pos + len].u.cost) {
            UpdateZopfliNode(nodes, pos, start, len, len_code, dist, 0, cost);
          }
        }
      }
    }
  }
}

static size_t ComputeShortestPathFromNodes(size_t num_bytes,
    ZopfliNode* nodes) {
  size_t index = num_bytes;
  size_t num_commands = 0;
  while (nodes[index].u.cost == kInfinity) --index;
  nodes[index].u.next = BROTLI_UINT32_MAX;
  while (index != 0) {
    size_t len = ZopfliNodeCommandLength(&nodes[index]);
    index -= len;
    nodes[index].u.next = (uint32_t)len;
    num_commands++;
  }
  return num_commands;
}

void BrotliZopfliCreateCommands(const size_t num_bytes,
                                const size_t block_start,
                                const size_t max_backward_limit,
                                const ZopfliNode* nodes,
                                int* dist_cache,
                                size_t* last_insert_len,
                                Command* commands,
                                size_t* num_literals) {
  size_t pos = 0;
  uint32_t offset = nodes[0].u.next;
  size_t i;
  for (i = 0; offset != BROTLI_UINT32_MAX; i++) {
    const ZopfliNode* next = &nodes[pos + offset];
    size_t copy_length = ZopfliNodeCopyLength(next);
    size_t insert_length = next->insert_length;
    pos += insert_length;
    offset = next->u.next;
    if (i == 0) {
      insert_length += *last_insert_len;
      *last_insert_len = 0;
    }
    {
      size_t distance = ZopfliNodeCopyDistance(next);
      size_t len_code = ZopfliNodeLengthCode(next);
      size_t max_distance =
          BROTLI_MIN(size_t, block_start + pos, max_backward_limit);
      int is_dictionary = (distance > max_distance) ? 1 : 0;
      size_t dist_code = ZopfliNodeDistanceCode(next);

      InitCommand(
          &commands[i], insert_length, copy_length, len_code, dist_code);

      if (!is_dictionary && dist_code > 0) {
        dist_cache[3] = dist_cache[2];
        dist_cache[2] = dist_cache[1];
        dist_cache[1] = dist_cache[0];
        dist_cache[0] = (int)distance;
      }
    }

    *num_literals += insert_length;
    pos += copy_length;
  }
  *last_insert_len += num_bytes - pos;
}

static size_t ZopfliIterate(size_t num_bytes,
                            size_t position,
                            const uint8_t* ringbuffer,
                            size_t ringbuffer_mask,
                            const int quality,
                            const size_t max_backward_limit,
                            const int* dist_cache,
                            const ZopfliCostModel* model,
                            const uint32_t* num_matches,
                            const BackwardMatch* matches,
                            ZopfliNode* nodes) {
  const size_t max_zopfli_len = MaxZopfliLenForQuality(quality);
  StartPosQueue queue;
  size_t cur_match_pos = 0;
  size_t i;
  nodes[0].length = 0;
  nodes[0].u.cost = 0;
  InitStartPosQueue(&queue);
  for (i = 0; i + 3 < num_bytes; i++) {
    UpdateNodes(num_bytes, position, i, ringbuffer, ringbuffer_mask,
                quality, max_backward_limit, dist_cache, num_matches[i],
                &matches[cur_match_pos], model, &queue, nodes);
    cur_match_pos += num_matches[i];
    /* The zopflification can be too slow in case of very long lengths, so in
       such case skip it all, it does not cost a lot of compression ratio. */
    if (num_matches[i] == 1 &&
        BackwardMatchLength(&matches[cur_match_pos - 1]) > max_zopfli_len) {
      i += BackwardMatchLength(&matches[cur_match_pos - 1]) - 1;
      InitStartPosQueue(&queue);
    }
  }
  return ComputeShortestPathFromNodes(num_bytes, nodes);
}


size_t BrotliZopfliComputeShortestPath(MemoryManager* m,
                                       size_t num_bytes,
                                       size_t position,
                                       const uint8_t* ringbuffer,
                                       size_t ringbuffer_mask,
                                       const int quality,
                                       const size_t max_backward_limit,
                                       const int* dist_cache,
                                       H10* hasher,
                                       ZopfliNode* nodes) {
  const size_t max_zopfli_len = MaxZopfliLenForQuality(quality);
  ZopfliCostModel model;
  StartPosQueue queue;
  BackwardMatch matches[MAX_NUM_MATCHES_H10];
  const size_t store_end = num_bytes >= StoreLookaheadH10() ?
      position + num_bytes - StoreLookaheadH10() + 1 : position;
  size_t i;
  nodes[0].length = 0;
  nodes[0].u.cost = 0;
  InitZopfliCostModel(m, &model, num_bytes);
  if (BROTLI_IS_OOM(m)) return 0;
  ZopfliCostModelSetFromLiteralCosts(
      &model, position, ringbuffer, ringbuffer_mask);
  InitStartPosQueue(&queue);
  for (i = 0; i + HashTypeLengthH10() - 1 < num_bytes; i++) {
    const size_t pos = position + i;
    const size_t max_distance = BROTLI_MIN(size_t, pos, max_backward_limit);
    size_t num_matches = FindAllMatchesH10(hasher,
        ringbuffer, ringbuffer_mask, pos, num_bytes - i, max_distance,
        quality, matches);
    if (num_matches > 0 &&
        BackwardMatchLength(&matches[num_matches - 1]) > max_zopfli_len) {
      matches[0] = matches[num_matches - 1];
      num_matches = 1;
    }
    UpdateNodes(num_bytes, position, i, ringbuffer, ringbuffer_mask,
                quality, max_backward_limit, dist_cache, num_matches, matches,
                &model, &queue, nodes);
    if (num_matches == 1 && BackwardMatchLength(&matches[0]) > max_zopfli_len) {
      /* Add the tail of the copy to the hasher. */
      StoreRangeH10(hasher, ringbuffer, ringbuffer_mask, pos + 1, BROTLI_MIN(
          size_t, pos + BackwardMatchLength(&matches[0]), store_end));
      i += BackwardMatchLength(&matches[0]) - 1;
      InitStartPosQueue(&queue);
    }
  }
  CleanupZopfliCostModel(m, &model);
  return ComputeShortestPathFromNodes(num_bytes, nodes);
}

#define EXPAND_CAT(a, b) CAT(a, b)
#define CAT(a, b) a ## b
#define FN(X) EXPAND_CAT(X, HASHER())

#define HASHER() H2
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H3
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H4
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H5
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H6
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H7
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H8
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#define HASHER() H9
/* NOLINTNEXTLINE(build/include) */
#include "../../brotli/enc/backward_references_inc.h"
#undef HASHER

#undef FN
#undef CAT
#undef EXPAND_CAT

void BrotliCreateBackwardReferences(MemoryManager* m,
                                    size_t num_bytes,
                                    size_t position,
                                    int is_last,
                                    const uint8_t* ringbuffer,
                                    size_t ringbuffer_mask,
                                    const int quality,
                                    const int lgwin,
                                    Hashers* hashers,
                                    int hash_type,
                                    int* dist_cache,
                                    size_t* last_insert_len,
                                    Command* commands,
                                    size_t* num_commands,
                                    size_t* num_literals) {
  if (quality > 9) {  /* Zopflify. */
    H10* hasher = hashers->hash_h10;
    const size_t max_backward_limit = MaxBackwardLimit(lgwin);
    InitH10(m, hasher, ringbuffer, lgwin, position, num_bytes, is_last);
    if (BROTLI_IS_OOM(m)) return;
    StitchToPreviousBlockH10(hasher, num_bytes, position,
                             ringbuffer, ringbuffer_mask);
    /* Set maximum distance, see section 9.1. of the spec. */
    if (quality == 10) {
      ZopfliNode* nodes = BROTLI_ALLOC(m, ZopfliNode, num_bytes + 1);
      if (BROTLI_IS_OOM(m)) return;
      BrotliInitZopfliNodes(nodes, num_bytes + 1);
      *num_commands += BrotliZopfliComputeShortestPath(m, num_bytes, position,
          ringbuffer, ringbuffer_mask, quality, max_backward_limit, dist_cache,
          hasher, nodes);
      if (BROTLI_IS_OOM(m)) return;
      BrotliZopfliCreateCommands(num_bytes, position, max_backward_limit, nodes,
          dist_cache, last_insert_len, commands, num_literals);
      BROTLI_FREE(m, nodes);
      return;
    } else {
      uint32_t* num_matches = BROTLI_ALLOC(m, uint32_t, num_bytes);
      size_t matches_size = 4 * num_bytes;
      BackwardMatch* matches = BROTLI_ALLOC(m, BackwardMatch, matches_size);
      const size_t store_end = num_bytes >= StoreLookaheadH10() ?
          position + num_bytes - StoreLookaheadH10() + 1 : position;
      size_t cur_match_pos = 0;
      size_t i;
      size_t orig_num_literals;
      size_t orig_last_insert_len;
      int orig_dist_cache[4];
      size_t orig_num_commands;
      ZopfliCostModel model;
      ZopfliNode* nodes;
      if (BROTLI_IS_OOM(m)) return;
      for (i = 0; i + HashTypeLengthH10() - 1 < num_bytes; ++i) {
        const size_t pos = position + i;
        size_t max_distance = BROTLI_MIN(size_t, pos, max_backward_limit);
        size_t max_length = num_bytes - i;
        size_t num_found_matches;
        size_t cur_match_end;
        size_t j;
        /* Ensure that we have enough free slots. */
        BROTLI_ENSURE_CAPACITY(m, BackwardMatch, matches, matches_size,
            cur_match_pos + MAX_NUM_MATCHES_H10);
        if (BROTLI_IS_OOM(m)) return;
        num_found_matches = FindAllMatchesH10(hasher, ringbuffer,
            ringbuffer_mask, pos, max_length, max_distance, quality,
            &matches[cur_match_pos]);
        cur_match_end = cur_match_pos + num_found_matches;
        for (j = cur_match_pos; j + 1 < cur_match_end; ++j) {
          assert(BackwardMatchLength(&matches[j]) <
              BackwardMatchLength(&matches[j + 1]));
          assert(matches[j].distance > max_distance ||
                 matches[j].distance <= matches[j + 1].distance);
        }
        num_matches[i] = (uint32_t)num_found_matches;
        if (num_found_matches > 0) {
          const size_t match_len =
              BackwardMatchLength(&matches[cur_match_end - 1]);
          if (match_len > kMaxZopfliLenQuality11) {
            const size_t skip = match_len - 1;
            matches[cur_match_pos++] = matches[cur_match_end - 1];
            num_matches[i] = 1;
            /* Add the tail of the copy to the hasher. */
            StoreRangeH10(hasher, ringbuffer, ringbuffer_mask, pos + 1,
                          BROTLI_MIN(size_t, pos + match_len, store_end));
            memset(&num_matches[i + 1], 0, skip * sizeof(num_matches[0]));
            i += skip;
          } else {
            cur_match_pos = cur_match_end;
          }
        }
      }
      orig_num_literals = *num_literals;
      orig_last_insert_len = *last_insert_len;
      memcpy(orig_dist_cache, dist_cache, 4 * sizeof(dist_cache[0]));
      orig_num_commands = *num_commands;
      nodes = BROTLI_ALLOC(m, ZopfliNode, num_bytes + 1);
      if (BROTLI_IS_OOM(m)) return;
      InitZopfliCostModel(m, &model, num_bytes);
      if (BROTLI_IS_OOM(m)) return;
      for (i = 0; i < 2; i++) {
        BrotliInitZopfliNodes(nodes, num_bytes + 1);
        if (i == 0) {
          ZopfliCostModelSetFromLiteralCosts(
              &model, position, ringbuffer, ringbuffer_mask);
        } else {
          ZopfliCostModelSetFromCommands(&model, position, ringbuffer,
              ringbuffer_mask, commands, *num_commands - orig_num_commands,
              orig_last_insert_len);
        }
        *num_commands = orig_num_commands;
        *num_literals = orig_num_literals;
        *last_insert_len = orig_last_insert_len;
        memcpy(dist_cache, orig_dist_cache, 4 * sizeof(dist_cache[0]));
        *num_commands += ZopfliIterate(num_bytes, position, ringbuffer,
            ringbuffer_mask, quality, max_backward_limit, dist_cache, &model,
            num_matches, matches, nodes);
        BrotliZopfliCreateCommands(num_bytes, position, max_backward_limit,
            nodes, dist_cache, last_insert_len, commands, num_literals);
      }
      CleanupZopfliCostModel(m, &model);
      BROTLI_FREE(m, nodes);
      BROTLI_FREE(m, matches);
      BROTLI_FREE(m, num_matches);
    }
    return;
  }

  switch (hash_type) {
    case 2:
      CreateBackwardReferencesH2(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h2, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 3:
      CreateBackwardReferencesH3(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h3, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 4:
      CreateBackwardReferencesH4(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h4, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 5:
      CreateBackwardReferencesH5(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h5, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 6:
      CreateBackwardReferencesH6(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h6, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 7:
      CreateBackwardReferencesH7(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h7, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 8:
      CreateBackwardReferencesH8(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h8, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    case 9:
      CreateBackwardReferencesH9(
          m, num_bytes, position, is_last, ringbuffer, ringbuffer_mask,
          quality, lgwin, hashers->hash_h9, dist_cache,
          last_insert_len, commands, num_commands, num_literals);
      break;
    default:
      break;
  }
  if (BROTLI_IS_OOM(m)) return;
}

#if defined(__cplusplus) || defined(c_plusplus)
}  /* extern "C" */
#endif
