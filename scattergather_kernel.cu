/* Copyright 2019 Stanford
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

#include "gnn.h"
#include "cuda_helper.h"
#include <cub/cub.cuh>

__global__
void copy_kernel(DATATYPE* dst,
                 const DATATYPE* src,
                 size_t size)
{
  for (size_t i = blockIdx.x * blockDim.x + threadIdx.x; i < size;
       i+= blockDim.x * gridDim.x)
  {
    dst[i] = src[i];
  }
}

__global__
void aggre_coop_kernel(V_ID rowLeft,
                       V_ID rowRight,
                       E_ID colLeft,
                       int hiddenDim,
                       const NodeStruct* row_ptrs,
                       const EdgeStruct* col_idxs,
                       const DATATYPE* input,
                       DATATYPE* output)
{
  assert(blockDim.x % hiddenDim == 0);
  //assert(aggrType == AGGR_SUM || aggrType == AGGR_AVG);
  int vtxPerBlock = blockDim.x / hiddenDim;
  typedef cub::BlockScan<E_ID, CUDA_NUM_THREADS> BlockScan;
  __shared__ BlockScan::TempStorage temp_storage;
  __shared__ E_ID blkColStart;
  __shared__ DATATYPE acc_h[CUDA_NUM_THREADS];
  int tidDiv = threadIdx.x / hiddenDim;
  int tidMod = threadIdx.x % hiddenDim;
  for (V_ID blkRowStart = blockIdx.x * vtxPerBlock + rowLeft;
       blkRowStart <= rowRight;
       blkRowStart += vtxPerBlock * gridDim.x)
  {
    E_ID myNumEdges = 0, scratchOffset, totalNumEdges = 0;
    if (threadIdx.x + blkRowStart <= rowRight && threadIdx.x < vtxPerBlock) {
      V_ID curVtx = threadIdx.x + blkRowStart;
      E_ID startColIdx, endColIdx = row_ptrs[curVtx-rowLeft].index;
      if (curVtx == rowLeft)
        startColIdx = colLeft;
      else
        startColIdx = row_ptrs[curVtx-rowLeft-1].index;
      myNumEdges = endColIdx - startColIdx;
      if (threadIdx.x == 0)
        blkColStart = startColIdx;
    }
    //if (myNumEdges > 0) printf("tid(%d) myNumEdges(%d)\n", threadIdx.x, myNumEdges);
    acc_h[threadIdx.x] = 0.0f;
    __syncthreads();
    BlockScan(temp_storage).ExclusiveSum(myNumEdges, scratchOffset, totalNumEdges);
    E_ID done = 0;
    while (totalNumEdges > 0) {
      if (tidDiv < totalNumEdges) {
        EdgeStruct es = col_idxs[blkColStart + done + tidDiv - colLeft];
        DATATYPE val = input[es.src * hiddenDim + tidMod];
        int offset = (es.dst - blkRowStart) * hiddenDim + tidMod;
        atomicAdd(&acc_h[offset], val);
      }
      done += vtxPerBlock;
      totalNumEdges -= (totalNumEdges > vtxPerBlock) ? vtxPerBlock : totalNumEdges;
    }
    __syncthreads();
    if (tidDiv + blkRowStart <= rowRight) {
      output[(blkRowStart-rowLeft)*hiddenDim+threadIdx.x] = acc_h[threadIdx.x];
    }
  }
}

__host__
void ScatterGather::forward_task(const Task *task,
                                 const std::vector<PhysicalRegion> &regions,
                                 Context ctx, Runtime *runtime)
{
  assert(regions.size() == 4);
  assert(task->regions.size() == 4);
  const AccessorRO<NodeStruct, 1> accRowPtr(regions[0], FID_DATA);
  const AccessorRO<EdgeStruct, 1> accColIdx(regions[1], FID_DATA);
  const AccessorRO<DATATYPE, 2> accInput(regions[2], FID_DATA);
  const AccessorWO<DATATYPE, 2> accOutput(regions[3], FID_DATA);
  Rect<1> rectRowPtr = runtime->get_index_space_domain(
                             ctx, task->regions[0].region.get_index_space());
  Rect<1> rectColIdx = runtime->get_index_space_domain(
                             ctx, task->regions[1].region.get_index_space());
  Rect<2> rectInput = runtime->get_index_space_domain(
                            ctx, task->regions[2].region.get_index_space());
  Rect<2> rectOutput = runtime->get_index_space_domain(
                            ctx, task->regions[3].region.get_index_space());
  assert(accRowPtr.accessor.is_dense_arbitrary(rectRowPtr));
  assert(accColIdx.accessor.is_dense_arbitrary(rectColIdx));
  assert(accInput.accessor.is_dense_arbitrary(rectInput));
  assert(accOutput.accessor.is_dense_arbitrary(rectOutput));
  const NodeStruct* rowPtrs = accRowPtr.ptr(rectRowPtr);
  const EdgeStruct* colIdxs = accColIdx.ptr(rectColIdx);
  const DATATYPE* zcInput = accInput.ptr(rectInput);
  DATATYPE* zcOutput = accOutput.ptr(rectOutput);
  V_ID rowLeft = rectRowPtr.lo[0], rowRight = rectRowPtr.hi[0];
  E_ID colLeft = rectColIdx.lo[0], colRight = rectColIdx.hi[0];
  int hiddenDim = rectInput.hi[0]-rectInput.lo[0]+1;
  assert(hiddenDim == rectOutput.hi[0]-rectOutput.lo[0]+1);
  assert(rectOutput.lo[1] == rectRowPtr.lo[0]);
  assert(rectOutput.hi[1] == rectRowPtr.hi[0]);

  //const ScatterGather* op = (ScatterGather*) task->args;
  ResourceManager* manager = *((ResourceManager**) task->local_args);
  assert(manager->proc_id == task->current_proc.id);
  std::set<int> assigned;
  int inputId = manager->assign(regions[2].get_logical_region(),
                                rectInput.volume(), assigned);
  int outputId = manager->assign(regions[3].get_logical_region(),
                                 rectOutput.volume(), assigned);
  copy_kernel<<<GET_BLOCKS(rectInput.volume()), CUDA_NUM_THREADS>>>(
      manager->fbCache[inputId].ptr, zcInput, rectInput.volume());
  aggre_coop_kernel<<<GET_BLOCKS(rectOutput.volume()), CUDA_NUM_THREADS>>>(
      rowLeft, rowRight, colLeft, hiddenDim, rowPtrs, colIdxs,
      manager->fbCache[inputId].ptr, manager->fbCache[outputId].ptr);
  // Need to copy results back to new_pr
  cudaDeviceSynchronize();
  checkCUDA(cudaMemcpy(zcOutput, manager->fbCache[outputId].ptr,
                       rectOutput.volume() * sizeof(DATATYPE),
                       cudaMemcpyDeviceToHost));
  //copy_kernel<<<GET_BLOCKS(rectOutput.volume()), CUDA_NUM_THREADS>>>(
  //    zcOuptut, manager->fbCache[outputId], rectOutput.volume());
}

__host__
void ScatterGather::backward_task(const Task *task,
                                  const std::vector<PhysicalRegion> &regions,
                                  Context ctx, Runtime *runtime)
{
  // Forward and backward do exact same thing
  return forward_task(task, regions, ctx, runtime);
}

__host__
void ScatterGather::update_task(const Task *task,
                                const std::vector<PhysicalRegion> &regions,
                                Context ctx, Runtime *runtime)
{
}                         
