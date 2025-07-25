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

/*
                                 platform_types.h
                             -------------------
  AUTHOR  : Lionel GAUTHIER
  COMPANY : EURECOM
  EMAIL   : Lionel.Gauthier@eurecom.fr

 ***************************************************************************/
#ifndef __PLATFORM_TYPES_H__
#define __PLATFORM_TYPES_H__

#include <stdint.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// GENERIC ACCESS STRATUM TYPES
//-----------------------------------------------------------------------------
typedef int32_t sdu_size_t;
typedef uint32_t frame_t;
typedef int32_t sframe_t;
typedef uint32_t sub_frame_t;
typedef uint32_t slot_t;
typedef uint16_t module_id_t;
typedef uint8_t slice_id_t;
typedef uint8_t eNB_index_t;
typedef uint64_t ue_id_t;
typedef int16_t smodule_id_t;
typedef long rb_id_t;
typedef long srb_id_t;

typedef bool MBMS_flag_t;
#define MBMS_FLAG_NO false
#define MBMS_FLAG_YES true

typedef bool eNB_flag_t;
#define ENB_FLAG_NO false
#define ENB_FLAG_YES true

typedef bool gNB_flag_t;
#define GNB_FLAG_NO false
#define GNB_FLAG_YES true

typedef bool srb_flag_t;
#define SRB_FLAG_NO false
#define SRB_FLAG_YES true

typedef bool sl_discovery_flag_t;
#define SL_DISCOVERY_FLAG_NO false
#define SL_DISCOVERY_FLAG_YES true

typedef enum {
  TDD = 1,
  FDD = 0
} frame_type_t;

typedef enum link_direction_e { UNKNOWN_DIR = 0, DIR_UPLINK = 1, DIR_DOWNLINK = 2 } link_direction_t;

typedef enum rb_type_e { UNKNOWN_RADIO_BEARER = 0, SIGNALLING_RADIO_BEARER = 1, RADIO_ACCESS_BEARER = 2 } rb_type_t;

typedef enum { CR_ROUND = 0, CR_SRB12 = 1, CR_HOL = 2, CR_LC = 3, CR_CQI = 4, CR_LCP = 5, CR_NUM = 6 } sorting_criterion_t;

typedef enum { POL_FAIR = 0, POL_GREEDY = 1, POL_NUM = 2 } accounting_policy_t;
//-----------------------------------------------------------------------------
// PHY TYPES
//-----------------------------------------------------------------------------
typedef uint8_t crc8_t;
typedef uint16_t crc16_t;
typedef uint32_t crc32_t;
typedef unsigned int crc_t;

//-----------------------------------------------------------------------------
// MAC TYPES
//-----------------------------------------------------------------------------
typedef sdu_size_t tbs_size_t;
typedef sdu_size_t tb_size_t;
typedef unsigned int logical_chan_id_t;
typedef unsigned int num_tb_t;
typedef uint8_t mac_enb_index_t;

//-----------------------------------------------------------------------------
// RLC TYPES
//-----------------------------------------------------------------------------
typedef unsigned int mui_t;
typedef unsigned int confirm_t;
typedef unsigned int rlc_tx_status_t;
typedef int16_t rlc_sn_t;
typedef uint16_t rlc_usn_t;
typedef int32_t rlc_buffer_occupancy_t;
typedef signed int rlc_op_status_t;

typedef enum rlc_mode_e { RLC_MODE_NONE = 0, RLC_MODE_AM = 1, RLC_MODE_UM = 2, RLC_MODE_TM = 4 } rlc_mode_t;

/*! \struct  mac_rlc_status_resp_t
 * \brief Primitive exchanged between RLC and MAC informing about the buffer occupancy of the RLC protocol instance.
 */
typedef struct {
  rlc_buffer_occupancy_t bytes_in_buffer; /*!< \brief Bytes buffered in RLC protocol instance. */
  rlc_buffer_occupancy_t pdus_in_buffer; /*!< \brief Number of PDUs buffered in RLC protocol instance (OBSOLETE). */
  frame_t head_sdu_creation_time; /*!< \brief Head SDU creation time. */
  sdu_size_t head_sdu_remaining_size_to_send; /*!< \brief remaining size of sdu: could be the total size or the remaining size of
                                                 already segmented sdu */
  bool head_sdu_is_segmented; /*!< \brief 0 if head SDU has not been segmented, 1 if already segmented */
} mac_rlc_status_resp_t;

#define SDU_CONFIRM_NO false
#define SDU_CONFIRM_YES true
//-----------------------------------------------------------------------------
// PDCP TYPES
//-----------------------------------------------------------------------------
typedef uint16_t pdcp_sn_t;
typedef uint32_t pdcp_hfn_t;
typedef int16_t pdcp_hfn_offset_t;

typedef enum pdcp_transmission_mode_e {
  PDCP_TRANSMISSION_MODE_UNKNOWN = 0,
  PDCP_TRANSMISSION_MODE_CONTROL = 1,
  PDCP_TRANSMISSION_MODE_DATA = 2,
  PDCP_TRANSMISSION_MODE_TRANSPARENT = 3
} pdcp_transmission_mode_t;
//-----------------------------------------------------------------------------
// IP DRIVER / PDCP TYPES
//-----------------------------------------------------------------------------
typedef uint16_t tcp_udp_port_t;
typedef enum ip_traffic_type_e {
  TRAFFIC_IPVX_TYPE_UNKNOWN = 0,
  TRAFFIC_IPV6_TYPE_UNICAST = 1,
  TRAFFIC_IPV6_TYPE_MULTICAST = 2,
  TRAFFIC_IPV6_TYPE_UNKNOWN = 3,
  TRAFFIC_IPV4_TYPE_UNICAST = 5,
  TRAFFIC_IPV4_TYPE_MULTICAST = 6,
  TRAFFIC_IPV4_TYPE_BROADCAST = 7,
  TRAFFIC_IPV4_TYPE_UNKNOWN = 8,
  TRAFFIC_PC5S_SIGNALLING = 9,
  TRAFFIC_PC5S_SESSION_INIT = 10
} ip_traffic_type_t;

typedef enum {
  PDCCH_AGG_LEVEL1 = 0,
  PDCCH_AGG_LEVEL2,
  PDCCH_AGG_LEVEL4,
  PDCCH_AGG_LEVEL8,
  PDCCH_AGG_LEVEL16,
  NUM_PDCCH_AGG_LEVELS
} Pdcch_Aggregation_Level_t;

typedef struct net_ip_address_s {
  unsigned ipv4: 1;
  unsigned ipv6: 1;
  char ipv4_address[16];
  char ipv6_address[46];
} net_ip_address_t;

//-----------------------------------------------------------------------------
// RRC TYPES
//-----------------------------------------------------------------------------
typedef uint32_t mbms_session_id_t;
typedef uint16_t mbms_service_id_t;
typedef uint16_t rnti_t;
typedef uint8_t rrc_enb_index_t;
typedef uint8_t mme_code_t;
typedef uint32_t m_tmsi_t;

// Random UE identity length = 40 bits
#define NOT_A_RANDOM_UE_IDENTITY (uint64_t)0xFFFFFFFF
#define NOT_A_RNTI (rnti_t)0
#define M_RNTI (rnti_t)0xFFFD
#define P_RNTI (rnti_t)0xFFFE
#define SI_RNTI (rnti_t)0xFFFF
#define CBA_RNTI (rnti_t)0xfff4
#define OAI_C_RNTI (rnti_t)0x1234
typedef enum config_action_e {
  CONFIG_ACTION_NULL = 0,
  CONFIG_ACTION_ADD = 1,
  CONFIG_ACTION_REMOVE = 2,
  CONFIG_ACTION_MODIFY = 3,
  CONFIG_ACTION_SET_SECURITY_MODE = 4,
  CONFIG_ACTION_MBMS_ADD = 10,
  CONFIG_ACTION_MBMS_MODIFY = 11
} config_action_t;

/* Maximum size of any message we might send or receive (e.g., via a socket) */
#define MAX_MESSAGE_SIZE 8192

typedef struct nsa_msg_t {
  uint8_t msg_type;
  uint8_t msg_buffer[MAX_MESSAGE_SIZE];
} nsa_msg_t;

typedef enum nr_lcid_rb_type { NR_LCID_NONE = 0, NR_LCID_SRB = 1, NR_LCID_DRB = 2 } nr_lcid_rb_type;

typedef struct nr_lcid_rb_t {
  nr_lcid_rb_type type;
  union {
    int srb_id;
    int drb_id;
  } choice;
} nr_lcid_rb_t;

typedef struct transport_layer_addr_s {
  /**
   * Transport Layer Address as a bitstring:
   * - 32 bits for IPv4 (RFC 791),
   * - 128 bits for IPv6 (RFC 2460),
   * - 160 bits for both IPv4 and IPv6, with IPv4 in the first 32 bits.
   * The S1AP/NGAP layer forwards this address (bitstring<1..160>)
   * to S1-U/NG-U without interpreting it.
   */
  uint8_t length;
  /// Buffer: address in network byte order
  uint8_t buffer[20];
} transport_layer_addr_t;

//-----------------------------------------------------------------------------
// GTPV1U TYPES
//-----------------------------------------------------------------------------
typedef uint32_t teid_t; // tunnel endpoint identifier
typedef uint8_t ebi_t; // eps bearer id
typedef uint8_t pdusessionid_t;

//-----------------------------------------------------------------------------
//
//-----------------------------------------------------------------------------
// may be ITTI not enabled, but type instance is useful also for OTG,
typedef intptr_t instance_t;

typedef struct protocol_ctxt_s {
  module_id_t module_id; /*!< \brief  Virtualized module identifier      */
  eNB_flag_t enb_flag; /*!< \brief  Flag to indicate eNB (1) or UE (0) */
  instance_t instance; /*!< \brief  ITTI or OTG module identifier      */
  ue_id_t rntiMaybeUEid;
  frame_t frame; /*!< \brief  LTE frame number.*/
  sub_frame_t subframe; /*!< \brief  LTE sub frame number.*/
  eNB_index_t eNB_index; /*!< \brief  valid for UE indicating the index of connected eNB(s)      */
  bool brOption;
} protocol_ctxt_t;
// warning time hardcoded
#define PROTOCOL_CTXT_TIME_MILLI_SECONDS(CtXt_h) ((CtXt_h)->frame * 10 + (CtXt_h)->subframe)

#define UE_MODULE_ID_TO_INSTANCE(mODULE_iD) mODULE_iD + RC.nb_inst
#define ENB_MODULE_ID_TO_INSTANCE(mODULE_iD) mODULE_iD
#define UE_INSTANCE_TO_MODULE_ID(iNSTANCE) iNSTANCE - RC.nb_inst
#define ENB_INSTANCE_TO_MODULE_ID(iNSTANCE) iNSTANCE

// NR
#define GNB_MODULE_ID_TO_INSTANCE(mODULE_iD) mODULE_iD

#define MODULE_ID_TO_INSTANCE(mODULE_iD, iNSTANCE, eNB_fLAG)                                                          \
  do {                                                                                                                \
    iNSTANCE = eNB_fLAG == ENB_FLAG_YES ? ENB_MODULE_ID_TO_INSTANCE(mODULE_iD) : UE_MODULE_ID_TO_INSTANCE(mODULE_iD); \
  } while (0)

#define INSTANCE_TO_MODULE_ID(iNSTANCE, mODULE_iD, eNB_fLAG)                                                         \
  do {                                                                                                               \
    mODULE_iD = eNB_fLAG == ENB_FLAG_YES ? ENB_INSTANCE_TO_MODULE_ID(iNSTANCE) : UE_INSTANCE_TO_MODULE_ID(iNSTANCE); \
  } while (0)

#define PROTOCOL_CTXT_COMPUTE_MODULE_ID(CtXt_h) INSTANCE_TO_MODULE_ID((CtXt_h)->instance, (CtXt_h)->module_id, (CtXt_h)->enb_flag)

#define PROTOCOL_CTXT_COMPUTE_INSTANCE(CtXt_h) MODULE_ID_TO_INSTANCE((CtXt_h)->module_id, (CtXt_h)->instance, (CtXt_h)->enb_flag)

#define PROTOCOL_CTXT_SET_BY_MODULE_ID(Ctxt_Pp, mODULE_iD, eNB_fLAG, rNTI, fRAME, sUBfRAME, eNB_iNDEX) \
  do {                                                                                                 \
    (Ctxt_Pp)->module_id = mODULE_iD;                                                                  \
    (Ctxt_Pp)->enb_flag = eNB_fLAG;                                                                    \
    (Ctxt_Pp)->rntiMaybeUEid = rNTI;                                                                   \
    (Ctxt_Pp)->frame = fRAME;                                                                          \
    (Ctxt_Pp)->subframe = sUBfRAME;                                                                    \
    (Ctxt_Pp)->eNB_index = eNB_iNDEX;                                                                  \
    (Ctxt_Pp)->brOption = false; /* set a default value */                                             \
    PROTOCOL_CTXT_COMPUTE_INSTANCE(Ctxt_Pp);                                                           \
  } while (0)

#define PROTOCOL_CTXT_SET_BY_INSTANCE(Ctxt_Pp, iNSTANCE, eNB_fLAG, rNTI, fRAME, sUBfRAME) \
  do {                                                                                    \
    (Ctxt_Pp)->instance = iNSTANCE;                                                       \
    (Ctxt_Pp)->enb_flag = eNB_fLAG;                                                       \
    (Ctxt_Pp)->rntiMaybeUEid = rNTI;                                                      \
    (Ctxt_Pp)->frame = fRAME;                                                             \
    (Ctxt_Pp)->subframe = sUBfRAME;                                                       \
    (Ctxt_Pp)->eNB_index = 0; /* set a default value */                                   \
    (Ctxt_Pp)->brOption = false; /* set a default value */                                \
    PROTOCOL_CTXT_COMPUTE_MODULE_ID(Ctxt_Pp);                                             \
  } while (0)

#define PROTOCOL_CTXT_FMT "[FRAME %05u][%s][MOD %02d][RNTI %" PRIx64 "]"
#define PROTOCOL_CTXT_ARGS(CTXT_Pp) \
  (CTXT_Pp)->frame, ((CTXT_Pp)->enb_flag == ENB_FLAG_YES) ? "eNB" : " UE", (CTXT_Pp)->module_id, (CTXT_Pp)->rntiMaybeUEid

#define PROTOCOL_NR_CTXT_ARGS(CTXT_Pp) \
  (CTXT_Pp)->frame, ((CTXT_Pp)->enb_flag == GNB_FLAG_YES) ? "gNB" : " UE", (CTXT_Pp)->module_id, (CTXT_Pp)->rntiMaybeUEid

#define CHECK_CTXT_ARGS(CTXT_Pp)


static inline int ceil_mod(const unsigned int v, const unsigned int mod)
{
  return ((v + mod - 1) / mod) * mod;
}

#define exit_fun(msg) exit_function(__FILE__, __FUNCTION__, __LINE__, "exit_fun", OAI_EXIT_NORMAL)
#ifdef __cplusplus
extern "C" {
#endif
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert);
#ifdef __cplusplus
}
#endif
#endif
