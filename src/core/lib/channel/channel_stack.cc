//
//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include "src/core/lib/channel/channel_stack.h"

#include <grpc/support/port_platform.h>
#include <stdint.h>

#include <memory>
#include <utility>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "src/core/channelz/property_list.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/channel_fwd.h"
#include "src/core/lib/surface/channel_init.h"
#include "src/core/util/alloc.h"

using grpc_event_engine::experimental::EventEngine;

static int register_get_name_fn = []() {
  grpc_core::NameFromChannelFilter = [](const grpc_channel_filter* filter) {
    return filter->name;
  };
  return 0;
}();

// Memory layouts.

// Channel stack is laid out as: {
//   grpc_channel_stack stk;
//   padding to GPR_MAX_ALIGNMENT
//   grpc_channel_element[stk.count];
//   per-filter memory, aligned to GPR_MAX_ALIGNMENT
// }

// Call stack is laid out as: {
//   grpc_call_stack stk;
//   padding to GPR_MAX_ALIGNMENT
//   grpc_call_element[stk.count];
//   per-filter memory, aligned to GPR_MAX_ALIGNMENT
// }

size_t grpc_channel_stack_size(const grpc_channel_filter** filters,
                               size_t filter_count) {
  // always need the header, and size for the channel elements
  size_t size = GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_channel_stack)) +
                GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filter_count *
                                               sizeof(grpc_channel_element));
  size_t i;

  CHECK((GPR_MAX_ALIGNMENT & (GPR_MAX_ALIGNMENT - 1)) == 0)
      << "GPR_MAX_ALIGNMENT must be a power of two";

  // add the size for each filter
  for (i = 0; i < filter_count; i++) {
    size += GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_channel_data);
  }

  return size;
}

#define CHANNEL_ELEMS_FROM_STACK(stk)                                     \
  ((grpc_channel_element*)((char*)(stk) + GPR_ROUND_UP_TO_ALIGNMENT_SIZE( \
                                              sizeof(grpc_channel_stack))))

#define CALL_ELEMS_FROM_STACK(stk)                                     \
  ((grpc_call_element*)((char*)(stk) + GPR_ROUND_UP_TO_ALIGNMENT_SIZE( \
                                           sizeof(grpc_call_stack))))

grpc_channel_element* grpc_channel_stack_element(
    grpc_channel_stack* channel_stack, size_t index) {
  return CHANNEL_ELEMS_FROM_STACK(channel_stack) + index;
}

grpc_channel_element* grpc_channel_stack_last_element(
    grpc_channel_stack* channel_stack) {
  return grpc_channel_stack_element(channel_stack, channel_stack->count - 1);
}

size_t grpc_channel_stack_filter_instance_number(
    grpc_channel_stack* channel_stack, grpc_channel_element* elem) {
  size_t num_found = 0;
  for (size_t i = 0; i < channel_stack->count; ++i) {
    grpc_channel_element* element =
        grpc_channel_stack_element(channel_stack, i);
    if (element == elem) break;
    if (element->filter == elem->filter) ++num_found;
  }
  return num_found;
}

grpc_call_element* grpc_call_stack_element(grpc_call_stack* call_stack,
                                           size_t index) {
  return CALL_ELEMS_FROM_STACK(call_stack) + index;
}

grpc_error_handle grpc_channel_stack_init(
    int initial_refs, grpc_iomgr_cb_func destroy, void* destroy_arg,
    const grpc_channel_filter** filters, size_t filter_count,
    const grpc_core::ChannelArgs& channel_args, const char* name,
    grpc_channel_stack* stack, const grpc_core::Blackboard* blackboard) {
  if (GRPC_TRACE_FLAG_ENABLED(channel_stack)) {
    LOG(INFO) << "CHANNEL_STACK: init " << name;
    for (size_t i = 0; i < filter_count; i++) {
      LOG(INFO) << "CHANNEL_STACK:   filter " << filters[i]->name;
    }
  }

  stack->on_destroy.Init([]() {});
  stack->event_engine.Init(channel_args.GetObjectRef<EventEngine>());
  stack->stats_plugin_group.Init();

  size_t call_size =
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call_stack)) +
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filter_count * sizeof(grpc_call_element));
  grpc_channel_element* elems;
  grpc_channel_element_args args;
  char* user_data;
  size_t i;

  stack->count = filter_count;
  GRPC_STREAM_REF_INIT(&stack->refcount, initial_refs, destroy, destroy_arg,
                       name);
  elems = CHANNEL_ELEMS_FROM_STACK(stack);
  user_data = (reinterpret_cast<char*>(elems)) +
              GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filter_count *
                                             sizeof(grpc_channel_element));

  // init per-filter data
  args.blackboard = blackboard;
  grpc_error_handle first_error;
  for (i = 0; i < filter_count; i++) {
    args.channel_stack = stack;
    args.channel_args = channel_args;
    args.is_first = i == 0;
    args.is_last = i == (filter_count - 1);
    elems[i].filter = filters[i];
    elems[i].channel_data = user_data;
    grpc_error_handle error =
        elems[i].filter->init_channel_elem(&elems[i], &args);
    if (!error.ok()) {
      if (first_error.ok()) {
        first_error = error;
      }
    }
    user_data +=
        GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_channel_data);
    call_size += GPR_ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_call_data);
  }

  CHECK(user_data > (char*)stack);
  CHECK((uintptr_t)(user_data - (char*)stack) ==
        grpc_channel_stack_size(filters, filter_count));

  stack->call_stack_size = call_size;
  stack->channelz_data_source.Init(
      channel_args.GetObjectRef<grpc_core::channelz::BaseNode>());
  return first_error;
}

void grpc_channel_stack::ChannelStackDataSource::AddData(
    grpc_core::channelz::DataSink sink) {
  grpc_channel_stack* channel_stack = reinterpret_cast<grpc_channel_stack*>(
      reinterpret_cast<char*>(this) -
      offsetof(grpc_channel_stack, channelz_data_source));
  sink.AddData(
      "channel_stack",
      grpc_core::channelz::PropertyList()
          .Set("type", "v1")
          .Set("elements", [channel_stack]() {
            grpc_core::channelz::PropertyTable elements;
            grpc_channel_element* elems =
                CHANNEL_ELEMS_FROM_STACK(channel_stack);
            for (size_t i = 0; i < channel_stack->count; i++) {
              grpc_channel_element& e = elems[i];
              elements.AppendRow(
                  grpc_core::channelz::PropertyList()
                      .Set("type", e.filter->name.name())
                      .Set("call_data_size", e.filter->sizeof_call_data)
                      .Set("channel_data_size", e.filter->sizeof_channel_data));
            }
            return elements;
          }()));
}

void grpc_channel_stack_destroy(grpc_channel_stack* stack) {
  grpc_channel_element* channel_elems = CHANNEL_ELEMS_FROM_STACK(stack);
  size_t count = stack->count;
  size_t i;

  stack->channelz_data_source.Destroy();

  // destroy per-filter data
  for (i = 0; i < count; i++) {
    channel_elems[i].filter->destroy_channel_elem(&channel_elems[i]);
  }

  (*stack->on_destroy)();
  stack->on_destroy.Destroy();
  stack->event_engine.Destroy();
  stack->stats_plugin_group.Destroy();
}

grpc_error_handle grpc_call_stack_init(
    grpc_channel_stack* channel_stack, int initial_refs,
    grpc_iomgr_cb_func destroy, void* destroy_arg,
    const grpc_call_element_args* elem_args) {
  grpc_channel_element* channel_elems = CHANNEL_ELEMS_FROM_STACK(channel_stack);
  size_t count = channel_stack->count;
  grpc_call_element* call_elems;
  char* user_data;

  elem_args->call_stack->count = count;
  GRPC_STREAM_REF_INIT(&elem_args->call_stack->refcount, initial_refs, destroy,
                       destroy_arg, "CALL_STACK");
  call_elems = CALL_ELEMS_FROM_STACK(elem_args->call_stack);
  user_data = (reinterpret_cast<char*>(call_elems)) +
              GPR_ROUND_UP_TO_ALIGNMENT_SIZE(count * sizeof(grpc_call_element));

  // init per-filter data
  grpc_error_handle first_error;
  for (size_t i = 0; i < count; i++) {
    call_elems[i].filter = channel_elems[i].filter;
    call_elems[i].channel_data = channel_elems[i].channel_data;
    call_elems[i].call_data = user_data;
    user_data +=
        GPR_ROUND_UP_TO_ALIGNMENT_SIZE(call_elems[i].filter->sizeof_call_data);
  }
  for (size_t i = 0; i < count; i++) {
    grpc_error_handle error =
        call_elems[i].filter->init_call_elem(&call_elems[i], elem_args);
    if (!error.ok()) {
      if (first_error.ok()) {
        first_error = error;
      }
    }
  }
  return first_error;
}

void grpc_call_stack_set_pollset_or_pollset_set(grpc_call_stack* call_stack,
                                                grpc_polling_entity* pollent) {
  size_t count = call_stack->count;
  grpc_call_element* call_elems;
  size_t i;

  call_elems = CALL_ELEMS_FROM_STACK(call_stack);

  // init per-filter data
  for (i = 0; i < count; i++) {
    call_elems[i].filter->set_pollset_or_pollset_set(&call_elems[i], pollent);
  }
}

void grpc_call_stack_ignore_set_pollset_or_pollset_set(
    grpc_call_element* /*elem*/, grpc_polling_entity* /*pollent*/) {}

void grpc_call_stack_destroy(grpc_call_stack* stack,
                             const grpc_call_final_info* final_info,
                             grpc_closure* then_schedule_closure) {
  grpc_call_element* elems = CALL_ELEMS_FROM_STACK(stack);
  size_t count = stack->count;
  size_t i;

  // destroy per-filter data
  for (i = 0; i < count; i++) {
    elems[i].filter->destroy_call_elem(
        &elems[i], final_info,
        i == count - 1 ? then_schedule_closure : nullptr);
  }
}

void grpc_call_next_op(grpc_call_element* elem,
                       grpc_transport_stream_op_batch* op) {
  grpc_call_element* next_elem = elem + 1;
  GRPC_TRACE_LOG(channel, INFO)
      << "OP[" << elem->filter->name << ":" << elem
      << "]: " << grpc_transport_stream_op_batch_string(op, false);
  next_elem->filter->start_transport_stream_op_batch(next_elem, op);
}

void grpc_channel_next_get_info(grpc_channel_element* elem,
                                const grpc_channel_info* channel_info) {
  grpc_channel_element* next_elem = elem + 1;
  next_elem->filter->get_channel_info(next_elem, channel_info);
}

void grpc_channel_next_op(grpc_channel_element* elem, grpc_transport_op* op) {
  grpc_channel_element* next_elem = elem + 1;
  next_elem->filter->start_transport_op(next_elem, op);
}

grpc_channel_stack* grpc_channel_stack_from_top_element(
    grpc_channel_element* elem) {
  return reinterpret_cast<grpc_channel_stack*>(
      reinterpret_cast<char*>(elem) -
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_channel_stack)));
}

grpc_call_stack* grpc_call_stack_from_top_element(grpc_call_element* elem) {
  return reinterpret_cast<grpc_call_stack*>(
      reinterpret_cast<char*>(elem) -
      GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call_stack)));
}

void grpc_channel_stack_no_post_init(grpc_channel_stack*,
                                     grpc_channel_element*) {}
