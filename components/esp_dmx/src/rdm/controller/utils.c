#include "rdm/controller/include/utils.h"

#include <string.h>

#include "dmx/include/driver.h"
#include "dmx/include/service.h"
#include "rdm/include/driver.h"
#include "rdm/include/uid.h"

size_t rdm_send_request(dmx_port_t dmx_num, const rdm_request_t *request,
                        const char *format, void *pd, size_t size,
                        rdm_ack_t *ack) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(request != NULL);
  assert(request->dest_uid != NULL);
  assert(request->sub_device < RDM_SUB_DEVICE_MAX ||
         request->sub_device == RDM_SUB_DEVICE_ALL);
  assert(request->pid > 0);
  assert(rdm_cc_is_valid(request->cc) && rdm_cc_is_request(request->cc));
  assert(request->sub_device != RDM_SUB_DEVICE_ALL ||
         request->cc == RDM_CC_SET_COMMAND);
  assert(rdm_format_is_valid(request->format));
  assert(request->format != NULL || request->pd == NULL);
  assert(request->pd != NULL || request->pdl == 0);
  assert(request->pdl < RDM_PD_SIZE_MAX);
  assert(rdm_format_is_valid(format));
  assert(dmx_driver_is_installed(dmx_num));

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  /* WIFIDMX: every way out leaves a true answer, never the caller's last one */
  if (ack != NULL) {
    *ack = (rdm_ack_t){.err = DMX_OK, .type = RDM_RESPONSE_TYPE_NONE};
  }

  // Attempt to take the mutex and wait until the driver is done sending
  if (!xSemaphoreTakeRecursive(driver->mux, portMAX_DELAY)) {
    return 0;
  }
  if (!dmx_wait_sent(dmx_num, dmx_ms_to_ticks(DMX_SEND_WAIT_MS))) {
    xSemaphoreGiveRecursive(driver->mux);
    return 0;
  }

  // Construct the header using the default arguments and the caller's arguments
  rdm_header_t header = {
      .message_len = 24 + request->pdl,
      .tn = rdm_get_transaction_num(dmx_num),
      .port_id = dmx_num + 1,
      .message_count = 0,
      .sub_device = request->sub_device,
      .cc = request->cc,
      .pid = request->pid,
      .pdl = request->pdl,
  };
  memcpy(&header.dest_uid, request->dest_uid, sizeof(header.dest_uid));
  memcpy(&header.src_uid, rdm_uid_get(dmx_num), sizeof(header.src_uid));

  // Copy the old data in the DMX buffer to a temporary buffer
  uint8_t old_data[257];
  const size_t packet_size = header.message_len + 2; 
  dmx_read(dmx_num, old_data, packet_size);

  // Write and send the RDM request
  const uint8_t tn = header.tn;
  /* WIFIDMX: if it can't be written, don't send what was in the buffer */
  if (!rdm_write(dmx_num, &header, request->format, request->pd) ||
      !dmx_send(dmx_num)) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->err = DMX_OK;
      ack->size = 0;
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Return early if no response is expected
  if (rdm_uid_is_broadcast(request->dest_uid) &&
      request->pid != RDM_PID_DISC_UNIQUE_BRANCH) {
    dmx_wait_sent(dmx_num, dmx_ms_to_ticks(DMX_SEND_WAIT_MS));
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->err = DMX_OK;
      ack->size = 0;
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Attempt to receive the RDM response
  dmx_packet_t packet;
  dmx_receive(dmx_num, &packet, dmx_ms_to_ticks(RDM_EXCHANGE_WAIT_MS));
  if (ack != NULL) {
    ack->err = packet.err;
    ack->size = packet.size;
  }

  // Return early if no response was received
  if (packet.size == 0) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Return early if the response checksum was invalid
  if (!rdm_read_header(dmx_num, &header)) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_INVALID;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  /* WIFIDMX: is it the answer to this request? Upstream took any packet
  with a good checksum: another fixture's, a late one, or its own request. */
  bool valid;
  if (request->pid == RDM_PID_DISC_UNIQUE_BRANCH &&
      request->cc == RDM_CC_DISC_COMMAND) {
    valid = header.cc == RDM_CC_DISC_COMMAND_RESPONSE &&
            header.pid == RDM_PID_DISC_UNIQUE_BRANCH;
  } else {
    // a queued message brings its own PID, and the response class with it
    const bool queued = request->pid == RDM_PID_QUEUED_MESSAGE &&
                        request->cc == RDM_CC_GET_COMMAND;
    valid = (header.cc == (rdm_cc_t)(request->cc + 1) ||
             (queued && header.cc == RDM_CC_SET_COMMAND_RESPONSE)) &&
            (header.pid == request->pid || queued) &&
            rdm_uid_is_eq(&header.src_uid, request->dest_uid) &&
            rdm_uid_is_eq(&header.dest_uid, rdm_uid_get(dmx_num)) &&
            header.pdl < RDM_PD_SIZE_MAX &&
            header.message_len == 24 + header.pdl &&
            packet.size >= (size_t)header.message_len + 2;
    if (valid && header.tn != tn) {
      ESP_LOGD(TAG, "response TN %u to request TN %u", (unsigned)header.tn,
               (unsigned)tn);  // some fixtures don't echo it: not fatal
    }
  }
  const bool is_timer_or_nack =
      header.response_type == RDM_RESPONSE_TYPE_ACK_TIMER ||
      header.response_type == RDM_RESPONSE_TYPE_NACK_REASON;
  if (valid) {
    valid = rdm_response_type_is_valid(header.response_type) &&
            (!is_timer_or_nack || header.pdl == sizeof(uint16_t));
  }
  if (!valid) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->type = RDM_RESPONSE_TYPE_INVALID;
    }
    return 0;
  }

  /* Copy the parameter data into the output (WIFIDMX: also when the caller
  doesn't want the ack: upstream only tried with pd == NULL) */
  if (!is_timer_or_nack && pd != NULL &&
      header.pid != RDM_PID_DISC_UNIQUE_BRANCH) {
    rdm_read_pd(dmx_num, format, pd, size);
  }

  // Copy the results into the ack struct
  if (ack != NULL) {
    memcpy(&ack->src_uid, &header.src_uid, sizeof(rdm_uid_t));
    ack->pid = header.pid;
    ack->type = header.response_type;
    if (is_timer_or_nack) {
      uint16_t value = 0;
      rdm_read_pd(dmx_num, "w", &value, sizeof(value));
      if (header.response_type == RDM_RESPONSE_TYPE_ACK_TIMER) {
        /* WIFIDMX: in tenths of a second (upstream took them as hundredths) */
        ack->timer = (TickType_t)((uint64_t)value * 100 * configTICK_RATE_HZ /
                                  1000);
      } else {
        ack->nack_reason = value;
      }
    } else {
      ack->pdl = header.pdl;
    }
    ack->message_count = header.message_count;
  }

  // Write the old data from before the request back into the DMX driver
  dmx_write(dmx_num, old_data, packet_size);

  // Give the mutex back and return the PDL or true on success
  xSemaphoreGiveRecursive(driver->mux);
  if (header.response_type == RDM_RESPONSE_TYPE_ACK) {
    if (header.pdl == 0) {
      return 1;
    } else {
      return header.pdl;
    }
  } else {

    return 0;
  }
}

uint32_t rdm_get_transaction_num(dmx_port_t dmx_num) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(dmx_driver_is_installed(dmx_num));

  uint32_t tn;
  taskENTER_CRITICAL(DMX_SPINLOCK(dmx_num));
  tn = dmx_driver[dmx_num]->rdm.tn;
  taskEXIT_CRITICAL(DMX_SPINLOCK(dmx_num));

  return tn;
}
