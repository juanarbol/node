// Copyright 2021 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/fast-api-calls.h"

#include "src/codegen/cpu-features.h"
#include "src/compiler/globals.h"

namespace v8 {

// Local handles should be trivially copyable so that the contained value can be
// efficiently passed by value in a register. This is important for two
// reasons: better performance and a simpler ABI for generated code and fast
// API calls.
ASSERT_TRIVIALLY_COPYABLE(api_internal::IndirectHandleBase);
#ifdef V8_ENABLE_DIRECT_HANDLE
ASSERT_TRIVIALLY_COPYABLE(api_internal::DirectHandleBase);
#endif
ASSERT_TRIVIALLY_COPYABLE(LocalBase<Object>);

#if !(defined(V8_ENABLE_LOCAL_OFF_STACK_CHECK) && V8_HAS_ATTRIBUTE_TRIVIAL_ABI)
// Direct local handles should be trivially copyable, for the same reasons as
// above. In debug builds, however, where we want to check that such handles are
// stack-allocated, we define a non-default copy constructor and destructor.
// This makes them non-trivially copyable. We only do it in builds where we can
// declare them as "trivial ABI", which guarantees that they can be efficiently
// passed by value in a register.
ASSERT_TRIVIALLY_COPYABLE(Local<Object>);
ASSERT_TRIVIALLY_COPYABLE(internal::LocalUnchecked<Object>);
ASSERT_TRIVIALLY_COPYABLE(MaybeLocal<Object>);
#endif

namespace internal {
namespace compiler {
namespace fast_api_call {

ElementsKind GetTypedArrayElementsKind(CTypeInfo::Type type) {
  switch (type) {
    case CTypeInfo::Type::kUint8:
      return UINT8_ELEMENTS;
    case CTypeInfo::Type::kInt32:
      return INT32_ELEMENTS;
    case CTypeInfo::Type::kUint32:
      return UINT32_ELEMENTS;
    case CTypeInfo::Type::kInt64:
      return BIGINT64_ELEMENTS;
    case CTypeInfo::Type::kUint64:
      return BIGUINT64_ELEMENTS;
    case CTypeInfo::Type::kFloat32:
      return FLOAT32_ELEMENTS;
    case CTypeInfo::Type::kFloat64:
      return FLOAT64_ELEMENTS;
    case CTypeInfo::Type::kVoid:
    case CTypeInfo::Type::kSeqOneByteString:
    case CTypeInfo::Type::kBool:
    case CTypeInfo::Type::kPointer:
    case CTypeInfo::Type::kV8Value:
    case CTypeInfo::Type::kApiObject:
    case CTypeInfo::Type::kAny:
      UNREACHABLE();
  }
}

bool CanOptimizeFastSignature(const CFunctionInfo* c_signature) {
  USE(c_signature);

#if defined(V8_OS_MACOS) && defined(V8_TARGET_ARCH_ARM64)
  // On MacArm64 hardware we don't support passing of arguments on the stack.
  if (c_signature->ArgumentCount() > 8) {
    return false;
  }
#endif  // defined(V8_OS_MACOS) && defined(V8_TARGET_ARCH_ARM64)

#ifndef V8_ENABLE_FP_PARAMS_IN_C_LINKAGE
  if (c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kFloat32 ||
      c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kFloat64) {
    return false;
  }
#endif

#ifdef V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS
  if (!v8_flags.fast_api_allow_float_in_sim &&
      (c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kFloat32 ||
       c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kFloat64)) {
    return false;
  }
#endif

#ifndef V8_TARGET_ARCH_64_BIT
  if (c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kInt64 ||
      c_signature->ReturnInfo().GetType() == CTypeInfo::Type::kUint64) {
    return false;
  }
#endif

  for (unsigned int i = 0; i < c_signature->ArgumentCount(); ++i) {
    USE(i);

#ifdef V8_TARGET_ARCH_X64
    // Clamp lowering in EffectControlLinearizer uses rounding.
    uint8_t flags = uint8_t(c_signature->ArgumentInfo(i).GetFlags());
    if (flags & uint8_t(CTypeInfo::Flags::kClampBit)) {
      return CpuFeatures::IsSupported(SSE4_2);
    }
#endif  // V8_TARGET_ARCH_X64

#ifndef V8_ENABLE_FP_PARAMS_IN_C_LINKAGE
    if (c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kFloat32 ||
        c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kFloat64) {
      return false;
    }
#endif

#ifdef V8_USE_SIMULATOR_WITH_GENERIC_C_CALLS
    if (!v8_flags.fast_api_allow_float_in_sim &&
        (c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kFloat32 ||
         c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kFloat64)) {
      return false;
    }
#endif

#ifndef V8_TARGET_ARCH_64_BIT
    if (c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kInt64 ||
        c_signature->ArgumentInfo(i).GetType() == CTypeInfo::Type::kUint64) {
      return false;
    }
#endif
  }

  return true;
}

#define __ gasm()->

class FastApiCallBuilder {
 public:
  FastApiCallBuilder(Isolate* isolate, TFGraph* graph,
                     GraphAssembler* graph_assembler,
                     const GetParameter& get_parameter,
                     const ConvertReturnValue& convert_return_value,
                     const InitializeOptions& initialize_options,
                     const GenerateSlowApiCall& generate_slow_api_call)
      : isolate_(isolate),
        graph_(graph),
        graph_assembler_(graph_assembler),
        get_parameter_(get_parameter),
        convert_return_value_(convert_return_value),
        initialize_options_(initialize_options),
        generate_slow_api_call_(generate_slow_api_call) {}

  Node* Build(FastApiCallFunction c_function, Node* data_argument);

 private:
  Node* WrapFastCall(const CallDescriptor* call_descriptor, int inputs_size,
                     Node** inputs, Node* target,
                     const CFunctionInfo* c_signature, int c_arg_count,
                     Node* stack_slot);
  void PropagateException();

  Isolate* isolate() const { return isolate_; }
  TFGraph* graph() const { return graph_; }
  GraphAssembler* gasm() const { return graph_assembler_; }
  Isolate* isolate_;
  TFGraph* graph_;
  GraphAssembler* graph_assembler_;
  const GetParameter& get_parameter_;
  const ConvertReturnValue& convert_return_value_;
  const InitializeOptions& initialize_options_;
  const GenerateSlowApiCall& generate_slow_api_call_;
};

Node* FastApiCallBuilder::WrapFastCall(const CallDescriptor* call_descriptor,
                                       int inputs_size, Node** inputs,
                                       Node* target,
                                       const CFunctionInfo* c_signature,
                                       int c_arg_count, Node* stack_slot) {
  // CPU profiler support
  Node* target_address = __ IsolateField(IsolateFieldId::kFastApiCallTarget);
  __ Store(StoreRepresentation(MachineType::PointerRepresentation(),
                               kNoWriteBarrier),
           target_address, 0, __ BitcastTaggedToWord(target));

  // Update effect and control
  if (stack_slot != nullptr) {
    inputs[c_arg_count + 1] = stack_slot;
    inputs[c_arg_count + 2] = __ effect();
    inputs[c_arg_count + 3] = __ control();
  } else {
    inputs[c_arg_count + 1] = __ effect();
    inputs[c_arg_count + 2] = __ control();
  }

  // Create the fast call
  Node* call = __ Call(call_descriptor, inputs_size, inputs);

  // Reset the CPU profiler target address.
  __ Store(StoreRepresentation(MachineType::PointerRepresentation(),
                               kNoWriteBarrier),
           target_address, 0, __ IntPtrConstant(0));

  return call;
}

void FastApiCallBuilder::PropagateException() {
  Runtime::FunctionId fun_id = Runtime::FunctionId::kPropagateException;
  const Runtime::Function* fun = Runtime::FunctionForId(fun_id);
  auto call_descriptor = Linkage::GetRuntimeCallDescriptor(
      graph()->zone(), fun_id, fun->nargs, Operator::kNoProperties,
      CallDescriptor::kNoFlags);
  // The CEntryStub is loaded from the IsolateRoot so that generated code is
  // Isolate independent. At the moment this is only done for CEntryStub(1).
  Node* isolate_root = __ LoadRootRegister();
  DCHECK_EQ(1, fun->result_size);
  auto centry_id = Builtin::kWasmCEntry;
  int builtin_slot_offset = IsolateData::BuiltinSlotOffset(centry_id);
  Node* centry_stub =
      __ Load(MachineType::Pointer(), isolate_root, builtin_slot_offset);
  const int kInputCount = 6;
  Node* inputs[kInputCount];
  int count = 0;
  inputs[count++] = centry_stub;
  inputs[count++] = __ ExternalConstant(ExternalReference::Create(fun_id));
  inputs[count++] = __ Int32Constant(fun->nargs);
  inputs[count++] = __ IntPtrConstant(0);
  inputs[count++] = __ effect();
  inputs[count++] = __ control();
  DCHECK_EQ(kInputCount, count);

  __ Call(call_descriptor, count, inputs);
}

Node* FastApiCallBuilder::Build(FastApiCallFunction c_function,
                                Node* data_argument) {
  const CFunctionInfo* c_signature = c_function.signature;
  const int c_arg_count = c_signature->ArgumentCount();

  // Hint to fast path.
  auto if_success = __ MakeLabel();
  auto if_error = __ MakeDeferredLabel();

  // Generate fast call.

  const int kFastTargetAddressInputIndex = 0;
  const int kFastTargetAddressInputCount = 1;

  const int kEffectAndControlInputCount = 2;

  int extra_input_count =
      kEffectAndControlInputCount + (c_signature->HasOptions() ? 1 : 0);

  Node** const inputs = graph()->zone()->AllocateArray<Node*>(
      kFastTargetAddressInputCount + c_arg_count + extra_input_count);

  ExternalReference::Type ref_type = ExternalReference::FAST_C_CALL;

  // The inputs to {Call} node for the fast call look like:
  // [fast callee, receiver, ... C arguments, [optional Options], effect,
  //  control].
  //
  // The first input node represents the target address for the fast call.
  // If the function is not overloaded (c_functions.size() == 1) this is the
  // address associated to the first and only element in the c_functions vector.
  // If there are multiple overloads the value of this input will be set later
  // with a Phi node created by AdaptOverloadedFastCallArgument.
  inputs[kFastTargetAddressInputIndex] = __ ExternalConstant(
      ExternalReference::Create(c_function.address, ref_type));

  for (int i = 0; i < c_arg_count; ++i) {
    inputs[i + kFastTargetAddressInputCount] = get_parameter_(i, &if_error);
  }
  DCHECK_NOT_NULL(inputs[kFastTargetAddressInputIndex]);

  MachineSignature::Builder builder(
      graph()->zone(), 1, c_arg_count + (c_signature->HasOptions() ? 1 : 0));
  MachineType return_type =
      MachineType::TypeForCType(c_signature->ReturnInfo());
  builder.AddReturn(return_type);
  for (int i = 0; i < c_arg_count; ++i) {
    CTypeInfo type = c_signature->ArgumentInfo(i);
    START_ALLOW_USE_DEPRECATED()
    MachineType machine_type =
        type.GetSequenceType() == CTypeInfo::SequenceType::kScalar
            ? MachineType::TypeForCType(type)
            : MachineType::AnyTagged();
    END_ALLOW_USE_DEPRECATED()
    builder.AddParam(machine_type);
  }

  Node* stack_slot = nullptr;
  if (c_signature->HasOptions()) {
    const int kAlign = alignof(v8::FastApiCallbackOptions);
    const int kSize = sizeof(v8::FastApiCallbackOptions);
    // If this check fails, you've probably added new fields to
    // v8::FastApiCallbackOptions, which means you'll need to write code
    // that initializes and reads from them too.
    static_assert(kSize == sizeof(uintptr_t) * 2);
    stack_slot = __ StackSlot(kSize, kAlign);

    __ Store(StoreRepresentation(MachineType::PointerRepresentation(),
                                 kNoWriteBarrier),
             stack_slot,
             static_cast<int>(offsetof(v8::FastApiCallbackOptions, isolate)),
             __ ExternalConstant(ExternalReference::isolate_address()));

    Node* data_argument_to_pass = __ AdaptLocalArgument(data_argument);

    __ Store(StoreRepresentation(MachineType::PointerRepresentation(),
                                 kNoWriteBarrier),
             stack_slot,
             static_cast<int>(offsetof(v8::FastApiCallbackOptions, data)),
             data_argument_to_pass);

    initialize_options_(stack_slot);

    builder.AddParam(MachineType::Pointer());  // stack_slot
  }

  CallDescriptor* call_descriptor =
      Linkage::GetSimplifiedCDescriptor(graph()->zone(), builder.Get());

  Node* c_call_result =
      WrapFastCall(call_descriptor, c_arg_count + extra_input_count + 1, inputs,
                   inputs[0], c_signature, c_arg_count, stack_slot);

  Node* exception = __ Load(MachineType::IntPtr(),
                            __ ExternalConstant(ExternalReference::Create(
                                IsolateAddressId::kExceptionAddress, isolate_)),
                            0);

  Node* the_hole =
      __ Load(MachineType::IntPtr(), __ LoadRootRegister(),
              IsolateData::root_slot_offset(RootIndex::kTheHoleValue));

  auto throw_label = __ MakeDeferredLabel();
  auto done = __ MakeLabel();
  __ GotoIfNot(__ IntPtrEqual(exception, the_hole), &throw_label);
  __ Goto(&done);

  __ Bind(&throw_label);
  PropagateException();
  __ Unreachable();

  __ Bind(&done);
  Node* fast_call_result = convert_return_value_(c_signature, c_call_result);

  auto merge = __ MakeLabel(MachineRepresentation::kTagged);
  __ Goto(&if_success);

  // We need to generate a fallback (both fast and slow call) in case
  // the generated code might fail, in case e.g. a Smi was passed where
  // a JSObject was expected and an error must be thrown
  if (if_error.IsUsed()) {
    // Generate direct slow call.
    __ Bind(&if_error);
    {
      Node* slow_call_result = generate_slow_api_call_();
      __ Goto(&merge, slow_call_result);
    }
  }

  __ Bind(&if_success);
  __ Goto(&merge, fast_call_result);

  __ Bind(&merge);
  return merge.PhiAt(0);
}

#undef __

Node* BuildFastApiCall(Isolate* isolate, TFGraph* graph,
                       GraphAssembler* graph_assembler,
                       FastApiCallFunction c_function, Node* data_argument,
                       const GetParameter& get_parameter,
                       const ConvertReturnValue& convert_return_value,
                       const InitializeOptions& initialize_options,
                       const GenerateSlowApiCall& generate_slow_api_call) {
  FastApiCallBuilder builder(isolate, graph, graph_assembler, get_parameter,
                             convert_return_value, initialize_options,
                             generate_slow_api_call);
  return builder.Build(c_function, data_argument);
}

FastApiCallFunction GetFastApiCallTarget(
    JSHeapBroker* broker, FunctionTemplateInfoRef function_template_info,
    size_t arg_count) {
  if (!v8_flags.turbo_fast_api_calls) return {0, nullptr};

  static constexpr int kReceiver = 1;

  const ZoneVector<const CFunctionInfo*>& signatures =
      function_template_info.c_signatures(broker);
  const size_t overloads_count = signatures.size();

  // Only considers entries whose type list length matches arg_count.
  for (size_t i = 0; i < overloads_count; i++) {
    const CFunctionInfo* c_signature = signatures[i];
    const size_t len = c_signature->ArgumentCount() - kReceiver;
    bool optimize_to_fast_call =
        (len == arg_count) &&
        fast_api_call::CanOptimizeFastSignature(c_signature);

    if (optimize_to_fast_call) {
      // TODO(nicohartmann@): {Flags::kEnforceRangeBit} is currently only
      // supported on 64 bit architectures. We should support this on 32 bit
      // architectures.
#if defined(V8_TARGET_ARCH_32_BIT)
      for (unsigned int j = 0; j < c_signature->ArgumentCount(); ++j) {
        const uint8_t flags =
            static_cast<uint8_t>(c_signature->ArgumentInfo(j).GetFlags());
        if (flags & static_cast<uint8_t>(CTypeInfo::Flags::kEnforceRangeBit)) {
          // Bailout
          return {0, nullptr};
        }
      }
#endif
      return {function_template_info.c_functions(broker)[i], c_signature};
    }
  }

  return {0, nullptr};
}

}  // namespace fast_api_call
}  // namespace compiler
}  // namespace internal
}  // namespace v8
