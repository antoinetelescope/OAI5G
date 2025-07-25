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

/*! \file gNB_scheduler_primitives.c
 * \brief primitives used by gNB for BCH, RACH, ULSCH, DLSCH scheduling
 * \author  Raymond Knopp, Guy De Souza
 * \date 2018, 2019
 * \email: knopp@eurecom.fr, desouza@eurecom.fr
 * \version 1.0
 * \company Eurecom
 * @ingroup _mac
 */

#include <softmodem-common.h>
#include "assertions.h"

#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "NR_MAC_gNB/mac_proto.h"
#include "common/utils/LOG/log.h"
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "common/utils/nr/nr_common.h"
#include "UTIL/OPT/opt.h"

#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "F1AP_CauseRadioNetwork.h"

#include "intertask_interface.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "F1AP_CauseRadioNetwork.h"

#include "T.h"

#include "uper_encoder.h"
#include "uper_decoder.h"

#include "SIMULATION/TOOLS/sim.h" // for taus

#define ENABLE_MAC_PAYLOAD_DEBUG
#define DEBUG_gNB_SCHEDULER 1

#include "common/ran_context.h"
#include "nfapi/oai_integration/vendor_ext.h"

#include "common/utils/alg/find.h"

// 3GPP TS 38.331 Section 12 Table 12.1-1: UE performance requirements for RRC procedures for UEs
#define NR_RRC_SETUP_DELAY_MS           10
#define NR_RRC_RECONFIGURATION_DELAY_MS 10
#define NR_RRC_BWP_SWITCHING_DELAY_MS   6

// #define DEBUG_DCI
//  CQI TABLES (10 times the value in 214 to adequately compare with R)
//  Table 1 (38.214 5.2.2.1-2)
static const uint16_t cqi_table1[16][2] = {{0, 0},
                                           {2, 780},
                                           {2, 1200},
                                           {2, 1930},
                                           {2, 3080},
                                           {2, 4490},
                                           {2, 6020},
                                           {4, 3780},
                                           {4, 4900},
                                           {4, 6160},
                                           {6, 4660},
                                           {6, 5670},
                                           {6, 6660},
                                           {6, 7720},
                                           {6, 8730},
                                           {6, 9480}};

// Table 2 (38.214 5.2.2.1-3)
static const uint16_t cqi_table2[16][2] = {{0, 0},
                                           {2, 780},
                                           {2, 1930},
                                           {2, 4490},
                                           {4, 3780},
                                           {4, 4900},
                                           {4, 6160},
                                           {6, 4660},
                                           {6, 5670},
                                           {6, 6660},
                                           {6, 7720},
                                           {6, 8730},
                                           {8, 7110},
                                           {8, 7970},
                                           {8, 8850},
                                           {8, 9480}};

// Table 2 (38.214 5.2.2.1-4)
static const uint16_t cqi_table3[16][2] = {{0, 0},
                                           {2, 300},
                                           {2, 500},
                                           {2, 780},
                                           {2, 1200},
                                           {2, 1930},
                                           {2, 3080},
                                           {2, 4490},
                                           {2, 6020},
                                           {4, 3780},
                                           {4, 4900},
                                           {4, 6160},
                                           {6, 4660},
                                           {6, 5670},
                                           {6, 6660},
                                           {6, 7720}};

static void determine_aggregation_level_search_order(int agg_level_search_order[NUM_PDCCH_AGG_LEVELS],
                                                     float pdcch_cl_adjust);

uint8_t get_dl_nrOfLayers(const NR_UE_sched_ctrl_t *sched_ctrl, const nr_dci_format_t dci_format)
{
  // TODO check this but it should be enough for now
  // if there is not csi report activated RI is 0 from initialization
  if(dci_format == NR_DL_DCI_FORMAT_1_0)
    return 1;
  else
    return sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.ri + 1;
}

int get_ul_nrOfLayers(const NR_UE_sched_ctrl_t *sched_ctrl, const nr_dci_format_t dci_format)
{
  if(dci_format == NR_UL_DCI_FORMAT_0_0)
    return 1;
  else
    return sched_ctrl->srs_feedback.ul_ri + 1;
}

// Table 5.2.2.2.1-3 and Table 5.2.2.2.1-4 in 38.214
void get_k1_k2_indices(const int layers, const int N1, const int N2, const int i13, int *k1, int *k2)
{
  AssertFatal(layers > 0 && layers < 5, "Number of layers %d not supported\n", layers);
  *k1 = 0;
  *k2 = 0;
  if (layers == 2) {
    if (N2 == 1)
      *k1 = i13;
    else if (N1 == N2) {
      *k1 = i13 & 1;
      *k2 = i13 >> 1;
    }
    else {
      *k1 = (i13 & 1) + (i13 == 3);
      *k2 = (i13 == 2);
    }
  }
  if (layers == 3 || layers == 4) {
    if (N2 == 1)
      *k1 = i13 + 1;
    else if (N1 == 2 && N2 == 2) {
      *k1 = !(i13 & 1);
      *k2 = (i13 > 0);
    }
    else {
      if (i13 == 0)
        *k1 = 1;
      if (i13 == 1)
        *k2 = 1;
      if (i13 == 2) {
        *k1 = 1;
        *k2 = 1;
      }
      if (i13 == 3)
        *k1 = 2;
    }
  }
}

uint16_t get_pm_index(const gNB_MAC_INST *nrmac,
                      const NR_UE_info_t *UE,
                      nr_dci_format_t dci_format,
                      int layers,
                      int xp_pdsch_antenna_ports)
{
  if (dci_format == NR_DL_DCI_FORMAT_1_0 || nrmac->identity_pm || xp_pdsch_antenna_ports == 1)
    return 0; //identity matrix (basic 5G configuration handled by PMI report is with XP antennas)
  const NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  const int report_id = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.csi_report_id;
  const nr_csi_report_t *csi_report = &UE->csi_report_template[report_id];
  const int N1 = csi_report->N1;
  const int N2 = csi_report->N2;
  const int antenna_ports = (N1 * N2) << 1;
  if (antenna_ports < 2)
    return 0; // single antenna port

  int x1 = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.pmi_x1;
  const int x2 = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.pmi_x2;
  LOG_D(NR_MAC,"PMI report: x1 %d x2 %d layers: %d\n", x1, x2, layers);

  int prev_layers_size = 0;
  for (int i = 1; i < layers; i++)
    prev_layers_size += nrmac->precoding_matrix_size[i - 1];

  // need to return PM index to matrix initialized in init_DL_MIMO_codebook
  // index 0 is for identity matrix
  // order of matrices depends on layers to be transmitted
  // elements from 1 to n for 1 layer
  // elements from n+1 to m for 2 layers etc.
  if (antenna_ports == 2)
    return 1 + prev_layers_size + x2;  // 0 for identity matrix
  else {
    int idx = layers - 1;
    // the order of i1x in X1 report needs to be verified
    // the standard is not very clear (Table 6.3.1.1.2-7 in 38.212)
    // it says: PMI wideband information fields X1 , from left to right
    int bitlen = csi_report->csi_meas_bitlen.pmi_i13_bitlen[idx];
    if (layers == 1 && bitlen != 0) {
      LOG_E(NR_MAC, "Invalid i13 bit length %d for single layer! It should be 0\n", bitlen);
      return 0;
    }
    const int i13 = x1 & ((1 << bitlen) - 1);
    x1 >>= bitlen;
    bitlen = csi_report->csi_meas_bitlen.pmi_i12_bitlen[idx];
    const int i12 = x1 & ((1 << bitlen) - 1);
    x1 >>= bitlen;
    bitlen = csi_report->csi_meas_bitlen.pmi_i11_bitlen[idx];
    const int i11 = x1 & ((1 << bitlen) - 1);
    const int i2 = x2;
    int k1, k2;
    get_k1_k2_indices(layers, N1, N2, i13, &k1, &k2); // get indices k1 and k2 for PHY matrix (not actual k1 and k2 values)
    const int O2 = N2 == 1 ? 1 : 4;
    int O1 = 4; //Horizontal beam oversampling = 4 for more than 2 antenna ports
    int max_i2 = 0;
    int lay_index = 0;
    max_i2 = layers == 1 ? 4 : 2;
    int K1, K2;
    get_K1_K2(N1, N2, &K1, &K2, layers);
    // computing precoding matrix index according to rule set in allocation function init_codebook_gNB
    lay_index = i2 + (i11 * max_i2) + (i12 * max_i2 * N1 * O1) + (k1 * max_i2 * N1 * O1 * N2 * O2) + (k2 * max_i2 * N1 * O1 * N2 * O2 * K1);
    return 1 + prev_layers_size + lay_index;
  }
}

uint8_t get_mcs_from_cqi(int mcs_table, int cqi_table, int cqi_idx)
{
  if (cqi_idx <= 0) {
    LOG_E(NR_MAC, "invalid cqi_idx %d, default to MCS 9\n", cqi_idx);
    return 9;
  }

  if (mcs_table != cqi_table) {
    LOG_E(NR_MAC, "indices of CQI (%d) and MCS (%d) tables don't correspond yet\n", cqi_table, mcs_table);
    return 9;
  }

  uint16_t target_coderate, target_qm;
  switch (cqi_table) {
    case 0:
      target_qm = cqi_table1[cqi_idx][0];
      target_coderate = cqi_table1[cqi_idx][1];
      break;
    case 1:
      target_qm = cqi_table2[cqi_idx][0];
      target_coderate = cqi_table2[cqi_idx][1];
      break;
    case 2:
      target_qm = cqi_table3[cqi_idx][0];
      target_coderate = cqi_table3[cqi_idx][1];
      break;
    default:
      AssertFatal(1==0,"Invalid cqi table index %d\n",cqi_table);
  }
  const int max_mcs = mcs_table == 1 ? 27 : 28;
  for (int i = 0; i <= max_mcs; i++) {
    const int R = nr_get_code_rate_dl(i, mcs_table);
    const int Qm = nr_get_Qm_dl(i, mcs_table);
    if (Qm == target_qm && target_coderate <= R)
      return i;
  }

  LOG_E(NR_MAC, "could not find maximum MCS from cqi_idx %d, default to 9\n", cqi_idx);
  return 9;
}

NR_pdsch_dmrs_t get_dl_dmrs_params(const NR_ServingCellConfigCommon_t *scc,
                                   const NR_UE_DL_BWP_t *dl_bwp,
                                   const NR_tda_info_t *tda_info,
                                   const int Layers)
{
  NR_pdsch_dmrs_t dmrs = {0};
  int frontloaded_symb = 1; // default value
  nr_dci_format_t dci_format = dl_bwp ? dl_bwp->dci_format : NR_DL_DCI_FORMAT_1_0;
  if (dci_format == NR_DL_DCI_FORMAT_1_0) {
    dmrs.numDmrsCdmGrpsNoData = tda_info->nrOfSymbols <= 2 ? 1 : 2;
    dmrs.dmrs_ports_id = 0;
  }
  else {
    //TODO first basic implementation of dmrs port selection
    //     only vaild for a single codeword
    //     for now it assumes a selection of Nl consecutive dmrs ports
    //     and a single front loaded symbol
    //     dmrs_ports_id is the index of Tables 7.3.1.2.2-1/2/3/4
    //     number of front loaded symbols need to be consistent with maxLength
    //     when a more complete implementation is done

    switch (Layers) {
      case 1:
#ifdef  ENABLE_AERIAL
        dmrs.dmrs_ports_id = 3;
        dmrs.numDmrsCdmGrpsNoData = 2;
#else
        dmrs.dmrs_ports_id = 0;
        dmrs.numDmrsCdmGrpsNoData = 1;
#endif
        frontloaded_symb = 1;
        break;
      case 2:
#ifdef  ENABLE_AERIAL
        dmrs.dmrs_ports_id = 7;
        dmrs.numDmrsCdmGrpsNoData = 2;
#else
        dmrs.dmrs_ports_id = 2;
        dmrs.numDmrsCdmGrpsNoData = 1;
#endif
        frontloaded_symb = 1;
        break;
      case 3:
        dmrs.dmrs_ports_id = 9;
        dmrs.numDmrsCdmGrpsNoData = 2;
        frontloaded_symb = 1;
        break;
      case 4:
        dmrs.dmrs_ports_id = 10;
        dmrs.numDmrsCdmGrpsNoData = 2;
        frontloaded_symb = 1;
        break;
      default:
        AssertFatal(1==0,"Number of layers %d\n not supported or not valid\n",Layers);
    }
  }

  NR_PDSCH_Config_t *pdsch_Config = dl_bwp ? dl_bwp->pdsch_Config : NULL;
  if (pdsch_Config) {
    if (tda_info->mapping_type == typeB)
      dmrs.dmrsConfigType = pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeB->choice.setup->dmrs_Type != NULL;
    else
      dmrs.dmrsConfigType = pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA->choice.setup->dmrs_Type != NULL;
  }
  else
    dmrs.dmrsConfigType = NFAPI_NR_DMRS_TYPE1;

  dmrs.N_PRB_DMRS = dmrs.numDmrsCdmGrpsNoData * (dmrs.dmrsConfigType == NFAPI_NR_DMRS_TYPE1 ? 6 : 4);
  dmrs.dl_dmrs_symb_pos = fill_dmrs_mask(pdsch_Config, dci_format, scc->dmrs_TypeA_Position, tda_info->nrOfSymbols, tda_info->startSymbolIndex, tda_info->mapping_type, frontloaded_symb);
  dmrs.N_DMRS_SLOT = get_num_dmrs(dmrs.dl_dmrs_symb_pos);
  LOG_D(NR_MAC,"Filling dmrs info, ps->N_PRB_DMRS %d, ps->dl_dmrs_symb_pos %x, ps->N_DMRS_SLOT %d\n",dmrs.N_PRB_DMRS,dmrs.dl_dmrs_symb_pos,dmrs.N_DMRS_SLOT);
  return dmrs;
}

NR_ControlResourceSet_t *get_coreset(gNB_MAC_INST *nrmac,
                                     NR_ServingCellConfigCommon_t *scc,
                                     void *bwp,
                                     NR_SearchSpace_t *ss,
                                     NR_SearchSpace__searchSpaceType_PR ss_type) {

  NR_ControlResourceSetId_t coreset_id = *ss->controlResourceSetId;

  if (ss_type == NR_SearchSpace__searchSpaceType_PR_common) { // common search space
    NR_ControlResourceSet_t *coreset;
    if(coreset_id == 0) {
      coreset =  nrmac->sched_ctrlCommon->coreset; // this is coreset 0
    } else if (bwp) {
      coreset = ((NR_BWP_Downlink_t*)bwp)->bwp_Common->pdcch_ConfigCommon->choice.setup->commonControlResourceSet;
    } else if (scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonControlResourceSet) {
      coreset = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonControlResourceSet;
    } else {
      coreset = NULL;
    }

    if (coreset) AssertFatal(coreset_id == coreset->controlResourceSetId,
			     "ID of common ss coreset does not correspond to id set in the "
			     "search space\n");
    return coreset;
  } else {
    const int n = ((NR_BWP_DownlinkDedicated_t*)bwp)->pdcch_Config->choice.setup->controlResourceSetToAddModList->list.count;
    for (int i = 0; i < n; i++) {
      NR_ControlResourceSet_t *coreset =
          ((NR_BWP_DownlinkDedicated_t*)bwp)->pdcch_Config->choice.setup->controlResourceSetToAddModList->list.array[i];
      if (coreset_id == coreset->controlResourceSetId) {
        return coreset;
      }
    }
    AssertFatal(0, "Couldn't find coreset with id %ld\n", coreset_id);
  }
}

static NR_SearchSpace_t *get_searchspace(NR_ServingCellConfigCommon_t *scc,
                                         NR_BWP_DownlinkDedicated_t *bwp_Dedicated,
                                         NR_SearchSpace__searchSpaceType_PR target_ss)
{
  int n = 0;
  if(bwp_Dedicated)
    n = bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList->list.count;
  else
    n = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList->list.count;

  for (int i=0;i<n;i++) {
    NR_SearchSpace_t *ss = NULL;
    if(bwp_Dedicated)
      ss = bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList->list.array[i];
    else
      ss = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList->list.array[i];
    AssertFatal(ss->controlResourceSetId != NULL, "ss->controlResourceSetId is null\n");
    AssertFatal(ss->searchSpaceType != NULL, "ss->searchSpaceType is null\n");
    if (ss->searchSpaceType->present == target_ss) {
      return ss;
    }
  }
  AssertFatal(0, "Couldn't find an adequate searchspace bwp_Dedicated %p\n",bwp_Dedicated);
}

NR_sched_pdcch_t set_pdcch_structure(gNB_MAC_INST *gNB_mac,
                                     NR_SearchSpace_t *ss,
                                     NR_ControlResourceSet_t *coreset,
                                     NR_ServingCellConfigCommon_t *scc,
                                     NR_BWP_t *bwp,
                                     NR_Type0_PDCCH_CSS_config_t *type0_PDCCH_CSS_config) {

  int sps;
  NR_sched_pdcch_t pdcch;

  AssertFatal(*ss->controlResourceSetId == coreset->controlResourceSetId,
              "coreset id in SS %ld does not correspond to the one in coreset %ld",
              *ss->controlResourceSetId, coreset->controlResourceSetId);

  if (bwp) { // This is not for SIB1
    if(coreset->controlResourceSetId == 0){
      pdcch.BWPSize  = gNB_mac->cset0_bwp_size;
      pdcch.BWPStart = gNB_mac->cset0_bwp_start;
    }
    else {
      pdcch.BWPSize  = NRRIV2BW(bwp->locationAndBandwidth, MAX_BWP_SIZE);
      pdcch.BWPStart = NRRIV2PRBOFFSET(bwp->locationAndBandwidth, MAX_BWP_SIZE);
    }
    pdcch.SubcarrierSpacing = bwp->subcarrierSpacing;
    pdcch.CyclicPrefix = (bwp->cyclicPrefix==NULL) ? 0 : *bwp->cyclicPrefix;

    //AssertFatal(pdcch_scs==kHz15, "PDCCH SCS above 15kHz not allowed if a symbol above 2 is monitored");
    sps = bwp->cyclicPrefix == NULL ? 14 : 12;
  }
  else {
    AssertFatal(type0_PDCCH_CSS_config!=NULL,"type0_PDCCH_CSS_config is null,bwp %p\n",bwp);
    pdcch.BWPSize = type0_PDCCH_CSS_config->num_rbs;
    pdcch.BWPStart = type0_PDCCH_CSS_config->cset_start_rb;
    pdcch.SubcarrierSpacing = type0_PDCCH_CSS_config->scs_pdcch;
    pdcch.CyclicPrefix = 0;
    sps = 14;
  }

  AssertFatal(ss->monitoringSymbolsWithinSlot!=NULL,"ss->monitoringSymbolsWithinSlot is null\n");
  AssertFatal(ss->monitoringSymbolsWithinSlot->buf!=NULL,"ss->monitoringSymbolsWithinSlot->buf is null\n");

  // for SPS=14 8 MSBs in positions 13 downto 6
  uint16_t monitoringSymbolsWithinSlot = (ss->monitoringSymbolsWithinSlot->buf[0]<<(sps-8)) |
                                         (ss->monitoringSymbolsWithinSlot->buf[1]>>(16-sps));

  for (int i=0; i<sps; i++) {
    if ((monitoringSymbolsWithinSlot>>(sps-1-i))&1) {
      pdcch.StartSymbolIndex=i;
      break;
    }
  }

  pdcch.DurationSymbols = coreset->duration;

  //cce-REG-MappingType
  pdcch.CceRegMappingType = coreset->cce_REG_MappingType.present == NR_ControlResourceSet__cce_REG_MappingType_PR_interleaved?
    NFAPI_NR_CCE_REG_MAPPING_INTERLEAVED : NFAPI_NR_CCE_REG_MAPPING_NON_INTERLEAVED;

  if (pdcch.CceRegMappingType == NFAPI_NR_CCE_REG_MAPPING_INTERLEAVED) {
    pdcch.RegBundleSize = (coreset->cce_REG_MappingType.choice.interleaved->reg_BundleSize ==
                                NR_ControlResourceSet__cce_REG_MappingType__interleaved__reg_BundleSize_n6) ? 6 : (2+coreset->cce_REG_MappingType.choice.interleaved->reg_BundleSize);
    pdcch.InterleaverSize = (coreset->cce_REG_MappingType.choice.interleaved->interleaverSize ==
                                  NR_ControlResourceSet__cce_REG_MappingType__interleaved__interleaverSize_n6) ? 6 : (2+coreset->cce_REG_MappingType.choice.interleaved->interleaverSize);
    AssertFatal(scc->physCellId != NULL,"scc->physCellId is null\n");
    pdcch.ShiftIndex = coreset->cce_REG_MappingType.choice.interleaved->shiftIndex != NULL ? *coreset->cce_REG_MappingType.choice.interleaved->shiftIndex : *scc->physCellId;
  }
  else {
    pdcch.RegBundleSize = 6;
    pdcch.InterleaverSize = 0;
    pdcch.ShiftIndex = 0;
  }

  uint16_t N_rb = 0; // nb of rbs of coreset per symbol
  uint16_t rb_start = 0;
  for (int i = 0; i < 6; i++) {
    for (int t = 0; t < 8; t++) {
      if (coreset->frequencyDomainResources.buf[i] >> (7 - t) & 1) {
        if (N_rb == 0) {
          rb_start = 48 * i + t * 6;
        }
        N_rb++;
      }
    }
  }

  pdcch.rb_start = rb_start;
  pdcch.n_rb = N_rb *= 6; // each bit of frequencyDomainResources represents 6 PRBs

  return pdcch;
}

int find_pdcch_candidate(const gNB_MAC_INST *mac,
                         int cc_id,
                         int aggregation,
                         int nr_of_candidates,
                         int beam_idx,
                         const NR_sched_pdcch_t *pdcch,
                         const NR_ControlResourceSet_t *coreset,
                         uint32_t Y)
{
  const uint16_t *vrb_map = mac->common_channels[cc_id].vrb_map[beam_idx];
  const int N_ci = 0;

  const int N_rb = pdcch->n_rb;  // nb of rbs of coreset per symbol
  const int N_symb = coreset->duration; // nb of coreset symbols
  const int N_regs = N_rb * N_symb; // nb of REGs per coreset
  const int N_cces = N_regs / NR_NB_REG_PER_CCE; // nb of cces in coreset
  const int R = pdcch->InterleaverSize;
  const int L = pdcch->RegBundleSize;
  const int C = R > 0 ? N_regs / (L * R) : 0;
  const int B_rb = L / N_symb; // nb of RBs occupied by each REG bundle

  // loop over all the available candidates
  // this implements TS 38.211 Sec. 7.3.2.2
  for(int m = 0; m < nr_of_candidates; m++) { // loop over candidates
    bool taken = false; // flag if the resource for a given candidate are taken
    int first_cce = aggregation * ((Y + ((m * N_cces) / (aggregation * nr_of_candidates)) + N_ci) % (N_cces / aggregation));
    LOG_D(NR_MAC,"Candidate %d of %d first_cce %d (L %d N_cces %d Y %d)\n", m, nr_of_candidates, first_cce, aggregation, N_cces, Y);
    for (int j = first_cce; (j < first_cce + aggregation) && !taken; j++) { // loop over CCEs
      for (int k = 6 * j / L; (k < (6 * j / L + 6 / L)) && !taken; k++) { // loop over REG bundles
        int f = cce_to_reg_interleaving(R, k, pdcch->ShiftIndex, C, L, N_regs);
        for(int rb = 0; rb < B_rb; rb++) { // loop over the RBs of the bundle
          if(vrb_map[pdcch->BWPStart + f * B_rb + rb] & SL_to_bitmap(pdcch->StartSymbolIndex,N_symb)) {
            taken = true;
            break;
          }
        }
      }
    }
    if(!taken)
      return first_cce;
  }
  return -1;
}


int get_cce_index(const gNB_MAC_INST *nrmac,
                  const int CC_id,
                  const int slot,
                  const rnti_t rnti,
                  uint8_t *aggregation_level,
                  int beam_idx,
                  const NR_SearchSpace_t *ss,
                  const NR_ControlResourceSet_t *coreset,
                  NR_sched_pdcch_t *sched_pdcch,
                  bool is_common,
                  float pdcch_cl_adjust)
{
  const uint32_t Y = is_common ? 0 : get_Y(ss, slot, rnti);
  uint8_t nr_of_candidates;

  int agg_level_search_order[NUM_PDCCH_AGG_LEVELS];
  determine_aggregation_level_search_order(agg_level_search_order, pdcch_cl_adjust);

  for (int i = 0; i < NUM_PDCCH_AGG_LEVELS; i++) {
    find_aggregation_candidates(aggregation_level, &nr_of_candidates, ss, 1 << agg_level_search_order[i]);
    if (nr_of_candidates > 0)
      break;
  }
  int CCEIndex = find_pdcch_candidate(nrmac, CC_id, *aggregation_level, nr_of_candidates, beam_idx, sched_pdcch, coreset, Y);
  return CCEIndex;
}

void fill_pdcch_vrb_map(gNB_MAC_INST *mac,
                        int CC_id,
                        NR_sched_pdcch_t *pdcch,
                        int first_cce,
                        int aggregation,
                        int beam)
{
  uint16_t *vrb_map = mac->common_channels[CC_id].vrb_map[beam];

  int N_rb = pdcch->n_rb; // nb of rbs of coreset per symbol
  int L = pdcch->RegBundleSize;
  int R = pdcch->InterleaverSize;
  int n_shift = pdcch->ShiftIndex;
  int N_symb = pdcch->DurationSymbols;
  int N_regs = N_rb*N_symb; // nb of REGs per coreset
  int B_rb = L/N_symb; // nb of RBs occupied by each REG bundle
  int C = R>0 ? N_regs/(L*R) : 0;

  for (int j=first_cce; j<first_cce+aggregation; j++) { // loop over CCEs
    for (int k=6*j/L; k<(6*j/L+6/L); k++) { // loop over REG bundles
      int f = cce_to_reg_interleaving(R, k, n_shift, C, L, N_regs);
      for(int rb=0; rb<B_rb; rb++) // loop over the RBs of the bundle
        vrb_map[pdcch->BWPStart + f*B_rb + rb] |= SL_to_bitmap(pdcch->StartSymbolIndex, N_symb);
    }
  }
}

static bool multiple_2_3_5(int rb)
{
  while (rb % 2 == 0)
    rb /= 2;
  while (rb % 3 == 0)
    rb /= 3;
  while (rb % 5 == 0)
    rb /= 5;

  return (rb == 1);
}

bool nr_find_nb_rb(uint16_t Qm,
                   uint16_t R,
                   long transform_precoding,
                   uint8_t nrOfLayers,
                   uint16_t nb_symb_sch,
                   uint16_t nb_dmrs_prb,
                   uint32_t bytes,
                   uint16_t nb_rb_min,
                   uint16_t nb_rb_max,
                   uint32_t *tbs,
                   uint16_t *nb_rb)
{
  // for transform precoding only RB = 2^a_2 * 3^a_3 * 5^a_5 is allowed with a non-negative
  while(transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled &&
        !multiple_2_3_5(nb_rb_max))
    nb_rb_max--;

  /* is the maximum (not even) enough? */
  *nb_rb = nb_rb_max;
  *tbs = nr_compute_tbs(Qm, R, *nb_rb, nb_symb_sch, nb_dmrs_prb, 0, 0, nrOfLayers) >> 3;
  /* check whether it does not fit, or whether it exactly fits. Some algorithms
   * might depend on the return value! */
  if (bytes > *tbs)
    return false;
  if (bytes == *tbs)
    return true;

  /* is the minimum enough? */
  *nb_rb = nb_rb_min;
  *tbs = nr_compute_tbs(Qm, R, *nb_rb, nb_symb_sch, nb_dmrs_prb, 0, 0, nrOfLayers) >> 3;
  if (bytes <= *tbs)
    return true;

  /* perform binary search to allocate all bytes within a TBS up to nb_rb_max
   * RBs */
  int hi = nb_rb_max;
  int lo = nb_rb_min;
  for (int p = (hi + lo) / 2; lo + 1 < hi; p = (hi + lo) / 2) {
    // for transform precoding only RB = 2^a_2 * 3^a_3 * 5^a_5 is allowed with a non-negative
    while(transform_precoding == NR_PUSCH_Config__transformPrecoder_enabled &&
          !multiple_2_3_5(p))
      p++;

    // If by increasing p for transform precoding we already hit the high, break to avoid infinite loop
    if (p == hi)
      break;

    const uint32_t TBS = nr_compute_tbs(Qm, R, p, nb_symb_sch, nb_dmrs_prb, 0, 0, nrOfLayers) >> 3;
    if (bytes == TBS) {
      hi = p;
      break;
    } else if (bytes < TBS) {
      hi = p;
    } else {
      lo = p;
    }
  }
  *nb_rb = hi;
  *tbs = nr_compute_tbs(Qm, R, *nb_rb, nb_symb_sch, nb_dmrs_prb, 0, 0, nrOfLayers) >> 3;
  /* return whether we could allocate all bytes and stay below nb_rb_max */
  return *tbs >= bytes && *nb_rb <= nb_rb_max;
}

const NR_DMRS_UplinkConfig_t *get_DMRS_UplinkConfig(const NR_PUSCH_Config_t *pusch_Config, const NR_tda_info_t *tda_info)
{
  if (pusch_Config == NULL)
    return NULL;

  return tda_info->mapping_type == typeA ? pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA->choice.setup
                                         : pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup;
}

NR_pusch_dmrs_t get_ul_dmrs_params(const NR_ServingCellConfigCommon_t *scc,
                                   const NR_UE_UL_BWP_t *ul_bwp,
                                   const NR_tda_info_t *tda_info,
                                   const int Layers) {

  NR_pusch_dmrs_t dmrs = {0};
  // TODO setting of cdm groups with no data to be redone for MIMO
  if(NFAPI_MODE == NFAPI_MODE_AERIAL) {
    dmrs.num_dmrs_cdm_grps_no_data = 2;
  } else {
    if (ul_bwp->transform_precoding && Layers < 3)
      dmrs.num_dmrs_cdm_grps_no_data = ul_bwp->dci_format == NR_UL_DCI_FORMAT_0_1 || tda_info->nrOfSymbols <= 2 ? 1 : 2;
    else
      dmrs.num_dmrs_cdm_grps_no_data = 2;
  }

  NR_DMRS_UplinkConfig_t *NR_DMRS_UplinkConfig = ul_bwp->pusch_Config ?
                                                 (tda_info->mapping_type == typeA ?
                                                 ul_bwp->pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA->choice.setup :
                                                 ul_bwp->pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup) : NULL;

  dmrs.dmrs_config_type = NR_DMRS_UplinkConfig && NR_DMRS_UplinkConfig->dmrs_Type ? 1 : 0;

  const pusch_dmrs_AdditionalPosition_t additional_pos = (NR_DMRS_UplinkConfig && NR_DMRS_UplinkConfig->dmrs_AdditionalPosition) ?
                                                         (*NR_DMRS_UplinkConfig->dmrs_AdditionalPosition ==
                                                         NR_DMRS_UplinkConfig__dmrs_AdditionalPosition_pos3 ?
                                                         3 : *NR_DMRS_UplinkConfig->dmrs_AdditionalPosition) : 2;

  const pusch_maxLength_t pusch_maxLength = NR_DMRS_UplinkConfig ? (NR_DMRS_UplinkConfig->maxLength == NULL ? 1 : 2) : 1;
  dmrs.ul_dmrs_symb_pos = get_l_prime(tda_info->nrOfSymbols,
                                       tda_info->mapping_type,
                                       additional_pos,
                                       pusch_maxLength,
                                       tda_info->startSymbolIndex,
                                       scc->dmrs_TypeA_Position);

  uint8_t num_dmrs_symb = 0;
  for(int i = tda_info->startSymbolIndex; i < tda_info->startSymbolIndex + tda_info->nrOfSymbols; i++)
    num_dmrs_symb += (dmrs.ul_dmrs_symb_pos >> i) & 1;
  dmrs.num_dmrs_symb = num_dmrs_symb;
  dmrs.N_PRB_DMRS = dmrs.num_dmrs_cdm_grps_no_data * (dmrs.dmrs_config_type == 0 ? 6 : 4);
  return dmrs;
}

#define BLER_UPDATE_FRAME 10
#define BLER_FILTER 0.9f
int get_mcs_from_bler(const NR_bler_options_t *bler_options,
                      const NR_mac_dir_stats_t *stats,
                      NR_bler_stats_t *bler_stats,
                      int max_mcs,
                      frame_t frame)
{
  int diff = frame - bler_stats->last_frame;
  if (diff < 0) // wrap around
    diff += 1024;

  max_mcs = min(max_mcs, bler_options->max_mcs);
  const uint8_t old_mcs = min(bler_stats->mcs, max_mcs);
  if (diff < BLER_UPDATE_FRAME)
    return old_mcs; // no update

  // last update is longer than x frames ago
  const int num_dl_sched = (int)(stats->rounds[0] - bler_stats->rounds[0]);
  const int num_dl_retx = (int)(stats->rounds[1] - bler_stats->rounds[1]);
  const float bler_window = num_dl_sched > 0 ? (float) num_dl_retx / num_dl_sched : bler_stats->bler;
  bler_stats->bler = BLER_FILTER * bler_stats->bler + (1 - BLER_FILTER) * bler_window;

  int new_mcs = old_mcs;
  if (bler_stats->bler < bler_options->lower && old_mcs < max_mcs && num_dl_sched > 3)
    new_mcs += 1;
  else if (bler_stats->bler > bler_options->upper || num_dl_sched <= 3) // above threshold or no activity
    new_mcs -= 1;
  // else we are within threshold boundaries

  new_mcs = max(new_mcs, bler_options->min_mcs);
  bler_stats->last_frame = frame;
  bler_stats->mcs = new_mcs;
  memcpy(bler_stats->rounds, stats->rounds, sizeof(stats->rounds));
  LOG_D(MAC, "frame %4d MCS %d -> %d (num_dl_sched %d, num_dl_retx %d, BLER wnd %.3f avg %.6f)\n",
        frame, old_mcs, new_mcs, num_dl_sched, num_dl_retx, bler_window, bler_stats->bler);
  return new_mcs;
}

void config_uldci(const NR_UE_ServingCell_Info_t *sc_info,
                  const nfapi_nr_pusch_pdu_t *pusch_pdu,
                  dci_pdu_rel15_t *dci_pdu_rel15,
                  nr_srs_feedback_t *srs_feedback,
                  int *tpmi,
                  int time_domain_assignment,
                  uint8_t tpc,
                  uint8_t ndi,
                  NR_UE_UL_BWP_t *ul_bwp,
                  NR_SearchSpace__searchSpaceType_PR ss_type)
{
  int bwp_id = ul_bwp->bwp_id;
  nr_dci_format_t dci_format = ul_bwp->dci_format;

  // 3GPP TS 38.214 Section 6.1.2.2.2 Uplink resource allocation type 1
  uint16_t riv_bwp_size = ul_bwp->BWPSize;
  if (dci_format == NR_UL_DCI_FORMAT_0_0 && ss_type == NR_SearchSpace__searchSpaceType_PR_common && ul_bwp->pusch_Config
      && ul_bwp->pusch_Config->resourceAllocation == NR_PUSCH_Config__resourceAllocation_resourceAllocationType1) {
    riv_bwp_size = sc_info->initial_ul_BWPSize;
  }
  dci_pdu_rel15->frequency_domain_assignment.val =
      PRBalloc_to_locationandbandwidth0(pusch_pdu->rb_size, pusch_pdu->rb_start, riv_bwp_size);
  dci_pdu_rel15->time_domain_assignment.val = time_domain_assignment;
  dci_pdu_rel15->frequency_hopping_flag.val = pusch_pdu->frequency_hopping;
  dci_pdu_rel15->mcs = pusch_pdu->mcs_index;
  dci_pdu_rel15->ndi = ndi;
  dci_pdu_rel15->rv = pusch_pdu->pusch_data.rv_index;
  dci_pdu_rel15->harq_pid.val = pusch_pdu->pusch_data.harq_process_id;
  dci_pdu_rel15->tpc = tpc;

  NR_PUSCH_Config_t *pusch_Config = ul_bwp->pusch_Config;
  if (pusch_Config)
    AssertFatal(pusch_Config->resourceAllocation == NR_PUSCH_Config__resourceAllocation_resourceAllocationType1,
                "Only frequency resource allocation type 1 is currently supported\n");

  switch (dci_format) {
    case NR_UL_DCI_FORMAT_0_0:
      dci_pdu_rel15->format_indicator = 0;
      break;
    case NR_UL_DCI_FORMAT_0_1:
      LOG_D(NR_MAC,"Configuring DCI Format 0_1\n");
      dci_pdu_rel15->dai[0].val = 0; //TODO
      // bwp indicator as per table 7.3.1.1.2-1 in 38.212
      dci_pdu_rel15->bwp_indicator.val = sc_info->n_ul_bwp < 4 ? bwp_id : bwp_id - 1;
      // SRS resource indicator
      if (pusch_Config &&
          pusch_Config->txConfig != NULL) {
        AssertFatal(*pusch_Config->txConfig == NR_PUSCH_Config__txConfig_codebook,
                    "Non Codebook configuration non supported\n");
        compute_srs_resource_indicator(sc_info->maxMIMO_Layers_PUSCH,
                                       pusch_Config,
                                       ul_bwp->srs_Config,
                                       srs_feedback,
                                       &dci_pdu_rel15->srs_resource_indicator.val);
      }
      compute_precoding_information(pusch_Config,
                                    ul_bwp->srs_Config,
                                    dci_pdu_rel15->srs_resource_indicator,
                                    srs_feedback,
                                    &pusch_pdu->nrOfLayers,
                                    tpmi,
                                    &dci_pdu_rel15->precoding_information.val);

      // antenna_ports.val = 0 for transform precoder is disabled, dmrs-Type=1, maxLength=1, Rank=1/2/3/4
      // Antenna Ports

      dci_pdu_rel15->antenna_ports.val = NFAPI_MODE == NFAPI_MODE_AERIAL ? 2 : 0;

      // DMRS sequence initialization
      dci_pdu_rel15->dmrs_sequence_initialization.val = pusch_pdu->scid;
      break;
    default :
      AssertFatal(0, "Valid UL formats are 0_0 and 0_1\n");
  }

  LOG_D(NR_MAC,
        "%s() ULDCI type 0 payload: dci_format %d, freq_alloc %d, time_alloc %d, freq_hop_flag %d, precoding_information.val %d antenna_ports.val %d mcs %d tpc %d ndi %d rv %d\n",
        __func__,
        dci_format,
        dci_pdu_rel15->frequency_domain_assignment.val,
        dci_pdu_rel15->time_domain_assignment.val,
        dci_pdu_rel15->frequency_hopping_flag.val,
        dci_pdu_rel15->precoding_information.val,
        dci_pdu_rel15->antenna_ports.val,
        dci_pdu_rel15->mcs,
        dci_pdu_rel15->tpc,
        dci_pdu_rel15->ndi,
        dci_pdu_rel15->rv);
}

const int default_pucch_fmt[]       = {0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1};
const int default_pucch_firstsymb[] = {12,12,12,10,10,10,10,4,4,4,4,0,0,0,0,0};
const int default_pucch_numbsymb[]  = {2,2,2,2,4,4,4,4,10,10,10,10,14,14,14,14,14};
const int default_pucch_prboffset[] = {0,0,3,0,0,2,4,0,0,2,4,0,0,2,4,-1};
const int default_pucch_csset[]     = {2,3,3,2,4,4,4,2,4,4,4,2,4,4,4,4};

int nr_get_default_pucch_res(int pucch_ResourceCommon) {

  AssertFatal(pucch_ResourceCommon>=0 && pucch_ResourceCommon < 16, "illegal pucch_ResourceCommon %d\n",pucch_ResourceCommon);

  return(default_pucch_csset[pucch_ResourceCommon]);
}

void nr_configure_pdcch(nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu, NR_ControlResourceSet_t *coreset, NR_sched_pdcch_t *pdcch)
{
  pdcch_pdu->BWPSize = pdcch->BWPSize;
  pdcch_pdu->BWPStart = pdcch->BWPStart;
  pdcch_pdu->SubcarrierSpacing = pdcch->SubcarrierSpacing;
  pdcch_pdu->CyclicPrefix = pdcch->CyclicPrefix;
  pdcch_pdu->StartSymbolIndex = pdcch->StartSymbolIndex;

  pdcch_pdu->DurationSymbols  = coreset->duration;

  for (int i = 0; i < 6; i++)
    pdcch_pdu->FreqDomainResource[i] = coreset->frequencyDomainResources.buf[i];

  LOG_D(NR_MAC,
        "Coreset : BWPstart %d, BWPsize %d, SCS %d, freq %x, , duration %ld\n",
        pdcch_pdu->BWPStart,
        pdcch_pdu->BWPSize,
        pdcch_pdu->SubcarrierSpacing,
        coreset->frequencyDomainResources.buf[0],
        coreset->duration);

  pdcch_pdu->CceRegMappingType = pdcch->CceRegMappingType;
  pdcch_pdu->RegBundleSize = pdcch->RegBundleSize;
  pdcch_pdu->InterleaverSize = pdcch->InterleaverSize;
  pdcch_pdu->ShiftIndex = pdcch->ShiftIndex;

  pdcch_pdu->CoreSetType = coreset->controlResourceSetId != 0 ? NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG : NFAPI_NR_CSET_CONFIG_MIB_SIB1;

  //precoderGranularity
  pdcch_pdu->precoderGranularity = coreset->precoderGranularity;
}

int nr_get_pucch_resource(NR_ControlResourceSet_t *coreset,
                          NR_PUCCH_Config_t *pucch_Config,
                          int CCEIndex) {
  int r_pucch = -1;
  if(pucch_Config == NULL) {
    int n_rb,rb_offset;
    get_coreset_rballoc(coreset->frequencyDomainResources.buf,&n_rb,&rb_offset);
    const uint16_t N_cce = n_rb * coreset->duration / NR_NB_REG_PER_CCE;
    const int delta_PRI=0;
    r_pucch = ((CCEIndex<<1)/N_cce)+(delta_PRI<<1);
  }
  return r_pucch;
}

// This function configures pucch pdu fapi structure
void nr_configure_pucch(nfapi_nr_pucch_pdu_t *pucch_pdu,
                        NR_ServingCellConfigCommon_t *scc,
                        NR_UE_info_t *UE,
                        uint8_t pucch_resource,
                        uint16_t O_csi,
                        uint16_t O_ack,
                        uint8_t O_sr,
                        int r_pucch)
{
  NR_PUCCH_Resource_t *pucchres;
  NR_PUCCH_FormatConfig_t *pucchfmt;
  NR_UE_UL_BWP_t *current_BWP = &UE->current_UL_BWP;

  int res_found = 0;

  pucch_pdu->bit_len_harq = O_ack;
  pucch_pdu->bit_len_csi_part1 = O_csi;

  uint16_t O_uci = O_csi + O_ack;

  NR_PUSCH_Config_t *pusch_Config = current_BWP->pusch_Config;

  long *pusch_id = pusch_Config ? pusch_Config->dataScramblingIdentityPUSCH : NULL;

  long *id0 = NULL;
  if (pusch_Config &&
      pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA != NULL &&
      pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA->choice.setup->transformPrecodingDisabled != NULL)
    id0 = pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA->choice.setup->transformPrecodingDisabled->scramblingID0;
  else if (pusch_Config && pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB != NULL &&
           pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup->transformPrecodingDisabled != NULL)
    id0 = pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup->transformPrecodingDisabled->scramblingID0;
  else id0 = scc->physCellId;

  NR_PUCCH_ConfigCommon_t *pucch_ConfigCommon = current_BWP->pucch_ConfigCommon;

  // hop flags and hopping id are valid for any BWP
  switch (pucch_ConfigCommon->pucch_GroupHopping){
  case 0 :
    // if neither, both disabled
    pucch_pdu->group_hop_flag = 0;
    pucch_pdu->sequence_hop_flag = 0;
    break;
  case 1 :
    // if enable, group enabled
    pucch_pdu->group_hop_flag = 1;
    pucch_pdu->sequence_hop_flag = 0;
    break;
  case 2 :
    // if disable, sequence disabled
    pucch_pdu->group_hop_flag = 0;
    pucch_pdu->sequence_hop_flag = 1;
    break;
  default:
    AssertFatal(1==0,"Group hopping flag %ld undefined (0,1,2) \n", pucch_ConfigCommon->pucch_GroupHopping);
  }

  if (pucch_ConfigCommon->hoppingId != NULL)
    pucch_pdu->hopping_id = *pucch_ConfigCommon->hoppingId;
  else
    pucch_pdu->hopping_id = *scc->physCellId;

  pucch_pdu->bwp_size  = current_BWP->BWPSize;
  pucch_pdu->bwp_start = current_BWP->BWPStart;
  pucch_pdu->subcarrier_spacing = current_BWP->scs;
  pucch_pdu->cyclic_prefix = (current_BWP->cyclicprefix==NULL) ? 0 : *current_BWP->cyclicprefix;

  NR_PUCCH_Config_t *pucch_Config = current_BWP->pucch_Config;
  if (r_pucch < 0 || pucch_Config) {
    LOG_D(NR_MAC, "pucch_acknak: Filling dedicated configuration for PUCCH\n");

    int resource_id = get_pucch_resourceid(pucch_Config, O_uci, pucch_resource);

    AssertFatal(pucch_Config->resourceToAddModList!=NULL,
                "PUCCH resourceToAddModList is null\n");

    int n_list = pucch_Config->resourceToAddModList->list.count;
    AssertFatal(n_list > 0, "PUCCH resourceToAddModList is empty\n");

    // going through the list of PUCCH resources to find the one indexed by resource_id
    for (int i = 0; i < n_list; i++) {
      pucchres = pucch_Config->resourceToAddModList->list.array[i];
      if (pucchres->pucch_ResourceId == resource_id) {
        res_found = 1;
        pucch_pdu->prb_start = pucchres->startingPRB;
        pucch_pdu->rnti = UE->rnti;
        // FIXME why there is only one frequency hopping flag
        // what about inter slot frequency hopping?
        pucch_pdu->freq_hop_flag = pucchres->intraSlotFrequencyHopping ? 1 : 0;
        pucch_pdu->second_hop_prb = pucchres->secondHopPRB!= NULL ?  *pucchres->secondHopPRB : 0;
        switch(pucchres->format.present) {
          case NR_PUCCH_Resource__format_PR_format0 :
            pucch_pdu->format_type = 0;
            pucch_pdu->initial_cyclic_shift = pucchres->format.choice.format0->initialCyclicShift;
            pucch_pdu->nr_of_symbols = pucchres->format.choice.format0->nrofSymbols;
            pucch_pdu->start_symbol_index = pucchres->format.choice.format0->startingSymbolIndex;
            pucch_pdu->sr_flag = O_sr;
            pucch_pdu->prb_size = 1;
            break;
          case NR_PUCCH_Resource__format_PR_format1 :
            pucch_pdu->format_type = 1;
            pucch_pdu->initial_cyclic_shift = pucchres->format.choice.format1->initialCyclicShift;
            pucch_pdu->nr_of_symbols = pucchres->format.choice.format1->nrofSymbols;
            pucch_pdu->start_symbol_index = pucchres->format.choice.format1->startingSymbolIndex;
            pucch_pdu->time_domain_occ_idx = pucchres->format.choice.format1->timeDomainOCC;
            pucch_pdu->sr_flag = O_sr;
            pucch_pdu->prb_size = 1;
            break;
          case NR_PUCCH_Resource__format_PR_format2 :
            pucch_pdu->format_type = 2;
            pucch_pdu->sr_flag = O_sr;
            pucch_pdu->nr_of_symbols = pucchres->format.choice.format2->nrofSymbols;
            pucch_pdu->start_symbol_index = pucchres->format.choice.format2->startingSymbolIndex;
            pucch_pdu->data_scrambling_id = pusch_id ? *pusch_id : *scc->physCellId;
            pucch_pdu->dmrs_scrambling_id = id0 ? *id0 : *scc->physCellId;
            pucch_pdu->prb_size = compute_pucch_prb_size(2,
                                                         pucchres->format.choice.format2->nrofPRBs,
                                                         O_csi,
                                                         O_ack,
                                                         O_sr,
                                                         pucch_Config->format2->choice.setup->maxCodeRate,
                                                         2,
                                                         pucchres->format.choice.format2->nrofSymbols,
                                                         8);
            pucch_pdu->bit_len_csi_part1 = O_csi;
            break;
          case NR_PUCCH_Resource__format_PR_format3 :
            pucch_pdu->format_type = 3;
            pucch_pdu->nr_of_symbols = pucchres->format.choice.format3->nrofSymbols;
            pucch_pdu->start_symbol_index = pucchres->format.choice.format3->startingSymbolIndex;
            pucch_pdu->data_scrambling_id = pusch_id ? *pusch_id : *scc->physCellId;
            if (pucch_Config->format3 == NULL) {
              pucch_pdu->pi_2bpsk = 0;
              pucch_pdu->add_dmrs_flag = 0;
            }
            else {
              pucchfmt = pucch_Config->format3->choice.setup;
              pucch_pdu->pi_2bpsk = pucchfmt->pi2BPSK ? 1 : 0;
              pucch_pdu->add_dmrs_flag = pucchfmt->additionalDMRS ? 1 : 0;
            }
            int f3_dmrs_symbols = get_f3_dmrs_symbols(pucchres, pucch_Config);
            pucch_pdu->prb_size = compute_pucch_prb_size(3,
                                                         pucchres->format.choice.format3->nrofPRBs,
                                                         O_csi,
                                                         O_ack,
                                                         O_sr,
                                                         pucch_Config->format3->choice.setup->maxCodeRate,
                                                         2 - pucch_pdu->pi_2bpsk,
                                                         pucchres->format.choice.format3->nrofSymbols - f3_dmrs_symbols,
                                                         12);
            pucch_pdu->bit_len_csi_part1 = O_csi;
            break;
          case NR_PUCCH_Resource__format_PR_format4 :
            pucch_pdu->format_type = 4;
            pucch_pdu->nr_of_symbols = pucchres->format.choice.format4->nrofSymbols;
            pucch_pdu->start_symbol_index = pucchres->format.choice.format4->startingSymbolIndex;
            pucch_pdu->pre_dft_occ_len = pucchres->format.choice.format4->occ_Length;
            pucch_pdu->pre_dft_occ_idx = pucchres->format.choice.format4->occ_Index;
            pucch_pdu->data_scrambling_id = pusch_id!= NULL ? *pusch_id : *scc->physCellId;
            if (pucch_Config->format3 == NULL) {
              pucch_pdu->pi_2bpsk = 0;
              pucch_pdu->add_dmrs_flag = 0;
            }
            else {
              pucchfmt = pucch_Config->format3->choice.setup;
              pucch_pdu->pi_2bpsk = pucchfmt->pi2BPSK != NULL;
              pucch_pdu->add_dmrs_flag = pucchfmt->additionalDMRS != NULL;
            }
            pucch_pdu->bit_len_csi_part1 = O_csi;
            break;
          default :
            AssertFatal(1==0,"Undefined PUCCH format \n");
        }
      }
    }
    AssertFatal(res_found == 1, "No PUCCH resource found corresponding to id %d\n", resource_id);
    LOG_D(NR_MAC,
          "Configure pucch: pucch_pdu->format_type %d pucch_pdu->bit_len_harq %d, pucch->pdu->bit_len_csi %d\n",
          pucch_pdu->format_type,
          pucch_pdu->bit_len_harq,
          pucch_pdu->bit_len_csi_part1);
  } else { // this is the default PUCCH configuration, PUCCH format 0 or 1
    LOG_D(NR_MAC,
          "pucch_acknak: Filling default PUCCH configuration from Tables (r_pucch %d, pucch_Config %p)\n",
          r_pucch,
          pucch_Config);
    int rsetindex = *pucch_ConfigCommon->pucch_ResourceCommon;
    int prb_start, second_hop_prb, nr_of_symb, start_symb;
    set_r_pucch_parms(rsetindex,
                      r_pucch,
                      pucch_pdu->bwp_size,
                      &prb_start,
                      &second_hop_prb,
                      &nr_of_symb,
                      &start_symb);

    pucch_pdu->prb_start = prb_start;
    pucch_pdu->rnti = UE->rnti;
    pucch_pdu->freq_hop_flag = 1;
    pucch_pdu->second_hop_prb = second_hop_prb;
    pucch_pdu->format_type = default_pucch_fmt[rsetindex];
    pucch_pdu->initial_cyclic_shift = r_pucch%default_pucch_csset[rsetindex];
    if (rsetindex==3||rsetindex==7||rsetindex==11) pucch_pdu->initial_cyclic_shift*=6;
    else if (rsetindex==1||rsetindex==2) pucch_pdu->initial_cyclic_shift*=4;
    else pucch_pdu->initial_cyclic_shift*=3;
    pucch_pdu->nr_of_symbols = nr_of_symb;
    pucch_pdu->start_symbol_index = start_symb;
    if (pucch_pdu->format_type == 1) pucch_pdu->time_domain_occ_idx = 0; // check this!!
    pucch_pdu->sr_flag = O_sr;
    pucch_pdu->prb_size=1;
  }
  // Beamforming
  pucch_pdu->beamforming.num_prgs = 0;
  pucch_pdu->beamforming.prg_size = 0; // pucch_pdu->prb_size;
  pucch_pdu->beamforming.dig_bf_interface = 0;
  pucch_pdu->beamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = UE->UE_beam_index;
}

void set_r_pucch_parms(int rsetindex,
                       int r_pucch,
                       int bwp_size,
                       int *prb_start,
                       int *second_hop_prb,
                       int *nr_of_symbols,
                       int *start_symbol_index) {

  // procedure described in 38.213 section 9.2.1

  int prboffset = r_pucch/default_pucch_csset[rsetindex];
  int prboffsetm8 = (r_pucch-8)/default_pucch_csset[rsetindex];

  *prb_start = (r_pucch>>3)==0 ?
              default_pucch_prboffset[rsetindex] + prboffset:
              bwp_size-1-default_pucch_prboffset[rsetindex]-prboffsetm8;

  *second_hop_prb = (r_pucch>>3)==0?
                   bwp_size-1-default_pucch_prboffset[rsetindex]-prboffset:
                   default_pucch_prboffset[rsetindex] + prboffsetm8;

  *nr_of_symbols = default_pucch_numbsymb[rsetindex];
  *start_symbol_index = default_pucch_firstsymb[rsetindex];
}

void prepare_dci(const NR_UE_ServingCell_Info_t *servingCellInfo,
                 const NR_UE_DL_BWP_t *current_BWP,
                 const NR_ControlResourceSet_t *coreset,
                 dci_pdu_rel15_t *dci_pdu_rel15,
                 nr_dci_format_t format)
{
  const NR_PDSCH_Config_t *pdsch_Config = current_BWP ? current_BWP->pdsch_Config : NULL;

  switch(format) {
    case NR_UL_DCI_FORMAT_0_1:
      // format indicator
      dci_pdu_rel15->format_indicator = 0;
      // carrier indicator
      if (servingCellInfo->crossCarrierSchedulingConfig != NULL)
        AssertFatal(1==0,"Cross Carrier Scheduling Config currently not supported\n");
      // supplementary uplink
      if (servingCellInfo->supplementaryUplink != NULL)
        AssertFatal(1==0,"Supplementary Uplink currently not supported\n");
      // SRS request
      dci_pdu_rel15->srs_request.val = 0;
      dci_pdu_rel15->ulsch_indicator = 1;
      break;
    case NR_DL_DCI_FORMAT_1_1:
      // format indicator
      dci_pdu_rel15->format_indicator = 1;
      // carrier indicator
      if (servingCellInfo->crossCarrierSchedulingConfig != NULL)
        AssertFatal(1==0,"Cross Carrier Scheduling Config currently not supported\n");
      //vrb to prb mapping
      if (pdsch_Config->vrb_ToPRB_Interleaver==NULL)
        dci_pdu_rel15->vrb_to_prb_mapping.val = 0;
      else
        dci_pdu_rel15->vrb_to_prb_mapping.val = 1;
      //bundling size indicator
      if (pdsch_Config->prb_BundlingType.present == NR_PDSCH_Config__prb_BundlingType_PR_dynamicBundling)
        AssertFatal(1==0,"Dynamic PRB bundling type currently not supported\n");
      //rate matching indicator
      uint16_t msb = (pdsch_Config->rateMatchPatternGroup1==NULL)?0:1;
      uint16_t lsb = (pdsch_Config->rateMatchPatternGroup2==NULL)?0:1;
      dci_pdu_rel15->rate_matching_indicator.val = lsb | (msb<<1);
      // aperiodic ZP CSI-RS trigger
      if (pdsch_Config->aperiodic_ZP_CSI_RS_ResourceSetsToAddModList != NULL)
        AssertFatal(1==0,"Aperiodic ZP CSI-RS currently not supported\n");
      // transmission configuration indication
      if (coreset->tci_PresentInDCI != NULL)
        AssertFatal(1==0,"TCI in DCI currently not supported\n");
      //srs resource set
      if (servingCellInfo->carrierSwitching) {
        if (servingCellInfo->carrierSwitching->srs_TPC_PDCCH_Group) {
          switch (servingCellInfo->carrierSwitching->srs_TPC_PDCCH_Group->present) {
            case NR_SRS_CarrierSwitching__srs_TPC_PDCCH_Group_PR_NOTHING:
              dci_pdu_rel15->srs_request.val = 0;
              break;
            case NR_SRS_CarrierSwitching__srs_TPC_PDCCH_Group_PR_typeA:
              AssertFatal(1==0,"SRS TPC PRCCH group type A currently not supported\n");
              break;
            case NR_SRS_CarrierSwitching__srs_TPC_PDCCH_Group_PR_typeB:
              AssertFatal(1==0,"SRS TPC PRCCH group type B currently not supported\n");
              break;
          }
        } else
          dci_pdu_rel15->srs_request.val = 0;
      } else
        dci_pdu_rel15->srs_request.val = 0;
    // CBGTI and CBGFI
      if (servingCellInfo->pdsch_CGB_Transmission)
        AssertFatal(1 == 0, "CBG transmission currently not supported\n");
      break;
  default :
    AssertFatal(1==0,"Prepare dci currently only implemented for 1_1 and 0_1 \n");
  }
}

void fill_dci_pdu_rel15(const NR_UE_ServingCell_Info_t *servingCellInfo,
                        const NR_UE_DL_BWP_t *current_DL_BWP,
                        const NR_UE_UL_BWP_t *current_UL_BWP,
                        nfapi_nr_dl_dci_pdu_t *pdcch_dci_pdu,
                        dci_pdu_rel15_t *dci_pdu_rel15,
                        int dci_format,
                        int rnti_type,
                        int bwp_id,
                        NR_SearchSpace_t *ss,
                        NR_ControlResourceSet_t *coreset,
                        long pdsch_HARQ_ACK_Codebook,
                        uint16_t cset0_bwp_size)
{
  uint8_t fsize = 0, pos = 0;
  uint64_t *dci_pdu = (uint64_t *)pdcch_dci_pdu->Payload;
  *dci_pdu = 0;
  uint16_t alt_size = 0;
  uint16_t N_RB;
  if(current_DL_BWP) {
    N_RB = get_rb_bwp_dci(dci_format,
                          ss->searchSpaceType->present,
                          cset0_bwp_size,
                          current_UL_BWP->BWPSize,
                          current_DL_BWP->BWPSize,
                          servingCellInfo->initial_ul_BWPSize,
                          servingCellInfo->initial_dl_BWPSize);

    // computing alternative size for padding
    dci_pdu_rel15_t temp_pdu;
    if(dci_format == NR_DL_DCI_FORMAT_1_0)
      alt_size = nr_dci_size(current_DL_BWP,
                             current_UL_BWP,
                             servingCellInfo,
                             pdsch_HARQ_ACK_Codebook,
                             &temp_pdu,
                             NR_UL_DCI_FORMAT_0_0,
                             rnti_type,
                             coreset,
                             bwp_id,
                             ss->searchSpaceType->present,
                             cset0_bwp_size,
                             0);

    if(dci_format == NR_UL_DCI_FORMAT_0_0)
      alt_size = nr_dci_size(current_DL_BWP,
                             current_UL_BWP,
                             servingCellInfo,
                             pdsch_HARQ_ACK_Codebook,
                             &temp_pdu,
                             NR_DL_DCI_FORMAT_1_0,
                             rnti_type,
                             coreset,
                             bwp_id,
                             ss->searchSpaceType->present,
                             cset0_bwp_size,
                             0);

  }
  else
    N_RB = cset0_bwp_size;

  int dci_size = nr_dci_size(current_DL_BWP,
                             current_UL_BWP,
                             servingCellInfo,
                             pdsch_HARQ_ACK_Codebook,
                             dci_pdu_rel15,
                             dci_format,
                             rnti_type,
                             coreset,
                             bwp_id,
                             ss->searchSpaceType->present,
                             cset0_bwp_size,
                             alt_size);
  if (dci_size == 0)
    return;
  pdcch_dci_pdu->PayloadSizeBits = dci_size;
  AssertFatal(dci_size <= 64, "DCI sizes above 64 bits not yet supported");
  if (dci_format == NR_DL_DCI_FORMAT_1_1 || dci_format == NR_UL_DCI_FORMAT_0_1)
    prepare_dci(servingCellInfo, current_DL_BWP, coreset, dci_pdu_rel15, dci_format);

  /// Payload generation
  switch (dci_format) {
  case NR_DL_DCI_FORMAT_1_0:
    switch (rnti_type) {
      case TYPE_RA_RNTI_:
        // Freq domain assignment
        fsize = (int)ceil(log2((N_RB * (N_RB + 1)) >> 1));
        pos = fsize;
        *dci_pdu |= (((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val & ((1 << fsize) - 1)) << (dci_size - pos));
        LOG_D(NR_MAC,
              "RA_RNTI, size %d frequency-domain assignment %d (%d bits) N_RB_BWP %d=> %d (0x%lx)\n",
              dci_size,
              dci_pdu_rel15->frequency_domain_assignment.val,
              fsize,
              N_RB,
              dci_size - pos,
              *dci_pdu);
        // Time domain assignment
        pos += 4;
        *dci_pdu |= (((uint64_t)dci_pdu_rel15->time_domain_assignment.val & 0xf) << (dci_size - pos));
        LOG_D(NR_MAC,
              "time-domain assignment %d  (4 bits)=> %d (0x%lx)\n",
              dci_pdu_rel15->time_domain_assignment.val,
              dci_size - pos,
              *dci_pdu);
        // VRB to PRB mapping
        pos++;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->vrb_to_prb_mapping.val & 0x1) << (dci_size - pos);
        LOG_D(NR_MAC,
              "vrb to prb mapping %d  (1 bits)=> %d (0x%lx)\n",
              dci_pdu_rel15->vrb_to_prb_mapping.val,
              dci_size - pos,
              *dci_pdu);
        // MCS
        pos += 5;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
        LOG_D(NR_MAC, "mcs %d  (5 bits)=> %d (0x%lx)\n", dci_pdu_rel15->mcs, dci_size - pos, *dci_pdu);
        // TB scaling
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->tb_scaling & 0x3) << (dci_size - pos);
        LOG_D(NR_MAC, "tb_scaling %d  (2 bits)=> %d (0x%lx)\n", dci_pdu_rel15->tb_scaling, dci_size - pos, *dci_pdu);
        break;

      case TYPE_C_RNTI_:
        // indicating a DL DCI format 1bit
        pos++;
        *dci_pdu |= ((uint64_t)1) << (dci_size - pos);
        LOG_D(NR_MAC,
              "DCI1_0 (size %d): Format indicator %d (%d bits) N_RB_BWP %d => %d (0x%lx)\n",
              dci_size,
              dci_pdu_rel15->format_indicator,
              1,
              N_RB,
              dci_size - pos,
              *dci_pdu);
        // Freq domain assignment (275rb >> fsize = 16)
        fsize = (int)ceil(log2((N_RB * (N_RB + 1)) >> 1));
        pos += fsize;
        *dci_pdu |= (((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val & ((1 << fsize) - 1)) << (dci_size - pos));
        LOG_D(NR_MAC,
              "Freq domain assignment %d (%d bits)=> %d (0x%lx)\n",
              dci_pdu_rel15->frequency_domain_assignment.val,
              fsize,
              dci_size - pos,
              *dci_pdu);
        uint16_t is_ra = 1;
        for (int i = 0; i < fsize; i++) {
          if (!((dci_pdu_rel15->frequency_domain_assignment.val >> i) & 1)) {
            is_ra = 0;
            break;
          }
        }
        if (is_ra) { // fsize are all 1  38.212 p86
          // ra_preamble_index 6 bits
          pos += 6;
          *dci_pdu |= ((dci_pdu_rel15->ra_preamble_index & 0x3f) << (dci_size - pos));
          // UL/SUL indicator  1 bit
          pos++;
          *dci_pdu |= (dci_pdu_rel15->ul_sul_indicator.val & 1) << (dci_size - pos);
          // SS/PBCH index  6 bits
          pos += 6;
          *dci_pdu |= ((dci_pdu_rel15->ss_pbch_index & 0x3f) << (dci_size - pos));
          //  prach_mask_index  4 bits
          pos += 4;
          *dci_pdu |= ((dci_pdu_rel15->prach_mask_index & 0xf) << (dci_size - pos));
        } else {
          // Time domain assignment 4bit
          pos += 4;
          *dci_pdu |= ((dci_pdu_rel15->time_domain_assignment.val & 0xf) << (dci_size - pos));
          LOG_D(NR_MAC,
                "Time domain assignment %d (%d bits)=> %d (0x%lx)\n",
                dci_pdu_rel15->time_domain_assignment.val,
                4,
                dci_size - pos,
                *dci_pdu);
          // VRB to PRB mapping  1bit
          pos++;
          *dci_pdu |= (dci_pdu_rel15->vrb_to_prb_mapping.val & 1) << (dci_size - pos);
          LOG_D(NR_MAC,
                "VRB to PRB %d (%d bits)=> %d (0x%lx)\n",
                dci_pdu_rel15->vrb_to_prb_mapping.val,
                1,
                dci_size - pos,
                *dci_pdu);
          // MCS 5bit  //bit over 32, so dci_pdu ++
          pos += 5;
          *dci_pdu |= (dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
          LOG_D(NR_MAC, "MCS %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->mcs, 5, dci_size - pos, *dci_pdu);
          // New data indicator 1bit
          pos++;
          *dci_pdu |= (dci_pdu_rel15->ndi & 1) << (dci_size - pos);
          LOG_D(NR_MAC, "NDI %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->ndi, 1, dci_size - pos, *dci_pdu);
          // Redundancy version  2bit
          pos += 2;
          *dci_pdu |= (dci_pdu_rel15->rv & 0x3) << (dci_size - pos);
          LOG_D(NR_MAC, "RV %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->rv, 2, dci_size - pos, *dci_pdu);
          // HARQ process number  4bit/5bit
          fsize = dci_pdu_rel15->harq_pid.nbits;
          pos += fsize;
          *dci_pdu |= ((dci_pdu_rel15->harq_pid.val & ((1 << fsize) - 1)) << (dci_size - pos));
          LOG_D(NR_MAC, "HARQ_PID %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->harq_pid.val, fsize, dci_size - pos, *dci_pdu);
          // Downlink assignment index  2bit
          pos += 2;
          *dci_pdu |= ((dci_pdu_rel15->dai[0].val & 3) << (dci_size - pos));
          LOG_D(NR_MAC, "DAI %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->dai[0].val, 2, dci_size - pos, *dci_pdu);
          // TPC command for scheduled PUCCH  2bit
          pos += 2;
          *dci_pdu |= ((dci_pdu_rel15->tpc & 3) << (dci_size - pos));
          LOG_D(NR_MAC, "TPC %d (%d bits)=> %d (0x%lx)\n", dci_pdu_rel15->tpc, 2, dci_size - pos, *dci_pdu);
          // PUCCH resource indicator  3bit
          pos += 3;
          *dci_pdu |= ((dci_pdu_rel15->pucch_resource_indicator & 0x7) << (dci_size - pos));
          LOG_D(NR_MAC,
                "PUCCH RI %d (%d bits)=> %d (0x%lx)\n",
                dci_pdu_rel15->pucch_resource_indicator,
                3,
                dci_size - pos,
                *dci_pdu);
          // PDSCH-to-HARQ_feedback timing indicator 3bit
          pos += 3;
          *dci_pdu |= ((dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val & 0x7) << (dci_size - pos));
          LOG_D(NR_MAC,
                "PDSCH to HARQ TI %d (%d bits)=> %d (0x%lx)\n",
                dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val,
                3,
                dci_size - pos,
                *dci_pdu);
        } // end else
        break;

      case TYPE_P_RNTI_:
        // Short Messages Indicator – 2 bits
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->short_messages_indicator >> (1 - i)) & 1) << (dci_size - pos++);
        // Short Messages – 8 bits
        for (int i = 0; i < 8; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->short_messages >> (7 - i)) & 1) << (dci_size - pos++);
        // Freq domain assignment 0-16 bit
        fsize = (int)ceil(log2((N_RB * (N_RB + 1)) >> 1));
        for (int i = 0; i < fsize; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val >> (fsize - i - 1)) & 1) << (dci_size - pos++);
        // Time domain assignment 4 bit
        for (int i = 0; i < 4; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->time_domain_assignment.val >> (3 - i)) & 1) << (dci_size - pos++);
        // VRB to PRB mapping 1 bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->vrb_to_prb_mapping.val & 1) << (dci_size - pos++);
        // MCS 5 bit
        for (int i = 0; i < 5; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->mcs >> (4 - i)) & 1) << (dci_size - pos++);
        // TB scaling 2 bit
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->tb_scaling >> (1 - i)) & 1) << (dci_size - pos++);
        break;

      case TYPE_SI_RNTI_:
        pos = 1;
        // Freq domain assignment 0-16 bit
        fsize = (int)ceil(log2((N_RB * (N_RB + 1)) >> 1));
        LOG_D(NR_MAC, "fsize = %i\n", fsize);
        for (int i = 0; i < fsize; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val >> (fsize - i - 1)) & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_domain_assignment.val = %i\n", dci_pdu_rel15->frequency_domain_assignment.val);
        // Time domain assignment 4 bit
        for (int i = 0; i < 4; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->time_domain_assignment.val >> (3 - i)) & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->time_domain_assignment.val = %i\n", dci_pdu_rel15->time_domain_assignment.val);
        // VRB to PRB mapping 1 bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->vrb_to_prb_mapping.val & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->vrb_to_prb_mapping.val = %i\n", dci_pdu_rel15->vrb_to_prb_mapping.val);
        // MCS 5bit  //bit over 32, so dci_pdu ++
        for (int i = 0; i < 5; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->mcs >> (4 - i)) & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->mcs = %i\n", dci_pdu_rel15->mcs);
        // Redundancy version  2bit
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->rv >> (1 - i)) & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->rv = %i\n", dci_pdu_rel15->rv);
        // System information indicator 1bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->system_info_indicator & 1) << (dci_size - pos++);
        LOG_D(NR_MAC, "dci_pdu_rel15->system_info_indicator = %i\n", dci_pdu_rel15->system_info_indicator);
        break;

      case TYPE_TC_RNTI_:
        pos = 1;
        // indicating a DL DCI format 1bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->format_indicator & 1) << (dci_size - pos++);
        // Freq domain assignment 0-16 bit
        fsize = (int)ceil(log2((N_RB * (N_RB + 1)) >> 1));
        for (int i = 0; i < fsize; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val >> (fsize - i - 1)) & 1) << (dci_size - pos++);
        // Time domain assignment 4 bit
        for (int i = 0; i < 4; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->time_domain_assignment.val >> (3 - i)) & 1) << (dci_size - pos++);
        // VRB to PRB mapping 1 bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->vrb_to_prb_mapping.val & 1) << (dci_size - pos++);
        // MCS 5bit  //bit over 32, so dci_pdu ++
        for (int i = 0; i < 5; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->mcs >> (4 - i)) & 1) << (dci_size - pos++);
        // New data indicator 1bit
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi & 1) << (dci_size - pos++);
        // Redundancy version  2bit
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->rv >> (1 - i)) & 1) << (dci_size - pos++);
        // HARQ process number  4bit/5bit
        fsize = dci_pdu_rel15->harq_pid.nbits;
        for (int i = 0; i < fsize; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->harq_pid.val >> (fsize - i - 1)) & 1) << (dci_size - pos++);
        // Downlink assignment index – 2 bits
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->dai[0].val >> (1 - i)) & 1) << (dci_size - pos++);
        // TPC command for scheduled PUCCH – 2 bits
        for (int i = 0; i < 2; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->tpc >> (1 - i)) & 1) << (dci_size - pos++);
        // PUCCH resource indicator – 3 bits
        for (int i = 0; i < 3; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->pucch_resource_indicator >> (2 - i)) & 1) << (dci_size - pos++);
        // PDSCH-to-HARQ_feedback timing indicator – 3 bits
        for (int i = 0; i < 3; i++)
          *dci_pdu |= (((uint64_t)dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val >> (2 - i)) & 1) << (dci_size - pos++);

        LOG_D(NR_MAC, "N_RB = %i\n", N_RB);
        LOG_D(NR_MAC, "dci_size = %i\n", dci_size);
        LOG_D(NR_MAC, "fsize = %i\n", fsize);
        LOG_D(NR_MAC, "dci_pdu_rel15->format_indicator = %i\n", dci_pdu_rel15->format_indicator);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_domain_assignment.val = %i\n", dci_pdu_rel15->frequency_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->time_domain_assignment.val = %i\n", dci_pdu_rel15->time_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->vrb_to_prb_mapping.val = %i\n", dci_pdu_rel15->vrb_to_prb_mapping.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->mcs = %i\n", dci_pdu_rel15->mcs);
        LOG_D(NR_MAC, "dci_pdu_rel15->rv = %i\n", dci_pdu_rel15->rv);
        LOG_D(NR_MAC, "dci_pdu_rel15->harq_pid = %i\n", dci_pdu_rel15->harq_pid.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->dai[0].val = %i\n", dci_pdu_rel15->dai[0].val);
        LOG_D(NR_MAC, "dci_pdu_rel15->tpc = %i\n", dci_pdu_rel15->tpc);
        LOG_D(NR_MAC,
              "dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val = %i\n",
              dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val);

        break;
    }
    break;

  case NR_UL_DCI_FORMAT_0_0:
    switch (rnti_type) {
      case TYPE_C_RNTI_:
        LOG_D(NR_MAC,
              "Filling format 0_0 DCI for CRNTI (size %d bits, format ind %d)\n",
              dci_size,
              dci_pdu_rel15->format_indicator);
        // indicating a UL DCI format 1bit
        pos = 1;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->format_indicator & 1) << (dci_size - pos);
        // Freq domain assignment  max 16 bit
        fsize = dci_pdu_rel15->frequency_domain_assignment.nbits;
        pos += fsize;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val & ((1 << fsize) - 1)) << (dci_size - pos);
        // Time domain assignment 4bit
        pos += 4;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->time_domain_assignment.val & ((1 << 4) - 1)) << (dci_size - pos);
        // Frequency hopping flag – 1 bit
        pos++;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_hopping_flag.val & 1) << (dci_size - pos);
        // MCS  5 bit
        pos += 5;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
        // New data indicator 1bit
        pos++;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi & 1) << (dci_size - pos);
        // Redundancy version  2bit
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->rv & 0x3) << (dci_size - pos);
        // HARQ process number  4bit/5bit
        fsize = dci_pdu_rel15->harq_pid.nbits;
        pos += fsize;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->harq_pid.val & ((1 << fsize) - 1)) << (dci_size - pos);
        // TPC command for scheduled PUSCH – 2 bits
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->tpc & 0x3) << (dci_size - pos);
        // Padding bits
        for (int a = pos; a < dci_size; a++)
          *dci_pdu |= ((uint64_t)dci_pdu_rel15->padding & 1) << (dci_size - pos++);
        // UL/SUL indicator – 1 bit
        /* commented for now (RK): need to get this from BWP descriptor
        if (cfg->pucch_config.pucch_GroupHopping.value)
          *dci_pdu |=
        ((uint64_t)dci_pdu_rel15->ul_sul_indicator.val&1)<<(dci_size-pos++);
          */

        LOG_D(NR_MAC, "N_RB = %i\n", N_RB);
        LOG_D(NR_MAC, "dci_size = %i\n", dci_size);
        LOG_D(NR_MAC, "fsize = %i\n", fsize);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_domain_assignment.val = %i\n", dci_pdu_rel15->frequency_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->time_domain_assignment.val = %i\n", dci_pdu_rel15->time_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_hopping_flag.val = %i\n", dci_pdu_rel15->frequency_hopping_flag.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->mcs = %i\n", dci_pdu_rel15->mcs);
        LOG_D(NR_MAC, "dci_pdu_rel15->ndi = %i\n", dci_pdu_rel15->ndi);
        LOG_D(NR_MAC, "dci_pdu_rel15->rv = %i\n", dci_pdu_rel15->rv);
        LOG_D(NR_MAC, "dci_pdu_rel15->harq_pid = %i\n", dci_pdu_rel15->harq_pid.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->tpc = %i\n", dci_pdu_rel15->tpc);
        LOG_D(NR_MAC, "dci_pdu_rel15->padding = %i\n", dci_pdu_rel15->padding);
        break;

      case TYPE_TC_RNTI_:
        // indicating a UL DCI format 1bit
        pos = 1;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->format_indicator & 1) << (dci_size - pos);
        // Freq domain assignment  max 16 bit
        fsize = dci_pdu_rel15->frequency_domain_assignment.nbits;
        pos += fsize;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val & ((1 << fsize) - 1)) << (dci_size - pos);
        // Time domain assignment 4bit
        pos += 4;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->time_domain_assignment.val & ((1 << 4) - 1)) << (dci_size - pos);
        // Frequency hopping flag – 1 bit
        pos++;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_hopping_flag.val & 1) << (dci_size - pos);
        // MCS  5 bit
        pos += 5;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
        // New data indicator 1bit
        pos++;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi & 1) << (dci_size - pos);
        // Redundancy version  2bit
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->rv & 0x3) << (dci_size - pos);
        // HARQ process number  4bit/5bit
        fsize = dci_pdu_rel15->harq_pid.nbits;
        pos += fsize;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->harq_pid.val & ((1 << fsize) - 1)) << (dci_size - pos);
        // Padding bits
        for (int a = pos; a < dci_size; a++)
          *dci_pdu |= ((uint64_t)dci_pdu_rel15->padding & 1) << (dci_size - pos++);
        // UL/SUL indicator – 1 bit
        /* commented for now (RK): need to get this from BWP descriptor
        if (cfg->pucch_config.pucch_GroupHopping.value)
          *dci_pdu |=
        ((uint64_t)dci_pdu_rel15->ul_sul_indicator.val&1)<<(dci_size-pos++);
          */
        LOG_D(NR_MAC, "N_RB = %i\n", N_RB);
        LOG_D(NR_MAC, "dci_size = %i\n", dci_size);
        LOG_D(NR_MAC, "fsize = %i\n", fsize);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_domain_assignment.val = %i\n", dci_pdu_rel15->frequency_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->time_domain_assignment.val = %i\n", dci_pdu_rel15->time_domain_assignment.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->frequency_hopping_flag.val = %i\n", dci_pdu_rel15->frequency_hopping_flag.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->mcs = %i\n", dci_pdu_rel15->mcs);
        LOG_D(NR_MAC, "dci_pdu_rel15->ndi = %i\n", dci_pdu_rel15->ndi);
        LOG_D(NR_MAC, "dci_pdu_rel15->rv = %i\n", dci_pdu_rel15->rv);
        LOG_D(NR_MAC, "dci_pdu_rel15->harq_pid = %i\n", dci_pdu_rel15->harq_pid.val);
        LOG_D(NR_MAC, "dci_pdu_rel15->tpc = %i\n", dci_pdu_rel15->tpc);
        LOG_D(NR_MAC, "dci_pdu_rel15->padding = %i\n", dci_pdu_rel15->padding);

        break;
    }
    break;

  case NR_UL_DCI_FORMAT_0_1:
    switch (rnti_type) {
      case TYPE_C_RNTI_:
        LOG_D(NR_MAC, "Filling NR_UL_DCI_FORMAT_0_1 size %d format indicator %d\n", dci_size, dci_pdu_rel15->format_indicator);
        // Indicating a DL DCI format 1bit
        pos = 1;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->format_indicator & 0x1) << (dci_size - pos);
        // Carrier indicator
        pos += dci_pdu_rel15->carrier_indicator.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->carrier_indicator.val & ((1 << dci_pdu_rel15->carrier_indicator.nbits) - 1))
                    << (dci_size - pos);
        // UL/SUL Indicator
        pos += dci_pdu_rel15->ul_sul_indicator.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ul_sul_indicator.val & ((1 << dci_pdu_rel15->ul_sul_indicator.nbits) - 1))
                    << (dci_size - pos);
        // BWP indicator
        pos += dci_pdu_rel15->bwp_indicator.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->bwp_indicator.val & ((1 << dci_pdu_rel15->bwp_indicator.nbits) - 1))
                    << (dci_size - pos);
        // Frequency domain resource assignment
        pos += dci_pdu_rel15->frequency_domain_assignment.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val
                     & ((1 << dci_pdu_rel15->frequency_domain_assignment.nbits) - 1))
                    << (dci_size - pos);
        // Time domain resource assignment
        pos += dci_pdu_rel15->time_domain_assignment.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->time_domain_assignment.val & ((1 << dci_pdu_rel15->time_domain_assignment.nbits) - 1))
                    << (dci_size - pos);
        // Frequency hopping
        pos += dci_pdu_rel15->frequency_hopping_flag.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_hopping_flag.val & ((1 << dci_pdu_rel15->frequency_hopping_flag.nbits) - 1))
                    << (dci_size - pos);
        // MCS 5bit
        pos += 5;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
        // New data indicator 1bit
        pos += 1;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi & 0x1) << (dci_size - pos);
        // Redundancy version  2bit
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->rv & 0x3) << (dci_size - pos);
        // HARQ process number  4bit/5bit
        fsize = dci_pdu_rel15->harq_pid.nbits;
        pos += fsize;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->harq_pid.val & ((1 << fsize) - 1)) << (dci_size - pos);
        // 1st Downlink assignment index
        pos += dci_pdu_rel15->dai[0].nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->dai[0].val & ((1 << dci_pdu_rel15->dai[0].nbits) - 1)) << (dci_size - pos);
        // 2nd Downlink assignment index
        pos += dci_pdu_rel15->dai[1].nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->dai[1].val & ((1 << dci_pdu_rel15->dai[1].nbits) - 1)) << (dci_size - pos);
        // TPC command for scheduled PUSCH  2bit
        pos += 2;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->tpc & 0x3) << (dci_size - pos);
        // SRS resource indicator
        pos += dci_pdu_rel15->srs_resource_indicator.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->srs_resource_indicator.val & ((1 << dci_pdu_rel15->srs_resource_indicator.nbits) - 1))
                    << (dci_size - pos);
        // Precoding info and n. of layers
        pos += dci_pdu_rel15->precoding_information.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->precoding_information.val & ((1 << dci_pdu_rel15->precoding_information.nbits) - 1))
                    << (dci_size - pos);
        // Antenna ports
        pos += dci_pdu_rel15->antenna_ports.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->antenna_ports.val & ((1 << dci_pdu_rel15->antenna_ports.nbits) - 1))
                    << (dci_size - pos);
        // SRS request
        pos += dci_pdu_rel15->srs_request.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->srs_request.val & ((1 << dci_pdu_rel15->srs_request.nbits) - 1)) << (dci_size - pos);
        // CSI request
        pos += dci_pdu_rel15->csi_request.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->csi_request.val & ((1 << dci_pdu_rel15->csi_request.nbits) - 1)) << (dci_size - pos);
        // CBG transmission information
        pos += dci_pdu_rel15->cbgti.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->cbgti.val & ((1 << dci_pdu_rel15->cbgti.nbits) - 1)) << (dci_size - pos);
        // PTRS DMRS association
        pos += dci_pdu_rel15->ptrs_dmrs_association.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ptrs_dmrs_association.val & ((1 << dci_pdu_rel15->ptrs_dmrs_association.nbits) - 1))
                    << (dci_size - pos);
        // Beta offset indicator
        pos += dci_pdu_rel15->beta_offset_indicator.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->beta_offset_indicator.val & ((1 << dci_pdu_rel15->beta_offset_indicator.nbits) - 1))
                    << (dci_size - pos);
        // DMRS sequence initialization
        pos += dci_pdu_rel15->dmrs_sequence_initialization.nbits;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->dmrs_sequence_initialization.val
                     & ((1 << dci_pdu_rel15->dmrs_sequence_initialization.nbits) - 1))
                    << (dci_size - pos);
        // UL-SCH indicator
        pos += 1;
        *dci_pdu |= ((uint64_t)dci_pdu_rel15->ulsch_indicator & 0x1) << (dci_size - pos);

#ifdef DEBUG_DCI
        LOG_I(NR_MAC,"============= NR_UL_DCI_FORMAT_0_1 =============\n");
        LOG_I(NR_MAC,"dci_size = %i\n", dci_size);
        LOG_I(NR_MAC,"dci_pdu_rel15->format_indicator = %i\n", dci_pdu_rel15->format_indicator);
        LOG_I(NR_MAC,"dci_pdu_rel15->carrier_indicator.val = %i\n", dci_pdu_rel15->carrier_indicator.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->ul_sul_indicator.val = %i\n", dci_pdu_rel15->ul_sul_indicator.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->bwp_indicator.val = %i\n", dci_pdu_rel15->bwp_indicator.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->frequency_domain_assignment.val = %i\n", dci_pdu_rel15->frequency_domain_assignment.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->time_domain_assignment.val = %i\n", dci_pdu_rel15->time_domain_assignment.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->frequency_hopping_flag.val = %i\n", dci_pdu_rel15->frequency_hopping_flag.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->mcs = %i\n", dci_pdu_rel15->mcs);
        LOG_I(NR_MAC,"dci_pdu_rel15->ndi = %i\n", dci_pdu_rel15->ndi);
        LOG_I(NR_MAC,"dci_pdu_rel15->rv= %i\n", dci_pdu_rel15->rv);
        LOG_I(NR_MAC,"dci_pdu_rel15->harq_pid = %i\n", dci_pdu_rel15->harq_pid);
        LOG_I(NR_MAC,"dci_pdu_rel15->dai[0].val = %i\n", dci_pdu_rel15->dai[0].val);
        LOG_I(NR_MAC,"dci_pdu_rel15->dai[1].val = %i\n", dci_pdu_rel15->dai[1].val);
        LOG_I(NR_MAC,"dci_pdu_rel15->tpc = %i\n", dci_pdu_rel15->tpc);
        LOG_I(NR_MAC,"dci_pdu_rel15->srs_resource_indicator.val = %i\n", dci_pdu_rel15->srs_resource_indicator.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->precoding_information.val = %i\n", dci_pdu_rel15->precoding_information.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->antenna_ports.val = %i\n", dci_pdu_rel15->antenna_ports.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->srs_request.val = %i\n", dci_pdu_rel15->srs_request.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->csi_request.val = %i\n", dci_pdu_rel15->csi_request.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->cbgti.val = %i\n", dci_pdu_rel15->cbgti.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->ptrs_dmrs_association.val = %i\n", dci_pdu_rel15->ptrs_dmrs_association.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->beta_offset_indicator.val = %i\n", dci_pdu_rel15->beta_offset_indicator.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->dmrs_sequence_initialization.val = %i\n", dci_pdu_rel15->dmrs_sequence_initialization.val);
        LOG_I(NR_MAC,"dci_pdu_rel15->ulsch_indicator = %i\n", dci_pdu_rel15->ulsch_indicator);
#endif

        break;
    }
    break;

  case NR_DL_DCI_FORMAT_1_1:
    // Indicating a DL DCI format 1bit
    LOG_D(NR_MAC,"Filling Format 1_1 DCI of size %d\n",dci_size);
    pos = 1;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->format_indicator & 0x1) << (dci_size - pos);
    // Carrier indicator
    pos += dci_pdu_rel15->carrier_indicator.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->carrier_indicator.val & ((1 << dci_pdu_rel15->carrier_indicator.nbits) - 1)) << (dci_size - pos);
    // BWP indicator
    pos += dci_pdu_rel15->bwp_indicator.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->bwp_indicator.val & ((1 << dci_pdu_rel15->bwp_indicator.nbits) - 1)) << (dci_size - pos);
    // Frequency domain resource assignment
    pos += dci_pdu_rel15->frequency_domain_assignment.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->frequency_domain_assignment.val & ((1 << dci_pdu_rel15->frequency_domain_assignment.nbits) - 1)) << (dci_size - pos);
    // Time domain resource assignment
    pos += dci_pdu_rel15->time_domain_assignment.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->time_domain_assignment.val & ((1 << dci_pdu_rel15->time_domain_assignment.nbits) - 1)) << (dci_size - pos);
    // VRB-to-PRB mapping
    pos += dci_pdu_rel15->vrb_to_prb_mapping.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->vrb_to_prb_mapping.val & ((1 << dci_pdu_rel15->vrb_to_prb_mapping.nbits) - 1)) << (dci_size - pos);
    // PRB bundling size indicator
    pos += dci_pdu_rel15->prb_bundling_size_indicator.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->prb_bundling_size_indicator.val & ((1 << dci_pdu_rel15->prb_bundling_size_indicator.nbits) - 1)) << (dci_size - pos);
    // Rate matching indicator
    pos += dci_pdu_rel15->rate_matching_indicator.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->rate_matching_indicator.val & ((1 << dci_pdu_rel15->rate_matching_indicator.nbits) - 1)) << (dci_size - pos);
    // ZP CSI-RS trigger
    pos += dci_pdu_rel15->zp_csi_rs_trigger.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->zp_csi_rs_trigger.val & ((1 << dci_pdu_rel15->zp_csi_rs_trigger.nbits) - 1)) << (dci_size - pos);
    // TB1
    // MCS 5bit
    pos += 5;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs & 0x1f) << (dci_size - pos);
    // New data indicator 1bit
    pos += 1;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi & 0x1) << (dci_size - pos);
    // Redundancy version  2bit
    pos += 2;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->rv & 0x3) << (dci_size - pos);
    // TB2
    // MCS 5bit
    pos += dci_pdu_rel15->mcs2.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->mcs2.val & ((1 << dci_pdu_rel15->mcs2.nbits) - 1)) << (dci_size - pos);
    // New data indicator 1bit
    pos += dci_pdu_rel15->ndi2.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->ndi2.val & ((1 << dci_pdu_rel15->ndi2.nbits) - 1)) << (dci_size - pos);
    // Redundancy version  2bit
    pos += dci_pdu_rel15->rv2.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->rv2.val & ((1 << dci_pdu_rel15->rv2.nbits) - 1)) << (dci_size - pos);
    // HARQ process number  4bit/5bit
    fsize = dci_pdu_rel15->harq_pid.nbits;
    pos += fsize;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->harq_pid.val & ((1 << fsize) - 1)) << (dci_size - pos);
    // Downlink assignment index
    pos += dci_pdu_rel15->dai[0].nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->dai[0].val & ((1 << dci_pdu_rel15->dai[0].nbits) - 1)) << (dci_size - pos);
    // TPC command for scheduled PUCCH  2bit
    pos += 2;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->tpc & 0x3) << (dci_size - pos);
    // PUCCH resource indicator  3bit
    pos += 3;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->pucch_resource_indicator & 0x7) << (dci_size - pos);
    // PDSCH-to-HARQ_feedback timing indicator
    pos += dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.val & ((1 << dci_pdu_rel15->pdsch_to_harq_feedback_timing_indicator.nbits) - 1)) << (dci_size - pos);
    // Antenna ports
    pos += dci_pdu_rel15->antenna_ports.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->antenna_ports.val & ((1 << dci_pdu_rel15->antenna_ports.nbits) - 1)) << (dci_size - pos);
    // TCI
    pos += dci_pdu_rel15->transmission_configuration_indication.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->transmission_configuration_indication.val & ((1 << dci_pdu_rel15->transmission_configuration_indication.nbits) - 1)) << (dci_size - pos);
    // SRS request
    pos += dci_pdu_rel15->srs_request.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->srs_request.val & ((1 << dci_pdu_rel15->srs_request.nbits) - 1)) << (dci_size - pos);
    // CBG transmission information
    pos += dci_pdu_rel15->cbgti.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->cbgti.val & ((1 << dci_pdu_rel15->cbgti.nbits) - 1)) << (dci_size - pos);
    // CBG flushing out information
    pos += dci_pdu_rel15->cbgfi.nbits;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->cbgfi.val & ((1 << dci_pdu_rel15->cbgfi.nbits) - 1)) << (dci_size - pos);
    // DMRS sequence init
    pos += 1;
    *dci_pdu |= ((uint64_t)dci_pdu_rel15->dmrs_sequence_initialization.val & 0x1) << (dci_size - pos);
  }
  LOG_D(NR_MAC, "DCI has %d bits and the payload is %lx\n", dci_size, *dci_pdu);
}

int get_spf(nfapi_nr_config_request_scf_t *cfg) {

  int mu = cfg->ssb_config.scs_common.value;
  AssertFatal(mu>=0&&mu<4,"Illegal scs %d\n",mu);

  return(10 * (1<<mu));
} 

int to_absslot(nfapi_nr_config_request_scf_t *cfg,int frame,int slot) {

  return(get_spf(cfg)*frame) + slot; 

}

int extract_startSymbol(int startSymbolAndLength) {
  int tmp = startSymbolAndLength/14;
  int tmp2 = startSymbolAndLength%14;

  if (tmp > 0 && tmp < (14-tmp2)) return(tmp2);
  else                            return(13-tmp2);
}

int extract_length(int startSymbolAndLength) {
  int tmp = startSymbolAndLength/14;
  int tmp2 = startSymbolAndLength%14;

  if (tmp > 0 && tmp < (14-tmp2)) return(tmp);
  else                            return(15-tmp2);
}

/*
 * Dump the UL or DL UE_info into LOG_T(MAC)
 */
void dump_nr_list(NR_UE_info_t **list)
{
  UE_iterator(list, UE) {
    LOG_T(NR_MAC, "NR list UEs rntis %04x\n", (*list)->rnti);
  }
}

/*
 * Create a new NR_list
 */
void create_nr_list(NR_list_t *list, int len)
{
  list->head = -1;
  list->next = malloc(len * sizeof(*list->next));
  AssertFatal(list->next, "cannot malloc() memory for NR_list_t->next\n");
  for (int i = 0; i < len; ++i)
    list->next[i] = -1;
  list->tail = -1;
  list->len = len;
}

/*
 * Resize an NR_list
 */
void resize_nr_list(NR_list_t *list, int new_len)
{
  if (new_len == list->len)
    return;
  if (new_len > list->len) {
    /* list->head remains */
    const int old_len = list->len;
    int* n = realloc(list->next, new_len * sizeof(*list->next));
    AssertFatal(n, "cannot realloc() memory for NR_list_t->next\n");
    list->next = n;
    for (int i = old_len; i < new_len; ++i)
      list->next[i] = -1;
    /* list->tail remains */
    list->len = new_len;
  } else { /* new_len < len */
    AssertFatal(list->head < new_len, "shortened list head out of index %d (new len %d)\n", list->head, new_len);
    AssertFatal(list->tail < new_len, "shortened list tail out of index %d (new len %d)\n", list->head, new_len);
    for (int i = 0; i < list->len; ++i)
      AssertFatal(list->next[i] < new_len, "shortened list entry out of index %d (new len %d)\n", list->next[i], new_len);
    /* list->head remains */
    int *n = realloc(list->next, new_len * sizeof(*list->next));
    AssertFatal(n, "cannot realloc() memory for NR_list_t->next\n");
    list->next = n;
    /* list->tail remains */
    list->len = new_len;
  }
}

/*
 * Destroy an NR_list
 */
void destroy_nr_list(NR_list_t *list)
{
  free(list->next);
}

/*
 * Add an ID to an NR_list at the end, traversing the whole list. Note:
 * add_tail_nr_list() is a faster alternative, but this implementation ensures
 * we do not add an existing ID.
 */
void add_nr_list(NR_list_t *listP, int id)
{
  int *cur = &listP->head;
  while (*cur >= 0) {
    AssertFatal(*cur != id, "id %d already in NR_UE_list!\n", id);
    cur = &listP->next[*cur];
  }
  *cur = id;
  if (listP->next[id] < 0)
    listP->tail = id;
}

/*
 * Remove an ID from an NR_list
 */
void remove_nr_list(NR_list_t *listP, int id)
{
  int *cur = &listP->head;
  int *prev = &listP->head;
  while (*cur != -1 && *cur != id) {
    prev = cur;
    cur = &listP->next[*cur];
  }
  AssertFatal(*cur != -1, "ID %d not found in UE_list\n", id);
  int *next = &listP->next[*cur];
  *cur = listP->next[*cur];
  *next = -1;
  listP->tail = *prev >= 0 && listP->next[*prev] >= 0 ? listP->tail : *prev;
}

/*
 * Add an ID to the tail of the NR_list in O(1). Note that there is
 * corresponding remove_tail_nr_list(), as we cannot set the tail backwards and
 * therefore need to go through the whole list (use remove_nr_list())
 */
void add_tail_nr_list(NR_list_t *listP, int id)
{
  int *last = listP->tail < 0 ? &listP->head : &listP->next[listP->tail];
  *last = id;
  listP->next[id] = -1;
  listP->tail = id;
}

/*
 * Add an ID to the front of the NR_list in O(1)
 */
void add_front_nr_list(NR_list_t *listP, int id)
{
  const int ohead = listP->head;
  listP->head = id;
  listP->next[id] = ohead;
  if (listP->tail < 0)
    listP->tail = id;
}

/*
 * Remove an ID from the front of the NR_list in O(1)
 */
void remove_front_nr_list(NR_list_t *listP)
{
  AssertFatal(listP->head >= 0, "Nothing to remove\n");
  const int ohead = listP->head;
  listP->head = listP->next[ohead];
  listP->next[ohead] = -1;
  if (listP->head < 0)
    listP->tail = -1;
}

NR_UE_info_t *find_nr_UE(NR_UEs_t *UEs, rnti_t rntiP)
{
  UE_iterator(UEs->connected_ue_list, UE) {
    if (UE->rnti == rntiP) {
      LOG_D(NR_MAC,"Search and found rnti: %04x\n", rntiP);
      return UE;
    }
  }
  return NULL;
}

NR_UE_info_t *find_ra_UE(NR_UEs_t *UEs, rnti_t rntiP)
{
  UE_iterator(UEs->access_ue_list, UE) {
    if (UE->rnti == rntiP) {
      LOG_D(NR_MAC,"Search and found rnti: %04x\n", rntiP);
      return UE;
    }
  }
  return NULL;
}

void delete_nr_ue_data(NR_UE_info_t *UE, NR_COMMON_channels_t *ccPtr, uid_allocator_t *uia)
{
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, UE->CellGroup);
  ASN_STRUCT_FREE(asn_DEF_NR_SpCellConfig, UE->reconfigSpCellConfig);
  ASN_STRUCT_FREE(asn_DEF_NR_UE_NR_Capability, UE->capability);
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  seq_arr_free(&sched_ctrl->lc_config, NULL);
  destroy_nr_list(&sched_ctrl->available_dl_harq);
  destroy_nr_list(&sched_ctrl->feedback_dl_harq);
  destroy_nr_list(&sched_ctrl->retrans_dl_harq);
  destroy_nr_list(&sched_ctrl->available_ul_harq);
  destroy_nr_list(&sched_ctrl->feedback_ul_harq);
  destroy_nr_list(&sched_ctrl->retrans_ul_harq);
  for (int i = 0; i < NR_MAX_HARQ_PROCESSES; ++i)
    free_transportBlock_buffer(&sched_ctrl->harq_processes[i].transportBlock);
  free_sched_pucch_list(sched_ctrl);
  uid_linear_allocator_free(uia, UE->uid);
  LOG_I(NR_MAC, "Remove NR rnti 0x%04x\n", UE->rnti);
  free_and_zero(UE->ra);
  free(UE);
}

#define TB_SINGLE_LAYER (32 * 1024)
uint8_t *allocate_transportBlock_buffer(byte_array_t *tb, uint32_t needed)
{
  DevAssert(needed > 0);
  if (tb->buf != NULL && needed <= tb->len)
    return tb->buf; // nothing to do, current is enough

  uint32_t size = TB_SINGLE_LAYER;
  while (needed > size)
    size *= 2;
  LOG_D(NR_MAC, "allocating new TB block of size %d\n", size);
  free(tb->buf);
  tb->buf = malloc_or_fail(size);
  tb->len = size;
  return tb->buf;
}

void free_transportBlock_buffer(byte_array_t *tb)
{
  free_byte_array(*tb);
}

void set_max_fb_time(NR_UE_UL_BWP_t *UL_BWP, const NR_UE_DL_BWP_t *DL_BWP)
{
  UL_BWP->max_fb_time = 8; // default value
  // take the maximum in dl_DataToUL_ACK list
  if (DL_BWP->dci_format != NR_DL_DCI_FORMAT_1_0 && UL_BWP->pucch_Config) {
    const struct NR_PUCCH_Config__dl_DataToUL_ACK *fb_times = UL_BWP->pucch_Config->dl_DataToUL_ACK;
    for (int i = 0; i < fb_times->list.count; i++) {
      if(*fb_times->list.array[i] > UL_BWP->max_fb_time)
        UL_BWP->max_fb_time = *fb_times->list.array[i];
    }
  }
}

void reset_sched_ctrl(NR_UE_sched_ctrl_t *sched_ctrl)
{
  sched_ctrl->srs_feedback.ul_ri = 0;
  sched_ctrl->srs_feedback.tpmi = 0;
  sched_ctrl->srs_feedback.sri = 0;
}

int get_dlbw_tbslbrm(int scc_bwpsize, const NR_ServingCellConfig_t *servingCellConfig)
{
  int bw = scc_bwpsize;
  if (servingCellConfig) {
    if (servingCellConfig->downlinkBWP_ToAddModList) {
      const struct NR_ServingCellConfig__downlinkBWP_ToAddModList *BWP_list = servingCellConfig->downlinkBWP_ToAddModList;
      for (int i = 0; i < BWP_list->list.count; i++) {
        NR_BWP_t genericParameters = BWP_list->list.array[i]->bwp_Common->genericParameters;
        int curr_bw = NRRIV2BW(genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
        if (curr_bw > bw)
          bw = curr_bw;
      }
    }
  }
  return bw;
}

int get_ulbw_tbslbrm(int scc_bwpsize, const NR_ServingCellConfig_t *servingCellConfig)
{
  int bw = scc_bwpsize;
  if (servingCellConfig) {
    if (servingCellConfig->uplinkConfig && servingCellConfig->uplinkConfig->uplinkBWP_ToAddModList) {
      const struct NR_UplinkConfig__uplinkBWP_ToAddModList *BWP_list = servingCellConfig->uplinkConfig->uplinkBWP_ToAddModList;
      for (int i = 0; i < BWP_list->list.count; i++) {
        NR_BWP_t genericParameters = BWP_list->list.array[i]->bwp_Common->genericParameters;
        int curr_bw = NRRIV2BW(genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
        if (curr_bw > bw)
          bw = curr_bw;
      }
    }
  }
  return bw;
}

static void set_sched_pucch_list(NR_UE_sched_ctrl_t *sched_ctrl,
                                 const NR_UE_UL_BWP_t *ul_bwp,
                                 const NR_ServingCellConfigCommon_t *scc,
                                 const frame_structure_t *fs)
{
  const int NTN_gNB_Koffset = get_NTN_Koffset(scc);
  const int n_ul_slots_period = get_ul_slots_per_period(fs);

  // PUCCH list size is given by the number of UL slots in the PUCCH period
  // the length PUCCH period is determined by max_fb_time since we may need to prepare PUCCH for ACK/NACK max_fb_time slots ahead
  const int list_size = n_ul_slots_period << (int)ceil(log2((ul_bwp->max_fb_time + NTN_gNB_Koffset) / fs->numb_slots_period + 1));

  if (!sched_ctrl->sched_pucch) {
    sched_ctrl->sched_pucch = calloc_or_fail(list_size, sizeof(*sched_ctrl->sched_pucch));
    sched_ctrl->sched_pucch_size = list_size;
  } else if (list_size > sched_ctrl->sched_pucch_size) {
    sched_ctrl->sched_pucch = realloc(sched_ctrl->sched_pucch, list_size * sizeof(*sched_ctrl->sched_pucch));
    for (int i = sched_ctrl->sched_pucch_size; i < list_size; i++) {
      NR_sched_pucch_t *curr_pucch = &sched_ctrl->sched_pucch[i];
      memset(curr_pucch, 0, sizeof(*curr_pucch));
    }
    sched_ctrl->sched_pucch_size = list_size;
  }
}

static void create_dl_harq_list(NR_UE_sched_ctrl_t *sched_ctrl, const NR_UE_ServingCell_Info_t *sc_info, bool dci_00_10)
{
  int nrofHARQ = get_nrofHARQ_ProcessesForPDSCH(sc_info);
  // limit the number of HARQ PIDs to 16 in case of common search space
  if (dci_00_10 && nrofHARQ > 16)
    nrofHARQ = 16;
  // add all available DL HARQ processes for this UE
  AssertFatal(sched_ctrl->available_dl_harq.len == sched_ctrl->feedback_dl_harq.len
              && sched_ctrl->available_dl_harq.len == sched_ctrl->retrans_dl_harq.len,
              "HARQ lists have different lengths (%d/%d/%d)\n",
              sched_ctrl->available_dl_harq.len,
              sched_ctrl->feedback_dl_harq.len,
              sched_ctrl->retrans_dl_harq.len);
  if (sched_ctrl->available_dl_harq.len == 0) {
    create_nr_list(&sched_ctrl->available_dl_harq, nrofHARQ);
    for (int harq = 0; harq < nrofHARQ; harq++)
      add_tail_nr_list(&sched_ctrl->available_dl_harq, harq);
    create_nr_list(&sched_ctrl->feedback_dl_harq, nrofHARQ);
    create_nr_list(&sched_ctrl->retrans_dl_harq, nrofHARQ);
  } else if (sched_ctrl->available_dl_harq.len == nrofHARQ) {
    LOG_D(NR_MAC, "nrofHARQ %d already configured\n", nrofHARQ);
  } else {
    const int old_nrofHARQ = sched_ctrl->available_dl_harq.len;
    AssertFatal(nrofHARQ > old_nrofHARQ,
                "cannot resize HARQ list to be smaller (nrofHARQ %d, old_nrofHARQ %d)\n",
                nrofHARQ, old_nrofHARQ);
    resize_nr_list(&sched_ctrl->available_dl_harq, nrofHARQ);
    for (int harq = old_nrofHARQ; harq < nrofHARQ; harq++)
      add_tail_nr_list(&sched_ctrl->available_dl_harq, harq);
    resize_nr_list(&sched_ctrl->feedback_dl_harq, nrofHARQ);
    resize_nr_list(&sched_ctrl->retrans_dl_harq, nrofHARQ);
  }
}

static void create_ul_harq_list(NR_UE_sched_ctrl_t *sched_ctrl, const NR_UE_ServingCell_Info_t *sc_info, bool dci_00_10)
{
  int nrofHARQ = get_nrofHARQ_ProcessesForPUSCH(sc_info);
  // limit the number of HARQ PIDs to 16 in case of common search space
  if (dci_00_10 && nrofHARQ > 16)
    nrofHARQ = 16;
  if (sched_ctrl->available_ul_harq.len == 0) {
    create_nr_list(&sched_ctrl->available_ul_harq, nrofHARQ);
    for (int harq = 0; harq < nrofHARQ; harq++)
      add_tail_nr_list(&sched_ctrl->available_ul_harq, harq);
    create_nr_list(&sched_ctrl->feedback_ul_harq, nrofHARQ);
    create_nr_list(&sched_ctrl->retrans_ul_harq, nrofHARQ);
  } else if (sched_ctrl->available_ul_harq.len == nrofHARQ) {
    LOG_D(NR_MAC, "nrofHARQ %d already configured\n", nrofHARQ);
  } else {
    const int old_nrofHARQ = sched_ctrl->available_ul_harq.len;
    AssertFatal(nrofHARQ > old_nrofHARQ,
                "cannot resize HARQ list to be smaller (nrofHARQ %d, old_nrofHARQ %d)\n",
                nrofHARQ, old_nrofHARQ);
    resize_nr_list(&sched_ctrl->available_ul_harq, nrofHARQ);
    for (int harq = old_nrofHARQ; harq < nrofHARQ; harq++)
      add_tail_nr_list(&sched_ctrl->available_ul_harq, harq);
    resize_nr_list(&sched_ctrl->feedback_ul_harq, nrofHARQ);
    resize_nr_list(&sched_ctrl->retrans_ul_harq, nrofHARQ);
  }
}

// main function to configure parameters of current BWP
void configure_UE_BWP(gNB_MAC_INST *nr_mac,
                      NR_ServingCellConfigCommon_t *scc,
                      NR_UE_info_t *UE,
                      bool is_RA,
                      int target_ss,
                      int dl_bwp_switch,
                      int ul_bwp_switch)
{
  NR_CellGroupConfig_t *CellGroup = UE->CellGroup;
  NR_UE_ServingCell_Info_t *sc_info = &UE->sc_info;
  NR_UE_DL_BWP_t *DL_BWP = &UE->current_DL_BWP;
  NR_UE_UL_BWP_t *UL_BWP = &UE->current_UL_BWP;

  NR_BWP_Downlink_t *dl_bwp = NULL;
  NR_BWP_Uplink_t *ul_bwp = NULL;
  NR_BWP_DownlinkDedicated_t *bwpd = NULL;
  NR_BWP_UplinkDedicated_t *ubwpd = NULL;
  // number of additional BWPs (excluding initial BWP)
  sc_info->n_dl_bwp = 0;
  sc_info->n_ul_bwp = 0;
  int old_dl_bwp_id = DL_BWP->bwp_id;
  int old_ul_bwp_id = UL_BWP->bwp_id;

  NR_ServingCellConfig_t *servingCellConfig = NULL;
  if (CellGroup && CellGroup->spCellConfig && CellGroup->spCellConfig->spCellConfigDedicated) {

    servingCellConfig  = CellGroup->spCellConfig->spCellConfigDedicated;

    if(dl_bwp_switch >= 0 && ul_bwp_switch >= 0) {
      AssertFatal(dl_bwp_switch == ul_bwp_switch, "Different UL and DL BWP not supported\n");
      DL_BWP->bwp_id = dl_bwp_switch;
      UL_BWP->bwp_id = ul_bwp_switch;
    }
    else {
      // (re)configuring BWP
      // TODO BWP switching not via RRC reconfiguration
      // via RRC if firstActiveXlinkBWP_Id is NULL, MAC stays on the same BWP as before
      if (servingCellConfig->firstActiveDownlinkBWP_Id)
        DL_BWP->bwp_id = *servingCellConfig->firstActiveDownlinkBWP_Id;
      if (servingCellConfig->uplinkConfig->firstActiveUplinkBWP_Id)
        UL_BWP->bwp_id = *servingCellConfig->uplinkConfig->firstActiveUplinkBWP_Id;
    }

    const struct NR_ServingCellConfig__downlinkBWP_ToAddModList *bwpList = servingCellConfig->downlinkBWP_ToAddModList;
    if(bwpList)
      sc_info->n_dl_bwp = bwpList->list.count;
    if (DL_BWP->bwp_id>0) {
      for (int i=0; i<bwpList->list.count; i++) {
        dl_bwp = bwpList->list.array[i];
        if(dl_bwp->bwp_Id == DL_BWP->bwp_id)
          break;
      }
      AssertFatal(dl_bwp!=NULL,"Couldn't find DLBWP corresponding to BWP ID %ld\n",DL_BWP->bwp_id);
    }

    const struct NR_UplinkConfig__uplinkBWP_ToAddModList *ubwpList = servingCellConfig->uplinkConfig->uplinkBWP_ToAddModList;
    if(ubwpList)
      sc_info->n_ul_bwp = ubwpList->list.count;
    if (UL_BWP->bwp_id>0) {
      for (int i=0; i<ubwpList->list.count; i++) {
        ul_bwp = ubwpList->list.array[i];
        if(ul_bwp->bwp_Id == UL_BWP->bwp_id)
          break;
      }
      AssertFatal(ul_bwp!=NULL,"Couldn't find ULBWP corresponding to BWP ID %ld\n",UL_BWP->bwp_id);
    }

    // selection of dedicated BWPs
    if(dl_bwp)
      bwpd = dl_bwp->bwp_Dedicated;
    else
      bwpd = servingCellConfig->initialDownlinkBWP;
    if(ul_bwp)
      ubwpd = ul_bwp->bwp_Dedicated;
    else
      ubwpd = servingCellConfig->uplinkConfig->initialUplinkBWP;

    DL_BWP->pdsch_Config = bwpd->pdsch_Config->choice.setup;
    UL_BWP->configuredGrantConfig = ubwpd->configuredGrantConfig ? ubwpd->configuredGrantConfig->choice.setup : NULL;
    UL_BWP->pusch_Config = ubwpd->pusch_Config->choice.setup;
    UL_BWP->pucch_Config = ubwpd->pucch_Config->choice.setup;
    UL_BWP->srs_Config = ubwpd->srs_Config->choice.setup;
  }
  else {
    DL_BWP->bwp_id = 0;
    UL_BWP->bwp_id = 0;
    DL_BWP->pdsch_Config = NULL;
    UL_BWP->pusch_Config = NULL;
    UL_BWP->pucch_Config = NULL;
    UL_BWP->configuredGrantConfig = NULL;
  }

  if (old_dl_bwp_id != DL_BWP->bwp_id)
    LOG_I(NR_MAC, "Switching to DL-BWP %li\n", DL_BWP->bwp_id);
  if (old_ul_bwp_id != UL_BWP->bwp_id)
    LOG_I(NR_MAC, "Switching to UL-BWP %li\n", UL_BWP->bwp_id);

  // TDA lists
  if (DL_BWP->bwp_id>0)
    DL_BWP->tdaList_Common = dl_bwp->bwp_Common->pdsch_ConfigCommon->choice.setup->pdsch_TimeDomainAllocationList;
  else
    DL_BWP->tdaList_Common = scc->downlinkConfigCommon->initialDownlinkBWP->pdsch_ConfigCommon->choice.setup->pdsch_TimeDomainAllocationList;

  if(UL_BWP->bwp_id>0)
    UL_BWP->tdaList_Common = ul_bwp->bwp_Common->pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList;
  else
    UL_BWP->tdaList_Common = scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList;

  // setting generic parameters
  NR_BWP_t dl_genericParameters = (DL_BWP->bwp_id > 0 && dl_bwp) ?
    dl_bwp->bwp_Common->genericParameters:
    scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters;

  DL_BWP->scs = dl_genericParameters.subcarrierSpacing;
  DL_BWP->cyclicprefix = dl_genericParameters.cyclicPrefix;
  DL_BWP->BWPSize = NRRIV2BW(dl_genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  DL_BWP->BWPStart = NRRIV2PRBOFFSET(dl_genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  sc_info->initial_dl_BWPSize =
      NRRIV2BW(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  sc_info->initial_dl_BWPStart =
      NRRIV2PRBOFFSET(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);

  NR_BWP_t ul_genericParameters = (UL_BWP->bwp_id > 0 && ul_bwp) ?
    ul_bwp->bwp_Common->genericParameters:
    scc->uplinkConfigCommon->initialUplinkBWP->genericParameters;

  UL_BWP->scs = ul_genericParameters.subcarrierSpacing;
  UL_BWP->cyclicprefix = ul_genericParameters.cyclicPrefix;
  UL_BWP->BWPSize = NRRIV2BW(ul_genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  UL_BWP->BWPStart = NRRIV2PRBOFFSET(ul_genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  sc_info->initial_ul_BWPSize =
      NRRIV2BW(scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  sc_info->initial_ul_BWPStart =
      NRRIV2PRBOFFSET(scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);

  sc_info->dl_bw_tbslbrm = get_dlbw_tbslbrm(sc_info->initial_dl_BWPSize, servingCellConfig);
  sc_info->dl_bw_tbslbrm = get_ulbw_tbslbrm(sc_info->initial_ul_BWPSize, servingCellConfig);

  if (UL_BWP->bwp_id > 0) {
    UL_BWP->pucch_ConfigCommon = ul_bwp->bwp_Common->pucch_ConfigCommon ? ul_bwp->bwp_Common->pucch_ConfigCommon->choice.setup : NULL;
    UL_BWP->rach_ConfigCommon = ul_bwp->bwp_Common->rach_ConfigCommon ? ul_bwp->bwp_Common->rach_ConfigCommon->choice.setup : NULL;
  } else {
    UL_BWP->pucch_ConfigCommon = scc->uplinkConfigCommon->initialUplinkBWP->pucch_ConfigCommon->choice.setup;
    UL_BWP->rach_ConfigCommon = scc->uplinkConfigCommon->initialUplinkBWP->rach_ConfigCommon->choice.setup;
  }

  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  // Reset required fields in sched_ctrl (e.g. ul_ri and tpmi)
  reset_sched_ctrl(sched_ctrl);
  if(!is_RA) {
    if (servingCellConfig) {
      if (servingCellConfig->csi_MeasConfig) {
        sc_info->csi_MeasConfig = servingCellConfig->csi_MeasConfig->choice.setup;
        compute_csi_bitlen(sc_info->csi_MeasConfig, UE->csi_report_template);
      }
      if (servingCellConfig->pdsch_ServingCellConfig && servingCellConfig->pdsch_ServingCellConfig->choice.setup) {
        NR_PDSCH_ServingCellConfig_t *pdsch_servingcellconfig = servingCellConfig->pdsch_ServingCellConfig->choice.setup;
        sc_info->nrofHARQ_ProcessesForPDSCH = pdsch_servingcellconfig->nrofHARQ_ProcessesForPDSCH;
        if (pdsch_servingcellconfig->ext1)
          sc_info->maxMIMO_Layers_PDSCH = pdsch_servingcellconfig->ext1->maxMIMO_Layers;
        if (pdsch_servingcellconfig->ext3 &&
            pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17)
          sc_info->downlinkHARQ_FeedbackDisabled_r17 = &pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup;
        if (pdsch_servingcellconfig->codeBlockGroupTransmission
            && pdsch_servingcellconfig->codeBlockGroupTransmission->choice.setup)
          sc_info->pdsch_CGB_Transmission = pdsch_servingcellconfig->codeBlockGroupTransmission->choice.setup;
        if (pdsch_servingcellconfig->ext3)
          sc_info->nrofHARQ_ProcessesForPDSCH_v1700 = pdsch_servingcellconfig->ext3->nrofHARQ_ProcessesForPDSCH_v1700;
      }
      sc_info->crossCarrierSchedulingConfig = servingCellConfig->crossCarrierSchedulingConfig;
      sc_info->supplementaryUplink = servingCellConfig->supplementaryUplink;
      if (servingCellConfig->uplinkConfig) {
        if (servingCellConfig->uplinkConfig->carrierSwitching)
          sc_info->carrierSwitching = servingCellConfig->uplinkConfig->carrierSwitching->choice.setup;
        if (servingCellConfig->uplinkConfig->pusch_ServingCellConfig
            && servingCellConfig->uplinkConfig->pusch_ServingCellConfig->choice.setup) {
          NR_PUSCH_ServingCellConfig_t *pusch_servingcellconfig =
              servingCellConfig->uplinkConfig->pusch_ServingCellConfig->choice.setup;
          if (pusch_servingcellconfig->ext1)
            sc_info->maxMIMO_Layers_PUSCH = pusch_servingcellconfig->ext1->maxMIMO_Layers;
          sc_info->rateMatching_PUSCH = pusch_servingcellconfig->rateMatching;
          if (pusch_servingcellconfig->codeBlockGroupTransmission
              && pusch_servingcellconfig->codeBlockGroupTransmission->choice.setup)
            sc_info->pusch_CGB_Transmission = pusch_servingcellconfig->codeBlockGroupTransmission->choice.setup;
          if (pusch_servingcellconfig->ext3)
            sc_info->nrofHARQ_ProcessesForPUSCH_r17 = pusch_servingcellconfig->ext3->nrofHARQ_ProcessesForPUSCH_r17;
        }
      }
    }

    if (CellGroup && CellGroup->physicalCellGroupConfig)
      UE->pdsch_HARQ_ACK_Codebook = CellGroup->physicalCellGroupConfig->pdsch_HARQ_ACK_Codebook;

    // setting PDCCH related structures for sched_ctrl
    sched_ctrl->search_space = get_searchspace(scc, bwpd, target_ss);
    sched_ctrl->coreset = get_coreset(nr_mac, scc, bwpd, sched_ctrl->search_space, target_ss);

    sched_ctrl->sched_pdcch = set_pdcch_structure(nr_mac,
                                                  sched_ctrl->search_space,
                                                  sched_ctrl->coreset,
                                                  scc,
                                                  &dl_genericParameters,
                                                  nr_mac->type0_PDCCH_CSS_config);

    // set DL DCI format
    DL_BWP->dci_format = (sched_ctrl->search_space->searchSpaceType &&
                         sched_ctrl->search_space->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_ue_Specific) ?
                         (sched_ctrl->search_space->searchSpaceType->choice.ue_Specific->dci_Formats == NR_SearchSpace__searchSpaceType__ue_Specific__dci_Formats_formats0_1_And_1_1 ?
                         NR_DL_DCI_FORMAT_1_1 : NR_DL_DCI_FORMAT_1_0) :
                         NR_DL_DCI_FORMAT_1_0;
    // set UL DCI format
    UL_BWP->dci_format = (sched_ctrl->search_space->searchSpaceType &&
                         sched_ctrl->search_space->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_ue_Specific) ?
                         (sched_ctrl->search_space->searchSpaceType->choice.ue_Specific->dci_Formats == NR_SearchSpace__searchSpaceType__ue_Specific__dci_Formats_formats0_1_And_1_1 ?
                         NR_UL_DCI_FORMAT_0_1 : NR_UL_DCI_FORMAT_0_0) :
                         NR_UL_DCI_FORMAT_0_0;

  } else {
    // setting PDCCH related structures for RA
    struct NR_PDCCH_ConfigCommon__commonSearchSpaceList *commonSearchSpaceList = NULL;
    NR_SearchSpaceId_t ra_SearchSpace = 0;
    if(dl_bwp) {
      commonSearchSpaceList = dl_bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList;
      ra_SearchSpace = *dl_bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ra_SearchSpace;
    } else {
      commonSearchSpaceList = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList;
      ra_SearchSpace = *scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->ra_SearchSpace;
    }
    AssertFatal(commonSearchSpaceList->list.count > 0, "common SearchSpace list has 0 elements\n");
    for (int i = 0; i < commonSearchSpaceList->list.count; i++) {
      NR_SearchSpace_t *ss = commonSearchSpaceList->list.array[i];
      if (ss->searchSpaceId == ra_SearchSpace)
        sched_ctrl->search_space = ss;
    }
    AssertFatal(sched_ctrl->search_space != NULL, "SearchSpace cannot be null for RA\n");

    sched_ctrl->coreset = get_coreset(nr_mac, scc, dl_bwp, sched_ctrl->search_space, NR_SearchSpace__searchSpaceType_PR_common);
    NR_COMMON_channels_t *cc = &nr_mac->common_channels[0];
    int ssb_index = cc->ssb_index[UE->UE_beam_index];
    sched_ctrl->sched_pdcch = set_pdcch_structure(nr_mac,
                                                  sched_ctrl->search_space,
                                                  sched_ctrl->coreset,
                                                  scc,
                                                  &dl_genericParameters,
                                                  &nr_mac->type0_PDCCH_CSS_config[ssb_index]);

    UL_BWP->dci_format = NR_UL_DCI_FORMAT_0_0;
    DL_BWP->dci_format = NR_DL_DCI_FORMAT_1_0;
  }

  bool format_00_10 = DL_BWP->dci_format == NR_DL_DCI_FORMAT_1_0; // if DL is 10 UL is surely 00
  create_dl_harq_list(sched_ctrl, sc_info, format_00_10);
  create_ul_harq_list(sched_ctrl, sc_info, format_00_10);

  set_max_fb_time(UL_BWP, DL_BWP);
  set_sched_pucch_list(sched_ctrl, UL_BWP, scc, &nr_mac->frame_structure);

  // Set MCS tables
  long *dl_mcs_Table = DL_BWP->pdsch_Config ? DL_BWP->pdsch_Config->mcs_Table : NULL;
  DL_BWP->mcsTableIdx = get_pdsch_mcs_table(dl_mcs_Table, DL_BWP->dci_format, TYPE_C_RNTI_, target_ss);

  // 0 precoding enabled 1 precoding disabled
  UL_BWP->transform_precoding = get_transformPrecoding(UL_BWP, UL_BWP->dci_format, 0);
  // Set uplink MCS table
  long *mcs_Table = NULL;
  if (UL_BWP->pusch_Config)
    mcs_Table = UL_BWP->transform_precoding ? UL_BWP->pusch_Config->mcs_Table : UL_BWP->pusch_Config->mcs_TableTransformPrecoder;

  UL_BWP->mcs_table = get_pusch_mcs_table(mcs_Table, !UL_BWP->transform_precoding, UL_BWP->dci_format, TYPE_C_RNTI_, target_ss, false);
}

void reset_srs_stats(NR_UE_info_t *UE) {
  if (UE) {
    UE->mac_stats.srs_stats[0] = '\0';
  }
}

static void init_bler_stats(const NR_bler_options_t *bler_options, NR_bler_stats_t *bler_stats, frame_t frame)
{
  bler_stats->last_frame = frame;
  bler_stats->mcs = bler_options->min_mcs;
  bler_stats->bler = (float)(bler_options->lower + bler_options->upper) / 2.0f;
}

/* @brief returns a new UE allocated instance.
 *
 * It will be typically added to the access_ue_list, but not always (e.g.,
 * phytest mode), so this is not done in this function (and also, to allow
 * error handling). Remove with delete_nr_ue_data().  */
NR_UE_info_t *get_new_nr_ue_inst(uid_allocator_t *uia, rnti_t rnti, NR_CellGroupConfig_t *CellGroup)
{
  uid_t uid = uid_linear_allocator_new(uia);
  /* if the UE list is full, we should reject the UE with an RRC reject
   * message, but we do not have this functionality. To keep it simple, do not
   * create a UE context here, so we can print an error message. */
  if (uid >= MAX_MOBILES_PER_GNB) {
    uid_linear_allocator_free(uia, uid);
    return NULL;
  }

  NR_UE_info_t *UE = calloc_or_fail(1, sizeof(NR_UE_info_t));
  UE->rnti = rnti;
  UE->CellGroup = CellGroup;
  UE->uid = uid;
  UE->ra = calloc(1, sizeof(*UE->ra));
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  sched_ctrl->ta_update = 31;

  /* set illegal time domain allocation to force recomputation of all fields */
  sched_ctrl->sched_pdsch.time_domain_allocation = -1;
  sched_ctrl->sched_pusch.time_domain_allocation = -1;

  /* Set default BWPs */
  AssertFatal(UE->sc_info.n_ul_bwp <= NR_MAX_NUM_BWP, "uplinkBWP_ToAddModList has %d BWP!\n", UE->sc_info.n_ul_bwp);

  // initialize LCID structure
  seq_arr_init(&sched_ctrl->lc_config, sizeof(nr_lc_config_t));
  return UE;
}

bool add_UE_to_list(int list_size, NR_UE_info_t *list[list_size], NR_UE_info_t *UE)
{
  for (int i = 0; i < list_size; i++) {
    if (!list[i]) {
      list[i] = UE;
      return true;
    }
  }
  return false;
}

NR_UE_info_t *remove_UE_from_list(int list_size, NR_UE_info_t *list[list_size], rnti_t rnti)
{
  for (int i = 0; i < list_size; i++) {
    NR_UE_info_t *curr_UE = list[i];
    if (curr_UE->rnti != rnti)
      continue;

    /* remove this UE from the list, return the pointer */
    memmove(&list[i], &list[i+1], sizeof(list[0]) * (list_size - i - 1));
    list[list_size - 1] = NULL;
    return curr_UE;
  }
  LOG_E(NR_MAC, "UE %04x to be removed not found in list\n", rnti);
  return NULL;
}

/** @brief Transitions a UE from access list to connected list (i.e., the RA
 * list to the "normal" UE context list. */
bool transition_ra_connected_nr_ue(gNB_MAC_INST *nr_mac, NR_UE_info_t *UE)
{
  NR_UEs_t *UE_info = &nr_mac->UE_info;

  // remove UE from initial access list (moved to connected mode)
  NR_UE_info_t *r = remove_UE_from_list(NR_NB_RA_PROC_MAX, UE_info->access_ue_list, UE->rnti);
  DevAssert(r == UE); /* sanity check: we should have removed the current UE ptr from list */

  free_and_zero(UE->ra);

  return add_connected_nr_ue(nr_mac, UE);
}

/** @brief Add a UE to the list of UEs in * connected mode.
 *
 * To remove the UE, use mac_remove_nr_ue(). */
bool add_connected_nr_ue(gNB_MAC_INST *nr_mac, NR_UE_info_t *UE)
{
  LOG_I(NR_MAC, "Adding new UE context with RNTI 0x%04x\n", UE->rnti);
  NR_UEs_t *UE_info = &nr_mac->UE_info;
  dump_nr_list(UE_info->connected_ue_list);
  AssertFatal(!UE->ra, "UE in connected cannot have RA process\n");

  NR_SCHED_LOCK(&UE_info->mutex);

  bool success = add_UE_to_list(MAX_MOBILES_PER_GNB, UE_info->connected_ue_list, UE);
  if (!success) {
    LOG_E(NR_MAC,"Try to add UE %04x but the list is full\n", UE->rnti);
    delete_nr_ue_data(UE, NULL, &UE_info->uid_allocator);
    NR_SCHED_UNLOCK(&UE_info->mutex);
    return false;
  }

  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  sched_ctrl->dl_max_mcs = 28; /* do not limit MCS for individual UEs */
  sched_ctrl->sched_srs.frame = -1;
  sched_ctrl->sched_srs.slot = -1;
  sched_ctrl->pdcch_cl_adjust = 0;
  reset_srs_stats(UE);

  // Initialize bler_stats
  init_bler_stats(&nr_mac->dl_bler, &sched_ctrl->dl_bler_stats, nr_mac->frame);
  init_bler_stats(&nr_mac->ul_bler, &sched_ctrl->ul_bler_stats, nr_mac->frame);

  NR_SCHED_UNLOCK(&UE_info->mutex);
  dump_nr_list(UE_info->connected_ue_list);
  return true;
}

void free_sched_pucch_list(NR_UE_sched_ctrl_t *sched_ctrl)
{
  free(sched_ctrl->sched_pucch);
}

void reset_dl_harq_list(NR_UE_sched_ctrl_t *sched_ctrl) {
  int harq;
  while ((harq = sched_ctrl->feedback_dl_harq.head) >= 0) {
    remove_front_nr_list(&sched_ctrl->feedback_dl_harq);
    add_tail_nr_list(&sched_ctrl->available_dl_harq, harq);
  }

  while ((harq = sched_ctrl->retrans_dl_harq.head) >= 0) {
    remove_front_nr_list(&sched_ctrl->retrans_dl_harq);
    add_tail_nr_list(&sched_ctrl->available_dl_harq, harq);
  }

  for (int i = 0; i < NR_MAX_HARQ_PROCESSES; i++) {
    sched_ctrl->harq_processes[i].feedback_slot = -1;
    sched_ctrl->harq_processes[i].round = 0;
    sched_ctrl->harq_processes[i].is_waiting = false;
  }
}

void reset_ul_harq_list(NR_UE_sched_ctrl_t *sched_ctrl) {
  int harq;
  while ((harq = sched_ctrl->feedback_ul_harq.head) >= 0) {
    remove_front_nr_list(&sched_ctrl->feedback_ul_harq);
    add_tail_nr_list(&sched_ctrl->available_ul_harq, harq);
  }

  while ((harq = sched_ctrl->retrans_ul_harq.head) >= 0) {
    remove_front_nr_list(&sched_ctrl->retrans_ul_harq);
    add_tail_nr_list(&sched_ctrl->available_ul_harq, harq);
  }

  for (int i = 0; i < NR_MAX_HARQ_PROCESSES; i++) {
    sched_ctrl->ul_harq_processes[i].feedback_slot = -1;
    sched_ctrl->ul_harq_processes[i].round = 0;
    sched_ctrl->ul_harq_processes[i].is_waiting = false;
  }
}

void mac_remove_nr_ue(gNB_MAC_INST *nr_mac, rnti_t rnti)
{
  /* already mutex protected */
  NR_SCHED_ENSURE_LOCKED(&nr_mac->sched_lock);
  NR_UEs_t *UE_info = &nr_mac->UE_info;
  NR_SCHED_LOCK(&UE_info->mutex);
  NR_UE_info_t *UE = remove_UE_from_list(MAX_MOBILES_PER_GNB + 1, UE_info->connected_ue_list, rnti);
  NR_SCHED_UNLOCK(&UE_info->mutex);

  delete_nr_ue_data(UE, nr_mac->common_channels, &UE_info->uid_allocator);
}

// all values passed to this function are in dB x10
uint8_t nr_get_tpc(int target, uint8_t cqi, int incr, int tx_power)
{
  // al values passed to this function are x10
  int snrx10 = (cqi * 5) - 640 - (tx_power * 10);
  LOG_D(NR_MAC, "tpc : target %d, cqi %d, snrx10 %d, tx_power %d\n", target, ((int)cqi * 5) - 640, snrx10, tx_power);
  if (snrx10 > target + incr) return 0; // decrease 1dB
  if (snrx10 < target - (3*incr)) return 3; // increase 3dB
  if (snrx10 < target - incr) return 2; // increase 1dB
  LOG_D(NR_MAC,"tpc : target %d, snrx10 %d\n",target,snrx10);
  return 1; // no change
}

/**
 * @brief Limits the power control commands (TPC) in NR by checking the RSSI threshold.
 *
 * This function evaluates the received signal strength indicator (RSSI) and adjusts the
 * transmit power control (TPC) commands accordingly to ensure they remain within acceptable
 * limits. It helps in maintaining the signal quality and preventing excessive power usage.
 *
 * @param rssi The received signal strength indicator value, as defined in FAPI specifications.
 * @param tpc The transmit power control command to be limited.
 * @param rssi_threshold RSSI threshold in 0.1 dBm/dBFS, range -1280 to 0
 * @return The adjusted TPC command after applying the RSSI threshold check.
 */
uint8_t nr_limit_tpc(int tpc, int rssi, int rssi_threshold)
{
  if (rssi == 0xFFFF) {
    // RSSI not available, keep tpc
    return tpc;
  }
  // Convert RSSI threshold to FAPI scale
  const int fapi_rssi_0dBm_or_0dBFS = 1280;
  int rssi_fapi_threshold = fapi_rssi_0dBm_or_0dBFS + rssi_threshold;
  // Further limit TPC if above or near RSSI threshold
  int tpc_to_db[] = {-1, 0, 1, 3};
  if (rssi > rssi_fapi_threshold) {
    // RSSI above theshold, reduce power
    return 0;
  } else if (rssi + tpc_to_db[tpc] * 10 > rssi_fapi_threshold) {
    // Cannot apply required TPC, check 1 dB increment
    if (rssi + 10 > rssi_fapi_threshold) {
      // Still cannot apply required TPC, keep power
      return 1;
    } else {
      // Can apply 1dB increment, but 3 was requested
      return 2;
    }
  }
  return tpc;
}

int get_pdsch_to_harq_feedback(NR_PUCCH_Config_t *pucch_Config,
                                nr_dci_format_t dci_format,
                                uint8_t *pdsch_to_harq_feedback) {
  /* already mutex protected: held in nr_acknack_scheduling() */

  if (dci_format == NR_DL_DCI_FORMAT_1_0) {
    for (int i = 0; i < 8; i++)
      pdsch_to_harq_feedback[i] = i + 1;
    return 8;
  }
  else {
    AssertFatal(pucch_Config != NULL && pucch_Config->dl_DataToUL_ACK != NULL,"dl_DataToUL_ACK shouldn't be null here\n");
    for (int i = 0; i < pucch_Config->dl_DataToUL_ACK->list.count; i++) {
      pdsch_to_harq_feedback[i] = *pucch_Config->dl_DataToUL_ACK->list.array[i];
    }
    return pucch_Config->dl_DataToUL_ACK->list.count;
  }
}

void nr_csirs_scheduling(int Mod_idP, frame_t frame, slot_t slot, nfapi_nr_dl_tti_request_t *DL_req)
{
  int CC_id = 0;
  NR_UEs_t *UE_info = &RC.nrmac[Mod_idP]->UE_info;
  gNB_MAC_INST *gNB_mac = RC.nrmac[Mod_idP];
  int n_slots_frame = gNB_mac->frame_structure.numb_slots_frame;
  NR_SCHED_ENSURE_LOCKED(&gNB_mac->sched_lock);

  UE_info->sched_csirs = 0;

  UE_iterator(UE_info->connected_ue_list, UE) {
    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;

    // CSI-RS is common to all UEs in a given BWP
    // therefore we need to schedule only once per BWP
    // the following condition verifies if CSI-RS
    // has been already scheduled in this BWP
    if (UE_info->sched_csirs & (1 << dl_bwp->bwp_id))
      continue;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    if (nr_timer_is_active(&sched_ctrl->transm_interrupt))
      continue;

    if (!UE->sc_info.csi_MeasConfig)
      continue;

    NR_CSI_MeasConfig_t *csi_measconfig = UE->sc_info.csi_MeasConfig;

    // looking for the correct CSI-RS resource in current BWP
    NR_NZP_CSI_RS_ResourceSetId_t *nzp = NULL;
    for (int csi_list=0; csi_list<csi_measconfig->csi_ResourceConfigToAddModList->list.count; csi_list++) {
      NR_CSI_ResourceConfig_t *csires = csi_measconfig->csi_ResourceConfigToAddModList->list.array[csi_list];
      if(csires->bwp_Id == dl_bwp->bwp_id &&
         csires->csi_RS_ResourceSetList.present == NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB &&
         csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList) {
        nzp = csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList->list.array[0];
      }
    }

    if (csi_measconfig->nzp_CSI_RS_ResourceToAddModList != NULL && nzp != NULL) {

      NR_NZP_CSI_RS_Resource_t *nzpcsi;
      int period, offset;

      nfapi_nr_dl_tti_request_body_t *dl_req = &DL_req->dl_tti_request_body;

      for (int id = 0; id < csi_measconfig->nzp_CSI_RS_ResourceToAddModList->list.count; id++){
        nzpcsi = csi_measconfig->nzp_CSI_RS_ResourceToAddModList->list.array[id];
        // transmitting CSI-RS only for current BWP
        if (nzpcsi->nzp_CSI_RS_ResourceId != *nzp)
          continue;

        NR_CSI_RS_ResourceMapping_t  resourceMapping = nzpcsi->resourceMapping;
        csi_period_offset(NULL, nzpcsi->periodicityAndOffset, &period, &offset);

        if((frame * n_slots_frame + slot - offset) % period == 0) {

          LOG_D(NR_MAC,"Scheduling CSI-RS in frame %d slot %d Resource ID %ld\n", frame, slot, nzpcsi->nzp_CSI_RS_ResourceId);
          NR_beam_alloc_t beam_csi = beam_allocation_procedure(&gNB_mac->beam_info, frame, slot, UE->UE_beam_index, n_slots_frame);
          AssertFatal(beam_csi.idx >= 0, "Cannot allocate CSI-RS in any available beam\n");
          uint16_t *vrb_map = gNB_mac->common_channels[CC_id].vrb_map[beam_csi.idx];
          UE_info->sched_csirs |= (1 << dl_bwp->bwp_id);

          nfapi_nr_dl_tti_request_pdu_t *dl_tti_csirs_pdu = &dl_req->dl_tti_pdu_list[dl_req->nPDUs];
          memset((void*)dl_tti_csirs_pdu,0,sizeof(nfapi_nr_dl_tti_request_pdu_t));
          dl_tti_csirs_pdu->PDUType = NFAPI_NR_DL_TTI_CSI_RS_PDU_TYPE;
          dl_tti_csirs_pdu->PDUSize = (uint8_t)(2+sizeof(nfapi_nr_dl_tti_csi_rs_pdu));

          nfapi_nr_dl_tti_csi_rs_pdu_rel15_t *csirs_pdu_rel15 = &dl_tti_csirs_pdu->csi_rs_pdu.csi_rs_pdu_rel15;
          csirs_pdu_rel15->precodingAndBeamforming.num_prgs = 1;
          csirs_pdu_rel15->precodingAndBeamforming.prg_size = resourceMapping.freqBand.nrofRBs; //1 PRG of max size
          csirs_pdu_rel15->precodingAndBeamforming.dig_bf_interfaces = 1;
          csirs_pdu_rel15->precodingAndBeamforming.prgs_list[0].pm_idx = 0;
          csirs_pdu_rel15->precodingAndBeamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = UE->UE_beam_index;
          csirs_pdu_rel15->bwp_size = dl_bwp->BWPSize;
          csirs_pdu_rel15->bwp_start = dl_bwp->BWPStart;
          csirs_pdu_rel15->subcarrier_spacing = dl_bwp->scs;
          if (dl_bwp->cyclicprefix)
            csirs_pdu_rel15->cyclic_prefix = *dl_bwp->cyclicprefix;
          else
            csirs_pdu_rel15->cyclic_prefix = 0;

          // According to last paragraph of TS 38.214 5.2.2.3.1
          if (resourceMapping.freqBand.startingRB < dl_bwp->BWPStart) {
            csirs_pdu_rel15->start_rb = dl_bwp->BWPStart;
          } else {
            csirs_pdu_rel15->start_rb = resourceMapping.freqBand.startingRB;
          }
          if (resourceMapping.freqBand.nrofRBs > (dl_bwp->BWPStart + dl_bwp->BWPSize - csirs_pdu_rel15->start_rb)) {
            csirs_pdu_rel15->nr_of_rbs = dl_bwp->BWPStart + dl_bwp->BWPSize - csirs_pdu_rel15->start_rb;
          } else {
            csirs_pdu_rel15->nr_of_rbs = resourceMapping.freqBand.nrofRBs;
          }
          AssertFatal(csirs_pdu_rel15->nr_of_rbs >= 24, "CSI-RS has %d RBs, but the minimum is 24\n", csirs_pdu_rel15->nr_of_rbs);

          csirs_pdu_rel15->csi_type = 1; // NZP-CSI-RS
          csirs_pdu_rel15->symb_l0 = resourceMapping.firstOFDMSymbolInTimeDomain;
          if (resourceMapping.firstOFDMSymbolInTimeDomain2)
            csirs_pdu_rel15->symb_l1 = *resourceMapping.firstOFDMSymbolInTimeDomain2;
          csirs_pdu_rel15->cdm_type = resourceMapping.cdm_Type;
          csirs_pdu_rel15->freq_density = resourceMapping.density.present;
          if ((resourceMapping.density.present == NR_CSI_RS_ResourceMapping__density_PR_dot5)
              && (resourceMapping.density.choice.dot5 == NR_CSI_RS_ResourceMapping__density__dot5_evenPRBs))
            csirs_pdu_rel15->freq_density--;
          csirs_pdu_rel15->scramb_id = nzpcsi->scramblingID;
          csirs_pdu_rel15->power_control_offset = nzpcsi->powerControlOffset + 8;
          if (nzpcsi->powerControlOffsetSS)
            csirs_pdu_rel15->power_control_offset_ss = *nzpcsi->powerControlOffsetSS;
          else
            csirs_pdu_rel15->power_control_offset_ss = 1; // 0 dB
          switch(resourceMapping.frequencyDomainAllocation.present){
            case NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_row1:
              csirs_pdu_rel15->row = 1;
              csirs_pdu_rel15->freq_domain = ((resourceMapping.frequencyDomainAllocation.choice.row1.buf[0])>>4)&0x0f;
              for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
              break;
            case NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_row2:
              csirs_pdu_rel15->row = 2;
              csirs_pdu_rel15->freq_domain = (((resourceMapping.frequencyDomainAllocation.choice.row2.buf[1]>>4)&0x0f) |
                                             ((resourceMapping.frequencyDomainAllocation.choice.row2.buf[0]<<4)&0xff0));
              for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
              break;
            case NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_row4:
              csirs_pdu_rel15->row = 4;
              csirs_pdu_rel15->freq_domain = ((resourceMapping.frequencyDomainAllocation.choice.row4.buf[0])>>5)&0x07;
              for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
              break;
            case NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_other:
              csirs_pdu_rel15->freq_domain = ((resourceMapping.frequencyDomainAllocation.choice.other.buf[0])>>2)&0x3f;
              // determining the row of table 7.4.1.5.3-1 in 38.211
              switch(resourceMapping.nrofPorts){
                case NR_CSI_RS_ResourceMapping__nrofPorts_p1:
                  AssertFatal(1==0,"Resource with 1 CSI port shouldn't be within other rows\n");
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p2:
                  csirs_pdu_rel15->row = 3;
                  for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                    vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p4:
                  csirs_pdu_rel15->row = 5;
                  for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                    vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2);
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p8:
                  if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm4_FD2_TD2) {
                    csirs_pdu_rel15->row = 8;
                    for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                      vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2);
                  }
                  else{
                    int num_k = 0;
                    for (int k=0; k<6; k++)
                      num_k+=(((csirs_pdu_rel15->freq_domain)>>k)&0x01);
                    if(num_k==4) {
                      csirs_pdu_rel15->row = 6;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
                    }
                    else {
                      csirs_pdu_rel15->row = 7;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2);
                    }
                  }
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p12:
                  if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm4_FD2_TD2) {
                    csirs_pdu_rel15->row = 10;
                    for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                      vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2);
                  }
                  else {
                    csirs_pdu_rel15->row = 9;
                    for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                      vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 1);
                  }
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p16:
                  if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm4_FD2_TD2)
                    csirs_pdu_rel15->row = 12;
                  else
                    csirs_pdu_rel15->row = 11;
                  for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                    vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2);
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p24:
                  if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm4_FD2_TD2) {
                    csirs_pdu_rel15->row = 14;
                    for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                      vrb_map[rb] |= (SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2) | SL_to_bitmap(csirs_pdu_rel15->symb_l1, 2));
                  }
                  else{
                    if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm8_FD2_TD4) {
                      csirs_pdu_rel15->row = 15;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 3);
                    }
                    else {
                      csirs_pdu_rel15->row = 13;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= (SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2) | SL_to_bitmap(csirs_pdu_rel15->symb_l1, 2));
                    }
                  }
                  break;
                case NR_CSI_RS_ResourceMapping__nrofPorts_p32:
                  if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm4_FD2_TD2) {
                    csirs_pdu_rel15->row = 17;
                    for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                      vrb_map[rb] |= (SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2) | SL_to_bitmap(csirs_pdu_rel15->symb_l1, 2));
                  }
                  else{
                    if (resourceMapping.cdm_Type == NR_CSI_RS_ResourceMapping__cdm_Type_cdm8_FD2_TD4) {
                      csirs_pdu_rel15->row = 18;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= SL_to_bitmap(csirs_pdu_rel15->symb_l0, 3);
                    }
                    else {
                      csirs_pdu_rel15->row = 16;
                      for (int rb = csirs_pdu_rel15->start_rb; rb < (csirs_pdu_rel15->start_rb + csirs_pdu_rel15->nr_of_rbs); rb++)
                        vrb_map[rb] |= (SL_to_bitmap(csirs_pdu_rel15->symb_l0, 2) | SL_to_bitmap(csirs_pdu_rel15->symb_l1, 2));
                    }
                  }
                  break;
              default:
                AssertFatal(1==0,"Invalid number of ports in CSI-RS resource\n");
              }
              break;
          default:
            AssertFatal(1==0,"Invalid freqency domain allocation in CSI-RS resource\n");
          }
          dl_req->nPDUs++;
        }
      }
    }
  }
}

void nr_mac_clean_cellgroup(NR_CellGroupConfig_t *cell_group)
{
  DevAssert(cell_group != NULL);
  /* remove a reconfigurationWithSync, we don't need it anymore */
  if (cell_group->spCellConfig && cell_group->spCellConfig->reconfigurationWithSync != NULL) {
    ASN_STRUCT_FREE(asn_DEF_NR_ReconfigurationWithSync, cell_group->spCellConfig->reconfigurationWithSync);
    cell_group->spCellConfig->reconfigurationWithSync = NULL;
  }
  /* remove the rlc_BearerToReleaseList, we don't need it anymore */
  if (cell_group->rlc_BearerToReleaseList != NULL) {
    struct NR_CellGroupConfig__rlc_BearerToReleaseList *l = cell_group->rlc_BearerToReleaseList;
    asn_sequence_del(&l->list, l->list.count, /* free */ 1);
    free(cell_group->rlc_BearerToReleaseList);
    cell_group->rlc_BearerToReleaseList = NULL;
  }
  /* remove reestablishRLC, we don't need it anymore */
  for (int i = 0; i < cell_group->rlc_BearerToAddModList->list.count; ++i)
    free_and_zero(cell_group->rlc_BearerToAddModList->list.array[i]->reestablishRLC);
}

int nr_mac_get_reconfig_delay_slots(NR_SubcarrierSpacing_t scs)
{
  /* we previously assumed a specific "slot ahead" value for the PHY processing
   * time. However, we cannot always know it (e.g., third-party PHY), so simply
   * assume a tentative worst-case slot processing time */
  const int sl_ahead = 10;
  /* 16ms seems to be the most common: See 38.331 Tab 12.1-1 */
  int delay_ms = NR_RRC_RECONFIGURATION_DELAY_MS + NR_RRC_BWP_SWITCHING_DELAY_MS;
  return (delay_ms << scs) + sl_ahead;
}

int nr_mac_interrupt_ue_transmission(gNB_MAC_INST *mac, NR_UE_info_t *UE, interrupt_followup_action_t action, int slots)
{
  DevAssert(mac != NULL);
  DevAssert(UE != NULL);
  NR_SCHED_ENSURE_LOCKED(&mac->sched_lock);

  nr_timer_setup(&UE->UE_sched_ctrl.transm_interrupt, slots, 1);
  nr_timer_start(&UE->UE_sched_ctrl.transm_interrupt);
  UE->interrupt_action = action;

  // it might happen that timing advance command should be sent during the UE
  // inactivity time. To prevent this, set a variable as if we would have just
  // sent it. This way, another TA command will for sure be sent in some
  // frames, after the inactivity of the UE.
  UE->UE_sched_ctrl.ta_frame = (mac->frame - 1 + 1024) % 1024;

  LOG_D(NR_MAC, "UE %04x: Interrupt UE transmission (%d slots) action %d\n", UE->rnti, slots, UE->interrupt_action);
  return 0;
}

int nr_transmission_action_indicator_stop(gNB_MAC_INST *mac, NR_UE_info_t *UE_info)
{
  int delay = nr_mac_get_reconfig_delay_slots(UE_info->current_DL_BWP.scs);
  nr_mac_interrupt_ue_transmission(mac, UE_info, FOLLOW_OUTOFSYNC, delay);
  LOG_I(NR_MAC, "gNB-DU received the TransmissionActionIndicator with Stop value for UE %04x\n", UE_info->rnti);
  return 0;
}

void nr_mac_trigger_release_complete(gNB_MAC_INST *mac, int rnti)
{
  // the CU might not know such UE, e.g., because we never sent a message to
  // it, so there might not be a corresponding entry for such UE in the look up
  // table. This can happen, e.g., on Msg.3 with C-RNTI, where we create a UE
  // MAC context, decode the PDU, find the C-RNTI MAC CE, and then throw the
  // newly created context away. See also in _nr_rx_sdu() and commit 93f59a3c6e56f
  if (!du_exists_f1_ue_data(rnti)) 
    return;

  // unlock the scheduler temporarily to prevent possible deadlocks with
  // du_remove_f1_ue_data() (and also while sending the message to RRC)
  NR_SCHED_UNLOCK(&mac->sched_lock);
  f1_ue_data_t ue_data = du_get_f1_ue_data(rnti);
  f1ap_ue_context_release_complete_t complete = {
    .gNB_CU_ue_id = ue_data.secondary_ue,
    .gNB_DU_ue_id = rnti,
  };
  mac->mac_rrc.ue_context_release_complete(&complete);

  du_remove_f1_ue_data(rnti);
  NR_SCHED_LOCK(&mac->sched_lock);
}

void nr_mac_release_ue(gNB_MAC_INST *mac, int rnti)
{
  NR_SCHED_ENSURE_LOCKED(&mac->sched_lock);

  nr_rlc_remove_ue(rnti);
  mac_remove_nr_ue(mac, rnti);
}

void nr_mac_update_timers(module_id_t module_id, frame_t frame, slot_t slot)
{
  gNB_MAC_INST *mac = RC.nrmac[module_id];

  /* already mutex protected: held in gNB_dlsch_ulsch_scheduler() */
  NR_SCHED_ENSURE_LOCKED(&mac->sched_lock);

  NR_UEs_t *UE_info = &mac->UE_info;
  UE_iterator(UE_info->connected_ue_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

    if (nr_mac_check_release(sched_ctrl, UE->rnti)) {
      // trigger release first as nr_mac_release_ue() invalidates UE ptr
      nr_mac_trigger_release_complete(mac, UE->rnti);
      nr_mac_release_ue(mac, UE->rnti);
      // go back to examine the next UE, which is at the position the
      // current UE was
      UE--;
      continue;
    }

    /* check if UL failure and trigger release request if necessary */
    nr_mac_check_ul_failure(mac, UE->rnti, sched_ctrl);

    if (nr_timer_tick(&sched_ctrl->transm_interrupt)) {
      /* expired */
      nr_timer_stop(&sched_ctrl->transm_interrupt);
      if (UE->interrupt_action == FOLLOW_OUTOFSYNC)
        nr_mac_trigger_ul_failure(sched_ctrl, UE->current_DL_BWP.scs);
    }
  }
}

int ul_buffer_index(int frame, int slot, int slots_per_frame, int size)
{
  const int abs_slot = frame * slots_per_frame + slot;
  return abs_slot % size;
}

void UL_tti_req_ahead_initialization(gNB_MAC_INST *gNB, int n, int CCid, frame_t frameP, int slotP)
{

  if(gNB->UL_tti_req_ahead[CCid][1].Slot == 1)
    return;

  /* fill in slot/frame numbers: slot is fixed, frame will be updated by scheduler
   * consider that scheduler runs sl_ahead: the first sl_ahead slots are
   * already "in the past" and thus we put frame 1 instead of 0! */
  for (int i = 0; i < gNB->UL_tti_req_ahead_size; ++i) {
    int abs_slot = frameP * n + slotP + i;
    nfapi_nr_ul_tti_request_t *req = &gNB->UL_tti_req_ahead[CCid][abs_slot % gNB->UL_tti_req_ahead_size];
    req->SFN = (abs_slot / n) % MAX_FRAME_NUMBER;
    req->Slot = abs_slot % n;
  }
}

int get_fapi_beamforming_index(gNB_MAC_INST *mac, int ssb_idx)
{
  int beam_idx = mac->fapi_beam_index[ssb_idx];
  AssertFatal(beam_idx >= 0, "Invalid beamforming index %d\n", beam_idx);
  return beam_idx;
}

// TODO this is a placeholder for a possibly more complex function
// for now the fapi beam index is the number of SSBs transmitted before ssb_index i
void fapi_beam_index_allocation(NR_ServingCellConfigCommon_t *scc, gNB_MAC_INST *mac)
{
  int len = 0;
  uint8_t* buf = NULL;
  switch (scc->ssb_PositionsInBurst->present) {
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_shortBitmap:
      len = 4;
      buf = scc->ssb_PositionsInBurst->choice.shortBitmap.buf;
      break;
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_mediumBitmap:
      len = 8;
      buf = scc->ssb_PositionsInBurst->choice.mediumBitmap.buf;
      break;
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_longBitmap:
      len = 64;
      buf = scc->ssb_PositionsInBurst->choice.longBitmap.buf;
      break;
    default :
      AssertFatal(false, "Invalid configuration\n");
  }
  int fapi_index = 0;
  for (int i = 0; i < len; ++i) {
    if ((buf[i / 8] >> (7 - i % 8)) & 0x1)
      mac->fapi_beam_index[i] = fapi_index++;
    else
      mac->fapi_beam_index[i] = -1;
  }
}

static inline int get_beam_index(const NR_beam_info_t *beam_info, int frame, int slot, int slots_per_frame)
{
  return ((frame * slots_per_frame + slot) / beam_info->beam_duration) % beam_info->beam_allocation_size;
}

NR_beam_alloc_t beam_allocation_procedure(NR_beam_info_t *beam_info, int frame, int slot, int beam_index, int slots_per_frame)
{
  // if no beam allocation for analog beamforming we always return beam index 0 (no multiple beams)
  if (!beam_info->beam_allocation)
    return (NR_beam_alloc_t) {.new_beam = false, .idx = 0};

  const int index = get_beam_index(beam_info, frame, slot, slots_per_frame);
  for (int i = 0; i < beam_info->beams_per_period; i++) {
    NR_beam_alloc_t beam_struct = {.new_beam = false, .idx = i};
    int *beam = &beam_info->beam_allocation[i][index];
    if (*beam == -1) {
      beam_struct.new_beam = true;
      *beam = beam_index;
    }
    if (*beam == beam_index) {
      LOG_D(NR_MAC, "%d.%d Using beam structure with index %d for beam %d (%s)\n", frame, slot, beam_struct.idx, beam_index, beam_struct.new_beam ? "new beam" : "old beam");
      return beam_struct;
    }
  }

  return (NR_beam_alloc_t) {.new_beam = false, .idx = -1};
}

void reset_beam_status(NR_beam_info_t *beam_info, int frame, int slot, int beam_index, int slots_per_frame, bool new_beam)
{
  if(!new_beam) // need to reset only if the beam was allocated specifically for this instance
    return;
  const int index = get_beam_index(beam_info, frame, slot, slots_per_frame);
  for (int i = 0; i < beam_info->beams_per_period; i++) {
    if (beam_info->beam_allocation[i][index] == beam_index)
      beam_info->beam_allocation[i][index] = -1;
  }
}

void beam_selection_procedures(gNB_MAC_INST *mac, NR_UE_info_t *UE)
{
  // do not perform beam procedures if there is no beam information
  if (!mac->beam_info.beam_allocation)
    return;
  RSRP_report_t *rsrp_report = &UE->UE_sched_ctrl.CSI_report.ssb_rsrp_report;
  // simple beam switching algorithm -> we select beam with highest RSRP from CSI report
  int new_bf_index = get_fapi_beamforming_index(mac, rsrp_report->resource_id[0]);
  if (UE->UE_beam_index == new_bf_index)
    return; // no beam change needed

  LOG_I(NR_MAC, "[UE %x] Switching to beam with ID %d (SSB number %d)\n", UE->rnti, new_bf_index, rsrp_report->resource_id[0]);
  UE->UE_beam_index = new_bf_index;
}

void send_initial_ul_rrc_message(int rnti, const uint8_t *sdu, sdu_size_t sdu_len, void *data)
{
  gNB_MAC_INST *mac = RC.nrmac[0];
  NR_UE_info_t *UE = (NR_UE_info_t *)data;
  NR_SCHED_ENSURE_LOCKED(&mac->sched_lock);

  uint8_t du2cu[1024];
  int encoded = encode_cellGroupConfig(UE->CellGroup, du2cu, sizeof(du2cu));

  DevAssert(mac->f1_config.setup_req != NULL);
  AssertFatal(mac->f1_config.setup_req->num_cells_available == 1, "can handle only one cell\n");
  const f1ap_initial_ul_rrc_message_t ul_rrc_msg = {
    .plmn = mac->f1_config.setup_req->cell[0].info.plmn,
    .nr_cellid = mac->f1_config.setup_req->cell[0].info.nr_cellid,
    .gNB_DU_ue_id = rnti,
    .crnti = rnti,
    .rrc_container = (uint8_t *) sdu,
    .rrc_container_length = sdu_len,
    .du2cu_rrc_container = (uint8_t *) du2cu,
    .du2cu_rrc_container_length = encoded
  };
  mac->mac_rrc.initial_ul_rrc_message_transfer(0, &ul_rrc_msg);
}

bool prepare_initial_ul_rrc_message(gNB_MAC_INST *mac, NR_UE_info_t *UE)
{
  NR_SCHED_ENSURE_LOCKED(&mac->sched_lock);

  /* activate SRB0 */
  if (!nr_rlc_activate_srb0(UE->rnti, UE, send_initial_ul_rrc_message))
    return false;

  /* create this UE's initial CellGroup */
  int CC_id = 0;
  int srb_id = 1;
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[CC_id].ServingCellConfigCommon;
  const NR_ServingCellConfig_t *sccd = UE->is_redcap ? NULL : mac->common_channels[CC_id].pre_ServingCellConfig;
  NR_CellGroupConfig_t *cellGroupConfig = get_initial_cellGroupConfig(UE->uid, scc, sccd, &mac->radio_config, &mac->rlc_config);
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, UE->CellGroup);
  UE->CellGroup = cellGroupConfig;

  /* the cellGroup sent to CU specifies there is SRB1, so create it */
  DevAssert(cellGroupConfig->rlc_BearerToAddModList->list.count == 1);
  const NR_RLC_BearerConfig_t *bearer = cellGroupConfig->rlc_BearerToAddModList->list.array[0];
  DevAssert(bearer->servedRadioBearer->choice.srb_Identity == srb_id);
  nr_rlc_add_srb(UE->rnti, bearer->servedRadioBearer->choice.srb_Identity, bearer);

  int priority = bearer->mac_LogicalChannelConfig->ul_SpecificParameters->priority;
  nr_lc_config_t c = {.lcid = bearer->logicalChannelIdentity, .priority = priority};
  nr_mac_add_lcid(&UE->UE_sched_ctrl, &c);
  return true;
}

void nr_mac_trigger_release_timer(NR_UE_sched_ctrl_t *sched_ctrl, NR_SubcarrierSpacing_t subcarrier_spacing)
{
  // trigger 60ms
  sched_ctrl->release_timer = 100 << subcarrier_spacing;
}

bool nr_mac_check_release(NR_UE_sched_ctrl_t *sched_ctrl, int rnti)
{
  if (sched_ctrl->release_timer == 0)
    return false;
  sched_ctrl->release_timer--;
  return sched_ctrl->release_timer == 0;
}

void nr_mac_trigger_ul_failure(NR_UE_sched_ctrl_t *sched_ctrl, NR_SubcarrierSpacing_t subcarrier_spacing)
{
  if (sched_ctrl->ul_failure) {
    /* already running */
    return;
  }
  sched_ctrl->ul_failure = true;
  // 30 seconds till triggering release request
  sched_ctrl->ul_failure_timer = 30000 << subcarrier_spacing;
}

void nr_mac_reset_ul_failure(NR_UE_sched_ctrl_t *sched_ctrl)
{
  sched_ctrl->ul_failure = false;
  sched_ctrl->ul_failure_timer = 0;
  sched_ctrl->pusch_consecutive_dtx_cnt = 0;
}

/* \brief trigger a release request towards the CU.
 * \returns true if release was requested, else false (in which case the CU
 * does not know the UE, and we can safely remove it). */
bool nr_mac_request_release_ue(const gNB_MAC_INST *nrmac, int rnti)
{
  if (!du_exists_f1_ue_data(rnti)) {
    LOG_W(NR_MAC, "UE %04x: no CU-UE ID stored, cannot request release\n", rnti);
    return false;
  }
  f1_ue_data_t ue_data = du_get_f1_ue_data(rnti);
  f1ap_ue_context_release_req_t request = {
    .gNB_CU_ue_id = ue_data.secondary_ue,
    .gNB_DU_ue_id = rnti,
    .cause = F1AP_CAUSE_RADIO_NETWORK,
    .cause_value = F1AP_CauseRadioNetwork_rl_failure_rlc,
  };
  nrmac->mac_rrc.ue_context_release_request(&request);
  return true;
}

void nr_mac_check_ul_failure(gNB_MAC_INST *nrmac, int rnti, NR_UE_sched_ctrl_t *sched_ctrl)
{
  if (!sched_ctrl->ul_failure)
    return;
  if (sched_ctrl->ul_failure_timer > 0)
    sched_ctrl->ul_failure_timer--;
  /* to trigger only once: trigger when ul_failure_timer == 1, but timer will
   * stop at 0 and we wait for a UE release command from upper layers */
  if (sched_ctrl->ul_failure_timer == 1) {
    LOG_W(MAC, "UE %04x: request release after UL failure timer expiry\n", rnti);
    bool requested = nr_mac_request_release_ue(nrmac, rnti);
    if (!requested)
      nr_mac_release_ue(nrmac, rnti);
  }
}

void nr_mac_trigger_reconfiguration(const gNB_MAC_INST *nrmac, const NR_UE_info_t *UE)
{
  DevAssert(UE->CellGroup != NULL);
  uint8_t buf[2048];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_CellGroupConfig, NULL, UE->CellGroup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0, "ASN1 encoding of CellGroupConfig failed, failed type %s\n", enc_rval.failed_type->name);
  du_to_cu_rrc_information_t du2cu = {
    .cellGroupConfig = buf,
    .cellGroupConfig_length = (enc_rval.encoded + 7) >> 3,
  };
  f1_ue_data_t ue_data = du_get_f1_ue_data(UE->rnti);
  f1ap_ue_context_modif_required_t required = {
    .gNB_CU_ue_id = ue_data.secondary_ue,
    .gNB_DU_ue_id = UE->rnti,
    .du_to_cu_rrc_information = &du2cu,
    .cause = F1AP_CAUSE_RADIO_NETWORK,
    .cause_value = F1AP_CauseRadioNetwork_action_desirable_for_radio_reasons,
  };
  nrmac->mac_rrc.ue_context_modification_required(&required);
}

/* \brief add bearers from CellGroupConfig.
 *
 * This is a kind of hack, as this should be processed through a F1 UE Context
 * setup request, but some modes do not use that (NSA/do-ra/phy_test).  */
void process_addmod_bearers_cellGroupConfig(NR_UE_sched_ctrl_t *sched_ctrl, const struct NR_CellGroupConfig__rlc_BearerToAddModList *addmod)
{
  if (addmod == NULL)
    return; /* nothing to do */

  for (int i = 0; i < addmod->list.count; ++i) {
    const NR_RLC_BearerConfig_t *conf = addmod->list.array[i];
    int lcid = conf->logicalChannelIdentity;
    nr_lc_config_t c = {.lcid = lcid};
    nr_mac_add_lcid(sched_ctrl, &c);
  }
}

long get_lcid_from_drbid(int drb_id)
{
  return drb_id + 3; /* LCID is DRB + 3 */
}

long get_lcid_from_srbid(int srb_id)
{
  return srb_id;
}

static bool eq_lcid_config(const void *vval, const void *vit)
{
  const nr_lc_config_t *val = (const nr_lc_config_t *)vval;
  const nr_lc_config_t *it = (const nr_lc_config_t *)vit;
  return it->lcid == val->lcid;
}

static int cmp_lc_config(const void *va, const void *vb)
{
  const nr_lc_config_t *a = (const nr_lc_config_t *)va;
  const nr_lc_config_t *b = (const nr_lc_config_t *)vb;

  if (a->priority < b->priority)
    return -1;
  if (a->priority == b->priority)
    return 0;
  return 1;
}

nr_lc_config_t *nr_mac_get_lc_config(NR_UE_sched_ctrl_t* sched_ctrl, int lcid)
{
  nr_lc_config_t c = {.lcid = lcid};
  elm_arr_t elm = find_if(&sched_ctrl->lc_config, &c, eq_lcid_config);
  if (elm.found)
    return elm.it;
  else
    return NULL;
}

bool nr_mac_add_lcid(NR_UE_sched_ctrl_t* sched_ctrl, const nr_lc_config_t *c)
{
  elm_arr_t elm = find_if(&sched_ctrl->lc_config, (void *) c, eq_lcid_config);
  if (elm.found) {
    LOG_I(NR_MAC, "cannot add LCID %d: already present, updating configuration\n", c->lcid);
    nr_lc_config_t *exist = (nr_lc_config_t *)elm.it;
    *exist = *c;
  } else {
    LOG_D(NR_MAC, "Add LCID %d\n", c->lcid);
    seq_arr_push_back(&sched_ctrl->lc_config, (void*) c, sizeof(*c));
  }
  void *base = seq_arr_front(&sched_ctrl->lc_config);
  size_t nmemb = seq_arr_size(&sched_ctrl->lc_config);
  size_t size = sizeof(*c);
  qsort(base, nmemb, size, cmp_lc_config);
  return true;
}

bool nr_mac_remove_lcid(NR_UE_sched_ctrl_t *sched_ctrl, long lcid)
{
  nr_lc_config_t c = {.lcid = lcid};
  elm_arr_t elm = find_if(&sched_ctrl->lc_config, &c, eq_lcid_config);
  if (!elm.found) {
    LOG_E(NR_MAC, "can not remove LC: no such LC with ID %ld\n", lcid);
    return false;
  }

  seq_arr_erase(&sched_ctrl->lc_config, elm.it);
  return true;
}

bool nr_mac_get_new_rnti(NR_UEs_t *UEs, rnti_t *rnti)
{
  int loop = 0;
  bool exist_connected_ue, exist_in_pending_ra_ue;
  do {
    *rnti = (taus() % 0xffef) + 1;
    exist_connected_ue = find_nr_UE(UEs, *rnti) != NULL;
    exist_in_pending_ra_ue = find_ra_UE(UEs, *rnti) != NULL;
    loop++;
  } while (loop < 100 && (exist_connected_ue || exist_in_pending_ra_ue));
  return loop < 100; // nothing found: loop count 100
}

/// @brief Orders PDCCH aggregation levels so that we first check desired aggregation level according to
///        pdcch_cl_adjust
/// @param agg_level_search_order in/out 5-element array of aggregation levels from 0 to 4
/// @param pdcch_cl_adjust value from 0 to 1 indication channel impariments (0 - good channel, 1 - bad channel)
static void determine_aggregation_level_search_order(int agg_level_search_order[NUM_PDCCH_AGG_LEVELS], float pdcch_cl_adjust)
{
  int desired_agg_level_index = round(4 * pdcch_cl_adjust);
  int agg_level_search_index = 0;
  for (int i = desired_agg_level_index; i < NUM_PDCCH_AGG_LEVELS; i++) {
    agg_level_search_order[agg_level_search_index++] = i;
  }
  for (int i = desired_agg_level_index - 1; i >= 0; i--) {
    agg_level_search_order[agg_level_search_index++] = i;
  }
}

/// @brief Update PDCCH closed loop adjust for UE depending on detection of feedback.
/// @param sched_ctrl UE scheduling control info
/// @param feedback_not_detected Whether feedback (PUSCH or HARQ) was detected
void nr_mac_update_pdcch_closed_loop_adjust(NR_UE_sched_ctrl_t *sched_ctrl, bool feedback_not_detected)
{
  if (feedback_not_detected) {
    sched_ctrl->pdcch_cl_adjust = min(1, sched_ctrl->pdcch_cl_adjust + 0.05);
  } else {
    sched_ctrl->pdcch_cl_adjust = max(0, sched_ctrl->pdcch_cl_adjust - 0.01);
  }
}
