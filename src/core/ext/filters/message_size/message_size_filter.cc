//
// Copyright 2016 gRPC authors.
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

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/message_size/message_size_filter.h"

#include <limits.h>
#include <string.h>

#include "absl/strings/str_format.h"

#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "src/core/ext/service_config/service_config_call_data.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/channel_stack_builder.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/ref_counted.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/surface/call.h"

static void recv_message_ready(void* user_data, grpc_error_handle error);
static void recv_trailing_metadata_ready(void* user_data,
                                         grpc_error_handle error);

namespace grpc_core {

namespace {
size_t g_message_size_parser_index;
}  // namespace

//
// MessageSizeParsedConfig
//

const MessageSizeParsedConfig* MessageSizeParsedConfig::GetFromCallContext(
    const grpc_call_context_element* context) {
  if (context == nullptr) return nullptr;
  auto* svc_cfg_call_data = static_cast<ServiceConfigCallData*>(
      context[GRPC_CONTEXT_SERVICE_CONFIG_CALL_DATA].value);
  if (svc_cfg_call_data == nullptr) return nullptr;
  return static_cast<const MessageSizeParsedConfig*>(
      svc_cfg_call_data->GetMethodParsedConfig(
          MessageSizeParser::ParserIndex()));
}

//
// MessageSizeParser
//

std::unique_ptr<ServiceConfigParser::ParsedConfig>
MessageSizeParser::ParsePerMethodParams(const grpc_channel_args* /*args*/,
                                        const Json& json,
                                        grpc_error_handle* error) {
  GPR_DEBUG_ASSERT(error != nullptr && *error == GRPC_ERROR_NONE);
  std::vector<grpc_error_handle> error_list;
  // Max request size.
  int max_request_message_bytes = -1;
  auto it = json.object_value().find("maxRequestMessageBytes");
  if (it != json.object_value().end()) {
    if (it->second.type() != Json::Type::STRING &&
        it->second.type() != Json::Type::NUMBER) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:maxRequestMessageBytes error:should be of type number"));
    } else {
      max_request_message_bytes =
          gpr_parse_nonnegative_int(it->second.string_value().c_str());
      if (max_request_message_bytes == -1) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:maxRequestMessageBytes error:should be non-negative"));
      }
    }
  }
  // Max response size.
  int max_response_message_bytes = -1;
  it = json.object_value().find("maxResponseMessageBytes");
  if (it != json.object_value().end()) {
    if (it->second.type() != Json::Type::STRING &&
        it->second.type() != Json::Type::NUMBER) {
      error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "field:maxResponseMessageBytes error:should be of type number"));
    } else {
      max_response_message_bytes =
          gpr_parse_nonnegative_int(it->second.string_value().c_str());
      if (max_response_message_bytes == -1) {
        error_list.push_back(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
            "field:maxResponseMessageBytes error:should be non-negative"));
      }
    }
  }
  if (!error_list.empty()) {
    *error = GRPC_ERROR_CREATE_FROM_VECTOR("Message size parser", &error_list);
    return nullptr;
  }
  return absl::make_unique<MessageSizeParsedConfig>(max_request_message_bytes,
                                                    max_response_message_bytes);
}

void MessageSizeParser::Register() {
  g_message_size_parser_index = ServiceConfigParser::RegisterParser(
      absl::make_unique<MessageSizeParser>());
}

size_t MessageSizeParser::ParserIndex() { return g_message_size_parser_index; }

int GetMaxRecvSizeFromChannelArgs(const grpc_channel_args* args) {
  if (grpc_channel_args_want_minimal_stack(args)) return -1;
  return grpc_channel_args_find_integer(
      args, GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH,
      {GRPC_DEFAULT_MAX_RECV_MESSAGE_LENGTH, -1, INT_MAX});
}

int GetMaxSendSizeFromChannelArgs(const grpc_channel_args* args) {
  if (grpc_channel_args_want_minimal_stack(args)) return -1;
  return grpc_channel_args_find_integer(
      args, GRPC_ARG_MAX_SEND_MESSAGE_LENGTH,
      {GRPC_DEFAULT_MAX_SEND_MESSAGE_LENGTH, -1, INT_MAX});
}

}  // namespace grpc_core

namespace {
struct channel_data {
  grpc_core::MessageSizeParsedConfig::message_size_limits limits;
};

struct call_data {
  call_data(grpc_call_element* elem, const channel_data& chand,
            const grpc_call_element_args& args)
      : call_combiner(args.call_combiner), limits(chand.limits) {
    GRPC_CLOSURE_INIT(&recv_message_ready, ::recv_message_ready, elem,
                      grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&recv_trailing_metadata_ready,
                      ::recv_trailing_metadata_ready, elem,
                      grpc_schedule_on_exec_ctx);
    // Get max sizes from channel data, then merge in per-method config values.
    // Note: Per-method config is only available on the client, so we
    // apply the max request size to the send limit and the max response
    // size to the receive limit.
    const grpc_core::MessageSizeParsedConfig* limits =
        grpc_core::MessageSizeParsedConfig::GetFromCallContext(args.context);
    if (limits != nullptr) {
      if (limits->limits().max_send_size >= 0 &&
          (limits->limits().max_send_size < this->limits.max_send_size ||
           this->limits.max_send_size < 0)) {
        this->limits.max_send_size = limits->limits().max_send_size;
      }
      if (limits->limits().max_recv_size >= 0 &&
          (limits->limits().max_recv_size < this->limits.max_recv_size ||
           this->limits.max_recv_size < 0)) {
        this->limits.max_recv_size = limits->limits().max_recv_size;
      }
    }
  }

  ~call_data() { GRPC_ERROR_UNREF(error); }

  grpc_core::CallCombiner* call_combiner;
  grpc_core::MessageSizeParsedConfig::message_size_limits limits;
  // Receive closures are chained: we inject this closure as the
  // recv_message_ready up-call on transport_stream_op, and remember to
  // call our next_recv_message_ready member after handling it.
  grpc_closure recv_message_ready;
  grpc_closure recv_trailing_metadata_ready;
  // The error caused by a message that is too large, or GRPC_ERROR_NONE
  grpc_error_handle error = GRPC_ERROR_NONE;
  // Used by recv_message_ready.
  grpc_core::OrphanablePtr<grpc_core::ByteStream>* recv_message = nullptr;
  // Original recv_message_ready callback, invoked after our own.
  grpc_closure* next_recv_message_ready = nullptr;
  // Original recv_trailing_metadata callback, invoked after our own.
  grpc_closure* original_recv_trailing_metadata_ready;
  bool seen_recv_trailing_metadata = false;
  grpc_error_handle recv_trailing_metadata_error;
};

}  // namespace

// Callback invoked when we receive a message.  Here we check the max
// receive message size.
static void recv_message_ready(void* user_data, grpc_error_handle error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(user_data);
  call_data* calld = static_cast<call_data*>(elem->call_data);
  if (*calld->recv_message != nullptr && calld->limits.max_recv_size >= 0 &&
      (*calld->recv_message)->length() >
          static_cast<size_t>(calld->limits.max_recv_size)) {
    grpc_error_handle new_error = grpc_error_set_int(
        GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrFormat(
            "Received message larger than max (%u vs. %d)",
            (*calld->recv_message)->length(), calld->limits.max_recv_size)),
        GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_RESOURCE_EXHAUSTED);
    error = grpc_error_add_child(GRPC_ERROR_REF(error), new_error);
    GRPC_ERROR_UNREF(calld->error);
    calld->error = GRPC_ERROR_REF(error);
  } else {
    GRPC_ERROR_REF(error);
  }
  // Invoke the next callback.
  grpc_closure* closure = calld->next_recv_message_ready;
  calld->next_recv_message_ready = nullptr;
  if (calld->seen_recv_trailing_metadata) {
    /* We might potentially see another RECV_MESSAGE op. In that case, we do not
     * want to run the recv_trailing_metadata_ready closure again. The newer
     * RECV_MESSAGE op cannot cause any errors since the transport has already
     * invoked the recv_trailing_metadata_ready closure and all further
     * RECV_MESSAGE ops will get null payloads. */
    calld->seen_recv_trailing_metadata = false;
    GRPC_CALL_COMBINER_START(calld->call_combiner,
                             &calld->recv_trailing_metadata_ready,
                             calld->recv_trailing_metadata_error,
                             "continue recv_trailing_metadata_ready");
  }
  grpc_core::Closure::Run(DEBUG_LOCATION, closure, error);
}

// Callback invoked on completion of recv_trailing_metadata
// Notifies the recv_trailing_metadata batch of any message size failures
static void recv_trailing_metadata_ready(void* user_data,
                                         grpc_error_handle error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(user_data);
  call_data* calld = static_cast<call_data*>(elem->call_data);
  if (calld->next_recv_message_ready != nullptr) {
    calld->seen_recv_trailing_metadata = true;
    calld->recv_trailing_metadata_error = GRPC_ERROR_REF(error);
    GRPC_CALL_COMBINER_STOP(calld->call_combiner,
                            "deferring recv_trailing_metadata_ready until "
                            "after recv_message_ready");
    return;
  }
  error =
      grpc_error_add_child(GRPC_ERROR_REF(error), GRPC_ERROR_REF(calld->error));
  // Invoke the next callback.
  grpc_core::Closure::Run(DEBUG_LOCATION,
                          calld->original_recv_trailing_metadata_ready, error);
}

// Start transport stream op.
static void message_size_start_transport_stream_op_batch(
    grpc_call_element* elem, grpc_transport_stream_op_batch* op) {
  call_data* calld = static_cast<call_data*>(elem->call_data);
  // Check max send message size.
  if (op->send_message && calld->limits.max_send_size >= 0 &&
      op->payload->send_message.send_message->length() >
          static_cast<size_t>(calld->limits.max_send_size)) {
    grpc_transport_stream_op_batch_finish_with_failure(
        op,
        grpc_error_set_int(GRPC_ERROR_CREATE_FROM_CPP_STRING(absl::StrFormat(
                               "Sent message larger than max (%u vs. %d)",
                               op->payload->send_message.send_message->length(),
                               calld->limits.max_send_size)),
                           GRPC_ERROR_INT_GRPC_STATUS,
                           GRPC_STATUS_RESOURCE_EXHAUSTED),
        calld->call_combiner);
    return;
  }
  // Inject callback for receiving a message.
  if (op->recv_message) {
    calld->next_recv_message_ready =
        op->payload->recv_message.recv_message_ready;
    calld->recv_message = op->payload->recv_message.recv_message;
    op->payload->recv_message.recv_message_ready = &calld->recv_message_ready;
  }
  // Inject callback for receiving trailing metadata.
  if (op->recv_trailing_metadata) {
    calld->original_recv_trailing_metadata_ready =
        op->payload->recv_trailing_metadata.recv_trailing_metadata_ready;
    op->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
        &calld->recv_trailing_metadata_ready;
  }
  // Chain to the next filter.
  grpc_call_next_op(elem, op);
}

// Constructor for call_data.
static grpc_error_handle message_size_init_call_elem(
    grpc_call_element* elem, const grpc_call_element_args* args) {
  channel_data* chand = static_cast<channel_data*>(elem->channel_data);
  new (elem->call_data) call_data(elem, *chand, *args);
  return GRPC_ERROR_NONE;
}

// Destructor for call_data.
static void message_size_destroy_call_elem(
    grpc_call_element* elem, const grpc_call_final_info* /*final_info*/,
    grpc_closure* /*ignored*/) {
  call_data* calld = static_cast<call_data*>(elem->call_data);
  calld->~call_data();
}

grpc_core::MessageSizeParsedConfig::message_size_limits get_message_size_limits(
    const grpc_channel_args* channel_args) {
  grpc_core::MessageSizeParsedConfig::message_size_limits lim;
  lim.max_send_size = grpc_core::GetMaxSendSizeFromChannelArgs(channel_args);
  lim.max_recv_size = grpc_core::GetMaxRecvSizeFromChannelArgs(channel_args);
  return lim;
}

// Constructor for channel_data.
static grpc_error_handle message_size_init_channel_elem(
    grpc_channel_element* elem, grpc_channel_element_args* args) {
  GPR_ASSERT(!args->is_last);
  channel_data* chand = static_cast<channel_data*>(elem->channel_data);
  new (chand) channel_data();
  chand->limits = get_message_size_limits(args->channel_args);
  return GRPC_ERROR_NONE;
}

// Destructor for channel_data.
static void message_size_destroy_channel_elem(grpc_channel_element* elem) {
  channel_data* chand = static_cast<channel_data*>(elem->channel_data);
  chand->~channel_data();
}

const grpc_channel_filter grpc_message_size_filter = {
    message_size_start_transport_stream_op_batch,
    grpc_channel_next_op,
    sizeof(call_data),
    message_size_init_call_elem,
    grpc_call_stack_ignore_set_pollset_or_pollset_set,
    message_size_destroy_call_elem,
    sizeof(channel_data),
    message_size_init_channel_elem,
    message_size_destroy_channel_elem,
    grpc_channel_next_get_info,
    "message_size"};

// Used for GRPC_CLIENT_SUBCHANNEL
static bool maybe_add_message_size_filter_subchannel(
    grpc_channel_stack_builder* builder) {
  const grpc_channel_args* channel_args =
      grpc_channel_stack_builder_get_channel_arguments(builder);
  if (grpc_channel_args_want_minimal_stack(channel_args)) {
    return true;
  }
  return grpc_channel_stack_builder_prepend_filter(
      builder, &grpc_message_size_filter, nullptr, nullptr);
}

// Used for GRPC_CLIENT_DIRECT_CHANNEL and GRPC_SERVER_CHANNEL. Adds the filter
// only if message size limits or service config is specified.
static bool maybe_add_message_size_filter(grpc_channel_stack_builder* builder) {
  const grpc_channel_args* channel_args =
      grpc_channel_stack_builder_get_channel_arguments(builder);
  if (grpc_channel_args_want_minimal_stack(channel_args)) {
    return true;
  }
  bool enable = false;
  grpc_core::MessageSizeParsedConfig::message_size_limits lim =
      get_message_size_limits(channel_args);
  if (lim.max_send_size != -1 || lim.max_recv_size != -1) {
    enable = true;
  }
  const grpc_arg* a =
      grpc_channel_args_find(channel_args, GRPC_ARG_SERVICE_CONFIG);
  const char* svc_cfg_str = grpc_channel_arg_get_string(a);
  if (svc_cfg_str != nullptr) {
    enable = true;
  }
  if (enable) {
    return grpc_channel_stack_builder_prepend_filter(
        builder, &grpc_message_size_filter, nullptr, nullptr);
  } else {
    return true;
  }
}

void grpc_message_size_filter_init(void) {
  grpc_core::MessageSizeParser::Register();
}

void grpc_message_size_filter_shutdown(void) {}

namespace grpc_core {
void RegisterMessageSizeFilter(CoreConfiguration::Builder* builder) {
  builder->channel_init()->RegisterStage(
      GRPC_CLIENT_SUBCHANNEL, GRPC_CHANNEL_INIT_BUILTIN_PRIORITY,
      maybe_add_message_size_filter_subchannel);
  builder->channel_init()->RegisterStage(GRPC_CLIENT_DIRECT_CHANNEL,
                                         GRPC_CHANNEL_INIT_BUILTIN_PRIORITY,
                                         maybe_add_message_size_filter);
  builder->channel_init()->RegisterStage(GRPC_SERVER_CHANNEL,
                                         GRPC_CHANNEL_INIT_BUILTIN_PRIORITY,
                                         maybe_add_message_size_filter);
}
}  // namespace grpc_core
