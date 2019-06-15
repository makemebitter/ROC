/* Copyright 2019 Stanford, UT Austin, LANL
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

#include "legion.h"
#include "gnn.h"
#include "gnn_mapper.h"

LegionRuntime::Logger::Category log_gnn("gnn");

void parse_input_args(char **argv, int argc,
                      Config& config);

void top_level_task(const Task *task,
                    const std::vector<PhysicalRegion> &regions,
                    Context ctx,
                    Runtime *runtime)
{
  Config config;
  config.numGPUs = 0;
  config.filename = "";
  config.verbose = false;
  {
    const InputArgs &command_args = HighLevelRuntime::get_input_args();
    char **argv = command_args.argv;
    int argc = command_args.argc;
    parse_input_args(argv, argc, config);
    log_gnn.print("GNN settings: filename = %s", config.filename.c_str());
    config.numMachines = Realm::Machine::get_machine().get_address_space_count();
    config.totalGPUs = config.numMachines * config.numGPUs;
    assert(config.totalGPUs > 0);
  }
  Graph graph(ctx, runtime, config);
  // Model Construction
  Model model(graph, ctx, runtime, 64);
  Tensor t = model.get_input_tensor();
  for (int i = 0; i < 2; i++) {
    t = model.scatter_gather(t);
    t = model.indegree_norm(t);
  }
  model.init(config);
  for (int i = 0; i < 1; i++) {
    model.forward();
  }
}

void parse_input_args(char **argv, int argc, Config& config)
{
  for (int i = 1; i <argc; i++)
  {
    if ((!strcmp(argv[i], "-ng")) || (!strcmp(argv[i], "-ll:gpu"))) 
    {
      config.numGPUs = atoi(argv[++i]);
      continue;
    }
    if (!strcmp(argv[i], "-file"))
    {
      config.filename = std::string(argv[++i]);
      continue;
    }
    if ((!strcmp(argv[i], "-verbose")) || (!strcmp(argv[i], "-v")))
    {
      config.verbose = true;
      continue;
    }
  }
}

static void update_mappers(Machine machine, Runtime *rt,
                           const std::set<Processor> &local_procs)
{
  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    rt->replace_default_mapper(new GnnMapper(machine, rt, *it), *it);
  }
}

int main(int argc, char **argv)
{
  Runtime::set_top_level_task_id(TOP_LEVEL_TASK_ID);
  {
    TaskVariantRegistrar registrar(TOP_LEVEL_TASK_ID, "top_level");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    Runtime::preregister_task_variant<top_level_task>(registrar, "top_level");
  }
  {
    TaskVariantRegistrar registrar(NCCL_TASK_ID, "nccl_task");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<NcclInfo, nccl_task_impl>(
        registrar,  "nccl_task");
  }
  {
    TaskVariantRegistrar registrar(LOAD_TASK_ID, "load_graph");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<load_graph_impl>(
        registrar, "load_graph");
  }
  {
    TaskVariantRegistrar registrar(INIT_TASK_ID, "init_task");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<ResourceManager*, init_task_impl>(
        registrar, "init_task");
  }
  // scattergather op
  {
    TaskVariantRegistrar registrar(SCATTERGATHER_FWD_TASK_ID,
                                   "ScatterGather Forward");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<ScatterGather::forward_task>(
        registrar, "ScatterGather Forward Task");
  }
  {
    TaskVariantRegistrar registrar(SCATTERGATHER_BWD_TASK_ID,
                                   "ScatterGather Backward");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<ScatterGather::backward_task>(
        registrar, "ScatterGather Backward Task");
  }
  {
    TaskVariantRegistrar registrar(SCATTERGATHER_UPD_TASK_ID,
                                   "ScatterGather Update");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<ScatterGather::update_task>(
        registrar, "ScatterGather Update Task");
  }
  // InDegreeNorm
  {
    TaskVariantRegistrar registrar(INDEGREENORM_FWD_TASK_ID,
                                   "InDegreeNorm Forward");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<InDegreeNorm::forward_task>(
        registrar, "InDegreeNorm Forward Task");
  }
  {
    TaskVariantRegistrar registrar(INDEGREENORM_BWD_TASK_ID,
                                   "InDegreeNorm Backward");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<InDegreeNorm::backward_task>(
        registrar, "InDegreeNorm Backward Task");
  }
  {
    TaskVariantRegistrar registrar(INDEGREENORM_UPD_TASK_ID,
                                   "InDegreeNorm Update");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<InDegreeNorm::update_task>(
        registrar, "InDegreeNorm Update Task");
  }

  Runtime::add_registration_callback(update_mappers);

  return Runtime::start(argc, argv);
}

GnnOp::GnnOp(const Tensor& _input)
: numInputs(0)
{
  inputs[0] = _input;
}

Model::Model(const Graph& _graph,
             Context _ctx,
             Runtime* _runtime,
             int _inputDim)
: myGraph(_graph), ctx(_ctx), runtime(_runtime),
numParts(_graph.numParts)
{
  input = create_node_tensor(_inputDim);
  Rect<1> task_rect(0, numParts-1);
  taskIS = runtime->create_index_space(ctx, task_rect);
}

Tensor Model::create_node_tensor(int numHidden) const
{
  Tensor t(Tensor::NODE_TENSOR);
  t.numDim = 2;
  t.dims[0] = numHidden;
  t.dims[1] = myGraph.numNodes;
  Rect<1> rectRowPtr = runtime->get_index_space_domain(
                             ctx, myGraph.rowPtrLR.get_index_space());
  // Assert row_ptr_lr matches myGraph.numNodes
  assert(rectRowPtr.volume() == myGraph.numNodes);
  Rect<2> outputRect(Point<2>(0, 0), Point<2>(numHidden-1, myGraph.numNodes-1));
  printf("outputRect: lo(%lld %lld) hi(%lld %lld)\n", outputRect.lo[0], 
         outputRect.lo[1], outputRect.hi[0], outputRect.hi[1]);
  IndexSpaceT<2> outputIS =
      runtime->create_index_space(ctx, outputRect);
  runtime->attach_name(outputIS, "activation_index_space");
  // Create logical regions
  {
    FieldSpace outputFS = runtime->create_field_space(ctx);
    FieldAllocator allocator = runtime->create_field_allocator(ctx, outputFS);
    allocator.allocate_field(sizeof(DATATYPE), FID_DATA);
    t.region = runtime->create_logical_region(ctx, outputIS, outputFS);
    t.region_grad = runtime->create_logical_region(ctx, outputIS, outputFS);
  }
  // Create logical partitions
  {
    Rect<1> color_rect(Point<1>(0), Point<1>(numParts-1));
    LegionRuntime::Arrays::Rect<1> color_array(
        LegionRuntime::Arrays::Point<1>(0),
        LegionRuntime::Arrays::Point<1>(numParts-1));
    Domain color_domain = Domain::from_rect<1>(color_array);
    DomainColoring output_coloring;
    for (PointInRectIterator<1> it(color_rect); it(); it++) {
      LogicalRegion sub_lr = runtime->get_logical_subregion_by_color(
          ctx, myGraph.rowPtrLP, DomainPoint(*it));
      Rect<1> sub_rect = runtime->get_index_space_domain(
          ctx, sub_lr.get_index_space());
      LegionRuntime::Arrays::Rect<2> output_subrect(
          LegionRuntime::Arrays::Point<2>(
              LegionRuntime::Arrays::make_point(0, sub_rect.lo[0])),
          LegionRuntime::Arrays::Point<2>(
              LegionRuntime::Arrays::make_point(numHidden-1, sub_rect.hi[0])));
      output_coloring[*it] = Domain::from_rect<2>(output_subrect);
      printf("lo(%lld %lld) hi(%lld %lld)\n", output_subrect.lo[0],
             output_subrect.lo[1], output_subrect.hi[0], output_subrect.hi[1]);
    }
    IndexPartition output_ip = runtime->create_index_partition(
        ctx, outputIS, color_domain, output_coloring, true);
    assert(runtime->is_index_partition_disjoint(ctx, output_ip));
    assert(runtime->is_index_partition_complete(ctx, output_ip));
    t.part = runtime->get_logical_partition(ctx, t.region, output_ip);
    t.part_grad = runtime->get_logical_partition(ctx, t.region_grad, output_ip);
  }
  return t;
}

bool Model::init(const Config& config)
{
  ArgumentMap local_args;
  FutureMap fm;
  // Init NCCL
#ifdef DEADCODE
  assert(config.numMachines <= MAX_NUM_MACHINES);
  Rect<1> nccl_rect(Point<1>(0), Point<1>(config.numMachines-1));
  IndexSpaceT<1> ncclIS = runtime->create_index_space(ctx, nccl_rect);
  NcclTask nccl_task(myGraph, ncclIS, local_args);
  fm = runtime->execute_index_space(ctx, nccl_task);
  fm.wait_all_results();
  for (PointInRectIterator<1> it(nccl_rect); it(); it++) {
    NcclInfo info = fm.get_result<NcclInfo>(*it);
    for (int i = 0; i < config.numGPUs; i++)
      myGraph.nccl[(*it)*config.numGPUs+i] = info.comms[i];
  }
#endif
  // Load graphs
  myGraph.maxHidden = input.dims[0];
  for (size_t i = 0; i < layers.size(); i++)
    for (int j = 0; j < layers[i]->numOutputs; j++) {
      assert(layers[i]->outputs[j].numDim == 2);
      myGraph.maxHidden = std::max(myGraph.maxHidden,
                                   (int)layers[i]->outputs[j].dims[0]);
    }
  LoadTask load_task(myGraph, taskIS, local_args, config.filename);
  fm = runtime->execute_index_space(ctx, load_task);
  fm.wait_all_results();
  InitTask init_task(myGraph, get_input_tensor(), taskIS, local_args);
  fm = runtime->execute_index_space(ctx, init_task);
  fm.wait_all_results();
  Rect<1> task_rect = runtime->get_index_space_domain(ctx, taskIS);
  for (PointInRectIterator<1> it(task_rect); it(); it++) {
    ResourceManager* manager = fm.get_result<ResourceManager*>(*it);
    taskArgs.set_point(*it, TaskArgument(&manager, sizeof(ResourceManager*)));
  }

  return true;
}

void Model::forward(void)
{
  for (size_t l = 0; l < layers.size(); l++)
    layers[l]->forward(*this);
}

Tensor Model::get_input_tensor(void) const
{
  return input;
}

Graph::Graph(Context ctx,
             Runtime* runtime,
             const Config& config)
: numParts(config.totalGPUs), numMachines(config.numMachines)
{
  FILE* fd = fopen(config.filename.c_str(), "rb");
  assert(fd != NULL);
  size_t fread_ret = fread(&numNodes, sizeof(V_ID), 1, fd);
  assert(fread_ret == 1);
  fread_ret = fread(&numEdges, sizeof(E_ID), 1, fd);
  assert(fread_ret == 1);
  log_gnn.print("Load graph: numNodes(%u) numEdges(%zu)", numNodes, numEdges);
  Rect<1> vtx_rect(Point<1>(0), Point<1>(numNodes - 1));
  IndexSpaceT<1> vtx_is =
    runtime->create_index_space(ctx, vtx_rect);
  runtime->attach_name(vtx_is, "vertices_index_space");
  Rect<1> edge_rect(Point<1>(0), Point<1>(numEdges - 1));
  IndexSpaceT<1> edge_is =
    runtime->create_index_space(ctx, edge_rect);
  runtime->attach_name(edge_is, "edges_index_space");

  FieldSpace row_ptr_fs = runtime->create_field_space(ctx);
  runtime->attach_name(row_ptr_fs, "row_ptrs(NodeStruct)");
  FieldSpace raw_row_fs = runtime->create_field_space(ctx);
  runtime->attach_name(raw_row_fs, "raw_rows(E_ID)");
  FieldSpace col_idx_fs = runtime->create_field_space(ctx);
  runtime->attach_name(col_idx_fs, "col_idxs(EdgeStruct)");
  FieldSpace raw_col_fs = runtime->create_field_space(ctx);
  runtime->attach_name(raw_col_fs, "raw_cols(V_ID)");

  // Allocate fields
  alloc_fs<NodeStruct>(ctx, runtime, row_ptr_fs);
  alloc_fs<E_ID>(ctx, runtime, raw_row_fs);
  alloc_fs<EdgeStruct>(ctx, runtime, col_idx_fs);
  alloc_fs<V_ID>(ctx, runtime, raw_col_fs);

  // Make logical regions
  rowPtrLR = runtime->create_logical_region(ctx, vtx_is, row_ptr_fs);
  rawRowLR = runtime->create_logical_region(ctx, vtx_is, raw_row_fs);
  colIdxLR = runtime->create_logical_region(ctx, edge_is, col_idx_fs);
  rawColLR = runtime->create_logical_region(ctx, edge_is, raw_col_fs);

  E_ID* raw_rows = (E_ID*) malloc(numNodes * sizeof(E_ID));
  //double ts_start = Realm::Clock::current_time_in_microseconds();
  assert(fread(raw_rows, sizeof(E_ID), (size_t)numNodes, fd) == (size_t)numNodes);
  for (V_ID v = 1; v < numNodes; v++)
    assert(raw_rows[v] >= raw_rows[v-1]);
  assert(raw_rows[numNodes-1] == numEdges);
  fclose(fd);

  // Partition the graph
  //double ts_mid = Realm::Clock::current_time_in_microseconds();
  //printf("Loading time = %.2lfus\n", ts_mid - ts_start);
  V_ID left_bound = 0;
  E_ID edge_cnt = 0;
  E_ID edge_cap = (numEdges + numParts - 1) / numParts;
  std::vector<std::pair<V_ID, V_ID> > bounds;
  for (V_ID v = 0; v < numNodes; v++)
  {
    if (v == 0)
      edge_cnt += raw_rows[v];
    else
      edge_cnt += raw_rows[v] - raw_rows[v-1];
    if (edge_cnt > edge_cap)
    {
      bounds.push_back(std::make_pair(left_bound, v));
      edge_cnt = 0;
      left_bound = v + 1;
    }
  }
  if (edge_cnt > 0)
  {
    bounds.push_back(std::make_pair(left_bound, numNodes - 1));
  }
  //double ts_end = Realm::Clock::current_time_in_microseconds();
  //printf("Partitioning time = %.2lfus\n", ts_end - ts_mid);
  assert(bounds.size() == (size_t)numParts);

  // First, we partition the vertices
  LegionRuntime::Arrays::Rect<1> color_rect(
      LegionRuntime::Arrays::Point<1>(0),
      LegionRuntime::Arrays::Point<1>(numParts - 1));
  Domain color_domain = Domain::from_rect<1>(color_rect);
  {
    DomainColoring pvt_vtx_coloring;
    for (int color = 0; color < numParts; color++)
    {
      LegionRuntime::Arrays::Rect<1> subrect_pvt(
          LegionRuntime::Arrays::Point<1>(bounds[color].first),
          LegionRuntime::Arrays::Point<1>(bounds[color].second));
      pvt_vtx_coloring[color] = Domain::from_rect<1>(subrect_pvt);
    }
    IndexPartition vtx_ip
      = runtime->create_index_partition(ctx, vtx_is, color_domain,
                                        pvt_vtx_coloring, true);
    rowPtrLP = runtime->get_logical_partition(ctx, rowPtrLR, vtx_ip);
    rawRowLP = runtime->get_logical_partition(ctx, rawRowLR, vtx_ip);
  }
  // Second, we partition the edges
  {
    DomainColoring edges_coloring;
    E_ID index = 0;
    for (int color = 0; color < numParts; color++)
    {
      log_gnn.print("left_bound = %u right_bound = %u",
                    bounds[color].first, bounds[color].second);
      LegionRuntime::Arrays::Rect<1> subrect(
          LegionRuntime::Arrays::Point<1>(index),
          LegionRuntime::Arrays::Point<1>(raw_rows[bounds[color].second]- 1));
      index = raw_rows[bounds[color].second];
      edges_coloring[color] = Domain::from_rect<1>(subrect);
    }
    IndexPartition col_idx_ip
      = runtime->create_index_partition(ctx, edge_is, color_domain,
                                        edges_coloring, true);
    colIdxLP = runtime->get_logical_partition(ctx, colIdxLR, col_idx_ip);
    rawColLP = runtime->get_logical_partition(ctx, rawColLR, col_idx_ip);
  }
  free(raw_rows);
}