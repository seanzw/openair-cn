#include "sgw_gtpv1u_listener.h"

#include "common_defs.h"
#include "log.h"
#include "intertask_interface.h"

#include <sys/types.h> 
#include <sys/socket.h>
#include <pthread.h>

// OHHH I JUST WANT TO HACK NOW.
#ifdef OAILOG_INFO
#undef OAILOG_INFO
#define OAILOG_INFO OAILOG_CRITICAL
#endif

static void* sgw_gtpv1u_listener(void* p_fdv1u) {

    int fdv1u = *(int*)(p_fdv1u);
    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Listen to fd %d\n", fdv1u);

    const size_t BUFFER_SIZE = 2048;
    char buffer[BUFFER_SIZE];

    struct sockaddr_in client_addr;
    int client_len;
    while (1) {
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Waiting for input\n");
        memset(buffer, 0, BUFFER_SIZE);
        int n = recvfrom(
            fdv1u, 
            buffer, 
            BUFFER_SIZE, 
            0,
            (struct sockaddr*)&client_addr, 
            &client_len);
        if (n < 0) {
            OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER, "Error in recvfrom\n");
            continue;
        }
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, 
            "Received %d/%d bytes: %s\n",  strlen(buffer), n, buffer);

        // Send to SGW-APP task.
        MessageDef* message;
        message = itti_alloc_new_message(TASK_UNKNOWN, SGW_GTPV1U_LISTENER_RECV);
        message->ittiMsg.sgw_gtpv1u_listener_recv.buffer =
            malloc(n + 1);
        memset(message->ittiMsg.sgw_gtpv1u_listener_recv.buffer, 0, n + 1);
        memcpy(message->ittiMsg.sgw_gtpv1u_listener_recv.buffer, buffer, n);
        message->ittiMsg.sgw_gtpv1u_listener_recv.buffer_length = n + 1;

        int rv = itti_send_msg_to_task(TASK_SPGW_APP, INSTANCE_DEFAULT, message);
        OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Send message to task returned %d\n", rv);
    }
}

// Some global variables.
static pthread_t _listener_thread;
static int _fdv1u;

int sgw_gtpv1u_listener_init(const int fdv1u) {
    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, 
        "Initializing SGW GTPV1U Listener with fd %d\n", fdv1u);

    _fdv1u = fdv1u;

    int ret = pthread_create(&_listener_thread, NULL, &sgw_gtpv1u_listener, &_fdv1u);
    if (ret != 0) {
        OAILOG_CRITICAL(LOG_SPGW_GTPV1U_LISTENER, "Failed creating GTPV1U listner\n");
        return RETURNerror;
    }
    // Never join.

    OAILOG_INFO(LOG_SPGW_GTPV1U_LISTENER, "Initializing SGW GTPV1U Listener: DONE\n");
    return RETURNok;
}