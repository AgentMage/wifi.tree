#pragma once
#include <stdint.h>

// Spawns a FreeRTOS task that listens on UDP 53.
//
// spoof_all=true  (setup mode):  every query → ap_ip
// spoof_all=false (portal mode): captive-portal detection domains → ap_ip,
//                                everything else forwarded to 8.8.8.8
void dns_server_start(uint32_t ap_ip, bool spoof_all);
