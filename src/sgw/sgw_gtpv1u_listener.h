#include "spgw_config.h"
#include "intertask_interface.h"

int sgw_gtpv1u_listener_init(const int fdv1u, const uint32_t s1u_ip);
void sgw_send_dpcm_msg_to_task(task_id_t task, sgw_gtpv1u_dpcm_msg_t* dpcm_msg);