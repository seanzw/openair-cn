#include "sgw_gtpv1u_listener.h"

#include "common_defs.h"
#include "intertask_interface.h"
#include "log.h"

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
                                        sgw_gtpv1u_listener_recv_t* message) {
  // Read GTP-U message type.
  message->gtpv1u_msg_type = buffer[1];

  // DEST_IP:2152
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "*(BUFFER + 24) = %x\n",
              *(buffer + 24));
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "*(uint32_t*)(buffer + 24) = %x\n",
              *(uint32_t*)(buffer + 24));
  // message->old_sgw_gtpv1u_ip = *(uint32_t*)(buffer + 24);

  message->old_sgw_gtpv1u_ip =
      (((uint32_t)(buffer[24])) << 24) + (((uint32_t)(buffer[25])) << 16) +
      (((uint32_t)(buffer[26])) << 8) + (((uint32_t)(buffer[27])));

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "ip = %x\n",
              message->old_sgw_gtpv1u_ip);
  message->old_sgw_gtpv1u_port = 2152;

  size_t dpcmStatesOffset = 28;
  size_t dpcmStatesSize = n - dpcmStatesOffset;
  message->buffer = malloc(dpcmStatesSize);
  memset(message->buffer, 0, dpcmStatesSize);
  memcpy(message->buffer, buffer + dpcmStatesOffset, dpcmStatesSize);
  message->buffer_length = dpcmStatesSize;

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Parsed DPCM States. Msg type %d. Old gateway ip %x\n",
              message->gtpv1u_msg_type, message->old_sgw_gtpv1u_ip);

  return 0;
}

static int sgw_gtpv1u_forward_to_old_sgw(
    int udpfd, const sgw_gtpv1u_listener_recv_t* message, char* buffer,
    size_t n) {
  struct sockaddr_in old_sgw_addr;
  memset(&old_sgw_addr, 0, sizeof(old_sgw_addr));
  old_sgw_addr.sin_family = AF_INET;
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "forward to ip = %x\n",
              message->old_sgw_gtpv1u_ip);
  old_sgw_addr.sin_addr.s_addr = htonl(message->old_sgw_gtpv1u_ip);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "after to ns = %x\n",
              old_sgw_addr.sin_addr.s_addr);
  old_sgw_addr.sin_port = htons(message->old_sgw_gtpv1u_port);

  const size_t old_sgw_addr_str_buffer_length = 32;
  char old_sgw_addr_str_buffer[old_sgw_addr_str_buffer_length];
  inet_ntop(AF_INET, &(old_sgw_addr.sin_addr), old_sgw_addr_str_buffer,
            old_sgw_addr_str_buffer_length);
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Forward to old SGW at %s:%d\n",
              old_sgw_addr_str_buffer, ntohs(old_sgw_addr.sin_port));

  // Set GTPV1U message type to 28.
  buffer[1] = 28;

  int num = sendto(udpfd, buffer, n, 0, (struct sockaddr*)&old_sgw_addr,
                   sizeof(old_sgw_addr));
  if (num < 0) {
    OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER,
                    "Failed forwarding to old SGW at %s:%d\n",
                    old_sgw_addr_str_buffer, ntohs(old_sgw_addr.sin_port));
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

    // Send to SGW-APP task.
    MessageDef* message;
    message = itti_alloc_new_message(TASK_UNKNOWN, SGW_GTPV1U_LISTENER_RECV);
    int rv = parse_sgw_gtpv1u_recv_packet(
        buffer, n, &message->ittiMsg.sgw_gtpv1u_listener_recv);
    if (rv < 0) {
      OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER, "Failed parse message\n");
      continue;
    }

    // Forward to old gateway.
    if (message->ittiMsg.sgw_gtpv1u_listener_recv.old_sgw_gtpv1u_ip !=
        _s1u_ip) {
      rv = sgw_gtpv1u_forward_to_old_sgw(
          _fdv1u, &message->ittiMsg.sgw_gtpv1u_listener_recv, buffer, n);
    }

    rv = itti_send_msg_to_task(TASK_SPGW_APP, INSTANCE_DEFAULT, message);
    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Send message to task returned %d\n",
                rv);
  }
}

int sgw_gtpv1u_listener_init(const int fdv1u, const uint32_t s1u_ip) {
  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Initializing SGW GTPV1U Listener with fd %d\n", fdv1u);

  _fdv1u = fdv1u;
  _s1u_ip = s1u_ip;

  int ret = pthread_create(&_listener_thread, NULL, &sgw_gtpv1u_listener, NULL);
  if (ret != 0) {
    OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER,
                    "Failed creating GTPV1U listner\n");
    return RETURNerror;
  }
  // Never join.

  OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER,
              "Initializing SGW GTPV1U Listener: DONE\n");
  return RETURNok;
}