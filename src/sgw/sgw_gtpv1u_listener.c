#include "sgw_gtpv1u_listener.h"

#include "common_defs.h"
#include "log.h"
#include "msc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/ip.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>

// OHHH I JUST WANT TO HACK NOW.
#ifdef OAILOG_INFO
#undef OAILOG_INFO
#define OAILOG_INFO OAILOG_CRITICAL
#endif

// Some global variables.
static pthread_t _listener_thread;
static int _fdv1u;
// My ip address for s1u.
static uint32_t _s1u_ip;
// Used to forward to old_gateway.
static int _forward_udp_fd;

// Parse the received DPCM states into the message.
// Return 0 if succeed.
static int parse_sgw_gtpv1u_recv_packet(const char* buffer, int n,
                                        sgw_gtpv1u_dpcm_msg_t* message) {
  // Read GTP-U message type.
  message->gtpv1u_msg_type = buffer[1];

  // DEST_IP:2152
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "*(BUFFER + 24) = %x\n",
              *(buffer + 24));
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "*(uint32_t*)(buffer + 24) = %x\n",
              *(uint32_t*)(buffer + 24));
  // message->old_sgw_gtpv1u_ip = *(uint32_t*)(buffer + 24);

  message->ip = (((uint32_t)(buffer[24])) << 24) +
                (((uint32_t)(buffer[25])) << 16) +
                (((uint32_t)(buffer[26])) << 8) + (((uint32_t)(buffer[27])));

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "ip = %x\n", message->ip);
  message->port = 2152;

  size_t dpcmStatesOffset = 28;
  size_t dpcmStatesSize = n - dpcmStatesOffset;
  message->payload_buffer = malloc(dpcmStatesSize);
  memset(message->payload_buffer, 0, dpcmStatesSize);
  memcpy(message->payload_buffer, buffer + dpcmStatesOffset, dpcmStatesSize);
  message->payload_length = dpcmStatesSize;

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Parsed DPCM States. Msg type %d. Old gateway ip %x\n",
              message->gtpv1u_msg_type, message->ip);

  return 0;
}

static int sgw_gtpv1u_send_dpcm_msg(int udpfd,
                                    const sgw_gtpv1u_dpcm_msg_t* message) {
  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "forward to ip = %x\n", message->ip);
  dst_addr.sin_addr.s_addr = htonl(message->ip);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "after to ns = %x\n",
              dst_addr.sin_addr.s_addr);
  dst_addr.sin_port = htons(message->port);

  const size_t dst_addr_str_buffer_length = 32;
  char dst_addr_str_buffer[dst_addr_str_buffer_length];
  inet_ntop(AF_INET, &(dst_addr.sin_addr), dst_addr_str_buffer,
            dst_addr_str_buffer_length);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Forward to old SGW at %s:%d\n",
              dst_addr_str_buffer, ntohs(dst_addr.sin_port));

  // append GTPU header and IP header to payload
  const size_t GTPU_HEADER_LENGTH = 8;
  const size_t payload_with_header_length =
      GTPU_HEADER_LENGTH + sizeof(struct iphdr) + message->payload_length;
  char* payload_with_header = malloc(payload_with_header_length);
  memcpy(payload_with_header + GTPU_HEADER_LENGTH + sizeof(struct iphdr),
         message->payload_buffer, message->payload_length);

  // Set GTPV1U message type.
  payload_with_header[1] = message->gtpv1u_msg_type;

  struct iphdr* ip_header =
      (struct iphdr*)(payload_with_header + GTPU_HEADER_LENGTH);
  ip_header->version = 4;
  ip_header->tot_len = sizeof(struct iphdr) + message->payload_length;
  ip_header->protocol = IPPROTO_RAW;
  ip_header->daddr = inet_addr(dst_addr_str_buffer);

  int num = sendto(udpfd, payload_with_header, payload_with_header_length, 0,
                   (struct sockaddr*)&dst_addr, sizeof(dst_addr));

  free(payload_with_header);
  if (num < 0) {
    OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER,
                    "Failed forwarding to old SGW at %s:%d\n",
                    dst_addr_str_buffer, ntohs(dst_addr.sin_port));
    return RETURNerror;
  }

  return RETURNok;
}

static void sgw_gtpv1u_print_payload(char* buffer, size_t n) {
  // Print the payload in hex.
  const size_t HEX_BUFFER_SIZE = 4096;
  char hex_buffer[HEX_BUFFER_SIZE];
  for (size_t i = 0, pos = 0; i < n; ++i) {
    if (pos >= HEX_BUFFER_SIZE) {
      OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                  "Out of boundary! Boom! Not send to old "
                  "gateway.\n");
      return;
    }
    pos += sprintf(hex_buffer + pos, "%d:0x%02x\n", i, (unsigned)buffer[i]);
  }
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Payload: %s\n", hex_buffer);
}

static void deep_cpy(sgw_gtpv1u_dpcm_msg_t* dst, sgw_gtpv1u_dpcm_msg_t* src) {
  memcpy(dst, src, sizeof(sgw_gtpv1u_dpcm_msg_t));
  dst->payload_buffer = malloc(dst->payload_length);
  memcpy(dst->payload_buffer, src->payload_buffer, dst->payload_length);
}

// Take a copy of the dpcm_msg.
void sgw_send_dpcm_msg_to_task(task_id_t task,
                               sgw_gtpv1u_dpcm_msg_t* dpcm_msg) {
  MessageDef* message;
  message = itti_alloc_new_message(TASK_UNKNOWN, SGW_GTPV1U_DPCM_MSG);
  deep_cpy(&message->ittiMsg.sgw_gtpv1u_dpcm_msg, dpcm_msg);

  int rv = itti_send_msg_to_task(task, INSTANCE_DEFAULT, message);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Send message to task returned %d\n",
              rv);
}

static void* sgw_gtpv1u_listener(void* unused) {
  int fdv1u = _fdv1u;
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Listen to fd %d\n", fdv1u);

  const size_t BUFFER_SIZE = 4096;
  char buffer[BUFFER_SIZE];

  struct sockaddr_in client_addr;
  int client_len;
  while (1) {
    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Waiting for input\n");
    memset(buffer, 0, BUFFER_SIZE);
    int n = recvfrom(fdv1u, buffer, BUFFER_SIZE, 0,
                     (struct sockaddr*)&client_addr, &client_len);
    if (n < 0) {
      OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER, "Error in recvfrom\n");
      continue;
    }

    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Received %d bytes: %s\n", n, buffer);
    sgw_gtpv1u_print_payload(buffer, n);

    // Parse message.
    sgw_gtpv1u_dpcm_msg_t dpcm_msg;
    int rv = parse_sgw_gtpv1u_recv_packet(buffer, n, &dpcm_msg);
    if (rv < 0) {
      OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER, "Failed parse message\n");
      continue;
    }

    switch (dpcm_msg.gtpv1u_msg_type) {
      case DPCM_MSG_TYPE_P12_2: {
        // P12-2.
        // Forward to GW-APP task.
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                    "P12-2: New GW Received, forward SPGW-APP msg type %u\n",
                    dpcm_msg.gtpv1u_msg_type);
        dpcm_msg.ip = ntohl(client_addr.sin_addr.s_addr); // change to the gateway's own ip
        sgw_send_dpcm_msg_to_task(TASK_SPGW_APP, &dpcm_msg);

        // Forward to old gw.
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                    "P12-2: New GW Received, forward to 0x%x\n", dpcm_msg.ip);
        dpcm_msg.gtpv1u_msg_type = DPCM_MSG_TYPE_P12_3;
        sgw_send_dpcm_msg_to_task(TASK_DPCM_GW_SOCKET_SEND, &dpcm_msg);
        break;
      }
      case DPCM_MSG_TYPE_P12_3: {
        // P12-3. I am old GW.
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                    "P12-3: Old GW received ip 0x%x.\n", dpcm_msg.ip);

        // Change ip to new gateway (sender)'s ip.
        dpcm_msg.ip = ntohl(client_addr.sin_addr.s_addr);
        dpcm_msg.port = ntohs(client_addr.sin_port);
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "New GW is at %x:%u\n", dpcm_msg.ip,
                    dpcm_msg.port);

        // Forward to GW-APP task.
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                    "P12-3: Old GW Received, forward SPGW-APP msg type %u\n",
                    dpcm_msg.gtpv1u_msg_type);
        sgw_send_dpcm_msg_to_task(TASK_SPGW_APP, &dpcm_msg);
        break;
      }
      case DPCM_MSG_TYPE_P13_PROPOSE: {
        // New GW received old GW's propose (P13).
        // Forward to GW-APP task.
        OAILOG_INFO(
            LOG_SPGW_GTPV1U_LISTENER,
            "P13-Propose: New GW Received, forward SPGW-APP msg type %u\n",
            dpcm_msg.gtpv1u_msg_type);
        sgw_send_dpcm_msg_to_task(TASK_SPGW_APP, &dpcm_msg);
        break;
      }
      case DPCM_MSG_TYPE_P13_RESPONSE: {
        // Old GW received P13 response.
        // Can be either accept or reject with updated states.
        // Forward to GW-APP task.
        OAILOG_INFO(
            LOG_SPGW_GTPV1U_LISTENER,
            "P13-Response: Old GW Received, forward SPGW-APP msg type %u\n",
            dpcm_msg.gtpv1u_msg_type);
        sgw_send_dpcm_msg_to_task(TASK_SPGW_APP, &dpcm_msg);
        break;
      }
    }
    free(dpcm_msg.payload_buffer);
  }
  return 0;
}

static void* sgw_gtpv1u_dpcm_socket_send_thread(void* args) {
  itti_mark_task_ready(TASK_DPCM_GW_SOCKET_SEND);
  OAILOG_START_USE();
  MSC_START_USE();

  const int fdv1u = (int)args;
  while (1) {
    MessageDef* received_message = NULL;
    itti_receive_msg(TASK_DPCM_GW_SOCKET_SEND, &received_message);
    switch (ITTI_MSG_ID(received_message)) {
      case SGW_GTPV1U_DPCM_MSG: {
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
                    "Received sending request to 0x%x\n",
                    received_message->ittiMsg.sgw_gtpv1u_dpcm_msg.ip);
        sgw_gtpv1u_dpcm_msg_t* dpcm_msg =
            &received_message->ittiMsg.sgw_gtpv1u_dpcm_msg;
        sgw_gtpv1u_send_dpcm_msg(fdv1u, dpcm_msg);
        // Remember to free the buffer.
        free(dpcm_msg->payload_buffer);
        break;
      }
      default: {
        OAILOG_ERROR(LOG_SPGW_GTPV1U_LISTENER, "Unkown message received\n");
        break;
      }
    }
    itti_free(ITTI_MSG_ORIGIN_ID(received_message), received_message);
    received_message = NULL;
  }
  return 0;
}

int sgw_gtpv1u_dpcm_socket_send_init(const int fdv1u) {
  int ret = itti_create_task(TASK_DPCM_GW_SOCKET_SEND,
                             &sgw_gtpv1u_dpcm_socket_send_thread, fdv1u);
  if (ret < 0) {
    OAILOG_ERROR(LOG_SPGW_GTPV1U_LISTENER,
                 "Initialize dpcm socket send thread: %s\n", strerror(errno));
    return -1;
  }
  return 0;
}

int sgw_gtpv1u_listener_init(const int fdv1u, const uint32_t s1u_ip) {
  _fdv1u = fdv1u;
  _s1u_ip = ntohl(s1u_ip);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Initializing SGW GTPV1U Listener with fd %d, self ip 0x%x\n",
              fdv1u, _s1u_ip);

  int ret = pthread_create(&_listener_thread, NULL, &sgw_gtpv1u_listener, NULL);
  if (ret != 0) {
    OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER,
                    "Failed creating GTPV1U listner\n");
    return RETURNerror;
  }
  // Never join.

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Initialize socket sender task\n");
  sgw_gtpv1u_dpcm_socket_send_init(fdv1u);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Initialize socket sender task: DONE\n");

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Initializing SGW GTPV1U Listener: DONE\n");
  return RETURNok;
}
