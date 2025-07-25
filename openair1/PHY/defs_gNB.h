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

/*! \file PHY/defs_gNB.h
 \brief Top-level defines and structure definitions for gNB
 \author Guy De Souza
 \date 2018
 \version 0.1
 \company Eurecom
 \email: desouza@eurecom.fr
 \note
 \warning
*/

#ifndef __PHY_DEFS_GNB__H__
#define __PHY_DEFS_GNB__H__

#include "common/platform_constants.h"
#include "defs_nr_common.h"
#include "CODING/nrPolar_tools/nr_polar_pbch_defs.h"
#include "openair2/NR_PHY_INTERFACE/NR_IF_Module.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/impl_defs_top.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "PHY/CODING/nrLDPC_extern.h"
#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "nfapi_nr_interface_scf.h"

#define MAX_NUM_RU_PER_gNB 8
#define MAX_PUCCH0_NID 8

typedef struct {
  int nb_id;
  int Nid[MAX_PUCCH0_NID];
  int lut[MAX_PUCCH0_NID][160][14];
} NR_gNB_PUCCH0_LUT_t;

typedef struct {
  /// Nfapi DLSCH PDU
  nfapi_nr_dl_tti_pdsch_pdu pdsch_pdu;
  /// pointer to pdu from MAC interface (this is "a" in 36.212)
  uint8_t *pdu;
  /// Pointer to the payload
  uint8_t *b;
  /// Pointers to transport block segments
  uint8_t **c;
  /// Frame where current HARQ round was sent
  uint32_t frame;
  /// Interleaver outputs
  uint8_t *f;
  /// LDPC lifting size
  uint32_t Z;
  /// REs unavailable for DLSCH (overlapping with PTRS, CSIRS etc.)
  uint32_t unav_res;
} NR_DL_gNB_HARQ_t;

typedef struct {
  uint8_t active;
  nfapi_nr_dl_tti_csi_rs_pdu csirs_pdu;
} NR_gNB_CSIRS_t;

typedef struct {
  int dump_frame;
  int round_trials[8];
  int total_bytes_tx;
  int total_bytes_rx;
  int current_Qm;
  int current_RI;
  int power[MAX_ANT];
  int noise_power[MAX_ANT];
  int DTX;
  int sync_pos;
} NR_gNB_SCH_STATS_t;

typedef struct {
  int pucch0_sr_trials;
  int pucch0_sr_thres;
  int current_pucch0_sr_stat0;
  int current_pucch0_sr_stat1;
  int pucch0_positive_SR;
  int pucch01_trials;
  int pucch0_n00;
  int pucch0_n01;
  int pucch0_thres;
  int current_pucch0_stat0;
  int current_pucch0_stat1;
  int pucch01_DTX;
  int pucch02_trials;
  int pucch02_DTX;
  int pucch2_trials;
  int pucch2_DTX;
} NR_gNB_UCI_STATS_t;

typedef struct {
  int frame;
  uint16_t rnti;
  bool active;
  /// statistics for DLSCH measurement collection
  NR_gNB_SCH_STATS_t dlsch_stats;
  /// statistics for ULSCH measurement collection
  NR_gNB_SCH_STATS_t ulsch_stats;
  NR_gNB_UCI_STATS_t uci_stats;
} NR_gNB_PHY_STATS_t;

typedef struct {
  /// Pointers to variables related to DLSCH harq process
  NR_DL_gNB_HARQ_t harq_process;
  /// Active flag for baseband transmitter processing
  uint8_t active;
  /// Number of soft channel bits
  uint32_t G;
} NR_gNB_DLSCH_t;

typedef struct {
  bool active;
  nfapi_nr_dl_tti_ssb_pdu ssb_pdu;
} NR_gNB_SSB_t;

typedef struct {
  int frame;
  int slot;
  // identifier for concurrent beams
  // prach duration in slots
  int num_slots;
  int *beam_nb;
  nfapi_nr_prach_pdu_t pdu;  
} gNB_PRACH_list_t;

#define NUMBER_OF_NR_PRACH_MAX 8

typedef struct {
  /// \brief ?.
  /// second index: rx antenna [0..63] (hard coded) \note Hard coded array size indexed by \c nb_antennas_rx.
  /// third index: frequency-domain sample [0..ofdm_symbol_size*12[
  int16_t **rxsigF;
  /// \brief local buffer to compute prach_ifft
  int32_t *prach_ifft;
  gNB_PRACH_list_t list[NUMBER_OF_NR_PRACH_MAX];
} NR_gNB_PRACH;

typedef struct {
  uint8_t NumPRSResources;
  prs_config_t prs_cfg[NR_MAX_PRS_RESOURCES_PER_SET];
} NR_gNB_PRS;

typedef struct {
  /// Nfapi ULSCH PDU
  nfapi_nr_pusch_pdu_t ulsch_pdu;
  /// Index of current HARQ round for this DLSCH
  uint8_t round;
  bool new_rx;
  /////////////////////// ulsch decoding ///////////////////////
  /// flag used to clear d properly (together with d_to_be_cleared below)
  /// set to true in nr_fill_ulsch() when new_data_indicator is received
  bool harq_to_be_cleared;
  /// Transport block size (This is A from 38.212 V15.4.0 section 5.1)
  uint32_t TBS;
  /// Pointer to the payload (38.212 V15.4.0 section 5.1)
  uint8_t *b;
  /// Pointers to code blocks after code block segmentation and CRC attachment (38.212 V15.4.0 section 5.2.2)
  uint8_t **c;
  /// Number of bits in each code block (38.212 V15.4.0 section 5.2.2)
  uint32_t K;
  /// Number of "Filler" bits added in the code block segmentation (38.212 V15.4.0 section 5.2.2)
  uint32_t F;
  /// Number of code blocks after code block segmentation (38.212 V15.4.0 section 5.2.2)
  uint32_t C;
  /// Pointers to code blocks after LDPC coding (38.212 V15.4.0 section 5.3.2)
  int16_t **d;
  /// flag used to clear d properly (together with harq_to_be_cleared above)
  /// set to true in nr_ulsch_decoding() when harq_to_be_cleared is true
  /// when true, clear d in the next call to function nr_rate_matching_ldpc_rx()
  bool *d_to_be_cleared;
  /// LDPC lifting size (38.212 V15.4.0 table 5.3.2-1)
  uint32_t Z;
  /// Number of bits in each code block after rate matching for LDPC code (38.212 V15.4.0 section 5.4.2.1)
  uint32_t E;
  /// Number of segments processed so far
  uint32_t processedSegments;
  decode_abort_t abort_decode;
  /// Last index of LLR buffer that contains information.
  /// Used for computing LDPC decoder R
  int llrLen;
  //////////////////////////////////////////////////////////////
} NR_UL_gNB_HARQ_t;

static inline int lenWithCrc(int nbSeg, int len)
{
  if (nbSeg > 1)
    return (len + 24 + 24 * nbSeg) / nbSeg;
  return len + (len > NR_MAX_PDSCH_TBS ? 24 : 16);
}
static inline int crcType(int nbSeg, int len)
{
  if (nbSeg > 1)
    return CRC24_B;
  return len > NR_MAX_PDSCH_TBS ? CRC24_A : CRC16;
}

typedef struct {
  //! estimated rssi (dBm)
  int rx_rssi_dBm;
  /// Wideband CQI (sum of all RX antennas, in dB)
  char wideband_cqi_tot;
} ulsch_measurements_gNB;

typedef struct {
  uint32_t frame;
  uint32_t slot;
  // identifier for concurrent beams
  int beam_nb;
  uint32_t unav_res;
  /// Pointers to 16 HARQ processes for the ULSCH
  NR_UL_gNB_HARQ_t *harq_process;
  /// HARQ process mask, indicates which processes are currently active
  int harq_pid;
  /// Allocated RNTI for this ULSCH
  uint16_t rnti;
  /// Maximum number of LDPC iterations
  uint8_t max_ldpc_iterations;
  /// number of iterations used in last LDPC decoding
  int8_t last_iteration_cnt;
  /// Status Flag indicating for this ULSCH
  bool active;
  /// Flag to indicate that the UL configuration has been handled. Used to remove a stale ULSCH when frame wraps around
  uint8_t handled;
  delay_t delay;
  ulsch_measurements_gNB ulsch_measurements;
} NR_gNB_ULSCH_t;

typedef struct {
  bool active;
  // identifier for concurrent beams
  int beam_nb;
  /// Frame where current PUCCH pdu was sent
  uint32_t frame;
  /// Slot where current PUCCH pdu was sent
  uint32_t slot;
  /// ULSCH PDU
  nfapi_nr_pucch_pdu_t pucch_pdu;
} NR_gNB_PUCCH_t;

typedef struct {
  bool active;
  // identifier for concurrent beams
  int beam_nb;
  /// Frame where current SRS pdu was received
  uint32_t frame;
  /// Slot where current SRS pdu was received
  uint32_t slot;
  /// Measured SNR
  int8_t snr;
  /// ULSCH PDU
  nfapi_nr_srs_pdu_t srs_pdu;
} NR_gNB_SRS_t;

typedef struct {
  /// \brief Pointers (dynamic) to the received data in the frequency domain.
  /// - first index: rx antenna [0..nb_antennas_rx[
  /// - second index: ? [0..2*ofdm_symbol_size*frame_parms->symbols_per_tti[
  c16_t ***rxdataF;
  /// \brief holds the transmit data in the frequency domain.
  /// For IFFT_FPGA this points to the same memory as PHY_vars->rx_vars[a].RX_DMA_BUFFER. //?
  /// - first index: beam (for concurrent beams)
  /// - second index: tx antenna [0..14[ where 14 is the total supported antenna ports.
  /// - third index: sample [0..samples_per_frame_woCP]
  c16_t ***txdataF;
  /// \brief Anaglogue beam ID for each OFDM symbol (used when beamforming not done in RU)
  /// - first index: beam index (for concurrent beams)
  /// - second index: beam_id [0.. symbols_per_frame[
  int **beam_id;
  int num_beams_period;
  bool analog_bf;
  int32_t *debugBuff;
  int32_t debugBuff_sample_offset;
} NR_gNB_COMMON;

typedef struct {
  /// \brief Hold the channel estimates in frequency domain based on DRS.
  /// - first index: rx antenna id [0..nb_antennas_rx[
  /// - second index: ? [0..12*N_RB_UL*frame_parms->symbols_per_tti[
  int32_t **ul_ch_estimates;
  /// \brief Holds the compensated signal.
  /// - first index: rx antenna id [0..nb_antennas_rx[
  /// - second index: ? [0..12*N_RB_UL*frame_parms->symbols_per_tti[
  int32_t **rxdataF_comp;
  /// \f$\log_2(\max|H_i|^2)\f$
  int16_t log2_maxh;
  /// measured RX power based on DRS
  uint32_t ulsch_power[8];
  /// total signal over antennas
  uint32_t ulsch_power_tot;
  /// measured RX noise power
  uint32_t ulsch_noise_power[8];
  /// total noise over antennas
  uint32_t ulsch_noise_power_tot;
  /// \brief llr values.
  /// - first index: ? [0..1179743] (hard coded)
  int16_t *llr;
  // PTRS symbol index, to be updated every PTRS symbol within a slot.
  uint8_t ptrs_symbol_index;
  /// bit mask of PT-RS ofdm symbol indicies
  uint16_t ptrs_symbols;
  // PTRS subcarriers per OFDM symbol
  int32_t ptrs_re_per_slot;
  /// \brief Estimated phase error based upon PTRS on each symbol .
  /// - first index: ? [0..7] Number of Antenna
  /// - second index: ? [0...14] smybol per slot
  int32_t **ptrs_phase_per_slot;
  /// \brief Total RE count after DMRS/PTRS RE's are extracted from respective symbol.
  /// - first index: ? [0...14] smybol per slot
  int16_t *ul_valid_re_per_slot;
  /// \brief offset for llr corresponding to each symbol
  int llr_offset[14];
  /// flag to indicate DTX on reception
  int DTX;
} NR_gNB_PUSCH;

/// Context data structure for RX/TX portion of slot processing
typedef struct {
  /// Component Carrier index
  uint8_t CC_id;
  /// timestamp transmitted to HW
  openair0_timestamp timestamp_tx;
  /// slot to act upon for transmission
  int slot_tx;
  /// slot to act upon for reception
  int slot_rx;
  /// frame to act upon for transmission
  int frame_tx;
  /// frame to act upon for reception
  int frame_rx;
  /// \brief Instance count for RXn-TXnp4 processing thread.
  /// \internal This variable is protected by \ref mutex_rxtx.
  int instance_cnt;
  /// condition variable for tx processing thread
  pthread_cond_t cond;
  /// mutex for RXn-TXnp4 processing thread
  pthread_mutex_t mutex;
} gNB_L1_rxtx_proc_t;


/// Context data structure for eNB slot processing
typedef struct gNB_L1_proc_t_s {
  /// Component Carrier index
  uint8_t CC_id;
  /// timestamp received from HW
  openair0_timestamp timestamp_rx;
  /// timestamp to send to "slave rru"
  openair0_timestamp timestamp_tx;
  /// slot to act upon for reception
  int slot_rx;
  /// frame to act upon for reception
  int frame_rx;
  /// frame to act upon for transmission
  int frame_tx;
  /// pthread structure for dumping L1 stats
  pthread_t L1_stats_thread;
  /// set of scheduling variables RXn-TXnp4 threads
  gNB_L1_rxtx_proc_t L1_proc;
  gNB_L1_rxtx_proc_t L1_proc_tx;
} gNB_L1_proc_t;

typedef struct {
  // common measurements
  //! estimated noise power (linear)
  unsigned int   n0_power[MAX_NUM_RU_PER_gNB];
  //! estimated noise power (dB)
  unsigned int n0_power_dB[MAX_NUM_RU_PER_gNB];
  //! total estimated noise power (linear)
  unsigned int   n0_power_tot;
  //! estimated avg noise power (dB)
  unsigned int n0_power_tot_dB;
  //! estimated avg noise power per RB per RX ant (lin)
  fourDimArray_t *n0_subband_power;
  //! estimated avg noise power per RB per RX ant (dB)
  fourDimArray_t *n0_subband_power_dB;
  //! estimated avg subband noise power (dB)
  unsigned int n0_subband_power_avg_dB;
  //! estimated avg subband noise power per antenna (dB)
  unsigned int n0_subband_power_avg_perANT_dB[MAX_ANT];
  //! estimated avg noise power per RB (dB)
  int n0_subband_power_tot_dB[275];
  //! estimated avg noise power per RB (dBm)
  int n0_subband_power_tot_dBm[275];
  /// PRACH background noise level
  int prach_I0;
} PHY_MEASUREMENTS_gNB;

// the current RRC resource allocation is that each UE gets its
// "own" PUCCH resource (for F0) in a dedicated PRB in each slot
// therefore, we can have up to "number of UE" UCI PDUs
#define MAX_NUM_NR_UCI_PDUS MAX_MOBILES_PER_GNB

/// Top-level PHY Data Structure for gNB
typedef struct PHY_VARS_gNB_s {
  /// Module ID indicator for this instance
  module_id_t Mod_id;
  uint8_t CC_id;
  uint8_t configured;
  gNB_L1_proc_t proc;
  int num_RU;
  RU_t *RU_list[MAX_NUM_RU_PER_gNB];
  /// Ethernet parameters for northbound midhaul interface
  eth_params_t eth_params_n;
  /// Ethernet parameters for fronthaul interface
  eth_params_t eth_params;
  int rx_total_gain_dB;
  int (*nr_start_if)(struct RU_t_s *ru, struct PHY_VARS_gNB_s *gNB);
  nfapi_nr_config_request_scf_t gNB_config;
  NR_DL_FRAME_PARMS frame_parms;
  PHY_MEASUREMENTS_gNB measurements;
  NR_IF_Module_t *if_inst;

  nfapi_nr_ul_tti_request_t UL_tti_req;

  int max_nb_pucch;
  int max_nb_srs;
  int max_nb_pdsch;
  int max_nb_pusch;

  NR_gNB_COMMON common_vars;
  NR_gNB_PRACH prach_vars;
  NR_gNB_PRS prs_vars;
  NR_gNB_PUSCH *pusch_vars;
  NR_gNB_PUCCH_t *pucch;
  NR_gNB_SRS_t *srs;
  NR_gNB_ULSCH_t *ulsch;
  NR_gNB_PHY_STATS_t phy_stats[MAX_MOBILES_PER_GNB];
  t_nrPolar_params **polarParams;

  /// SRS variables
  nr_srs_info_t **nr_srs_info;

  // reference amplitude for TX
  int16_t TX_AMP;

  // flag to activate 3GPP phase symbolwise rotation
  bool phase_comp;

  // PUCCH0 Look-up table for cyclic-shifts
  NR_gNB_PUCCH0_LUT_t pucch0_lut;

  /// PBCH interleaver
  uint8_t nr_pbch_interleaver[NR_POLAR_PBCH_PAYLOAD_BITS];

  /// PRACH root sequence
  c16_t X_u[64][839];

  /// OFDM symbol offset divisor for UL
  uint32_t ofdm_offset_divisor;

  /// NR LDPC coding related
  nrLDPC_coding_interface_t nrLDPC_coding_interface;
  int max_ldpc_iterations;

  /// indicate the channel estimation technique in time domain
  int chest_time;
  /// indicate the channel estimation technique in freq domain
  int chest_freq;

  /// counter to average prach energh over first 100 prach opportunities
  int prach_energy_counter;

  int ap_N1;
  int ap_N2;
  int ap_XP;

  int pucch0_thres;
  int pusch_thres;
  int prach_thres;
  int srs_thres;
  uint64_t bad_pucch;
  int num_ulprbbl;
  uint16_t ulprbbl [MAX_BWP_SIZE];

  bool enable_analog_das;

  time_stats_t phy_proc_tx;
  time_stats_t phy_proc_rx;
  time_stats_t rx_prach;

  time_stats_t dlsch_encoding_stats;
  time_stats_t dlsch_modulation_stats;
  time_stats_t dlsch_scrambling_stats;
  time_stats_t dlsch_resource_mapping_stats;
  time_stats_t dlsch_precoding_stats;
  time_stats_t tinput;
  time_stats_t tprep;
  time_stats_t tparity;
  time_stats_t toutput;
  
  time_stats_t dlsch_rate_matching_stats;
  time_stats_t dlsch_interleaving_stats;
  time_stats_t dlsch_segmentation_stats;

  time_stats_t dci_generation_stats;
  time_stats_t phase_comp_stats;
  time_stats_t rx_pusch_stats;
  time_stats_t rx_pusch_init_stats;
  time_stats_t rx_pusch_symbol_processing_stats;
  time_stats_t ul_indication_stats;
  time_stats_t slot_indication_stats;
  time_stats_t schedule_response_stats;
  time_stats_t ulsch_decoding_stats;
  time_stats_t ulsch_deinterleaving_stats;
  time_stats_t ulsch_channel_estimation_stats;
  time_stats_t pusch_channel_estimation_antenna_processing_stats;
  time_stats_t ulsch_llr_stats;
  time_stats_t rx_srs_stats;
  time_stats_t generate_srs_stats;
  time_stats_t get_srs_signal_stats;
  time_stats_t srs_channel_estimation_stats;
  time_stats_t srs_timing_advance_stats;
  time_stats_t srs_report_tlv_stats;
  time_stats_t srs_beam_report_stats;
  time_stats_t srs_iq_matrix_stats;

  notifiedFIFO_t respPuschSymb;
  notifiedFIFO_t respDecode;
  notifiedFIFO_t resp_L1;
  notifiedFIFO_t L1_tx_free;
  notifiedFIFO_t L1_tx_filled;
  notifiedFIFO_t L1_tx_out;
  notifiedFIFO_t L1_rx_out;
  notifiedFIFO_t resp_RU_tx;
  tpool_t threadPool;
  int nbSymb;
  int nbAarx;
  int num_pusch_symbols_per_thread;
  int dmrs_num_antennas_per_thread;
  pthread_t L1_rx_thread;
  int L1_rx_thread_core;
  pthread_t L1_tx_thread;
  int L1_tx_thread_core;
  struct processingData_L1tx *msgDataTx;
  void *scopeData;
} PHY_VARS_gNB;

struct puschSymbolReqId {
  uint16_t ulsch_id;
  uint16_t frame;
  uint8_t  slot;
  uint16_t spare;
} __attribute__((packed));

union puschSymbolReqUnion {
  struct puschSymbolReqId s;
  uint64_t p;
};

struct puschAntennaReqId {
  uint16_t ul_id;
  uint16_t spare;
} __attribute__((packed));

union puschAntennaReqUnion {
  struct puschAntennaReqId s;
  uint64_t p;
};

typedef struct LDPCDecode_s {
  PHY_VARS_gNB *gNB;
  NR_UL_gNB_HARQ_t *ulsch_harq;
  t_nrLDPC_dec_params decoderParms;
  NR_gNB_ULSCH_t *ulsch;
  short* ulsch_llr; 
  int ulsch_id;
  int harq_pid;
  int rv_index;
  int A;
  int E;
  int Kc;
  int Qm;
  int Kr_bytes;
  int nbSegments;
  int segment_r;
  int r_offset;
  int offset;
  int decodeIterations;
  uint32_t tbslbrm;
  task_ans_t *ans;
} ldpcDecode_t;

struct ldpcReqId {
  uint16_t rnti;
  uint16_t frame;
  uint8_t  slot;
  uint8_t  codeblock;
  uint16_t spare;
} __attribute__((packed));

union ldpcReqUnion {
  struct ldpcReqId s;
  uint64_t p;
};

typedef struct processingData_L1 {
  int frame_rx;
  int slot_rx;
  openair0_timestamp timestamp_tx;
  PHY_VARS_gNB *gNB;
  notifiedFIFO_elt_t *elt;
} processingData_L1_t;

typedef struct processingData_L1tx {
  int frame;
  int slot;
  int frame_rx;
  int slot_rx;
  openair0_timestamp timestamp_tx;
  PHY_VARS_gNB *gNB;
  nfapi_nr_dl_tti_pdcch_pdu pdcch_pdu[NFAPI_NR_MAX_NB_CORESETS];
  nfapi_nr_ul_dci_request_pdus_t ul_pdcch_pdu[NFAPI_NR_MAX_NB_CORESETS];
  NR_gNB_CSIRS_t csirs_pdu[NR_SYMBOLS_PER_SLOT];
  NR_gNB_DLSCH_t **dlsch;
  NR_gNB_SSB_t ssb[64];
  uint16_t num_pdsch_slot;
  int num_dl_pdcch;
  int num_ul_pdcch;
  /* a reference to the sched_response, to release it when not needed anymore */
  int sched_response_id;
} processingData_L1tx_t;

typedef struct processingData_L1rx {
  int frame_rx;
  int slot_rx;
  PHY_VARS_gNB *gNB;
} processingData_L1rx_t;
#endif
