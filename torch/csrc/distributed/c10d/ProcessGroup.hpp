#pragma once

#include <torch/csrc/distributed/c10d/Backend.hpp>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ATen/ATen.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <c10/macros/Macros.h>

#include <torch/csrc/distributed/c10d/Work.hpp>
// *************************************************************************
// PROCESS GROUP collective communication API IS BEING CHANGED BETWEEN
// versions 1.7 and 1.8.
// PLEASE DO NOT ADD ANY DEPENDENCIES.
// SEE RFC: https://github.com/pytorch/pytorch/issues/39662
// *************************************************************************

constexpr auto kProcessGroupDefaultTimeout =
    std::chrono::milliseconds(30 * 60 * 1000);

namespace c10d {

// ProcessGroup is a base class that captures collective and point to
// point communication in a fixed set of processes.
//
// The functions specified in the class below describe the API alone;
// implementations are provided in subclasses.
//
// Every function that performs I/O is executed asynchronously by a
// thread pool owned by the ProcessGroup (by default). They return an
// object that can be used to wait for completion or error.
//
// The ProcessGroup can instantiate subgroups with fewer or an equal
// number of members. Implementations must take care that multiple
// process groups can be used in parallel and synchronize accordingly.
//
// The ProcessGroup assumes a fixed set of processes. If the set
// changes, existing instances must be destructed and instantiation
// and initialization must start from scratch. For members of the
// process group to find each other (referred to as rendezvous from
// hereon)
//
class TORCH_API ProcessGroup : public torch::CustomClassHolder {
 public:
  // ProcessGroup Options is a base struct that defines the basic options
  // when constructing a ProcessGroup. Each ProcessGroup subclass should
  // extend this struct and define its options if it wants to provide more
  // config options (beyond basic ones defined here) to end user.
  struct TORCH_API Options : torch::CustomClassHolder {
    explicit Options(
        std::string backend,
        std::chrono::milliseconds timeout = kProcessGroupDefaultTimeout)
        : timeout(timeout), backend(std::move(backend)) {}
    ~Options() override = default;

    std::chrono::milliseconds timeout;

    // backend name
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    const std::string backend;
  };

  enum BackendType {
    UNDEFINED = 0,
    GLOO = 1,
    NCCL = 2,
    UCC = 3,
    MPI = 4,
    CUSTOM = 5,
  };

  // Not used, set for backwards compatibility and only used for TypeDef in
  // Ops.cpp
  explicit ProcessGroup(int rank, int size);

  explicit ProcessGroup(
      const c10::intrusive_ptr<::c10d::Store>& store,
      int rank,
      int size,
      c10::intrusive_ptr<Options> options);
  ~ProcessGroup() override;

  int getRank() const {
    return rank_;
  }

  int getSize() const {
    return size_;
  }

  // Returns an unique opaque ID of this process group object.
  int64_t getID() const {
    return reinterpret_cast<std::intptr_t>(this);
  }

  // Returns an unique opaque ID of a backend for the specific backend type
  // that can correlate with this process group's collectives.
  int64_t getBackendID(BackendType backend_type) const {
    return reinterpret_cast<std::intptr_t>(getBackend(backend_type).get());
  }

  virtual const std::string getBackendName() const {
    return options_->backend;
  };

  BackendType getBackendType() const {
    return backendType_;
  };

  virtual void startCoalescing(c10::DeviceType deviceType) {
    // only nccl has implemented startCoalescing so only execute for nccl
    // backends
    auto backend = getBackend(deviceType);
    backend->startCoalescing();
  }

  virtual c10::intrusive_ptr<Work> endCoalescing(c10::DeviceType deviceType) {
    // only nccl has implemented endCoalescing so only execute for nccl
    // backends
    auto backend = getBackend(deviceType);
    auto work = backend->endCoalescing();
    return work;
  }

  virtual c10::intrusive_ptr<Work> broadcast(
      std::vector<at::Tensor>& tensors,
      const BroadcastOptions& opts = BroadcastOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::broadcast_", "")
            .typed<
                std::tuple<std::vector<at::Tensor>, c10::intrusive_ptr<Work>>(
                    at::TensorList,
                    const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                    int64_t,
                    int64_t,
                    bool,
                    int64_t)>();
    // It's awakward to unbox the opts here and box them again in the custom C++
    // op. But it's also complicated to make opts as a CustomClassHolder. Leave
    // it as it is now.
    return std::get<1>(op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.rootRank,
        opts.rootTensor,
        opts.asyncOp,
        opts.timeout.count()));
  }

  virtual c10::intrusive_ptr<Work> allreduce(
      std::vector<at::Tensor>& tensors,
      const AllreduceOptions& opts = AllreduceOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::allreduce_", "")
            .typed<
                std::tuple<std::vector<at::Tensor>, c10::intrusive_ptr<Work>>(
                    at::TensorList,
                    const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                    const c10::intrusive_ptr<::c10d::ReduceOp>&,
                    const c10::optional<at::Tensor>& sparse_indices,
                    int64_t)>();

    return std::get<1>(op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<ReduceOp>(opts.reduceOp),
        opts.sparseIndices,
        opts.timeout.count()));
  }

  virtual c10::intrusive_ptr<Work> allreduce_coalesced(
      std::vector<at::Tensor>& tensors,
      const AllreduceCoalescedOptions& opts = AllreduceCoalescedOptions()) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::allreduce_coalesced_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             const c10::intrusive_ptr<::c10d::ReduceOp>&,
                             int64_t)>();

    return op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<ReduceOp>(opts.reduceOp),
        opts.timeout.count());
  }

  virtual c10::intrusive_ptr<Work> reduce(
      std::vector<at::Tensor>& tensors,
      const ReduceOptions& opts = ReduceOptions()) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::reduce_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             const c10::intrusive_ptr<::c10d::ReduceOp>&,
                             int64_t,
                             int64_t,
                             int64_t)>();
    return op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<ReduceOp>(opts.reduceOp),
        opts.rootRank,
        opts.rootTensor,
        opts.timeout.count());
  }

  virtual c10::intrusive_ptr<Work> allgather(
      std::vector<std::vector<at::Tensor>>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const AllgatherOptions& opts = AllgatherOptions()) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::allgather_", "")
                         .typed<std::tuple<
                             std::vector<std::vector<at::Tensor>>,
                             c10::intrusive_ptr<Work>>(
                             const std::vector<std::vector<at::Tensor>>&,
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             int64_t)>();

    return std::get<1>(op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.timeout.count()));
  }

  // Gathers a single tensor inputBuffer into a single buffer outputBuffer that
  // is interpreted as a contiguous collection of size inputBuffer * WORLD_SIZE.
  // For implementers of ProcessGroup API and advanced users only.
  // Note: this function will be deprecated in near future.
  virtual c10::intrusive_ptr<Work> _allgather_base(
      at::Tensor& outputBuffer,
      at::Tensor& inputBuffer,
      const AllgatherOptions& opts = AllgatherOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::_allgather_base_", "")
            .typed<std::tuple<at::Tensor, c10::intrusive_ptr<Work>>(
                at::Tensor&,
                at::Tensor&,
                const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                bool,
                int64_t)>();

    return std::get<1>(op.call(
        outputBuffer,
        inputBuffer,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.asyncOp,
        opts.timeout.count()));
  }

  // This function is deprecated and will be moved out of ProcessGroup to comms:
  // * do not add dependencies on this function,
  // * do not implement it in your ProcessGroup, implement _allgather_base
  //   instead.
  virtual c10::intrusive_ptr<Work> allgather_coalesced(
      std::vector<std::vector<at::Tensor>>& outputTensorLists,
      std::vector<at::Tensor>& inputTensors,
      const AllgatherOptions& opts = AllgatherOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::allgather_coalesced_", "")
            .typed<c10::intrusive_ptr<Work>(
                const std::vector<std::vector<at::Tensor>>&,
                const at::TensorList&,
                const c10::intrusive_ptr<::c10d::ProcessGroup>&)>();

    return op.call(
        outputTensorLists,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this));
  }

  // This function is a coalesced version of `allgather_into_tensor` (currently
  // still named as `_allgather_base`). Each tensor in the vector corresponds to
  // an input/output of one `allgather_into_tensor` operation.
  virtual c10::intrusive_ptr<Work> allgather_into_tensor_coalesced(
      std::vector<at::Tensor>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const AllgatherOptions& opts = AllgatherOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::allgather_into_tensor_coalesced_", "")
            .typed<c10::intrusive_ptr<Work>(
                const at::TensorList,
                const at::TensorList,
                const c10::intrusive_ptr<::c10d::ProcessGroup>&)>();

    return op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this));
  }

  virtual c10::intrusive_ptr<Work> gather(
      std::vector<std::vector<at::Tensor>>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const GatherOptions& opts = GatherOptions()) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::gather_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             const std::vector<std::vector<at::Tensor>>&,
                             const at::TensorList&,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             int64_t,
                             int64_t)>();
    return op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.rootRank,
        opts.timeout.count());
  }

  virtual c10::intrusive_ptr<Work> scatter(
      std::vector<at::Tensor>& outputTensors,
      std::vector<std::vector<at::Tensor>>& inputTensors,
      const ScatterOptions& opts = ScatterOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::scatter_", "")
            .typed<
                std::tuple<std::vector<at::Tensor>, c10::intrusive_ptr<Work>>(
                    const at::TensorList&,
                    const std::vector<std::vector<at::Tensor>>&,
                    const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                    int64_t,
                    bool,
                    int64_t)>();
    return std::get<1>(op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.rootRank,
        opts.asyncOp,
        opts.timeout.count()));
  }

  virtual c10::intrusive_ptr<Work> reduce_scatter(
      std::vector<at::Tensor>& outputTensors,
      std::vector<std::vector<at::Tensor>>& inputTensors,
      const ReduceScatterOptions& opts = ReduceScatterOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::reduce_scatter_", "")
            .typed<
                std::tuple<std::vector<at::Tensor>, c10::intrusive_ptr<Work>>(
                    const at::TensorList&,
                    const std::vector<std::vector<at::Tensor>>&,
                    const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                    const c10::intrusive_ptr<::c10d::ReduceOp>&,
                    int64_t)>();
    return std::get<1>(op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<::c10d::ReduceOp>(opts.reduceOp),
        opts.timeout.count()));
  }

  virtual c10::intrusive_ptr<Work> _reduce_scatter_base(
      at::Tensor& outputBuffer,
      at::Tensor& inputBuffer,
      const ReduceScatterOptions& opts = ReduceScatterOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::_reduce_scatter_base_", "")
            .typed<std::tuple<at::Tensor, c10::intrusive_ptr<Work>>(
                at::Tensor&,
                at::Tensor&,
                const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                const c10::intrusive_ptr<::c10d::ReduceOp>&,
                bool,
                int64_t)>();
    return std::get<1>(op.call(
        outputBuffer,
        inputBuffer,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<::c10d::ReduceOp>(opts.reduceOp),
        opts.asyncOp,
        opts.timeout.count()));
  }

  // This function is a coalesced version of `reduce_scatter_tensor` (currently
  // still named as `_reduce_scatter_base`). Each tensor in the vector
  // corresponds to an input/output of one `reduce_scatter_tensor` operation.
  virtual c10::intrusive_ptr<Work> reduce_scatter_tensor_coalesced(
      std::vector<at::Tensor>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const ReduceScatterOptions& opts = ReduceScatterOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::reduce_scatter_tensor_coalesced_", "")
            .typed<c10::intrusive_ptr<Work>(
                const at::TensorList,
                const at::TensorList,
                const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                const c10::intrusive_ptr<::c10d::ReduceOp>&,
                int64_t)>();

    return op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        c10::make_intrusive<::c10d::ReduceOp>(opts.reduceOp),
        opts.timeout.count());
  }

  virtual c10::intrusive_ptr<Work> alltoall_base(
      at::Tensor& outputBuffer,
      at::Tensor& inputBuffer,
      std::vector<int64_t>& outputSplitSizes,
      std::vector<int64_t>& inputSplitSizes,
      const AllToAllOptions& opts = AllToAllOptions()) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::alltoall_base_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::Tensor&,
                             at::Tensor&,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             std::vector<int64_t>,
                             std::vector<int64_t>,
                             int64_t)>();
    return op.call(
        outputBuffer,
        inputBuffer,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        outputSplitSizes,
        inputSplitSizes,
        opts.timeout.count());
  }

  virtual c10::intrusive_ptr<Work> alltoall(
      std::vector<at::Tensor>& outputTensors,
      std::vector<at::Tensor>& inputTensors,
      const AllToAllOptions& opts = AllToAllOptions()) {
    static auto op =
        c10::Dispatcher::singleton()
            .findSchemaOrThrow("c10d::alltoall_", "")
            .typed<
                std::tuple<std::vector<at::Tensor>, c10::intrusive_ptr<Work>>(
                    const at::TensorList&,
                    const at::TensorList&,
                    const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                    int64_t)>();
    return std::get<1>(op.call(
        outputTensors,
        inputTensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.timeout.count()));
  }

  virtual void monitoredBarrier(
      const BarrierOptions& opts,
      bool wait_all_ranks = false) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::monitored_barrier_", "")
                         .typed<void(
                             at::Tensor,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             const std::vector<int64_t>&,
                             int64_t,
                             bool)>();
    // Default to using cpu implementation, monitored barrier is only for GLOO
    at::Tensor tensor = at::empty({0}, at::TensorOptions().device(at::kCPU));
    op.call(
        tensor,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.device_ids,
        opts.timeout.count(),
        wait_all_ranks);
  }

  // Agrees on an initial sequence number for the whole group by having rank 0
  // create it and broadcast it to other ranks using the store. Only implemented
  // for GLOO and NCCL backends currently.
  virtual void setSequenceNumberForGroup() {
    auto backendType = getBackendType();
    // TODO: HACK for backend name to get sequence number for that backend.
    if (backendType == ProcessGroup::BackendType::GLOO ||
        backendType == ProcessGroup::BackendType::NCCL ||
        backendType == ProcessGroup::BackendType::UCC) {
      getDefaultBackend()->setSequenceNumberForGroup();
    } else {
      TORCH_CHECK(
          false,
          c10::str(
              "ProcessGroup ",
              getBackendName(),
              " does not yet support sequence numbers."));
    }
  }

  // Retrieves the current sequence number for the whole group, which should be
  // in sync. If the returned number is not consistent across the group, it
  // may indicate that there is some sort of collective desynchronization.
  virtual uint64_t getSequenceNumberForGroup() {
    auto backendType = getBackendType();

    // TODO: HACK for backend name to get sequence number for that backend.
    if (backendType == ProcessGroup::BackendType::GLOO ||
        backendType == ProcessGroup::BackendType::NCCL ||
        backendType == ProcessGroup::BackendType::UCC) {
      return getDefaultBackend()->getSequenceNumberForGroup();
    } else {
      TORCH_CHECK(
          false,
          c10::str(
              "ProcessGroup ",
              getBackendName(),
              " does not yet support sequence numbers."));
    }
  }

  virtual c10::intrusive_ptr<Work> send(
      std::vector<at::Tensor>& tensors,
      int dstRank,
      int tag) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::send", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             int64_t,
                             int64_t)>();
    return op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        dstRank,
        tag);
  }

  virtual c10::intrusive_ptr<Work> recv(
      std::vector<at::Tensor>& tensors,
      int srcRank,
      int tag) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::recv_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             int64_t,
                             int64_t)>();
    return op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        srcRank,
        tag);
  }

  virtual c10::intrusive_ptr<Work> recvAnysource(
      std::vector<at::Tensor>& tensors,
      int tag) {
    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::recv_any_source_", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::TensorList,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             int64_t)>();
    return op.call(
        tensors,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        tag);
  }

  virtual c10::intrusive_ptr<Work> barrier(
      const BarrierOptions& opts = BarrierOptions()) {
    static at::Tensor tensor;
    // TODO: if nccl was specified then use it
    auto device = opts.device;
    if (device.has_value()) {
      // set device tensor from argument
      tensor = at::empty(
          {1}, at::TensorOptions().device(device.value()).dtype(at::kByte));
    } else if (backendType_ == c10d::ProcessGroup::BackendType::NCCL) {
      // set cuda tensor
      tensor = at::empty(
          {1},
          at::TensorOptions().device(at::DeviceType::CUDA).dtype(at::kByte));
    } else {
      // Default to using cpu implementation
      tensor = at::empty(
          {1},
          at::TensorOptions().device(at::DeviceType::CPU).dtype(at::kByte));
    }

    static auto op = c10::Dispatcher::singleton()
                         .findSchemaOrThrow("c10d::barrier", "")
                         .typed<c10::intrusive_ptr<::c10d::Work>(
                             at::Tensor,
                             const c10::intrusive_ptr<::c10d::ProcessGroup>&,
                             const std::vector<int64_t>&,
                             int64_t)>();

    return op.call(
        tensor,
        c10::intrusive_ptr<ProcessGroup>::unsafe_reclaim_from_nonowning(this),
        opts.device_ids,
        opts.timeout.count());
  }

  c10::intrusive_ptr<Options> getOptions() {
    return options_;
  }

  bool hasBackends() {
    return !deviceTypeToBackendType_.empty();
  }

  void setBackend(
      c10::DeviceType deviceType,
      BackendType backendType,
      const c10::optional<c10::intrusive_ptr<Backend>>& backend) {
    // TODO: should we add these entries after the backend setting succeeds?
    deviceTypeToBackendType_[deviceType] = backendType;
    deviceTypes_.insert(deviceType);
    // if the backendType is already set then reuse it for this device
    if (backendTypeToBackend_.find(backendType) !=
        backendTypeToBackend_.end()) {
      auto existingBackend = backendTypeToBackend_.at(backendType);
      deviceTypeToBackend_[deviceType] = existingBackend;
      TORCH_CHECK(
          existingBackend->getBoundDeviceId() ==
          (*backend)->getBoundDeviceId());
    } else {
      // check if backend has value
      if (backend.has_value()) {
        deviceTypeToBackend_[deviceType] = backend.value();
        backendTypeToBackend_[backendType] = backend.value();
        (*backend)->setBoundDeviceId(bound_device_id_);
      }
    }
  }

  c10::intrusive_ptr<Backend> getDefaultBackend() const {
    TORCH_CHECK(
        backendTypeToBackend_.find(backendType_) != backendTypeToBackend_.end(),
        "Could not find the default backend type ",
        backendType_,
        " for Process Group with name ",
        getBackendName(),
        ".");
    return backendTypeToBackend_.at(backendType_);
  }

  c10::intrusive_ptr<Backend> getBackend(c10::DeviceType deviceType);

  c10::intrusive_ptr<Backend> getBackend(BackendType backendType) const {
    TORCH_CHECK(
        backendTypeToBackend_.find(backendType) != backendTypeToBackend_.end(),
        "Could not find backend type ",
        backendType,
        ".");
    return backendTypeToBackend_.at(backendType);
  }

  // Return device types supported by this ProcessGroup.
  // Note: the return type is `Device` rather than `DeviceType` for the purpose
  // of easy comparison at Python level. The `Device` will have default index
  // (-1).
  std::vector<c10::Device> getDeviceTypes() const {
    std::vector<c10::Device> devices;
    devices.reserve(deviceTypes_.size());
    for (auto& dt : deviceTypes_) {
      devices.emplace_back(dt);
    }
    return devices;
  }

  void registerOnCompletionHook(
      std::function<void(std::shared_ptr<WorkInfo>)>&& hook) {
    getDefaultBackend()->registerOnCompletionHook(std::move(hook));
  }

  void waitForPendingWorks() {
    getDefaultBackend()->waitForPendingWorks();
  }

  bool hasHooks() const {
    return getDefaultBackend()->hasHooks();
  }

  const std::string& getGroupName() const;
  void setGroupName(const std::string& name);
  const std::string& getGroupDesc() const;
  void setGroupDesc(const std::string& name);
  void enableCollectivesTiming();

  void release_resources() override;

  // ProcessGroups optionally can be "bound" to a specific device.
  // Currently this is only for nccl and allows for some opt-in
  // optimizations such as automatic use of ncclCommSplit.  The device
  // is specified in `init_process_group` and eventually makes it
  // here and then down into the actual backend instances.
  c10::optional<at::Device> getBoundDeviceId() const {
    return bound_device_id_;
  }

  void setBoundDeviceId(c10::optional<at::Device> device) {
    if (device) {
      TORCH_CHECK(device->has_index(), "setBoundDeviceId must have an index");
    }
    bound_device_id_ = device;
  }

 protected:
  // Implementations of this interface need to call this to setup
  // appropriate logging etc.
  void init();

  c10::intrusive_ptr<c10d::Store> store_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  const int rank_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  const int size_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  const c10::intrusive_ptr<Options> options_;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  const BackendType backendType_;
  std::string pg_desc_;

  // Debug level setting. It is parsed once when ProcessGroup is constructed and
  // remains the same across use of this process group.
  DebugLevel dist_debug_level_{DebugLevel::Off};

  // Backend classes for this ProcessGroup
  std::unordered_set<c10::DeviceType> deviceTypes_;
  std::unordered_map<c10::DeviceType, BackendType> deviceTypeToBackendType_;
  std::unordered_map<c10::DeviceType, c10::intrusive_ptr<Backend>>
      deviceTypeToBackend_;
  std::unordered_map<BackendType, c10::intrusive_ptr<Backend>>
      backendTypeToBackend_;

  c10::optional<at::Device> bound_device_id_;
};

} // namespace c10d
