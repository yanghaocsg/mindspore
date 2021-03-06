/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>
#include <string>
#include <memory>
#include <algorithm>
#include "device/gpu/gpu_common.h"
#include "device/gpu/kernel_info_setter.h"
#include "device/gpu/gpu_device_manager.h"
#include "device/gpu/gpu_stream_assign.h"

namespace mindspore {
namespace device {
namespace gpu {
void AssignGpuStream(const std::shared_ptr<session::KernelGraph> &kernel_graph) {
  MS_EXCEPTION_IF_NULL(kernel_graph);
  std::vector<CNodePtr> allreduce_kernels;
  auto execution_kernels = kernel_graph->execution_order();
  for (auto kernel_node : execution_kernels) {
    std::string kernel_name = AnfAlgo::GetCNodeName(kernel_node);
    if (kernel_name == kAllReduceOpName) {
      allreduce_kernels.emplace_back(kernel_node);
    } else {
      DeviceStream compute_stream = GPUDeviceManager::GetInstance().default_stream();
      AnfAlgo::SetNodeAttr("stream_id", MakeValue(reinterpret_cast<uintptr_t>(compute_stream)), kernel_node);
    }
  }
  if (allreduce_kernels.size() > 1) {
    DeviceStream comm_stream = nullptr;
    GPUDeviceManager::GetInstance().CreateStream(&comm_stream);
    std::transform(
      allreduce_kernels.begin(), allreduce_kernels.end(), allreduce_kernels.begin(), [&](CNodePtr allreduce_kernel) {
        AnfAlgo::SetNodeAttr("stream_id", MakeValue(reinterpret_cast<uintptr_t>(comm_stream)), allreduce_kernel);
        return allreduce_kernel;
      });

    std::vector<SendRecvPair> send_recv_pairs;
    FindAllReduceStreamSwitchPos(kernel_graph, &send_recv_pairs);
    InsertStreamSwitchNode(kernel_graph, send_recv_pairs);
  }
}

void FindAllReduceStreamSwitchPos(const std::shared_ptr<session::KernelGraph> &kernel_graph,
                                  std::vector<SendRecvPair> *send_recv_pairs) {
  auto execution_kernels = kernel_graph->execution_order();
  std::vector<CNodePtr>::iterator iter, iter_begin;
  iter = iter_begin = execution_kernels.begin();
  std::vector<CNodePtr>::iterator iter_end = execution_kernels.end();
  for (; iter != execution_kernels.end(); ++iter) {
    std::string kernel_name = AnfAlgo::GetCNodeName(*iter);
    if (kernel_name == kAllReduceOpName) {
      // Find AllReduce node's last input node.
      std::vector<CNodePtr>::iterator mock_send_node_iter =
        FindSendNodePos(iter_begin, iter + 1, *iter, kAllReduceStreamSwitch);
      if (mock_send_node_iter == iter + 1) {
        MS_LOG(WARNING) << "Can't find send node place before AllReduce node.";
        continue;
      }
      SendRecvPair pair1 = {kAllReduceStreamSwitch, *mock_send_node_iter, *iter,
                            IntToSize(mock_send_node_iter - iter_begin + 1), IntToSize(iter - iter_begin)};
      send_recv_pairs->push_back(pair1);
      // Find node which uses AllReduce as input[0].
      std::vector<CNodePtr>::iterator mock_recv_node_iter =
        FindRecvNodePos(iter, iter_end, *iter, kAllReduceStreamSwitch);
      if (mock_recv_node_iter == iter_end) {
        MS_LOG(WARNING) << "Can't find send node place before AllReduce node.";
        continue;
      }
      SendRecvPair pair2 = {kAllReduceStreamSwitch, *iter, *mock_recv_node_iter, IntToSize(iter - iter_begin + 1),
                            IntToSize(mock_recv_node_iter - iter_begin)};
      send_recv_pairs->push_back(pair2);
    }
  }
}

std::vector<CNodePtr>::iterator FindSendNodePos(std::vector<CNodePtr>::iterator begin,
                                                std::vector<CNodePtr>::iterator end, const CNodePtr mock_recv_node,
                                                StreamSwitchType stream_switch_type) {
  MS_EXCEPTION_IF_NULL(mock_recv_node);
  if (stream_switch_type == kAllReduceStreamSwitch) {
    for (auto iter = begin; iter != end; iter++) {
      if (*(iter + 1) == mock_recv_node) {
        return iter;
      }
    }
  }
  return end;
}

std::vector<CNodePtr>::iterator FindRecvNodePos(std::vector<CNodePtr>::iterator begin,
                                                std::vector<CNodePtr>::iterator end, const CNodePtr mock_send_node,
                                                StreamSwitchType stream_switch_type) {
  MS_EXCEPTION_IF_NULL(mock_send_node);
  for (auto iter = begin; iter != end; iter++) {
    auto node = *iter;
    if (stream_switch_type == kAllReduceStreamSwitch) {
      for (auto input : node->inputs()) {
        if (mock_send_node == AnfAlgo::VisitKernel(input, 0).first) {
          return iter;
        }
      }
    }
  }
  return end;
}

void InsertStreamSwitchNode(const std::shared_ptr<session::KernelGraph> &kernel_graph,
                            const std::vector<SendRecvPair> &send_recv_pairs) {
  std::set<StreamSwitchNode> ordered_stream_switch_nodes;
  for (SendRecvPair pair : send_recv_pairs) {
    StreamSwitchType stream_switch_type = pair.stream_switch_type;
    CNodePtr mock_send_node = pair.mock_send_node;
    CNodePtr mock_recv_node = pair.mock_recv_node;
    size_t send_node_offset = pair.send_node_offset;
    size_t recv_node_offset = pair.recv_node_offset;
    CNodePtr send_node = nullptr;
    CNodePtr recv_node = nullptr;
    // Step 1: generate Send and Recv CNodes.
    if (stream_switch_type == kAllReduceStreamSwitch) {
      if (!GenSendRecvCNodesForAllReduce(kernel_graph, mock_send_node, mock_recv_node, &send_node, &recv_node)) {
        MS_LOG(EXCEPTION) << "Generating CNodes for send and recv failed. Stream switch type: kAllReduceStreamSwitch";
      }
    }
    // Step 2: sort send and recv CNodes by offset.
    ordered_stream_switch_nodes.insert({send_node_offset, send_node});
    ordered_stream_switch_nodes.insert({recv_node_offset, recv_node});
  }
  // Step 3: insert stream switch CNodes into execution kernel list.
  auto execution_kernels = kernel_graph->execution_order();
  for (auto node = ordered_stream_switch_nodes.rbegin(); node != ordered_stream_switch_nodes.rend(); node++) {
    execution_kernels.insert(execution_kernels.begin() + node->offset, node->cnode);
  }
  kernel_graph->set_execution_order(execution_kernels);
}

bool GenSendRecvCNodesForAllReduce(const std::shared_ptr<session::KernelGraph> &kernel_graph,
                                   const CNodePtr &mock_send_node, const CNodePtr &mock_recv_node, CNodePtr *send_node,
                                   CNodePtr *recv_node) {
  *send_node = CreateStreamSwitchNode(kernel_graph, kSendOpName);
  MS_EXCEPTION_IF_NULL(*send_node);
  *recv_node = CreateStreamSwitchNode(kernel_graph, kRecvOpName);
  MS_EXCEPTION_IF_NULL(*recv_node);

  cudaEvent_t event = nullptr;
  CHECK_CUDA_RET_WITH_EXCEPT(cudaEventCreate(&event, cudaEventDisableTiming), "Creating cuda event failed.");
  AnfAlgo::SetNodeAttr("record_event", MakeValue(reinterpret_cast<uintptr_t>(event)), *send_node);
  AnfAlgo::SetNodeAttr("wait_event", MakeValue(reinterpret_cast<uintptr_t>(event)), *recv_node);

  uintptr_t send_stream = AnfAlgo::GetNodeAttr<uintptr_t>(mock_send_node, "stream_id");
  AnfAlgo::SetNodeAttr("record_event_stream", MakeValue(send_stream), *send_node);
  uintptr_t recv_stream = AnfAlgo::GetNodeAttr<uintptr_t>(mock_recv_node, "stream_id");
  AnfAlgo::SetNodeAttr("wait_event_stream", MakeValue(recv_stream), *recv_node);
  return true;
}

CNodePtr CreateStreamSwitchNode(const std::shared_ptr<session::KernelGraph> &kernel_graph, const std::string &name) {
  auto op = std::make_shared<Primitive>(name);
  auto apply = std::make_shared<ValueNode>(op);
  std::vector<AnfNodePtr> input_list = {apply};
  CNodePtr node = kernel_graph->NewCNode(input_list);
  MS_EXCEPTION_IF_NULL(node);
  kernel::KernelBuildInfo::KernelBuildInfoBuilder selected_kernel_builder;
  AnfAlgo::SetSelectKernelBuildInfo(selected_kernel_builder.Build(), node.get());
  auto abstract_none = std::make_shared<abstract::AbstractNone>();
  node->set_abstract(abstract_none);
  SetKernelInfo(node);
  return node;
}
}  // namespace gpu
}  // namespace device
}  // namespace mindspore
