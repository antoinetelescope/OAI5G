/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* \file proto.h
 * \brief RRC functions prototypes for eNB and UE
 * \author R. Knopp, K.H. HSU
 * \date 2018
 * \version 0.1
 * \company Eurecom / NTUST
 * \email: knopp@eurecom.fr, kai-hsiang.hsu@eurecom.fr
 * \note
 * \warning
 */

#ifndef _RRC_PROTO_H_
#define _RRC_PROTO_H_

#include "oai_asn1.h"
#include "rrc_defs.h"
#include "NR_RRCReconfiguration.h"
#include "NR_MeasConfig.h"
#include "NR_CellGroupConfig.h"
#include "NR_RadioBearerConfig.h"
#include "common/utils/ocp_itti/intertask_interface.h"

NR_UE_RRC_INST_t *nr_rrc_init_ue(char* uecap_file, int nb_inst, int num_ant_tx);
NR_UE_RRC_INST_t* get_NR_UE_rrc_inst(int instance);
void init_nsa_message (NR_UE_RRC_INST_t *rrc, char* reconfig_file, char* rbconfig_file);

void process_nsa_message(NR_UE_RRC_INST_t *rrc, nsa_message_t nsa_message_type, void *message, int msg_len);

void nr_rrc_cellgroup_configuration(NR_UE_RRC_INST_t *rrc, NR_CellGroupConfig_t *cellGroupConfig, int gNB_index);

void nr_rrc_going_to_IDLE(NR_UE_RRC_INST_t *rrc,
                          NR_Release_Cause_t release_cause,
                          NR_RRCRelease_t *RRCRelease);

void handle_RRCRelease(NR_UE_RRC_INST_t *rrc);

void set_rlf_sib1_timers_and_constants(NR_UE_Timers_Constants_t *tac, NR_UE_TimersAndConstants_t *ue_TimersAndConstants);

/**\brief RRC UE task.
   \param void *args_p Pointer on arguments to start the task. */
void *rrc_nrue_task(void *args_p);
void *rrc_nrue(void *args_p);

void nr_rrc_handle_timers(NR_UE_RRC_INST_t *rrc);
void handle_rlf_detection(NR_UE_RRC_INST_t *rrc);

/**\brief RRC NSA UE task.
   \param void *args_p Pointer on arguments to start the task. */
void *recv_msgs_from_lte_ue(void *args_p);

void init_connections_with_lte_ue(void);

extern void start_oai_nrue_threads(void);

int get_from_lte_ue_fd();

void nr_rrc_SI_timers(NR_UE_RRC_SI_INFO *SInfo);
void init_SI_timers(NR_UE_RRC_SI_INFO *SInfo);

void nr_ue_rrc_timer_trigger(int module_id, int frame, int gnb_id);
void handle_t300_expiry(NR_UE_RRC_INST_t *rrc);
void handle_t430_expiry(NR_UE_RRC_INST_t *rrc);

void reset_rlf_timers_and_constants(NR_UE_Timers_Constants_t *tac);
void set_default_timers_and_constants(NR_UE_Timers_Constants_t *tac);
void nr_rrc_set_sib1_timers_and_constants(NR_UE_Timers_Constants_t *tac, NR_SIB1_t *sib1);
int nr_rrc_get_T304(long t304);
void handle_rlf_sync(NR_UE_Timers_Constants_t *tac, nr_sync_msg_t sync_msg);
void nr_rrc_handle_SetupRelease_RLF_TimersAndConstants(NR_UE_RRC_INST_t *rrc,
                                                       struct NR_SetupRelease_RLF_TimersAndConstants *rlf_TimersAndConstants);

int configure_NR_SL_Preconfig(NR_UE_RRC_INST_t *rrc,int sync_source);

void init_sidelink(NR_UE_RRC_INST_t *rrc);
void start_sidelink(int instance);

void rrc_ue_process_sidelink_Preconfiguration(NR_UE_RRC_INST_t *rrc_inst, int sync_ref);

void nr_rrc_ue_decode_NR_SBCCH_SL_BCH_Message(NR_UE_RRC_INST_t *rrc,
                                              const uint8_t gNB_index,
                                              const frame_t frame,
                                              const int slot,
                                              uint8_t* pduP,
                                              const sdu_size_t pdu_len,
                                              const uint16_t rx_slss_id);

/** @}*/
#endif

