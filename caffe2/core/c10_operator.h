#pragma once

#include <vector>
#include <ATen/core/dispatch/OpSchemaRegistration.h>
#include <ATen/core/dispatch/KernelRegistration.h>
#include <ATen/core/function_schema.h>

namespace caffe2 {
namespace detail {

using _CallCaffe2OpFunc = void(const c10::FunctionSchema& schema, std::vector<c10::IValue>& inputs, std::vector<c10::IValue*>& outputs);

template<class Caffe2Operator>
inline void _call_caffe2_op(const c10::FunctionSchema& schema, std::vector<c10::IValue>& inputs, std::vector<c10::IValue*>& outputs) {
  Caffe2Operator(schema, std::move(inputs), std::move(outputs)).Run();
}

// This function is inline in the hope that compilers optimizing for speed will
// inline it into call_caffe2_op_from_c10, allowing call_op to be inlined and
// avoiding the function pointer indirection, while compilers optimizing for
// binary size will keep it a separate function instead of inlining it into
// a template and will reuse the binary code of this function between ops.
// We measured and confirmed that binary size off the instagram ios app is
// reduced when having _call_caffe2_op_from_c10 separate from the templated
// call_caffe2_op_from_c10.
inline void _call_caffe2_op_from_c10(c10::Stack* stack, const c10::FunctionSchema& schema, _CallCaffe2OpFunc* call_op) {
  // precondition: on the stack, there's an IValue for each caffe2 input and an IValue for each caffe2 output.
  // (note: in the jit schema, these caffe2 outputs are explicitly listed as additional inputs).
  // The output ones could either be a preallocated tensor or ivalue::None.

  const size_t num_outputs = schema.returns().size();
  const size_t total_num_arguments = schema.arguments().size();
  const size_t num_inputs = total_num_arguments - num_outputs;

  // TODO Avoid vector allocation. One idea would be to keep the std::vector instances in the cache.
  auto outputs = torch::jit::pop(*stack, num_outputs);
  auto inputs = torch::jit::pop(*stack, num_inputs);

  const auto device = at::Device(at::DeviceType::CPU); // TODO Handle GPU devices

  for (auto& output : outputs) {
    if (output.isNone() || (output.isTensor() && !output.toTensor().defined())) {
      output = at::Tensor(c10::C10Tensor(caffe2::empty({0}, device)));
    }
  }

  std::vector<c10::IValue*> outputPtrs;
  outputPtrs.reserve(outputs.size());
  for (auto& output : outputs) {
    outputPtrs.push_back(&output);
  }

  (*call_op)(schema, inputs, outputPtrs);

  for (auto& output: outputs) {
    torch::jit::push(*stack, std::move(output));
  }

  // postcondition: All inputs are cleared from the stack, there's now one
  //                IValue for each output which holds the result. This
  //                might reuse one of the preallocated tensors but doesn't have to.
}

template<class Caffe2Operator> const c10::OperatorHandle& c10_op_handle_for_c2_op();
template <class Caffe2Operator>
void call_caffe2_op_from_c10(c10::Stack* stack, c10::KernelCache* cache) { // TODO Pass in correct cache type
  _call_caffe2_op_from_c10(stack, c10_op_handle_for_c2_op<Caffe2Operator>().schema(), &_call_caffe2_op<Caffe2Operator>);
}

inline c10::FunctionSchema make_function_schema_for_c10(const char* OperatorName, std::vector<c10::Argument> inputs, std::vector<c10::Argument> outputs) {
  // actual_inputs is the real inputs plus an optional tensor argument for each output.
  // this can be used to pass in a preallocated output tensor.
  std::vector<c10::Argument> actual_inputs = std::move(inputs);
  actual_inputs.reserve(actual_inputs.size() + outputs.size());
  for (const auto& elem : outputs) {
    AT_ASSERT(elem.type()->isSubtypeOf(c10::TensorType::get()));
    actual_inputs.push_back(c10::Argument(elem.name(), c10::OptionalType::create(elem.type()), nullopt, IValue()));
  }

  return c10::FunctionSchema(
    std::string("_caffe2::") + OperatorName,
    std::move(actual_inputs), std::move(outputs));
}

}
}

#define C10_DECLARE_CAFFE2_OPERATOR(OperatorName)                                                   \
  namespace caffe2 { namespace _c10_ops {                                                           \
    C10_DECLARE_OP_SCHEMA(OperatorName);                                                            \
  }}

/**
 * Call this macro to register a caffe2 operator with the c10 dispatcher.
 */
// TODO This macro should take a JIT schema string instead of a vector of inputs and outputs.
#define C10_REGISTER_CAFFE2_OPERATOR_CPU(                                      \
    OperatorName, Inputs, Outputs, OperatorClass)                              \
  /* Register the op schema with the c10 dispatcher */                         \
  namespace caffe2 {                                                           \
  namespace _c10_ops {                                                         \
  C10_DEFINE_OP_SCHEMA(                                                        \
      OperatorName,                                                            \
      caffe2::detail::make_function_schema_for_c10(                            \
          #OperatorName,                                                       \
          Inputs,                                                              \
          Outputs));                                                           \
  }                                                                            \
  /* Store the c10 operator handle so call_caffe2_op_from_c10 can access it */ \
  namespace detail {                                                           \
  template <>                                                                  \
  const c10::OperatorHandle& c10_op_handle_for_c2_op<OperatorClass>() {        \
    return caffe2::_c10_ops::OperatorName();                                   \
  }                                                                            \
  }                                                                            \
  }                                                                            \
  /* Register call_caffe2_op_from_c10 as a kernel with the c10 dispatcher */   \
  namespace c10 {                                                              \
  C10_REGISTER_KERNEL(caffe2::_c10_ops::OperatorName) /*.withCache<Cache>()*/  \
      .kernel<&caffe2::detail::call_caffe2_op_from_c10<OperatorClass>>()       \
      .dispatchKey(CPUTensorId());                                             \
  }
