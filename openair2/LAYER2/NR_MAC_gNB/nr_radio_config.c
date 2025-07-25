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

#include "nr_radio_config.h"

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "BIT_STRING.h"
#include "NULL.h"
#include "asn_SET_OF.h"
#include "asn_codecs.h"
#include "asn_internal.h"
#include "assertions.h"
#include "common/openairinterface5g_limits.h"
#include "common/utils/T/T.h"
#include "common/utils/nr/nr_common.h"
#include "constr_TYPE.h"
#include "executables/softmodem-common.h"
#include "oai_asn1.h"
#include "openair2/LAYER2/NR_MAC_gNB/nr_mac_gNB.h"
#include "openair2/LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair3/UTILS/conversions.h"
#include "LAYER2/nr_rlc/nr_rlc_asn1_utils.h"

#include "NR_MeasurementTimingConfiguration.h"
#include "uper_decoder.h"
#include "uper_encoder.h"
#include "utils.h"
#include "xer_encoder.h"

#define NR_MAX_SUPPORTED_DL_LAYERS 4

#define PUCCH2_SIZE 8
const uint8_t slotsperframe[5] = {10, 20, 40, 80, 160};

static NR_BWP_t clone_generic_parameters(const NR_BWP_t *gp)
{
  NR_BWP_t clone = {0};
  clone.locationAndBandwidth = gp->locationAndBandwidth;
  clone.subcarrierSpacing = gp->subcarrierSpacing;
  if (gp->cyclicPrefix) {
    asn1cCallocOne(clone.cyclicPrefix, *gp->cyclicPrefix);
  }
  return clone;
}

/**
 * @brief Verifies the aggregation level candidates
 *
 * This function checks the input aggregation level candidates and translates the value provided
 * in the config to a valid field in RRC message.
 *
 * @param[in] num_cce_in_coreset number of CCE in coreset
 * @param[in] in_num_agg_level_candidates input array of aggregation level candidates, interpreted as number of candidates.
 * @param[in] coresetid coreset id
 * @param[in] searchspaceid searchspace id
 * @param[out] out_num_agg_level_candidates array of aggregation level candidates, output is a valid 3gpp field value.
 * 
 */
static void verify_agg_levels(int num_cce_in_coreset,
                              const int in_num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS],
                              int coresetid,
                              int searchspaceid,
                              int out_num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS])
{
  int agg_level_to_n_cces[] = {1, 2, 4, 8, 16};
  for (int i = 0; i < NUM_PDCCH_AGG_LEVELS; i++) {
    // 7 is not a valid value, the mapping in 38.331 is from 0-7 mapped to 0-8 candidates and value 7 means 8 candidates instead. If
    // the user wants 7 candidates round it up to 8.
    int num_agg_level_candidates = in_num_agg_level_candidates[i];
    if (num_agg_level_candidates == 7) {
      num_agg_level_candidates = 8;
    }
    if (num_agg_level_candidates * agg_level_to_n_cces[i] > num_cce_in_coreset) {
      int new_agg_level_candidates = num_cce_in_coreset / agg_level_to_n_cces[i];
      LOG_E(NR_RRC,
            "Invalid configuration: Not enough CCEs in coreset %d, searchspace %d, agg_level %d, number of requested "
            "candidates = %d, number of CCES in coreset %d. Aggregation level candidates limited to %d\n",
            coresetid,
            searchspaceid,
            agg_level_to_n_cces[i],
            in_num_agg_level_candidates[i],
            num_cce_in_coreset,
            new_agg_level_candidates);
      num_agg_level_candidates = new_agg_level_candidates;
    }
    out_num_agg_level_candidates[i] = min(num_agg_level_candidates, 7);
  }
}

static NR_SetupRelease_RACH_ConfigCommon_t *clone_rach_configcommon(const NR_SetupRelease_RACH_ConfigCommon_t *rcc)
{
  if (rcc == NULL || rcc->present == NR_SetupRelease_RACH_ConfigCommon_PR_NOTHING)
    return NULL;
  NR_SetupRelease_RACH_ConfigCommon_t *clone = calloc_or_fail(1, sizeof(*clone));
  clone->present = rcc->present;
  if (clone->present == NR_SetupRelease_RACH_ConfigCommon_PR_release)
    return clone;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_RACH_ConfigCommon, NULL, rcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_RACH_ConfigCommon: problem while encoding\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_RACH_ConfigCommon, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_RACH_ConfigCommon: problem while decoding\n");
  return clone;
}

static NR_SetupRelease_MsgA_ConfigCommon_r16_t *clone_msga_configcommon(const NR_SetupRelease_MsgA_ConfigCommon_r16_t *mcc)
{
  if (mcc == NULL || mcc->present == NR_SetupRelease_MsgA_ConfigCommon_r16_PR_NOTHING)
    return NULL;
  NR_SetupRelease_MsgA_ConfigCommon_r16_t *clone = calloc_or_fail(1, sizeof(*clone));
  clone->present = mcc->present;
  if (clone->present == NR_SetupRelease_MsgA_ConfigCommon_r16_PR_release)
    return clone;
  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_MsgA_ConfigCommon_r16, NULL, mcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf),
              "could not clone NR_MsgA_ConfigCommon_r16: problem while encoding\n");
  asn_dec_rval_t dec_rval =
      uper_decode(NULL, &asn_DEF_NR_MsgA_ConfigCommon_r16, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded,
              "could not clone NR_MsgA_ConfigCommon_r16:: problem while decoding\n");
  return clone;
}

static NR_SetupRelease_PUSCH_ConfigCommon_t *clone_pusch_configcommon(const NR_SetupRelease_PUSCH_ConfigCommon_t *pcc)
{
  if (pcc == NULL || pcc->present == NR_SetupRelease_PUSCH_ConfigCommon_PR_NOTHING)
    return NULL;
  NR_SetupRelease_PUSCH_ConfigCommon_t *clone = calloc_or_fail(1, sizeof(*clone));
  clone->present = pcc->present;
  if (clone->present == NR_SetupRelease_PUSCH_ConfigCommon_PR_release)
    return clone;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_PUSCH_ConfigCommon, NULL, pcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_PUSCH_ConfigCommon: problem while encoding\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_PUSCH_ConfigCommon, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_PUSCH_ConfigCommon: problem while decoding\n");
  return clone;
}

static NR_SetupRelease_PUCCH_ConfigCommon_t *clone_pucch_configcommon(const NR_SetupRelease_PUCCH_ConfigCommon_t *pcc)
{
  if (pcc == NULL || pcc->present == NR_SetupRelease_PUCCH_ConfigCommon_PR_NOTHING)
    return NULL;
  NR_SetupRelease_PUCCH_ConfigCommon_t *clone = calloc_or_fail(1, sizeof(*clone));
  clone->present = pcc->present;
  if (clone->present == NR_SetupRelease_PUCCH_ConfigCommon_PR_release)
    return clone;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_PUCCH_ConfigCommon, NULL, pcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_PUCCH_ConfigCommon: problem while encoding\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_PUCCH_ConfigCommon, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_PUCCH_ConfigCommon: problem while decoding\n");
  return clone;
}

static NR_SetupRelease_PDCCH_ConfigCommon_t *clone_pdcch_configcommon(const NR_SetupRelease_PDCCH_ConfigCommon_t *pcc)
{
  if (pcc == NULL || pcc->present == NR_SetupRelease_PDCCH_ConfigCommon_PR_NOTHING)
    return NULL;
  NR_SetupRelease_PDCCH_ConfigCommon_t *clone = calloc(1, sizeof(*clone));
  clone->present = pcc->present;
  if (clone->present == NR_SetupRelease_PDCCH_ConfigCommon_PR_release)
    return clone;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_PDCCH_ConfigCommon, NULL, pcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_PDCCH_ConfigCommon: problem while encoding\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_PDCCH_ConfigCommon, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_PDCCH_ConfigCommon: problem while decoding\n");
  return clone;
}

static NR_SetupRelease_PDSCH_ConfigCommon_t *clone_pdsch_configcommon(const NR_SetupRelease_PDSCH_ConfigCommon_t *pcc)
{
  if (pcc == NULL || pcc->present == NR_SetupRelease_PDSCH_ConfigCommon_PR_NOTHING)
    return NULL;
  NR_SetupRelease_PDSCH_ConfigCommon_t *clone = calloc_or_fail(1, sizeof(*clone));
  clone->present = pcc->present;
  if (clone->present == NR_SetupRelease_PDSCH_ConfigCommon_PR_release)
    return clone;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_PDSCH_ConfigCommon, NULL, pcc->choice.setup, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_PDSCH_ConfigCommon: problem while encoding\n");
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_PDSCH_ConfigCommon, (void **)&clone->choice.setup, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_PDSCH_ConfigCommon: problem while decoding\n");
  return clone;
}

static NR_PUSCH_Config_t *clone_pusch_config(const NR_PUSCH_Config_t *pc)
{
  if (pc == NULL)
    return NULL;

  uint8_t buf[1024];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_PUSCH_Config, NULL, pc, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_PUSCH_Config: problem while encoding\n");
  NR_PUSCH_Config_t *clone = NULL;
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_PUSCH_Config, (void **)&clone, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_PUSCH_Config: problem while decoding\n");
  return clone;
}

static int get_nb_pucch2_per_slot(const NR_ServingCellConfigCommon_t *scc, int bwp_size)
{
  const NR_TDD_UL_DL_Pattern_t *tdd = scc->tdd_UL_DL_ConfigurationCommon ? &scc->tdd_UL_DL_ConfigurationCommon->pattern1 : NULL;
  const int n_slots_frame = slotsperframe[*scc->ssbSubcarrierSpacing];
  int ul_slots_period = tdd ? tdd->nrofUplinkSlots + (tdd->nrofUplinkSymbols > 0) : n_slots_frame;
  int n_slots_period = tdd ? n_slots_frame/get_nb_periods_per_frame(tdd->dl_UL_TransmissionPeriodicity) : n_slots_frame;
  int max_meas_report_period = 320; // slots
  int max_csi_reports = MAX_MOBILES_PER_GNB << 1; // 2 reports per UE (RSRP and RI-PMI-CQI)
  int available_report_occasions = max_meas_report_period * ul_slots_period / n_slots_period;
  int nb_pucch2 = (max_csi_reports / (available_report_occasions + 1)) + 1;
  // in current implementation we need (nb_pucch2 * PUCCH2_SIZE) prbs for PUCCH2
  // and MAX_MOBILES_PER_GNB prbs for PUCCH1
  AssertFatal((nb_pucch2 * PUCCH2_SIZE) + MAX_MOBILES_PER_GNB <= bwp_size,
              "Cannot allocate all required PUCCH resources for max number of %d UEs in BWP with %d PRBs\n",
              MAX_MOBILES_PER_GNB, bwp_size);
  return nb_pucch2;
}

NR_SearchSpace_t *rrc_searchspace_config(bool is_common,
                                         int searchspaceid,
                                         int coresetid,
                                         const int num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS])
{
  NR_SearchSpace_t *ss = calloc(1,sizeof(*ss));
  ss->searchSpaceId = searchspaceid;
  ss->controlResourceSetId = calloc(1,sizeof(*ss->controlResourceSetId));
  *ss->controlResourceSetId = coresetid;
  ss->monitoringSlotPeriodicityAndOffset = calloc(1,sizeof(*ss->monitoringSlotPeriodicityAndOffset));
  ss->monitoringSlotPeriodicityAndOffset->present = NR_SearchSpace__monitoringSlotPeriodicityAndOffset_PR_sl1;
  ss->monitoringSlotPeriodicityAndOffset->choice.sl1 = (NULL_t)0;
  ss->duration = NULL;
  ss->monitoringSymbolsWithinSlot = calloc(1,sizeof(*ss->monitoringSymbolsWithinSlot));
  ss->monitoringSymbolsWithinSlot->buf = calloc(1,2);
  ss->monitoringSymbolsWithinSlot->size = 2;
  ss->monitoringSymbolsWithinSlot->buf[0] = 0x80;
  ss->monitoringSymbolsWithinSlot->buf[1] = 0x0;
  ss->monitoringSymbolsWithinSlot->bits_unused = 2;
  ss->nrofCandidates = calloc(1,sizeof(*ss->nrofCandidates));
  ss->nrofCandidates->aggregationLevel1 = num_agg_level_candidates[PDCCH_AGG_LEVEL1];
  ss->nrofCandidates->aggregationLevel2 = num_agg_level_candidates[PDCCH_AGG_LEVEL2];
  ss->nrofCandidates->aggregationLevel4 = num_agg_level_candidates[PDCCH_AGG_LEVEL4];
  ss->nrofCandidates->aggregationLevel8 = num_agg_level_candidates[PDCCH_AGG_LEVEL8];
  ss->nrofCandidates->aggregationLevel16 = num_agg_level_candidates[PDCCH_AGG_LEVEL16];
  ss->searchSpaceType = calloc(1,sizeof(*ss->searchSpaceType));
  if (is_common) {
    ss->searchSpaceType->present = NR_SearchSpace__searchSpaceType_PR_common;
    ss->searchSpaceType->choice.common = calloc(1,sizeof(*ss->searchSpaceType->choice.common));
    ss->searchSpaceType->choice.common->dci_Format0_0_AndFormat1_0 = calloc(1,sizeof(*ss->searchSpaceType->choice.common->dci_Format0_0_AndFormat1_0));
  }
  else {
    ss->searchSpaceType->present = NR_SearchSpace__searchSpaceType_PR_ue_Specific;
    ss->searchSpaceType->choice.ue_Specific = calloc(1,sizeof(*ss->searchSpaceType->choice.ue_Specific));
    ss->searchSpaceType->choice.ue_Specific->dci_Formats=NR_SearchSpace__searchSpaceType__ue_Specific__dci_Formats_formats0_1_And_1_1;
  }
  return ss;
}

static NR_ControlResourceSet_t *get_coreset_config(int bwp_id, int curr_bwp, uint64_t ssb_bitmap)
{
  NR_ControlResourceSet_t *coreset = calloc(1, sizeof(*coreset));
  AssertFatal(coreset != NULL, "out of memory\n");
  // frequency domain resources depending on BWP size
  coreset->frequencyDomainResources.buf = calloc(1,6);
  coreset->frequencyDomainResources.buf[0] = (curr_bwp < 48) ? 0xf0 : 0xff;
  coreset->frequencyDomainResources.buf[1] = (curr_bwp < 96) ? 0x00 : 0xff;
  coreset->frequencyDomainResources.buf[2] = (curr_bwp < 144) ? 0x00 : 0xff;
  coreset->frequencyDomainResources.buf[3] = (curr_bwp < 192) ? 0x00 : 0xff;
  coreset->frequencyDomainResources.buf[4] = (curr_bwp < 240) ? 0x00 : 0xff;
  coreset->frequencyDomainResources.buf[5] = 0x00;
  coreset->frequencyDomainResources.size = 6;
  coreset->frequencyDomainResources.bits_unused = 3;
  coreset->duration = (curr_bwp < 48) ? 2 : 1;
  coreset->cce_REG_MappingType.present = NR_ControlResourceSet__cce_REG_MappingType_PR_nonInterleaved;
  coreset->precoderGranularity = NR_ControlResourceSet__precoderGranularity_sameAsREG_bundle;

  // The ID space is used across the BWPs of a Serving Cell as per 38.331
  coreset->controlResourceSetId = bwp_id + 1;

  coreset->tci_StatesPDCCH_ToAddList=calloc(1,sizeof(*coreset->tci_StatesPDCCH_ToAddList));
  NR_TCI_StateId_t *tci[64];
  for (int i=0;i<64;i++) {
    if ((ssb_bitmap>>(63-i))&0x01){
      tci[i]=calloc(1,sizeof(*tci[i]));
      *tci[i] = i;
      asn1cSeqAdd(&coreset->tci_StatesPDCCH_ToAddList->list,tci[i]);
    }
  }
  coreset->tci_StatesPDCCH_ToReleaseList = NULL;
  coreset->tci_PresentInDCI = NULL;
  coreset->pdcch_DMRS_ScramblingID = NULL;
  return coreset;
}

static uint64_t get_ssb_bitmap(const NR_ServingCellConfigCommon_t *scc)
{
  uint64_t bitmap=0;
  switch (scc->ssb_PositionsInBurst->present) {
    case 1 :
      bitmap = ((uint64_t) scc->ssb_PositionsInBurst->choice.shortBitmap.buf[0])<<56;
      break;
    case 2 :
      bitmap = ((uint64_t) scc->ssb_PositionsInBurst->choice.mediumBitmap.buf[0])<<56;
      break;
    case 3 :
      for (int i=0; i<8; i++) {
        bitmap |= (((uint64_t) scc->ssb_PositionsInBurst->choice.longBitmap.buf[i])<<((7-i)*8));
      }
      break;
    default:
      AssertFatal(1==0,"SSB bitmap size value %d undefined (allowed values 1,2,3) \n", scc->ssb_PositionsInBurst->present);
  }
  return bitmap;
}

static bool check_periodicity(int val, int ideal_period, const frame_structure_t *fs)
{
  bool valid_periodicity_for_tdd_period = fs->frame_type == FDD ? true : (val % fs->numb_slots_period == 0);
  return (ideal_period < val + 1) && valid_periodicity_for_tdd_period;
}

static int set_ideal_period(bool is_csi)
{
  const frame_structure_t *fs = &RC.nrmac[0]->frame_structure;
  const int nb_slots_per_period = fs->numb_slots_period;
  const int n_ul_slots_per_period = get_ul_slots_per_period(fs); // full UL + mixed with UL symbols
  // 2 reports per UE (RSRP and RI-PMI-CQI)
  return is_csi ? MAX_MOBILES_PER_GNB * 2 * nb_slots_per_period / n_ul_slots_per_period : nb_slots_per_period * MAX_MOBILES_PER_GNB;
}

static void set_csirs_periodicity(NR_NZP_CSI_RS_Resource_t *nzpcsi0,
                                  int id,
                                  int ideal_period,
                                  const frame_structure_t *fs)
{
  nzpcsi0->periodicityAndOffset = calloc(1,sizeof(*nzpcsi0->periodicityAndOffset));
  // TODO ideal period to be set according to estimation by the gNB on how fast the channel changes
  const int offset = fs->numb_slots_period * id;
  if (check_periodicity(4, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots4;
    nzpcsi0->periodicityAndOffset->choice.slots4 = offset;
  }
  else if (check_periodicity(5, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots5;
    nzpcsi0->periodicityAndOffset->choice.slots5 = offset;
  }
  else if (check_periodicity(8, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots8;
    nzpcsi0->periodicityAndOffset->choice.slots8 = offset;
  }
  else if (check_periodicity(10, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots10;
    nzpcsi0->periodicityAndOffset->choice.slots10 = offset;
  }
  else if (check_periodicity(16, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots16;
    nzpcsi0->periodicityAndOffset->choice.slots16 = offset;
  }
  else if (check_periodicity(20, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots20;
    nzpcsi0->periodicityAndOffset->choice.slots20 = offset;
  }
  else if (check_periodicity(40, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots40;
    nzpcsi0->periodicityAndOffset->choice.slots40 = offset;
  }
  else if (check_periodicity(80, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots80;
    nzpcsi0->periodicityAndOffset->choice.slots80 = offset;
  }
  else if (check_periodicity(160, ideal_period, fs)) {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots160;
    nzpcsi0->periodicityAndOffset->choice.slots160 = offset;
  }
  else {
    nzpcsi0->periodicityAndOffset->present = NR_CSI_ResourcePeriodicityAndOffset_PR_slots320;
    const int nb_dl_slots_period = get_full_dl_slots_per_period(fs); // full DL slots
    AssertFatal(offset / 320 < nb_dl_slots_period, "Cannot allocate CSI-RS for BWP %d. Not enough resources for CSI-RS\n", id);
    nzpcsi0->periodicityAndOffset->choice.slots320 = (offset % 320) + (offset / 320);
  }
}

static void config_csirs(const NR_ServingCellConfigCommon_t *servingcellconfigcommon,
                         NR_CSI_MeasConfig_t *csi_MeasConfig,
                         int num_dl_antenna_ports,
                         int curr_bwp,
                         int do_csirs,
                         int id)
{
  if (do_csirs) {

    if(!csi_MeasConfig->nzp_CSI_RS_ResourceSetToAddModList)
      csi_MeasConfig->nzp_CSI_RS_ResourceSetToAddModList  = calloc(1,sizeof(*csi_MeasConfig->nzp_CSI_RS_ResourceSetToAddModList));
    NR_NZP_CSI_RS_ResourceSet_t *nzpcsirs0 = calloc(1,sizeof(*nzpcsirs0));
    nzpcsirs0->nzp_CSI_ResourceSetId = id;
    NR_NZP_CSI_RS_ResourceId_t *nzpid0 = calloc(1,sizeof(*nzpid0));
    *nzpid0 = id;
    asn1cSeqAdd(&nzpcsirs0->nzp_CSI_RS_Resources,nzpid0);
    nzpcsirs0->repetition = NULL;
    nzpcsirs0->aperiodicTriggeringOffset = NULL;
    nzpcsirs0->trs_Info = NULL;
    asn1cSeqAdd(&csi_MeasConfig->nzp_CSI_RS_ResourceSetToAddModList->list,nzpcsirs0);
    if(!csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList)
      csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList = calloc(1,sizeof(*csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList));
    NR_NZP_CSI_RS_Resource_t *nzpcsi0 = calloc(1,sizeof(*nzpcsi0));
    nzpcsi0->nzp_CSI_RS_ResourceId = id;
    NR_CSI_RS_ResourceMapping_t resourceMapping = {0};
    switch (num_dl_antenna_ports) {
      case 1:
        resourceMapping.frequencyDomainAllocation.present = NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_row2;
        resourceMapping.frequencyDomainAllocation.choice.row2.buf = calloc(2, sizeof(uint8_t));
        resourceMapping.frequencyDomainAllocation.choice.row2.size = 2;
        resourceMapping.frequencyDomainAllocation.choice.row2.bits_unused = 4;
        resourceMapping.frequencyDomainAllocation.choice.row2.buf[0] = 0;
        resourceMapping.frequencyDomainAllocation.choice.row2.buf[1] = 16;
        resourceMapping.nrofPorts = NR_CSI_RS_ResourceMapping__nrofPorts_p1;
        resourceMapping.cdm_Type = NR_CSI_RS_ResourceMapping__cdm_Type_noCDM;
        break;
      case 2:
        resourceMapping.frequencyDomainAllocation.present = NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_other;
        resourceMapping.frequencyDomainAllocation.choice.other.buf = calloc(1, sizeof(uint8_t));
        resourceMapping.frequencyDomainAllocation.choice.other.size = 1;
        resourceMapping.frequencyDomainAllocation.choice.other.bits_unused = 2;
        resourceMapping.frequencyDomainAllocation.choice.other.buf[0] = 4;
        resourceMapping.nrofPorts = NR_CSI_RS_ResourceMapping__nrofPorts_p2;
        resourceMapping.cdm_Type = NR_CSI_RS_ResourceMapping__cdm_Type_fd_CDM2;
        break;
      case 4:
        resourceMapping.frequencyDomainAllocation.present = NR_CSI_RS_ResourceMapping__frequencyDomainAllocation_PR_row4;
        resourceMapping.frequencyDomainAllocation.choice.row4.buf = calloc(1, sizeof(uint8_t));
        resourceMapping.frequencyDomainAllocation.choice.row4.size = 1;
        resourceMapping.frequencyDomainAllocation.choice.row4.bits_unused = 5;
        resourceMapping.frequencyDomainAllocation.choice.row4.buf[0] = 32;
        resourceMapping.nrofPorts = NR_CSI_RS_ResourceMapping__nrofPorts_p4;
        resourceMapping.cdm_Type = NR_CSI_RS_ResourceMapping__cdm_Type_fd_CDM2;
        break;
      default:
        AssertFatal(1==0,"Number of ports not yet supported\n");
    }
    resourceMapping.firstOFDMSymbolInTimeDomain = 13;  // last symbol of slot
    resourceMapping.firstOFDMSymbolInTimeDomain2 = NULL;
    resourceMapping.density.present = NR_CSI_RS_ResourceMapping__density_PR_one;
    resourceMapping.density.choice.one = (NULL_t)0;
    resourceMapping.freqBand.startingRB = 0;
    resourceMapping.freqBand.nrofRBs = ((curr_bwp >> 2) + (curr_bwp % 4 > 0)) << 2;
    nzpcsi0->resourceMapping = resourceMapping;
    nzpcsi0->powerControlOffset = 0;
    nzpcsi0->powerControlOffsetSS = calloc(1,sizeof(*nzpcsi0->powerControlOffsetSS));
    *nzpcsi0->powerControlOffsetSS = NR_NZP_CSI_RS_Resource__powerControlOffsetSS_db0;
    nzpcsi0->scramblingID = *servingcellconfigcommon->physCellId;

    const int ideal_period = set_ideal_period(true); // same periodicity as CSI measurement report
    const frame_structure_t *fs = &(RC.nrmac[0]->frame_structure);
    set_csirs_periodicity(nzpcsi0, id, ideal_period, fs);

    nzpcsi0->qcl_InfoPeriodicCSI_RS = calloc(1,sizeof(*nzpcsi0->qcl_InfoPeriodicCSI_RS));
    *nzpcsi0->qcl_InfoPeriodicCSI_RS = 0;
    asn1cSeqAdd(&csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList->list,nzpcsi0);
  }
  else {
    csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList = NULL;
    csi_MeasConfig->nzp_CSI_RS_ResourceSetToAddModList  = NULL;
  }
  csi_MeasConfig->nzp_CSI_RS_ResourceSetToReleaseList = NULL;
  csi_MeasConfig->nzp_CSI_RS_ResourceToReleaseList = NULL;
}

static void set_csiim_offset(struct NR_CSI_ResourcePeriodicityAndOffset *periodicityAndOffset,
                             struct NR_CSI_ResourcePeriodicityAndOffset *target_periodicityAndOffset)
{
  switch(periodicityAndOffset->present) {
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots4:
      periodicityAndOffset->choice.slots4 = target_periodicityAndOffset->choice.slots4;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots5:
      periodicityAndOffset->choice.slots5 = target_periodicityAndOffset->choice.slots5;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots8:
      periodicityAndOffset->choice.slots8 = target_periodicityAndOffset->choice.slots8;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots10:
      periodicityAndOffset->choice.slots10 = target_periodicityAndOffset->choice.slots10;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots16:
      periodicityAndOffset->choice.slots16 = target_periodicityAndOffset->choice.slots16;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots20:
      periodicityAndOffset->choice.slots20 = target_periodicityAndOffset->choice.slots20;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots32:
      periodicityAndOffset->choice.slots32 = target_periodicityAndOffset->choice.slots32;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots40:
      periodicityAndOffset->choice.slots40 = target_periodicityAndOffset->choice.slots40;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots64:
      periodicityAndOffset->choice.slots64 = target_periodicityAndOffset->choice.slots64;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots80:
      periodicityAndOffset->choice.slots80 = target_periodicityAndOffset->choice.slots80;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots160:
      periodicityAndOffset->choice.slots160 = target_periodicityAndOffset->choice.slots160;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots320:
      periodicityAndOffset->choice.slots320 = target_periodicityAndOffset->choice.slots320;
      break;
    case NR_CSI_ResourcePeriodicityAndOffset_PR_slots640:
      periodicityAndOffset->choice.slots640 = target_periodicityAndOffset->choice.slots640;
      break;
    default:
      AssertFatal(1==0,"CSI periodicity not among allowed values\n");
  }

}

static void config_csiim(int do_csirs,
                         int dl_antenna_ports,
                         int curr_bwp,
                         NR_CSI_MeasConfig_t *csi_MeasConfig,
                         int id)
{
 if (do_csirs && dl_antenna_ports > 1) {
   if (!csi_MeasConfig->csi_IM_ResourceToAddModList)
     csi_MeasConfig->csi_IM_ResourceToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_IM_ResourceToAddModList));
   NR_CSI_IM_Resource_t *imres = calloc(1,sizeof(*imres));
   imres->csi_IM_ResourceId = id;
   NR_NZP_CSI_RS_Resource_t *nzpcsi = NULL;
   for (int i=0; i<csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList->list.count; i++){
     nzpcsi = csi_MeasConfig->nzp_CSI_RS_ResourceToAddModList->list.array[i];
     if (nzpcsi->nzp_CSI_RS_ResourceId == imres->csi_IM_ResourceId)
       break;
   }
   AssertFatal(nzpcsi->nzp_CSI_RS_ResourceId == imres->csi_IM_ResourceId, "Couldn't find NZP CSI-RS corresponding to CSI-IM\n");
   imres->csi_IM_ResourceElementPattern = calloc(1,sizeof(*imres->csi_IM_ResourceElementPattern));
   imres->csi_IM_ResourceElementPattern->present = NR_CSI_IM_Resource__csi_IM_ResourceElementPattern_PR_pattern1;
   imres->csi_IM_ResourceElementPattern->choice.pattern1 = calloc(1,sizeof(*imres->csi_IM_ResourceElementPattern->choice.pattern1));
   // starting subcarrier is 4 in the following configuration
   // this is ok for current possible CSI-RS configurations (using only the first 4 symbols)
   // TODO needs a more dynamic setting if CSI-RS is changed
   imres->csi_IM_ResourceElementPattern->choice.pattern1->subcarrierLocation_p1 = NR_CSI_IM_Resource__csi_IM_ResourceElementPattern__pattern1__subcarrierLocation_p1_s4;
   imres->csi_IM_ResourceElementPattern->choice.pattern1->symbolLocation_p1 = nzpcsi->resourceMapping.firstOFDMSymbolInTimeDomain; // same symbol as CSI-RS
   imres->freqBand = calloc(1,sizeof(*imres->freqBand));
   imres->freqBand->startingRB = 0;
   imres->freqBand->nrofRBs = ((curr_bwp>>2)+(curr_bwp%4>0))<<2;
   imres->periodicityAndOffset = calloc(1,sizeof(*imres->periodicityAndOffset));
   // same period and offset of the associated CSI-RS
   imres->periodicityAndOffset->present = nzpcsi->periodicityAndOffset->present;
   set_csiim_offset(imres->periodicityAndOffset, nzpcsi->periodicityAndOffset);
   asn1cSeqAdd(&csi_MeasConfig->csi_IM_ResourceToAddModList->list,imres);
   if (!csi_MeasConfig->csi_IM_ResourceSetToAddModList)
     csi_MeasConfig->csi_IM_ResourceSetToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_IM_ResourceSetToAddModList));
   NR_CSI_IM_ResourceSet_t *imset = calloc(1,sizeof(*imset));
   imset->csi_IM_ResourceSetId = id;
   NR_CSI_IM_ResourceId_t *res = calloc(1,sizeof(*res));
   *res = id;
   asn1cSeqAdd(&imset->csi_IM_Resources,res);
   asn1cSeqAdd(&csi_MeasConfig->csi_IM_ResourceSetToAddModList->list,imset);
 }
 else {
   csi_MeasConfig->csi_IM_ResourceToAddModList = NULL;
   csi_MeasConfig->csi_IM_ResourceSetToAddModList = NULL;
 }

 csi_MeasConfig->csi_IM_ResourceToReleaseList = NULL;
 csi_MeasConfig->csi_IM_ResourceSetToReleaseList = NULL;
}


void set_dl_maxmimolayers(NR_PDSCH_ServingCellConfig_t *pdsch_servingcellconfig,
                          const NR_ServingCellConfigCommon_t *scc,
                          const NR_UE_NR_Capability_t *uecap,
                          int maxMIMO_layers)
{
  if(!pdsch_servingcellconfig->ext1)
    pdsch_servingcellconfig->ext1=calloc(1,sizeof(*pdsch_servingcellconfig->ext1));
  if(!pdsch_servingcellconfig->ext1->maxMIMO_Layers)
    pdsch_servingcellconfig->ext1->maxMIMO_Layers = calloc(1,sizeof(*pdsch_servingcellconfig->ext1->maxMIMO_Layers));

  NR_SCS_SpecificCarrier_t *scs_carrier = scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0];
  int band = *scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0];
  const frequency_range_t freq_range = get_freq_range_from_band(band);
  const int scs = scs_carrier->subcarrierSpacing;
  const int bw_size = scs_carrier->carrierBandwidth;

  NR_FeatureSets_t *fs = uecap ? uecap->featureSets : NULL;
  if (fs) {
    const int bw_mhz = get_supported_bw_mhz(freq_range, get_supported_band_index(scs, freq_range, bw_size));
    // go through UL feature sets and look for one with current SCS
    for (int i = 0; i < fs->featureSetsDownlinkPerCC->list.count; i++) {
      NR_FeatureSetDownlinkPerCC_t *dl_fs = fs->featureSetsDownlinkPerCC->list.array[i];
      if (scs == dl_fs->supportedSubcarrierSpacingDL &&
          supported_bw_comparison(bw_mhz, &dl_fs->supportedBandwidthDL, dl_fs->channelBW_90mhz) &&
          dl_fs->maxNumberMIMO_LayersPDSCH) {
        long ue_supported_layers = (2 << *dl_fs->maxNumberMIMO_LayersPDSCH);
        if (maxMIMO_layers == -1) 
          *pdsch_servingcellconfig->ext1->maxMIMO_Layers = NR_MAX_SUPPORTED_DL_LAYERS < ue_supported_layers ? NR_MAX_SUPPORTED_DL_LAYERS : ue_supported_layers;
        else 
          *pdsch_servingcellconfig->ext1->maxMIMO_Layers = maxMIMO_layers < ue_supported_layers ? maxMIMO_layers : ue_supported_layers;
        return;
      }
    }
  }
  *pdsch_servingcellconfig->ext1->maxMIMO_Layers = 2;
}

// TODO: Implement to b_SRS = 1 and b_SRS = 2
long rrc_get_max_nr_csrs(const int max_rbs, const long b_SRS) {

  if(b_SRS>0) {
    LOG_E(NR_RRC,"rrc_get_max_nr_csrs(): Not implemented yet for b_SRS>0\n");
    return 0; // This c_srs is always valid
  }

  const uint16_t m_SRS[64] = { 4, 8, 12, 16, 16, 20, 24, 24, 28, 32, 36, 40, 48, 48, 52, 56, 60, 64, 72, 72, 76, 80, 88,
                               96, 96, 104, 112, 120, 120, 120, 128, 128, 128, 132, 136, 144, 144, 144, 144, 152, 160,
                               160, 160, 168, 176, 184, 192, 192, 192, 192, 208, 216, 224, 240, 240, 240, 240, 256, 256,
                               256, 264, 272, 272, 272 };

  long c_srs = 0;
  uint16_t m = 4;
  for(int c = 1; c<64; c++) {
    if(m_SRS[c]>m && m_SRS[c]<max_rbs) {
      c_srs = c;
      m = m_SRS[c];
    }
  }

  return c_srs;
}

static struct NR_SRS_Resource__resourceType__periodic *configure_periodic_srs(const NR_ServingCellConfigCommon_t *scc,
                                                                              const int uid)
{
  frame_structure_t *fs = &RC.nrmac[0]->frame_structure;
  int offset = get_ul_slot_offset(fs, uid, false); // only full UL slots for SRS

  AssertFatal(offset < 2560, "Cannot allocate SRS configuration for uid %d, not enough resources\n", uid);
  const int ideal_period = set_ideal_period(false);

  struct NR_SRS_Resource__resourceType__periodic *periodic_srs = calloc(1,sizeof(*periodic_srs));
  if (check_periodicity(4, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl4;
    periodic_srs->periodicityAndOffset_p.choice.sl4 = offset;
  }
  else if (check_periodicity(5, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl5;
    periodic_srs->periodicityAndOffset_p.choice.sl5 = offset;
  }
  else if (check_periodicity(8, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl8;
    periodic_srs->periodicityAndOffset_p.choice.sl8 = offset;
  }
  else if (check_periodicity(10, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl10;
    periodic_srs->periodicityAndOffset_p.choice.sl10 = offset;
  }
  else if (check_periodicity(16, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl16;
    periodic_srs->periodicityAndOffset_p.choice.sl16 = offset;
  }
  else if (check_periodicity(20, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl20;
    periodic_srs->periodicityAndOffset_p.choice.sl20 = offset;
  }
  else if (check_periodicity(32, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl32;
    periodic_srs->periodicityAndOffset_p.choice.sl32 = offset;
  }
  else if (check_periodicity(40, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl40;
    periodic_srs->periodicityAndOffset_p.choice.sl40 = offset;
  }
  else if (check_periodicity(64, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl64;
    periodic_srs->periodicityAndOffset_p.choice.sl64 = offset;
  }
  else if (check_periodicity(80, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl80;
    periodic_srs->periodicityAndOffset_p.choice.sl80 = offset;
  }
  else if (check_periodicity(160, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl160;
    periodic_srs->periodicityAndOffset_p.choice.sl160 = offset;
  }
  else if (check_periodicity(320, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl320;
    periodic_srs->periodicityAndOffset_p.choice.sl320 = offset;
  }
  else if (check_periodicity(640, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl640;
    periodic_srs->periodicityAndOffset_p.choice.sl640 = offset;
  }
  else if (check_periodicity(1280, ideal_period, fs)) {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl1280;
    periodic_srs->periodicityAndOffset_p.choice.sl1280 = offset;
  }
  else {
    periodic_srs->periodicityAndOffset_p.present = NR_SRS_PeriodicityAndOffset_PR_sl2560;
    periodic_srs->periodicityAndOffset_p.choice.sl2560 = offset;
  }
  return periodic_srs;
}

static NR_SRS_ResourceSet_t *get_srs_resourceset(const int resset_id,
                                                 const int res_id,
                                                 const long usage,
                                                 const int minRXTXTIME,
                                                 int do_srs)
{
  NR_SRS_ResourceSet_t *srs_resset = calloc_or_fail(1, sizeof(*srs_resset));
  srs_resset->srs_ResourceSetId = resset_id;
  srs_resset->srs_ResourceIdList = calloc_or_fail(1, sizeof(*srs_resset->srs_ResourceIdList));
  NR_SRS_ResourceId_t *srs_resset_id = calloc_or_fail(1, sizeof(*srs_resset_id));
  *srs_resset_id = res_id;
  asn1cSeqAdd(&srs_resset->srs_ResourceIdList->list, srs_resset_id);
  if (do_srs) {
    srs_resset->resourceType.present = NR_SRS_ResourceSet__resourceType_PR_periodic;
    srs_resset->resourceType.choice.periodic = calloc_or_fail(1, sizeof(*srs_resset->resourceType.choice.periodic));
    srs_resset->resourceType.choice.periodic->associatedCSI_RS = NULL;
  } else {
    srs_resset->resourceType.present = NR_SRS_ResourceSet__resourceType_PR_aperiodic;
    srs_resset->resourceType.choice.aperiodic = calloc_or_fail(1, sizeof(*srs_resset->resourceType.choice.aperiodic));
    srs_resset->resourceType.choice.aperiodic->aperiodicSRS_ResourceTrigger = 1;
    srs_resset->resourceType.choice.aperiodic->csi_RS = NULL;
    srs_resset->resourceType.choice.aperiodic->slotOffset =
        calloc_or_fail(1, sizeof(*srs_resset->resourceType.choice.aperiodic->slotOffset));
    *srs_resset->resourceType.choice.aperiodic->slotOffset = minRXTXTIME;
    srs_resset->resourceType.choice.aperiodic->ext1 = NULL;
  }
  srs_resset->usage = usage;
  srs_resset->alpha = calloc_or_fail(1, sizeof(*srs_resset->alpha));
  *srs_resset->alpha = NR_Alpha_alpha1;
  srs_resset->p0 = calloc_or_fail(1, sizeof(*srs_resset->p0));
  *srs_resset->p0 = -80;
  srs_resset->pathlossReferenceRS = NULL;
  srs_resset->srs_PowerControlAdjustmentStates = NULL;
  return srs_resset;
}

static NR_SRS_Resource_t *get_srs_resource(const NR_ServingCellConfigCommon_t *scc,
                                           const NR_UE_NR_Capability_t *uecap,
                                           const int curr_bwp,
                                           const int uid,
                                           const int res_id,
                                           const long maxMIMO_Layers,
                                           const NR_SRS_Resource__transmissionComb_PR tx_comb,
                                           int do_srs)
{
  NR_SRS_Resource_t *srs_res = calloc_or_fail(1, sizeof(*srs_res));
  srs_res->srs_ResourceId = res_id;
  srs_res->nrofSRS_Ports = NR_SRS_Resource__nrofSRS_Ports_port1;
  if (do_srs) {
    long nrofSRS_Ports = 1;
    if (uecap && uecap->featureSets && uecap->featureSets->featureSetsUplink
        && uecap->featureSets->featureSetsUplink->list.count > 0) {
      NR_FeatureSetUplink_t *ul_feature_setup = uecap->featureSets->featureSetsUplink->list.array[0];
      switch (ul_feature_setup->supportedSRS_Resources->maxNumberSRS_Ports_PerResource) {
        case NR_SRS_Resources__maxNumberSRS_Ports_PerResource_n1:
          nrofSRS_Ports = 1;
          break;
        case NR_SRS_Resources__maxNumberSRS_Ports_PerResource_n2:
          nrofSRS_Ports = 2;
          break;
        case NR_SRS_Resources__maxNumberSRS_Ports_PerResource_n4:
          nrofSRS_Ports = 4;
          break;
        default:
          LOG_E(NR_RRC,
                "Max Number of SRS Ports Per Resource %ld is invalid!\n",
                ul_feature_setup->supportedSRS_Resources->maxNumberSRS_Ports_PerResource);
      }
      nrofSRS_Ports = min(nrofSRS_Ports, maxMIMO_Layers);
      switch (nrofSRS_Ports) {
        case 1:
          srs_res->nrofSRS_Ports = NR_SRS_Resource__nrofSRS_Ports_port1;
          break;
        case 2:
          srs_res->nrofSRS_Ports = NR_SRS_Resource__nrofSRS_Ports_ports2;
          break;
        case 4:
          srs_res->nrofSRS_Ports = NR_SRS_Resource__nrofSRS_Ports_ports4;
          break;
        default:
          LOG_E(NR_RRC,
                "Number of SRS Ports Per Resource %ld is invalid!\n",
                ul_feature_setup->supportedSRS_Resources->maxNumberSRS_Ports_PerResource);
      }
    }
    LOG_I(NR_RRC, "SRS configured with %d ports\n", 1 << srs_res->nrofSRS_Ports);
  }
  srs_res->ptrs_PortIndex = NULL;
  srs_res->transmissionComb.present = tx_comb;
  switch (tx_comb) {
    case NR_SRS_Resource__transmissionComb_PR_n2:
      srs_res->transmissionComb.choice.n2 = calloc_or_fail(1, sizeof(*srs_res->transmissionComb.choice.n2));
      srs_res->transmissionComb.choice.n2->combOffset_n2 = 0;
      srs_res->transmissionComb.choice.n2->cyclicShift_n2 = 0;
      break;
    case NR_SRS_Resource__transmissionComb_PR_n4:
      srs_res->transmissionComb.choice.n4 = calloc_or_fail(1, sizeof(*srs_res->transmissionComb.choice.n4));
      srs_res->transmissionComb.choice.n4->combOffset_n4 = 0;
      srs_res->transmissionComb.choice.n4->cyclicShift_n4 = 0;
      break;
    default:
      AssertFatal(1 == 0, "Invalid transmission comb %d\n", tx_comb);
  }
  srs_res->resourceMapping.startPosition = 1;
  srs_res->resourceMapping.nrofSymbols = NR_SRS_Resource__resourceMapping__nrofSymbols_n1;
  srs_res->resourceMapping.repetitionFactor = NR_SRS_Resource__resourceMapping__repetitionFactor_n1;
  srs_res->freqDomainPosition = 0;
  srs_res->freqDomainShift = 0;
  srs_res->freqHopping.b_SRS = 0;
  srs_res->freqHopping.b_hop = 0;
  srs_res->freqHopping.c_SRS = rrc_get_max_nr_csrs(curr_bwp, srs_res->freqHopping.b_SRS);
  srs_res->groupOrSequenceHopping = NR_SRS_Resource__groupOrSequenceHopping_neither;
  if (do_srs) {
    srs_res->resourceType.present = NR_SRS_Resource__resourceType_PR_periodic;
    srs_res->resourceType.choice.periodic = configure_periodic_srs(scc, uid);
  } else {
    srs_res->resourceType.present = NR_SRS_Resource__resourceType_PR_aperiodic;
    srs_res->resourceType.choice.aperiodic = calloc_or_fail(1, sizeof(*srs_res->resourceType.choice.aperiodic));
  }
  srs_res->sequenceId = 40;
  srs_res->spatialRelationInfo = calloc_or_fail(1, sizeof(*srs_res->spatialRelationInfo));
  srs_res->spatialRelationInfo->servingCellId = NULL;
  // TODO include CSI as reference signal when BWPs are handled properly
  srs_res->spatialRelationInfo->referenceSignal.present = NR_SRS_SpatialRelationInfo__referenceSignal_PR_ssb_Index;
  srs_res->spatialRelationInfo->referenceSignal.choice.ssb_Index = 0;
  return srs_res;
}

static NR_SetupRelease_SRS_Config_t *get_config_srs(const NR_ServingCellConfigCommon_t *scc,
                                                    const NR_UE_NR_Capability_t *uecap,
                                                    const int curr_bwp,
                                                    const int uid,
                                                    const int res_id,
                                                    const long maxMIMO_Layers,
                                                    const int minRXTXTIME,
                                                    int do_srs)
{
  NR_SetupRelease_SRS_Config_t *setup_release_srs_Config = calloc_or_fail(1, sizeof(*setup_release_srs_Config));
  setup_release_srs_Config->present = NR_SetupRelease_SRS_Config_PR_setup;
  setup_release_srs_Config->choice.setup = calloc_or_fail(1, sizeof(*setup_release_srs_Config->choice.setup));
  NR_SRS_Config_t *srs_Config = setup_release_srs_Config->choice.setup;

  srs_Config->srs_ResourceToAddModList = calloc_or_fail(1, sizeof(*srs_Config->srs_ResourceToAddModList));
  NR_SRS_Resource_t *srs_res0 =
      get_srs_resource(scc, uecap, curr_bwp, uid, res_id, maxMIMO_Layers, NR_SRS_Resource__transmissionComb_PR_n2, do_srs);
  asn1cSeqAdd(&srs_Config->srs_ResourceToAddModList->list, srs_res0);

  srs_Config->srs_ResourceSetToAddModList = calloc_or_fail(1, sizeof(*srs_Config->srs_ResourceSetToAddModList));
  NR_SRS_ResourceSet_t *srs_resset0 = get_srs_resourceset(res_id, res_id, NR_SRS_ResourceSet__usage_codebook, minRXTXTIME, do_srs);
  asn1cSeqAdd(&srs_Config->srs_ResourceSetToAddModList->list, srs_resset0);

  srs_Config->srs_ResourceSetToReleaseList = NULL;
  srs_Config->srs_ResourceToReleaseList = NULL;

  return setup_release_srs_Config;
}

static int get_SupportedBandwidth_fr1_bw_index(int bw_index)
{
  int val = -1;

  switch (bw_index) {
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz5:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz10:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz15:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz20:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz25:
      val = bw_index;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz30:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz35:
      val = NR_SupportedBandwidth__fr1_mhz30;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz40:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz45:
      val = NR_SupportedBandwidth__fr1_mhz40;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz50:
      val = NR_SupportedBandwidth__fr1_mhz50;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz60:
      val = NR_SupportedBandwidth__fr1_mhz60;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz70:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz80:
      val = NR_SupportedBandwidth__fr1_mhz80;
      break;
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz90:
    case NR_SupportedBandwidth_v1700__fr1_r17_mhz100:
      val = NR_SupportedBandwidth__fr1_mhz100;
      break;
    default:
      AssertFatal(1==0, "Invalid bw index\n");
  }

  return val;
}


void prepare_sim_uecap(NR_UE_NR_Capability_t *cap,
                       NR_ServingCellConfigCommon_t *scc,
                       int numerology,
                       int rbsize,
                       int mcs_table_dl,
                       int mcs_table_ul)
{
  NR_Phy_Parameters_t *phy_Parameters = &cap->phy_Parameters;
  int band = *scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0];
  NR_BandNR_t *nr_bandnr = calloc(1, sizeof(NR_BandNR_t));
  nr_bandnr->bandNR = band;
  asn1cSeqAdd(&cap->rf_Parameters.supportedBandListNR.list,
                   nr_bandnr);
  NR_BandNR_t *bandNRinfo = cap->rf_Parameters.supportedBandListNR.list.array[0];

  if (mcs_table_ul == 1) {
    bandNRinfo->pusch_256QAM = calloc(1,sizeof(*bandNRinfo->pusch_256QAM));
    *bandNRinfo->pusch_256QAM = NR_BandNR__pusch_256QAM_supported;
  }
  if (mcs_table_dl == 1) {
    const frequency_range_t freq_range = get_freq_range_from_band(band);
    if (freq_range == FR2) {
      bandNRinfo->pdsch_256QAM_FR2 = calloc(1, sizeof(*bandNRinfo->pdsch_256QAM_FR2));
      *bandNRinfo->pdsch_256QAM_FR2 = NR_BandNR__pdsch_256QAM_FR2_supported;
    }
    else{
      phy_Parameters->phy_ParametersFR1 = calloc(1, sizeof(*phy_Parameters->phy_ParametersFR1));
      NR_Phy_ParametersFR1_t *phy_fr1 = phy_Parameters->phy_ParametersFR1;
      phy_fr1->pdsch_256QAM_FR1 = calloc(1, sizeof(*phy_fr1->pdsch_256QAM_FR1));
      *phy_fr1->pdsch_256QAM_FR1 = NR_Phy_ParametersFR1__pdsch_256QAM_FR1_supported;
    }
    cap->featureSets = calloc(1, sizeof(*cap->featureSets));
    NR_FeatureSets_t *fs=cap->featureSets;
    fs->featureSetsDownlinkPerCC = calloc(1, sizeof(*fs->featureSetsDownlinkPerCC));
    NR_FeatureSetDownlinkPerCC_t *fs_cc = calloc(1, sizeof(*fs_cc));
    fs_cc->supportedSubcarrierSpacingDL = numerology;
    int bw_index = get_supported_band_index(numerology, freq_range, rbsize);
    int bw = get_supported_bw_mhz(freq_range, bw_index);
    if (bw == 90) // 90MHz
      fs_cc->channelBW_90mhz = calloc(1, sizeof(*fs_cc->channelBW_90mhz));
    if(freq_range == FR2) {
      fs_cc->supportedBandwidthDL.present = NR_SupportedBandwidth_PR_fr2;
      fs_cc->supportedBandwidthDL.choice.fr2 = bw_index;
    }
    else{
      fs_cc->supportedBandwidthDL.present = NR_SupportedBandwidth_PR_fr1;
      fs_cc->supportedBandwidthDL.choice.fr1 = get_SupportedBandwidth_fr1_bw_index(bw_index);
    }
    fs_cc->maxNumberMIMO_LayersPDSCH = calloc(1, sizeof(*fs_cc->maxNumberMIMO_LayersPDSCH));
    *fs_cc->maxNumberMIMO_LayersPDSCH = NR_MIMO_LayersDL_fourLayers;
    fs_cc->supportedModulationOrderDL = calloc(1, sizeof(*fs_cc->supportedModulationOrderDL));
    *fs_cc->supportedModulationOrderDL = NR_ModulationOrder_qam256;
    asn1cSeqAdd(&fs->featureSetsDownlinkPerCC->list, fs_cc);

    if (bw == 35 || bw == 45 || bw == 70) {
      fs->ext6 = calloc(1, sizeof(*fs->ext6));
      fs->ext6->featureSetsDownlinkPerCC_v1700 = calloc(1, sizeof(*fs->ext6->featureSetsDownlinkPerCC_v1700));
      NR_FeatureSetDownlinkPerCC_v1700_t *fs_dlcc_v1700 = calloc(1, sizeof(*fs_dlcc_v1700));
      fs_dlcc_v1700->supportedBandwidthDL_v1710 = calloc(1, sizeof(*fs_dlcc_v1700->supportedBandwidthDL_v1710));
      fs_dlcc_v1700->supportedBandwidthDL_v1710->present = NR_SupportedBandwidth_v1700_PR_fr1_r17;
      fs_dlcc_v1700->supportedBandwidthDL_v1710->choice.fr1_r17 = bw_index;
      asn1cSeqAdd(&fs->ext6->featureSetsDownlinkPerCC_v1700->list, fs_dlcc_v1700);
    }
  }

  phy_Parameters->phy_ParametersFRX_Diff = calloc(1, sizeof(*phy_Parameters->phy_ParametersFRX_Diff));
  phy_Parameters->phy_ParametersFRX_Diff->pucch_F0_2WithoutFH = NULL;

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_UE_NR_Capability, cap);
  }
}

void nr_rrc_config_dl_tda(struct NR_PDSCH_TimeDomainResourceAllocationList *pdsch_TimeDomainAllocationList,
                          frame_type_t frame_type,
                          NR_TDD_UL_DL_ConfigCommon_t *tdd_UL_DL_ConfigurationCommon,
                          int curr_bwp) {

  // coreset duration setting to be improved in the framework of RRC harmonization, potentially using a common function
  int len_coreset = 1;
  if (curr_bwp < 48)
    len_coreset = 2;
  // setting default TDA for DL with TDA index 0
  struct NR_PDSCH_TimeDomainResourceAllocation *timedomainresourceallocation = CALLOC(1,sizeof(NR_PDSCH_TimeDomainResourceAllocation_t));
  // k0: Slot offset between DCI and its scheduled PDSCH (see TS 38.214 clause 5.1.2.1) When the field is absent the UE applies the value 0.
  //timedomainresourceallocation->k0 = calloc(1,sizeof(*timedomainresourceallocation->k0));
  //*timedomainresourceallocation->k0 = 0;
  timedomainresourceallocation->mappingType = NR_PDSCH_TimeDomainResourceAllocation__mappingType_typeA;
  timedomainresourceallocation->startSymbolAndLength = get_SLIV(len_coreset,14-len_coreset); // basic slot configuration starting in symbol 1 til the end of the slot
  asn1cSeqAdd(&pdsch_TimeDomainAllocationList->list, timedomainresourceallocation);
  // setting TDA for CSI-RS symbol with index 1
  struct NR_PDSCH_TimeDomainResourceAllocation *timedomainresourceallocation1 = CALLOC(1,sizeof(NR_PDSCH_TimeDomainResourceAllocation_t));
  timedomainresourceallocation1->mappingType = NR_PDSCH_TimeDomainResourceAllocation__mappingType_typeA;
  timedomainresourceallocation1->startSymbolAndLength = get_SLIV(len_coreset,14-len_coreset-1); // 1 symbol CSI-RS
  asn1cSeqAdd(&pdsch_TimeDomainAllocationList->list, timedomainresourceallocation1);
  if(frame_type==TDD) {
    // TDD
    if(tdd_UL_DL_ConfigurationCommon) {
      int dl_symb = 0;
      if (tdd_UL_DL_ConfigurationCommon->pattern2 && tdd_UL_DL_ConfigurationCommon->pattern2->nrofDownlinkSymbols)
        AssertFatal(tdd_UL_DL_ConfigurationCommon->pattern2->nrofDownlinkSymbols == tdd_UL_DL_ConfigurationCommon->pattern1.nrofDownlinkSymbols,
                    "nrofDownlinkSymbols in pattern1 %ld and pattern2 %ld must be the same in current implementation\n",
                    tdd_UL_DL_ConfigurationCommon->pattern2->nrofDownlinkSymbols,
                    tdd_UL_DL_ConfigurationCommon->pattern1.nrofDownlinkSymbols);
      if (tdd_UL_DL_ConfigurationCommon->pattern1.nrofDownlinkSymbols != 0) {
        dl_symb = tdd_UL_DL_ConfigurationCommon->pattern1.nrofDownlinkSymbols;
      } else if (tdd_UL_DL_ConfigurationCommon->pattern2) {
        dl_symb = tdd_UL_DL_ConfigurationCommon->pattern2->nrofDownlinkSymbols;
      }
      if(dl_symb > 1) {
        // mixed slot TDA with TDA index 2
        struct NR_PDSCH_TimeDomainResourceAllocation *timedomainresourceallocation2 = CALLOC(1,sizeof(NR_PDSCH_TimeDomainResourceAllocation_t));
        timedomainresourceallocation2->mappingType = NR_PDSCH_TimeDomainResourceAllocation__mappingType_typeA;
        timedomainresourceallocation2->startSymbolAndLength = get_SLIV(len_coreset,dl_symb-len_coreset); // mixed slot configuration starting in symbol 1 til the end of the dl allocation
        asn1cSeqAdd(&pdsch_TimeDomainAllocationList->list, timedomainresourceallocation2);
      }
    }
  }
}

static struct NR_PUSCH_TimeDomainResourceAllocation *set_TimeDomainResourceAllocation(const int k2, uint8_t index, int ul_symb)
{
  struct NR_PUSCH_TimeDomainResourceAllocation *puschTdrAlloc = calloc_or_fail(1, sizeof(*puschTdrAlloc));
  puschTdrAlloc->k2 = calloc_or_fail(1, sizeof(*puschTdrAlloc->k2));
  *puschTdrAlloc->k2 = k2;
  puschTdrAlloc->mappingType = NR_PUSCH_TimeDomainResourceAllocation__mappingType_typeB;
  switch (index) {
    case 0:
      puschTdrAlloc->startSymbolAndLength = get_SLIV(0, 13);
      break;
    case 1:
      puschTdrAlloc->startSymbolAndLength = get_SLIV(0, 12);
      break;
    case 2:
      // UL TDA index 2 for mixed slot (TDD)
      puschTdrAlloc->startSymbolAndLength =
          get_SLIV(NR_NUMBER_OF_SYMBOLS_PER_SLOT - ul_symb, ul_symb - 1); // starting in fist ul symbol til the last but one
      break;
    case 3: {
      // UL TDA index 3 for msg3 in the mixed slot (TDD)
      int no_mix_slot = ul_symb < 3 ? 1 : 0; // we need at least 2 symbols for scheduling Msg3
      *puschTdrAlloc->k2 += no_mix_slot;
      if (no_mix_slot)
        puschTdrAlloc->startSymbolAndLength = get_SLIV(0, 13); // full allocation if there is no mixed slot
      else
        puschTdrAlloc->startSymbolAndLength =
            get_SLIV(NR_NUMBER_OF_SYMBOLS_PER_SLOT - ul_symb, ul_symb - 1); // starting in fist ul symbol til the last but one
      }
      break;
    default:
      break;
  }
  return puschTdrAlloc;
}

void nr_rrc_config_ul_tda(NR_ServingCellConfigCommon_t *scc, int min_fb_delay)
{
  //TODO change to accomodate for SRS

  const NR_PUSCH_TimeDomainResourceAllocationList_t *tda =
      scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList;
  AssertFatal(tda->list.count == 0, "already have pusch_TimeDomainAllocationList members\n");

  const int k2 = min_fb_delay;
  int mu = scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.subcarrierSpacing;
  struct NR_SetupRelease_PUSCH_ConfigCommon *pusch_ConfigCommon = scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon;

  // UL TDA index 0 is basic slot configuration starting in symbol 0 til the last but one symbol
  asn1cSeqAdd(&pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList->list, set_TimeDomainResourceAllocation(k2, 0, 0));

  // UL TDA index 1 in case of SRS
  asn1cSeqAdd(&pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList->list, set_TimeDomainResourceAllocation(k2, 1, 0));

  if (scc->tdd_UL_DL_ConfigurationCommon) {
    int ul_symb = 0;
    NR_TDD_UL_DL_Pattern_t *p1 = &scc->tdd_UL_DL_ConfigurationCommon->pattern1;
    NR_TDD_UL_DL_Pattern_t *p2 = scc->tdd_UL_DL_ConfigurationCommon->pattern2;
    if (p2 && p2->nrofUplinkSymbols)
      AssertFatal(p2->nrofUplinkSymbols == p1->nrofUplinkSymbols,
                  "nrofDownlinkSymbols in pattern1 %ld and pattern2 %ld must be the same in current implementation\n",
                  p1->nrofUplinkSymbols,
                  p2->nrofUplinkSymbols);
    if (p1->nrofUplinkSymbols) {
      ul_symb = p1->nrofUplinkSymbols;
    } else if (p2) {
      ul_symb = p1->nrofUplinkSymbols;
    }
    if (ul_symb>1) {
      // UL TDA index 2 for mixed slot (TDD)
      asn1cSeqAdd(&pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList->list,
                  set_TimeDomainResourceAllocation(k2, 2, ul_symb));
    }
    // UL TDA index 3 for msg3 in the mixed slot (TDD)
    int tdd_period_idx = get_tdd_period_idx(scc->tdd_UL_DL_ConfigurationCommon);
    int nb_periods_per_frame = get_nb_periods_per_frame(tdd_period_idx);
    int nb_slots_per_period = ((1 << mu) * 10) / nb_periods_per_frame;
    int k2_msg3 = nb_slots_per_period - get_delta_for_k2(mu);
    struct NR_PUSCH_TimeDomainResourceAllocation *puschTdrAllocMsg3 = set_TimeDomainResourceAllocation(k2_msg3, 3, ul_symb);
    if (*puschTdrAllocMsg3->k2 < min_fb_delay)
      *puschTdrAllocMsg3->k2 += nb_slots_per_period;
    AssertFatal(*puschTdrAllocMsg3->k2 < 33,
                "Computed k2 for msg3 %ld is larger than the range allowed by RRC (0..32)\n",
                *puschTdrAllocMsg3->k2);
    asn1cSeqAdd(&pusch_ConfigCommon->choice.setup->pusch_TimeDomainAllocationList->list, puschTdrAllocMsg3);
  }
}

static void set_dl_DataToUL_ACK(NR_PUCCH_Config_t *pucch_Config, int min_feedback_time, NR_SubcarrierSpacing_t subcarrierSpacing)
{
  pucch_Config->dl_DataToUL_ACK = calloc(1,sizeof(*pucch_Config->dl_DataToUL_ACK));
  long *delay[8];
  for (int i = 0; i < 8; i++) {
    int curr_delay = i + min_feedback_time;
    delay[i] = calloc(1,sizeof(*delay[i]));
    *delay[i] = curr_delay;
    asn1cSeqAdd(&pucch_Config->dl_DataToUL_ACK->list,delay[i]);
  }
}

// PUCCH resource set 0 for configuration with O_uci <= 2 bits and/or a positive or negative SR (section 9.2.1 of 38.213)
static void config_pucch_resset0(NR_PUCCH_Config_t *pucch_Config,
                                 int uid,
                                 int curr_bwp,
                                 int num_pucch2,
                                 const NR_UE_NR_Capability_t *uecap)
{
  NR_PUCCH_ResourceSet_t *pucchresset = calloc(1,sizeof(*pucchresset));
  pucchresset->pucch_ResourceSetId = 0;
  NR_PUCCH_ResourceId_t *pucchid = calloc(1,sizeof(*pucchid));
  *pucchid = 0;
  asn1cSeqAdd(&pucchresset->resourceList.list,pucchid);
  pucchresset->maxPayloadSize = NULL;

  if(uecap) {
    long *pucch_F0_2WithoutFH = uecap->phy_Parameters.phy_ParametersFRX_Diff->pucch_F0_2WithoutFH;
    AssertFatal(pucch_F0_2WithoutFH == NULL,"UE does not support PUCCH F0 without frequency hopping. Current configuration is without FH\n");
  }

  NR_PUCCH_Resource_t *pucchres0 = calloc(1,sizeof(*pucchres0));
  pucchres0->pucch_ResourceId = *pucchid;
  pucchres0->startingPRB = (PUCCH2_SIZE * num_pucch2) + uid;
  AssertFatal(pucchres0->startingPRB < curr_bwp, "Not enough resources in current BWP (size %d) to allocate uid %d\n", curr_bwp, uid);
  pucchres0->intraSlotFrequencyHopping = NULL;
  pucchres0->secondHopPRB = NULL;
  pucchres0->format.present = NR_PUCCH_Resource__format_PR_format0;
  pucchres0->format.choice.format0 = calloc(1,sizeof(*pucchres0->format.choice.format0));
  pucchres0->format.choice.format0->initialCyclicShift = 0;
  pucchres0->format.choice.format0->nrofSymbols = 1;
  pucchres0->format.choice.format0->startingSymbolIndex = 13;
  asn1cSeqAdd(&pucch_Config->resourceToAddModList->list,pucchres0);

  asn1cSeqAdd(&pucch_Config->resourceSetToAddModList->list,pucchresset);
}


// PUCCH resource set 1 for configuration with O_uci > 2 bits (currently format2)
static void config_pucch_resset1(NR_PUCCH_Config_t *pucch_Config,
                                 int uid,
                                 int num_pucch2,
                                 const NR_UE_NR_Capability_t *uecap)
{
  NR_PUCCH_ResourceSet_t *pucchresset=calloc(1,sizeof(*pucchresset));
  pucchresset->pucch_ResourceSetId = 1;
  NR_PUCCH_ResourceId_t *pucchressetid=calloc(1,sizeof(*pucchressetid));
  *pucchressetid = 2;
  asn1cSeqAdd(&pucchresset->resourceList.list,pucchressetid);
  pucchresset->maxPayloadSize = NULL;

  if(uecap) {
    long *pucch_F0_2WithoutFH = uecap->phy_Parameters.phy_ParametersFRX_Diff->pucch_F0_2WithoutFH;
    AssertFatal(pucch_F0_2WithoutFH == NULL,"UE does not support PUCCH F2 without frequency hopping. Current configuration is without FH\n");
  }

  NR_PUCCH_Resource_t *pucchres2 = calloc(1,sizeof(*pucchres2));
  pucchres2->pucch_ResourceId = *pucchressetid;
  pucchres2->startingPRB = PUCCH2_SIZE * (uid % num_pucch2);
  pucchres2->intraSlotFrequencyHopping = NULL;
  pucchres2->secondHopPRB = NULL;
  pucchres2->format.present = NR_PUCCH_Resource__format_PR_format2;
  pucchres2->format.choice.format2 = calloc(1,sizeof(*pucchres2->format.choice.format2));
  pucchres2->format.choice.format2->nrofPRBs = PUCCH2_SIZE;
  pucchres2->format.choice.format2->nrofSymbols = 1;
  pucchres2->format.choice.format2->startingSymbolIndex = 13;
  asn1cSeqAdd(&pucch_Config->resourceToAddModList->list,pucchres2);

  asn1cSeqAdd(&pucch_Config->resourceSetToAddModList->list,pucchresset);

  pucch_Config->format2 = calloc(1,sizeof(*pucch_Config->format2));
  pucch_Config->format2->present = NR_SetupRelease_PUCCH_FormatConfig_PR_setup;
  NR_PUCCH_FormatConfig_t *pucchfmt2 = calloc(1,sizeof(*pucchfmt2));
  pucch_Config->format2->choice.setup = pucchfmt2;
  pucchfmt2->interslotFrequencyHopping = NULL;
  pucchfmt2->additionalDMRS = NULL;
  pucchfmt2->maxCodeRate = calloc(1,sizeof(*pucchfmt2->maxCodeRate));
  *pucchfmt2->maxCodeRate = NR_PUCCH_MaxCodeRate_zeroDot15;
  pucchfmt2->nrofSlots = NULL;
  pucchfmt2->pi2BPSK = NULL;

  // to check UE capabilities for that in principle
  pucchfmt2->simultaneousHARQ_ACK_CSI = calloc(1,sizeof(*pucchfmt2->simultaneousHARQ_ACK_CSI));
  *pucchfmt2->simultaneousHARQ_ACK_CSI = NR_PUCCH_FormatConfig__simultaneousHARQ_ACK_CSI_true;
}

void set_pucch_power_config(NR_PUCCH_Config_t *pucch_Config, int do_csirs) {

  pucch_Config->pucch_PowerControl = calloc(1,sizeof(*pucch_Config->pucch_PowerControl));
  NR_P0_PUCCH_t *p00 = calloc(1,sizeof(*p00));
  p00->p0_PUCCH_Id = 1;
  p00->p0_PUCCH_Value = 0;
  pucch_Config->pucch_PowerControl->p0_Set = calloc(1,sizeof(*pucch_Config->pucch_PowerControl->p0_Set));
  asn1cSeqAdd(&pucch_Config->pucch_PowerControl->p0_Set->list,p00);

  pucch_Config->pucch_PowerControl->pathlossReferenceRSs = calloc(1,sizeof(*pucch_Config->pucch_PowerControl->pathlossReferenceRSs));
  struct NR_PUCCH_PathlossReferenceRS *PL_ref_RS = calloc(1,sizeof(*PL_ref_RS));
  PL_ref_RS->pucch_PathlossReferenceRS_Id = 0;
  // TODO include CSI as reference signal when BWPs are handled properly
  PL_ref_RS->referenceSignal.present = NR_PUCCH_PathlossReferenceRS__referenceSignal_PR_ssb_Index;
  PL_ref_RS->referenceSignal.choice.ssb_Index = 0;
  asn1cSeqAdd(&pucch_Config->pucch_PowerControl->pathlossReferenceRSs->list,PL_ref_RS);

  pucch_Config->pucch_PowerControl->deltaF_PUCCH_f0 = calloc(1,sizeof(*pucch_Config->pucch_PowerControl->deltaF_PUCCH_f0));
  *pucch_Config->pucch_PowerControl->deltaF_PUCCH_f0 = 0;
  pucch_Config->pucch_PowerControl->deltaF_PUCCH_f2 = calloc(1,sizeof(*pucch_Config->pucch_PowerControl->deltaF_PUCCH_f2));
  *pucch_Config->pucch_PowerControl->deltaF_PUCCH_f2 = 0;

  pucch_Config->spatialRelationInfoToAddModList = calloc(1,sizeof(*pucch_Config->spatialRelationInfoToAddModList));
  pucch_Config->spatialRelationInfoToReleaseList=NULL;
  NR_PUCCH_SpatialRelationInfo_t *pucchspatial = calloc(1,sizeof(*pucchspatial));
  pucchspatial->pucch_SpatialRelationInfoId = 1;
  pucchspatial->servingCellId = NULL;
  // TODO include CSI as reference signal when BWPs are handled properly
  pucchspatial->referenceSignal.present = NR_PUCCH_SpatialRelationInfo__referenceSignal_PR_ssb_Index;
  pucchspatial->referenceSignal.choice.ssb_Index = 0;

  pucchspatial->pucch_PathlossReferenceRS_Id = PL_ref_RS->pucch_PathlossReferenceRS_Id;
  pucchspatial->p0_PUCCH_Id = p00->p0_PUCCH_Id;
  pucchspatial->closedLoopIndex = NR_PUCCH_SpatialRelationInfo__closedLoopIndex_i0;
  asn1cSeqAdd(&pucch_Config->spatialRelationInfoToAddModList->list,pucchspatial);
}

static void set_SR_periodandoffset(NR_SchedulingRequestResourceConfig_t *schedulingRequestResourceConfig, const NR_ServingCellConfigCommon_t *scc, int scs)
{
  const frame_structure_t *fs = &RC.nrmac[0]->frame_structure;
  int sr_slot = 1; // in FDD SR in slot 1
  if (fs->frame_type == TDD)
    sr_slot = get_first_ul_slot(fs, true);

  schedulingRequestResourceConfig->periodicityAndOffset = calloc(1,sizeof(*schedulingRequestResourceConfig->periodicityAndOffset));

  if(sr_slot < 10 && scs < NR_SubcarrierSpacing_kHz60){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl10;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl10 = sr_slot;
    return;
  }
  else if(sr_slot < 20 && scs < NR_SubcarrierSpacing_kHz120){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl20;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl20 = sr_slot;
    return;
  }
  else if(sr_slot < 40){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl40;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl40 = sr_slot;
    return;
  }
  else if(sr_slot < 80 || scs == NR_SubcarrierSpacing_kHz15){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl80;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl80 = sr_slot;
    return;
  }
  else if(sr_slot < 160 || scs == NR_SubcarrierSpacing_kHz30){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl160;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl160 = sr_slot;
    return;
  }
  else if(sr_slot < 320 || scs == NR_SubcarrierSpacing_kHz60){
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl320;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl320 = sr_slot;
    return;
  }
  else {
    schedulingRequestResourceConfig->periodicityAndOffset->present = NR_SchedulingRequestResourceConfig__periodicityAndOffset_PR_sl640;
    schedulingRequestResourceConfig->periodicityAndOffset->choice.sl640 = sr_slot;
  }
}

static void scheduling_request_config(const NR_ServingCellConfigCommon_t *scc, NR_PUCCH_Config_t *pucch_Config, int scs)
{
  // format with <=2 bits in pucch resource set 0
  NR_PUCCH_ResourceSet_t *pucchresset = pucch_Config->resourceSetToAddModList->list.array[0];
  // assigning the 1st pucch resource in the set to scheduling request
  NR_PUCCH_ResourceId_t *pucchressetid = pucchresset->resourceList.list.array[0];

  pucch_Config->schedulingRequestResourceToAddModList = calloc(1,sizeof(*pucch_Config->schedulingRequestResourceToAddModList));
  NR_SchedulingRequestResourceConfig_t *schedulingRequestResourceConfig = calloc(1,sizeof(*schedulingRequestResourceConfig));
  schedulingRequestResourceConfig->schedulingRequestResourceId = 1;
  schedulingRequestResourceConfig->schedulingRequestID = 0;

  set_SR_periodandoffset(schedulingRequestResourceConfig, scc, scs);

  schedulingRequestResourceConfig->resource = calloc(1,sizeof(*schedulingRequestResourceConfig->resource));
  *schedulingRequestResourceConfig->resource = *pucchressetid;
  asn1cSeqAdd(&pucch_Config->schedulingRequestResourceToAddModList->list,schedulingRequestResourceConfig);
}

static void set_ul_mcs_table(const NR_UE_NR_Capability_t *cap,
                             const NR_ServingCellConfigCommon_t *scc,
                             NR_PUSCH_Config_t *pusch_Config)
{

  if (cap == NULL){
    pusch_Config->mcs_Table = NULL;
    return;
  }

  int band;
  if (scc->uplinkConfigCommon->frequencyInfoUL->frequencyBandList)
    band = *scc->uplinkConfigCommon->frequencyInfoUL->frequencyBandList->list.array[0];
  else
    band = *scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0];
  bool supported = false;
  for (int i=0;i<cap->rf_Parameters.supportedBandListNR.list.count;i++) {
    NR_BandNR_t *bandNRinfo = cap->rf_Parameters.supportedBandListNR.list.array[i];
    if(bandNRinfo->bandNR == band && bandNRinfo->pusch_256QAM) {
      supported = true;
      break;
    }
  }
  if (supported) {
    if(pusch_Config->transformPrecoder == NULL ||
       *pusch_Config->transformPrecoder == NR_PUSCH_Config__transformPrecoder_disabled) {
      if(pusch_Config->mcs_Table == NULL)
        pusch_Config->mcs_Table = calloc(1, sizeof(*pusch_Config->mcs_Table));
      *pusch_Config->mcs_Table = NR_PUSCH_Config__mcs_Table_qam256;
    }
    else {
      if(pusch_Config->mcs_TableTransformPrecoder == NULL)
        pusch_Config->mcs_TableTransformPrecoder = calloc(1, sizeof(*pusch_Config->mcs_TableTransformPrecoder));
      *pusch_Config->mcs_TableTransformPrecoder = NR_PUSCH_Config__mcs_TableTransformPrecoder_qam256;
    }
  }
  else {
    pusch_Config->mcs_Table = NULL;
    pusch_Config->mcs_TableTransformPrecoder = NULL;
  }
}

static void set_dl_mcs_table(int scs,
                             const NR_UE_NR_Capability_t *cap,
                             NR_BWP_DownlinkDedicated_t *bwp_Dedicated,
                             const NR_ServingCellConfigCommon_t *scc)
{

  if (cap == NULL){
    bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table = NULL;
    return;
  }

  int band = *scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0];
  struct NR_FrequencyInfoDL__scs_SpecificCarrierList scs_list = scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList;
  int bw_rb = -1;
  for(int i = 0; i < scs_list.list.count; i++){
    if(scs == scs_list.list.array[i]->subcarrierSpacing){
      bw_rb = scs_list.list.array[i]->carrierBandwidth;
      break;
    }
  }
  AssertFatal(bw_rb > 0,"Could not find scs-SpecificCarrierList element for scs %d", scs);

  bool supported = false;
  const frequency_range_t freq_range = get_freq_range_from_band(band);
  if (freq_range == FR2) {
    for (int i = 0; i < cap->rf_Parameters.supportedBandListNR.list.count; i++) {
      NR_BandNR_t *bandNRinfo = cap->rf_Parameters.supportedBandListNR.list.array[i];
      if(bandNRinfo->bandNR == band && bandNRinfo->pdsch_256QAM_FR2) {
        supported = true;
        break;
      }
    }
  }
  else if (cap->phy_Parameters.phy_ParametersFR1 && cap->phy_Parameters.phy_ParametersFR1->pdsch_256QAM_FR1)
    supported = true;

  if (supported) {
    if(bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table == NULL)
      bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table = calloc(1, sizeof(*bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table));
    *bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table = NR_PDSCH_Config__mcs_Table_qam256;
  }
  else
    bwp_Dedicated->pdsch_Config->choice.setup->mcs_Table = NULL;
}

static struct NR_SetupRelease_PUSCH_Config *config_pusch(NR_PUSCH_Config_t *pusch_Config,
							 const bool use_deltaMCS,
                                                         const NR_ServingCellConfigCommon_t *scc,
                                                         const NR_UE_NR_Capability_t *uecap)
{
  struct NR_SetupRelease_PUSCH_Config *setup_puschconfig = calloc(1, sizeof(*setup_puschconfig));
  setup_puschconfig->present = NR_SetupRelease_PUSCH_Config_PR_setup;
  if (!pusch_Config)
    pusch_Config = calloc(1, sizeof(*pusch_Config));
  setup_puschconfig->choice.setup = pusch_Config;

  pusch_Config->dataScramblingIdentityPUSCH = NULL;
  if (!pusch_Config->txConfig)
    pusch_Config->txConfig = calloc(1, sizeof(*pusch_Config->txConfig));
  *pusch_Config->txConfig = NR_PUSCH_Config__txConfig_codebook;
  pusch_Config->dmrs_UplinkForPUSCH_MappingTypeA = NULL;
  if (!pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB)
    pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB = calloc(1, sizeof(*pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB));
  pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->present = NR_SetupRelease_DMRS_UplinkConfig_PR_setup;
  if (!pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup)
    pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup = calloc(1, sizeof(*pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup));
  NR_DMRS_UplinkConfig_t *NR_DMRS_UplinkConfig = pusch_Config->dmrs_UplinkForPUSCH_MappingTypeB->choice.setup;
  NR_DMRS_UplinkConfig->dmrs_Type = NULL;
  NR_DMRS_UplinkConfig->dmrs_AdditionalPosition = NULL;
  NR_DMRS_UplinkConfig->phaseTrackingRS = NULL;
  NR_DMRS_UplinkConfig->maxLength = NULL;
  if (!NR_DMRS_UplinkConfig->transformPrecodingDisabled)
    NR_DMRS_UplinkConfig->transformPrecodingDisabled = calloc(1, sizeof(*NR_DMRS_UplinkConfig->transformPrecodingDisabled));
  NR_DMRS_UplinkConfig->transformPrecodingDisabled->scramblingID0 = NULL;
  NR_DMRS_UplinkConfig->transformPrecodingDisabled->scramblingID1 = NULL;
  if (!NR_DMRS_UplinkConfig->transformPrecodingEnabled)
    NR_DMRS_UplinkConfig->transformPrecodingEnabled = calloc(1, sizeof(*NR_DMRS_UplinkConfig->transformPrecodingEnabled));
  NR_DMRS_UplinkConfig->transformPrecodingEnabled->nPUSCH_Identity = NULL;
  NR_DMRS_UplinkConfig->transformPrecodingEnabled->sequenceHopping = NULL;
  NR_DMRS_UplinkConfig->transformPrecodingEnabled->sequenceGroupHopping = NULL;
  if (!pusch_Config->pusch_PowerControl)
    pusch_Config->pusch_PowerControl = calloc(1, sizeof(*pusch_Config->pusch_PowerControl));
  pusch_Config->pusch_PowerControl->tpc_Accumulation = NULL;
  if (!pusch_Config->pusch_PowerControl->msg3_Alpha)
    pusch_Config->pusch_PowerControl->msg3_Alpha = calloc(1, sizeof(*pusch_Config->pusch_PowerControl->msg3_Alpha));
  *pusch_Config->pusch_PowerControl->msg3_Alpha = NR_Alpha_alpha1;
  pusch_Config->pusch_PowerControl->p0_NominalWithoutGrant = NULL;
  pusch_Config->pusch_PowerControl->p0_AlphaSets = calloc(1, sizeof(*pusch_Config->pusch_PowerControl->p0_AlphaSets));
  NR_P0_PUSCH_AlphaSet_t *aset = calloc(1, sizeof(*aset));
  aset->p0_PUSCH_AlphaSetId = 0;
  aset->p0 = calloc(1, sizeof(*aset->p0));
  *aset->p0 = 0;
  aset->alpha = calloc(1, sizeof(*aset->alpha));
  *aset->alpha = NR_Alpha_alpha1;
  asn1cSeqAdd(&pusch_Config->pusch_PowerControl->p0_AlphaSets->list, aset);
  pusch_Config->pusch_PowerControl->pathlossReferenceRSToAddModList = calloc(1, sizeof(*pusch_Config->pusch_PowerControl->pathlossReferenceRSToAddModList));
  NR_PUSCH_PathlossReferenceRS_t *plrefRS = calloc(1, sizeof(*plrefRS));
  plrefRS->pusch_PathlossReferenceRS_Id = 0;
  plrefRS->referenceSignal.present = NR_PUSCH_PathlossReferenceRS__referenceSignal_PR_ssb_Index;
  plrefRS->referenceSignal.choice.ssb_Index = 0;
  asn1cSeqAdd(&pusch_Config->pusch_PowerControl->pathlossReferenceRSToAddModList->list, plrefRS);
  pusch_Config->pusch_PowerControl->pathlossReferenceRSToReleaseList = NULL;
  pusch_Config->pusch_PowerControl->twoPUSCH_PC_AdjustmentStates = NULL;
  if (use_deltaMCS) {
    if (!pusch_Config->pusch_PowerControl->deltaMCS)
      pusch_Config->pusch_PowerControl->deltaMCS = calloc(1, sizeof(*pusch_Config->pusch_PowerControl->deltaMCS));
    *pusch_Config->pusch_PowerControl->deltaMCS = NR_PUSCH_PowerControl__deltaMCS_enabled;
  }
  else free(pusch_Config->pusch_PowerControl->deltaMCS);
  pusch_Config->pusch_PowerControl->sri_PUSCH_MappingToAddModList = NULL;
  pusch_Config->pusch_PowerControl->sri_PUSCH_MappingToReleaseList = NULL;
  pusch_Config->frequencyHopping = NULL;
  pusch_Config->frequencyHoppingOffsetLists = NULL;
  pusch_Config->resourceAllocation = NR_PUSCH_Config__resourceAllocation_resourceAllocationType1;
  pusch_Config->pusch_TimeDomainAllocationList = NULL;
  pusch_Config->pusch_AggregationFactor = NULL;
  set_ul_mcs_table(uecap, scc, pusch_Config);
  pusch_Config->transformPrecoder = NULL;
  if (!pusch_Config->codebookSubset)
    pusch_Config->codebookSubset = calloc(1, sizeof(*pusch_Config->codebookSubset));
  *pusch_Config->codebookSubset = NR_PUSCH_Config__codebookSubset_nonCoherent;
  if (!pusch_Config->maxRank)
    pusch_Config->maxRank = calloc(1, sizeof(*pusch_Config->maxRank));
  *pusch_Config->maxRank = 1;
  pusch_Config->rbg_Size = NULL;
  pusch_Config->uci_OnPUSCH = NULL;
  pusch_Config->tp_pi2BPSK = NULL;

  return setup_puschconfig;
}

static struct NR_SetupRelease_PDSCH_Config *config_pdsch(uint64_t ssb_bitmap, int bwp_Id, int dl_antenna_ports)
{
  struct NR_SetupRelease_PDSCH_Config *setup_pdsch_Config = calloc(1,sizeof(*setup_pdsch_Config));
  setup_pdsch_Config->present = NR_SetupRelease_PDSCH_Config_PR_setup;
  NR_PDSCH_Config_t *pdsch_Config = calloc(1, sizeof(*pdsch_Config));
  setup_pdsch_Config->choice.setup = pdsch_Config;
  pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA = calloc(1, sizeof(*pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA));
  pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA->present = NR_SetupRelease_DMRS_DownlinkConfig_PR_setup;
  pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA->choice.setup = calloc(1, sizeof(*pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA->choice.setup));
  NR_DMRS_DownlinkConfig_t *dmrs_DownlinkForPDSCH_MappingTypeA = pdsch_Config->dmrs_DownlinkForPDSCH_MappingTypeA->choice.setup;
  dmrs_DownlinkForPDSCH_MappingTypeA->dmrs_Type = NULL;
  dmrs_DownlinkForPDSCH_MappingTypeA->maxLength = NULL;
  dmrs_DownlinkForPDSCH_MappingTypeA->scramblingID0 = NULL;
  dmrs_DownlinkForPDSCH_MappingTypeA->scramblingID1 = NULL;
  dmrs_DownlinkForPDSCH_MappingTypeA->dmrs_AdditionalPosition = calloc(1, sizeof(*dmrs_DownlinkForPDSCH_MappingTypeA->dmrs_AdditionalPosition));
  // TODO possible improvement is to select based on some input additional position
  *dmrs_DownlinkForPDSCH_MappingTypeA->dmrs_AdditionalPosition = NR_DMRS_DownlinkConfig__dmrs_AdditionalPosition_pos1;

  pdsch_Config->dataScramblingIdentityPDSCH = NULL;
  pdsch_Config->resourceAllocation = NR_PDSCH_Config__resourceAllocation_resourceAllocationType1;

  pdsch_Config->prb_BundlingType.present = NR_PDSCH_Config__prb_BundlingType_PR_staticBundling;
  pdsch_Config->prb_BundlingType.choice.staticBundling = calloc(1, sizeof(*pdsch_Config->prb_BundlingType.choice.staticBundling));
  pdsch_Config->prb_BundlingType.choice.staticBundling->bundleSize = calloc(1, sizeof(*pdsch_Config->prb_BundlingType.choice.staticBundling->bundleSize));
  *pdsch_Config->prb_BundlingType.choice.staticBundling->bundleSize = NR_PDSCH_Config__prb_BundlingType__staticBundling__bundleSize_wideband;

  int n_ssb = 0;
  if (!pdsch_Config->tci_StatesToAddModList)
    pdsch_Config->tci_StatesToAddModList=calloc(1,sizeof(*pdsch_Config->tci_StatesToAddModList));
  for (int i = 0; i < 64; i++) {
    if (((ssb_bitmap >> (63 - i)) & 0x01) == 0)
      continue;
    asn1cSequenceAdd(pdsch_Config->tci_StatesToAddModList->list, NR_TCI_State_t, tcid);
    tcid->tci_StateId = n_ssb++;
    tcid->qcl_Type1.cell = NULL;
    asn1cCallocOne(tcid->qcl_Type1.bwp_Id, bwp_Id);
    tcid->qcl_Type1.referenceSignal.present = NR_QCL_Info__referenceSignal_PR_ssb;
    tcid->qcl_Type1.referenceSignal.choice.ssb = i;
    tcid->qcl_Type1.qcl_Type = NR_QCL_Info__qcl_Type_typeC;
  }
  return setup_pdsch_Config;
}

static void config_downlinkBWP(NR_BWP_Downlink_t *bwp,
                               const NR_ServingCellConfigCommon_t *scc,
                               const NR_ServingCellConfig_t *servingcellconfigdedicated,
                               const NR_UE_NR_Capability_t *uecap,
                               int dl_antenna_ports,
                               bool force_256qam_off,
                               int bwp_loop,
                               bool is_SA,
                               const int num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS])
{
  bwp->bwp_Common = calloc(1,sizeof(*bwp->bwp_Common));

  if(servingcellconfigdedicated->downlinkBWP_ToAddModList &&
     bwp_loop < servingcellconfigdedicated->downlinkBWP_ToAddModList->list.count) {
    bwp->bwp_Id = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Id;
    bwp->bwp_Common->genericParameters.locationAndBandwidth = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.locationAndBandwidth;
    bwp->bwp_Common->genericParameters.subcarrierSpacing = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.subcarrierSpacing;
    bwp->bwp_Common->genericParameters.cyclicPrefix = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.cyclicPrefix;
  } else {
    bwp->bwp_Id=bwp_loop+1;
    bwp->bwp_Common->genericParameters.locationAndBandwidth = PRBalloc_to_locationandbandwidth(scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth,0);
    bwp->bwp_Common->genericParameters.subcarrierSpacing = scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.subcarrierSpacing;
    bwp->bwp_Common->genericParameters.cyclicPrefix = scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.cyclicPrefix;
  }

  bwp->bwp_Common->pdcch_ConfigCommon=calloc(1,sizeof(*bwp->bwp_Common->pdcch_ConfigCommon));
  bwp->bwp_Common->pdcch_ConfigCommon->present = NR_SetupRelease_PDCCH_ConfigCommon_PR_setup;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup = calloc(1,sizeof(*bwp->bwp_Common->pdcch_ConfigCommon->choice.setup));
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->controlResourceSetZero = NULL;

  int curr_bwp = NRRIV2BW(bwp->bwp_Common->genericParameters.locationAndBandwidth,MAX_BWP_SIZE);

  uint64_t ssb_bitmap = get_ssb_bitmap(scc);
  NR_ControlResourceSet_t *coreset = get_coreset_config(bwp->bwp_Id, curr_bwp, ssb_bitmap);
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonControlResourceSet = coreset;

  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->searchSpaceZero=NULL;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList=NULL;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList=calloc(1,sizeof(*bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList));

  int searchspaceid = 5 + bwp->bwp_Id;
  int rrc_num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS];
  int num_cces = get_coreset_num_cces(coreset->frequencyDomainResources.buf, coreset->duration);
  verify_agg_levels(num_cces, num_agg_level_candidates, coreset->controlResourceSetId, searchspaceid, rrc_num_agg_level_candidates);
  NR_SearchSpace_t *ss = rrc_searchspace_config(true, searchspaceid, coreset->controlResourceSetId, rrc_num_agg_level_candidates);
  asn1cSeqAdd(&bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList->list, ss);

  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->searchSpaceSIB1=NULL;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->searchSpaceOtherSystemInformation=NULL;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->pagingSearchSpace=NULL;
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ra_SearchSpace=NULL;
  if(is_SA == false) {
    bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ra_SearchSpace=calloc(1,sizeof(*bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ra_SearchSpace));
    *bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ra_SearchSpace=ss->searchSpaceId;
  }
  bwp->bwp_Common->pdcch_ConfigCommon->choice.setup->ext1=NULL;
  bwp->bwp_Common->pdsch_ConfigCommon=calloc(1,sizeof(*bwp->bwp_Common->pdsch_ConfigCommon));
  bwp->bwp_Common->pdsch_ConfigCommon->present = NR_SetupRelease_PDSCH_ConfigCommon_PR_setup;
  bwp->bwp_Common->pdsch_ConfigCommon->choice.setup = calloc(1,sizeof(*bwp->bwp_Common->pdsch_ConfigCommon->choice.setup));
  bwp->bwp_Common->pdsch_ConfigCommon->choice.setup->pdsch_TimeDomainAllocationList = calloc(1,sizeof(*bwp->bwp_Common->pdsch_ConfigCommon->choice.setup->pdsch_TimeDomainAllocationList));

  nr_rrc_config_dl_tda(bwp->bwp_Common->pdsch_ConfigCommon->choice.setup->pdsch_TimeDomainAllocationList,
                       get_frame_type((int)*scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0], *scc->ssbSubcarrierSpacing),
                       scc->tdd_UL_DL_ConfigurationCommon,
                       curr_bwp);

  if (!bwp->bwp_Dedicated) {
    bwp->bwp_Dedicated=calloc(1,sizeof(*bwp->bwp_Dedicated));
  }
  bwp->bwp_Dedicated->pdcch_Config=calloc(1,sizeof(*bwp->bwp_Dedicated->pdcch_Config));
  bwp->bwp_Dedicated->pdcch_Config->present = NR_SetupRelease_PDCCH_Config_PR_setup;
  bwp->bwp_Dedicated->pdcch_Config->choice.setup = calloc(1,sizeof(*bwp->bwp_Dedicated->pdcch_Config->choice.setup));
  bwp->bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList = calloc(1,sizeof(*bwp->bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList));
  bwp->bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList = calloc(1,sizeof(*bwp->bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList));

  // coreset2 is identical to coreset above, but reallocated to prevent double
  // frees
  NR_ControlResourceSet_t *coreset2 = get_coreset_config(bwp->bwp_Id, curr_bwp, ssb_bitmap);
  asn1cSeqAdd(&bwp->bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList->list, coreset2);

  searchspaceid = 10 + bwp->bwp_Id;
  num_cces = get_coreset_num_cces(coreset2->frequencyDomainResources.buf, coreset2->duration);
  verify_agg_levels(num_cces, num_agg_level_candidates, coreset->controlResourceSetId, searchspaceid, rrc_num_agg_level_candidates);
  NR_SearchSpace_t *ss2 =
      rrc_searchspace_config(false, searchspaceid, coreset2->controlResourceSetId, rrc_num_agg_level_candidates);
  asn1cSeqAdd(&bwp->bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList->list, ss2);

  bwp->bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToReleaseList = NULL;
  bwp->bwp_Dedicated->pdsch_Config = config_pdsch(ssb_bitmap, bwp->bwp_Id, dl_antenna_ports);

  set_dl_mcs_table(bwp->bwp_Common->genericParameters.subcarrierSpacing,
                   force_256qam_off ? NULL : uecap,
                   bwp->bwp_Dedicated,
                   scc);
}

static void config_uplinkBWP(NR_BWP_Uplink_t *ubwp,
                             long bwp_loop,
                             bool is_SA,
                             int uid,
                             const nr_mac_config_t *configuration,
                             const NR_ServingCellConfig_t *servingcellconfigdedicated,
                             const NR_ServingCellConfigCommon_t *scc,
                             const NR_UE_NR_Capability_t *uecap)
{
  ubwp->bwp_Common = calloc(1,sizeof(*ubwp->bwp_Common));
  if(servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList &&
     bwp_loop < servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.count) {
    ubwp->bwp_Id = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Id;
    ubwp->bwp_Common->genericParameters.locationAndBandwidth = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.locationAndBandwidth;
    ubwp->bwp_Common->genericParameters.subcarrierSpacing = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.subcarrierSpacing;
    ubwp->bwp_Common->genericParameters.cyclicPrefix = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[bwp_loop]->bwp_Common->genericParameters.cyclicPrefix;
  } else {
    ubwp->bwp_Id=bwp_loop+1;
    ubwp->bwp_Common->genericParameters.locationAndBandwidth = PRBalloc_to_locationandbandwidth(scc->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth,0);
    ubwp->bwp_Common->genericParameters.subcarrierSpacing = scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.subcarrierSpacing;
    ubwp->bwp_Common->genericParameters.cyclicPrefix = scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.cyclicPrefix;
  }

  int curr_bwp = NRRIV2BW(ubwp->bwp_Common->genericParameters.locationAndBandwidth,MAX_BWP_SIZE);
  ubwp->bwp_Common->rach_ConfigCommon  = is_SA ? NULL : clone_rach_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->rach_ConfigCommon);
  ubwp->bwp_Common->pusch_ConfigCommon = clone_pusch_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon);
  ubwp->bwp_Common->pucch_ConfigCommon = CALLOC(1,sizeof(struct NR_SetupRelease_PUCCH_ConfigCommon));
  ubwp->bwp_Common->pucch_ConfigCommon->present= NR_SetupRelease_PUCCH_ConfigCommon_PR_setup;
  ubwp->bwp_Common->pucch_ConfigCommon->choice.setup = CALLOC(1,sizeof(struct NR_PUCCH_ConfigCommon));
  struct NR_PUCCH_ConfigCommon *pucch_ConfigCommon = ubwp->bwp_Common->pucch_ConfigCommon->choice.setup;
  pucch_ConfigCommon->pucch_ResourceCommon = NULL; // for BWP != 0 as per 38.213 section 9.2.1
  pucch_ConfigCommon->pucch_GroupHopping = scc->uplinkConfigCommon->initialUplinkBWP->pucch_ConfigCommon->choice.setup->pucch_GroupHopping;
  asn1cCallocOne(pucch_ConfigCommon->hoppingId, *scc->uplinkConfigCommon->initialUplinkBWP->pucch_ConfigCommon->choice.setup->hoppingId);
  asn1cCallocOne(pucch_ConfigCommon->p0_nominal, *scc->uplinkConfigCommon->initialUplinkBWP->pucch_ConfigCommon->choice.setup->p0_nominal);

  if (!ubwp->bwp_Dedicated) {
    ubwp->bwp_Dedicated = calloc(1,sizeof(*ubwp->bwp_Dedicated));
  }

  ubwp->bwp_Dedicated->pucch_Config = calloc(1,sizeof(*ubwp->bwp_Dedicated->pucch_Config));
  ubwp->bwp_Dedicated->pucch_Config->present = NR_SetupRelease_PUCCH_Config_PR_setup;
  NR_PUCCH_Config_t *pucch_Config = calloc(1,sizeof(*pucch_Config));
  ubwp->bwp_Dedicated->pucch_Config->choice.setup = pucch_Config;
  pucch_Config->resourceSetToAddModList = calloc(1,sizeof(*pucch_Config->resourceSetToAddModList));
  pucch_Config->resourceSetToReleaseList = NULL;
  pucch_Config->resourceToAddModList = calloc(1,sizeof(*pucch_Config->resourceToAddModList));
  pucch_Config->resourceToReleaseList = NULL;
  int num_pucch2 = get_nb_pucch2_per_slot(scc, curr_bwp);
  config_pucch_resset0(pucch_Config, uid, curr_bwp, num_pucch2, uecap);
  config_pucch_resset1(pucch_Config, uid, num_pucch2, uecap);
  set_pucch_power_config(pucch_Config, configuration->do_CSIRS);
  scheduling_request_config(scc, pucch_Config, ubwp->bwp_Common->genericParameters.subcarrierSpacing);
  set_dl_DataToUL_ACK(pucch_Config, configuration->minRXTXTIME, ubwp->bwp_Common->genericParameters.subcarrierSpacing);

  NR_PUSCH_Config_t *pusch_Config = NULL;
  if(servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList &&
     bwp_loop < servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.count) {
    pusch_Config = clone_pusch_config(servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[bwp_loop]
                                         ->bwp_Dedicated->pusch_Config->choice.setup);
  }
  ubwp->bwp_Dedicated->pusch_Config = config_pusch(pusch_Config, configuration->use_deltaMCS, scc, configuration->force_UL256qam_off ? NULL : uecap);

  long maxMIMO_Layers = servingcellconfigdedicated &&
                                servingcellconfigdedicated->uplinkConfig
                                && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig
                                && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1
                                && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers ?
                            *servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers : 1;

  ubwp->bwp_Dedicated->srs_Config = get_config_srs(scc,
                                                   NULL,
                                                   curr_bwp,
                                                   uid,
                                                   bwp_loop + 1,
                                                   maxMIMO_Layers,
                                                   configuration->minRXTXTIME,
                                                   configuration->do_SRS);

  ubwp->bwp_Dedicated->configuredGrantConfig = NULL;
  ubwp->bwp_Dedicated->beamFailureRecoveryConfig = NULL;
}

static void set_phr_config(NR_MAC_CellGroupConfig_t *mac_CellGroupConfig)
{
  mac_CellGroupConfig->phr_Config                                         = calloc(1, sizeof(*mac_CellGroupConfig->phr_Config));
  mac_CellGroupConfig->phr_Config->present                                = NR_SetupRelease_PHR_Config_PR_setup;
  mac_CellGroupConfig->phr_Config->choice.setup                           = calloc(1, sizeof(*mac_CellGroupConfig->phr_Config->choice.setup));
  mac_CellGroupConfig->phr_Config->choice.setup->phr_PeriodicTimer        = NR_PHR_Config__phr_PeriodicTimer_sf10;
  mac_CellGroupConfig->phr_Config->choice.setup->phr_ProhibitTimer        = NR_PHR_Config__phr_ProhibitTimer_sf10;
  mac_CellGroupConfig->phr_Config->choice.setup->phr_Tx_PowerFactorChange = NR_PHR_Config__phr_Tx_PowerFactorChange_dB1;
}

static void set_csi_meas_periodicity(const NR_ServingCellConfigCommon_t *scc,
                                     NR_CSI_ReportConfig_t *csirep,
                                     int uid,
                                     int curr_bwp,
                                     bool is_rsrp)
{
  const int ideal_period = set_ideal_period(true);
  const int num_pucch2 = get_nb_pucch2_per_slot(scc, curr_bwp);
  const int idx = (uid * 2 / num_pucch2) + is_rsrp;

  frame_structure_t *fs = &RC.nrmac[0]->frame_structure;
  int offset = get_ul_slot_offset(fs, idx, true);

  LOG_D(NR_MAC, "set_csi_meas_periodicity: uid = %d, offset = %d, ideal_period = %d", uid, offset, ideal_period);
  AssertFatal(offset < 320, "Not enough UL slots to accomodate all possible UEs. Need to rework the implementation\n");

  if (check_periodicity(4, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots4;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots4 = offset;
  } else if (check_periodicity(5, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots5;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots5 = offset;
  } else if (check_periodicity(8, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots8;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots8 = offset;
  } else if (check_periodicity(10, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots10;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots10 = offset;
  } else if (check_periodicity(16, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots16;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots16 = offset;
  } else if (check_periodicity(20, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots20;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots20 = offset;
  } else if (check_periodicity(40, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots40;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots40 = offset;
  } else if (check_periodicity(80, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots80;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots80 = offset;
  } else if (check_periodicity(160, ideal_period, fs)) {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots160;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots160 = offset;
  } else {
    csirep->reportConfigType.choice.periodic->reportSlotConfig.present = NR_CSI_ReportPeriodicityAndOffset_PR_slots320;
    csirep->reportConfigType.choice.periodic->reportSlotConfig.choice.slots320 = offset;
  }
}

static void config_csi_codebook(const nr_pdsch_AntennaPorts_t *antennaports,
                                const int max_layers,
                                struct NR_CodebookConfig *codebookConfig)
{
  const int num_ant_ports = antennaports->N1 * antennaports->N2 * antennaports->XP;
  codebookConfig->codebookType.present = NR_CodebookConfig__codebookType_PR_type1;
  if(!codebookConfig->codebookType.choice.type1)
    codebookConfig->codebookType.choice.type1 = calloc(1, sizeof(*codebookConfig->codebookType.choice.type1));
  // Single panel configuration
  codebookConfig->codebookType.choice.type1->subType.present = NR_CodebookConfig__codebookType__type1__subType_PR_typeI_SinglePanel;
  if(!codebookConfig->codebookType.choice.type1->subType.choice.typeI_SinglePanel)
    codebookConfig->codebookType.choice.type1->subType.choice.typeI_SinglePanel = calloc(1, sizeof(*codebookConfig->codebookType.choice.type1->subType.choice.typeI_SinglePanel));
  struct NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel *singlePanelConfig = codebookConfig->codebookType.choice.type1->subType.choice.typeI_SinglePanel;
  singlePanelConfig->typeI_SinglePanel_ri_Restriction.size = 1;
  singlePanelConfig->typeI_SinglePanel_ri_Restriction.bits_unused = 0;
  singlePanelConfig->typeI_SinglePanel_ri_Restriction.buf = malloc(1);
  singlePanelConfig->typeI_SinglePanel_ri_Restriction.buf[0] = (1 << max_layers) - 1; // max_layers bit set to 1
  if (num_ant_ports == 2) {
    singlePanelConfig->nrOfAntennaPorts.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts_PR_two;
    if(!singlePanelConfig->nrOfAntennaPorts.choice.two) {
      asn1cCalloc(singlePanelConfig->nrOfAntennaPorts.choice.two, two);
      two->twoTX_CodebookSubsetRestriction.size = 1;
      two->twoTX_CodebookSubsetRestriction.bits_unused = 2;
      asn1cCallocOne(two->twoTX_CodebookSubsetRestriction.buf, 0xfc); // no restriction (all 6 bits enabled)
    }
  } else {
    singlePanelConfig->nrOfAntennaPorts.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts_PR_moreThanTwo;
    if(!singlePanelConfig->nrOfAntennaPorts.choice.moreThanTwo) {
      singlePanelConfig->nrOfAntennaPorts.choice.moreThanTwo = calloc(1, sizeof(*singlePanelConfig->nrOfAntennaPorts.choice.moreThanTwo));
      struct NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo *moreThanTwo = singlePanelConfig->nrOfAntennaPorts.choice.moreThanTwo;
      switch (num_ant_ports) {
        case 4:
          moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_two_one_TypeI_SinglePanel_Restriction;
          moreThanTwo->n1_n2.choice.two_one_TypeI_SinglePanel_Restriction.size = 1;
          moreThanTwo->n1_n2.choice.two_one_TypeI_SinglePanel_Restriction.bits_unused = 0;
          // TODO verify the meaning of this parameter
          asn1cCallocOne(moreThanTwo->n1_n2.choice.two_one_TypeI_SinglePanel_Restriction.buf, 0xff);
          break;
        case 8:
          if (antennaports->N1 == 2) {
            AssertFatal(antennaports->N2 == 2, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_two_two_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.two_two_TypeI_SinglePanel_Restriction.size = 8;
            moreThanTwo->n1_n2.choice.two_two_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.two_two_TypeI_SinglePanel_Restriction.buf = calloc(8, sizeof(uint8_t));
            for (int i = 0; i < 8; i++)
              moreThanTwo->n1_n2.choice.two_two_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else if (antennaports->N1 == 4) {
            AssertFatal(antennaports->N2 == 1, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_four_one_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.four_one_TypeI_SinglePanel_Restriction.size = 2;
            moreThanTwo->n1_n2.choice.four_one_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.four_one_TypeI_SinglePanel_Restriction.buf = calloc(2, sizeof(uint8_t));
            for (int i = 0; i < 2; i++)
              moreThanTwo->n1_n2.choice.four_one_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else
            AssertFatal(1 == 0, "N1 %d and N2 %d not supported for %d antenna ports\n", antennaports->N1, antennaports->N2, num_ant_ports);
          break;
        case 12:
          if (antennaports->N1 == 3) {
            AssertFatal(antennaports->N2 == 2, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_three_two_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.three_two_TypeI_SinglePanel_Restriction.size = 12;
            moreThanTwo->n1_n2.choice.three_two_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.three_two_TypeI_SinglePanel_Restriction.buf = calloc(12, sizeof(uint8_t));
            for (int i = 0; i < 12; i++)
              moreThanTwo->n1_n2.choice.three_two_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else if (antennaports->N1 == 6) {
            AssertFatal(antennaports->N2 == 1, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_six_one_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.six_one_TypeI_SinglePanel_Restriction.size = 3;
            moreThanTwo->n1_n2.choice.six_one_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.six_one_TypeI_SinglePanel_Restriction.buf = calloc(3, sizeof(uint8_t));
            for (int i = 0; i < 3; i++)
             moreThanTwo->n1_n2.choice.six_one_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else
            AssertFatal(1 == 0, "N1 %d and N2 %d not supported for %d antenna ports\n", antennaports->N1, antennaports->N2, num_ant_ports);
          break;
        case 16:
          if (antennaports->N1 == 4) {
            AssertFatal(antennaports->N2 == 2, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_four_two_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.four_two_TypeI_SinglePanel_Restriction.size = 16;
            moreThanTwo->n1_n2.choice.four_two_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.four_two_TypeI_SinglePanel_Restriction.buf = calloc(16, sizeof(uint8_t));
            for (int i = 0; i < 16; i++)
              moreThanTwo->n1_n2.choice.four_two_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else if (antennaports->N1 == 8) {
            AssertFatal(antennaports->N2 == 1, "N1 and N2 not in accordace with the specifications\n");
            moreThanTwo->n1_n2.present = NR_CodebookConfig__codebookType__type1__subType__typeI_SinglePanel__nrOfAntennaPorts__moreThanTwo__n1_n2_PR_eight_one_TypeI_SinglePanel_Restriction;
            moreThanTwo->n1_n2.choice.eight_one_TypeI_SinglePanel_Restriction.size = 4;
            moreThanTwo->n1_n2.choice.eight_one_TypeI_SinglePanel_Restriction.bits_unused = 0;
            moreThanTwo->n1_n2.choice.eight_one_TypeI_SinglePanel_Restriction.buf = calloc(4, sizeof(uint8_t));
            for (int i = 0; i < 4; i++)
              moreThanTwo->n1_n2.choice.eight_one_TypeI_SinglePanel_Restriction.buf[i] = 0xff; // TODO verify the meaning of this parameter
          } else
            AssertFatal(1 == 0, "N1 %d and N2 %d not supported for %d antenna ports\n", antennaports->N1, antennaports->N2, num_ant_ports);
          break;
        default:
          AssertFatal(1 == 0, "%d antenna ports not supported\n", num_ant_ports);
      }
    }
  }
  codebookConfig->codebookType.choice.type1->codebookMode = 1;
}

static void config_csi_meas_report(NR_CSI_MeasConfig_t *csi_MeasConfig,
                                   const NR_ServingCellConfigCommon_t *servingcellconfigcommon,
                                   NR_PUCCH_CSI_Resource_t *pucchcsires,
                                   struct NR_SetupRelease_PDSCH_Config *pdsch_Config,
                                   const nr_pdsch_AntennaPorts_t *antennaports,
                                   const int max_layers,
                                   int rep_id,
                                   int uid,
                                   int curr_bwp)
{
  int resource_id = -1;
  int im_id = -1;
  for (int csi_list = 0; csi_list < csi_MeasConfig->csi_ResourceConfigToAddModList->list.count; csi_list++) {
    NR_CSI_ResourceConfig_t *csires = csi_MeasConfig->csi_ResourceConfigToAddModList->list.array[csi_list];
    if (csires->csi_RS_ResourceSetList.present == NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB) {
      if (csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList) {
        resource_id = csires->csi_ResourceConfigId;
      }
    }
    if (csires->csi_RS_ResourceSetList.present == NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_csi_IM_ResourceSetList) {
      if (csires->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList) {
        im_id = csires->csi_ResourceConfigId;
      }
    }
  }
  // if there are no associated resources, do not configure
  if (resource_id < 0 || im_id < 0)
    return;
  NR_CSI_ReportConfig_t *csirep = calloc(1, sizeof(*csirep));
  csirep->reportConfigId = rep_id;
  csirep->carrier = NULL;
  csirep->resourcesForChannelMeasurement = resource_id;
  csirep->csi_IM_ResourcesForInterference = calloc(1, sizeof(*csirep->csi_IM_ResourcesForInterference));
  *csirep->csi_IM_ResourcesForInterference = im_id;
  csirep->nzp_CSI_RS_ResourcesForInterference = NULL;
  csirep->reportConfigType.present = NR_CSI_ReportConfig__reportConfigType_PR_periodic;
  csirep->reportConfigType.choice.periodic = calloc(1, sizeof(*csirep->reportConfigType.choice.periodic));
  set_csi_meas_periodicity(servingcellconfigcommon, csirep, uid, curr_bwp, false);
  asn1cSeqAdd(&csirep->reportConfigType.choice.periodic->pucch_CSI_ResourceList.list, pucchcsires);
  csirep->reportQuantity.present = NR_CSI_ReportConfig__reportQuantity_PR_cri_RI_PMI_CQI;
  csirep->reportQuantity.choice.cri_RI_PMI_CQI = (NULL_t)0;
  csirep->reportFreqConfiguration = calloc(1, sizeof(*csirep->reportFreqConfiguration));
  // Wideband configuration
  csirep->reportFreqConfiguration->cqi_FormatIndicator = calloc(1, sizeof(*csirep->reportFreqConfiguration->cqi_FormatIndicator));
  *csirep->reportFreqConfiguration->cqi_FormatIndicator = NR_CSI_ReportConfig__reportFreqConfiguration__cqi_FormatIndicator_widebandCQI;
  csirep->reportFreqConfiguration->pmi_FormatIndicator = calloc(1, sizeof(*csirep->reportFreqConfiguration->pmi_FormatIndicator));
  *csirep->reportFreqConfiguration->pmi_FormatIndicator = NR_CSI_ReportConfig__reportFreqConfiguration__pmi_FormatIndicator_widebandPMI;
  csirep->reportFreqConfiguration->csi_ReportingBand = NULL;
  csirep->timeRestrictionForChannelMeasurements = NR_CSI_ReportConfig__timeRestrictionForChannelMeasurements_notConfigured;
  csirep->timeRestrictionForInterferenceMeasurements = NR_CSI_ReportConfig__timeRestrictionForInterferenceMeasurements_notConfigured;
  csirep->codebookConfig = calloc(1, sizeof(*csirep->codebookConfig));
  config_csi_codebook(antennaports, max_layers, csirep->codebookConfig);
  csirep->dummy = NULL;
  csirep->groupBasedBeamReporting.present = NR_CSI_ReportConfig__groupBasedBeamReporting_PR_disabled;
  csirep->groupBasedBeamReporting.choice.disabled = calloc(1, sizeof(*csirep->groupBasedBeamReporting.choice.disabled));
  csirep->cqi_Table = calloc(1, sizeof(*csirep->cqi_Table));
  if (pdsch_Config->choice.setup->mcs_Table != NULL)
    *csirep->cqi_Table = NR_CSI_ReportConfig__cqi_Table_table2;
  else
    *csirep->cqi_Table = NR_CSI_ReportConfig__cqi_Table_table1;
  csirep->subbandSize = NR_CSI_ReportConfig__subbandSize_value2;
  csirep->non_PMI_PortIndication = NULL;
  csirep->ext1 = NULL;
  asn1cSeqAdd(&csi_MeasConfig->csi_ReportConfigToAddModList->list, csirep);
}

static void config_rsrp_meas_report(NR_CSI_MeasConfig_t *csi_MeasConfig,
                                    const NR_ServingCellConfigCommon_t *servingcellconfigcommon,
                                    NR_PUCCH_CSI_Resource_t *pucchcsires,
                                    int do_csi, // if rsrp is based on CSI or SSB
                                    int rep_id,
                                    int uid,
                                    int curr_bwp,
                                    int num_antenna_ports,
                                    bool do_sinr)
{
  int resource_id = -1;
  for (int csi_list = 0; csi_list < csi_MeasConfig->csi_ResourceConfigToAddModList->list.count; csi_list++) {
    NR_CSI_ResourceConfig_t *csires = csi_MeasConfig->csi_ResourceConfigToAddModList->list.array[csi_list];
    if (csires->csi_RS_ResourceSetList.present == NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB) {
      if (do_csi && num_antenna_ports < 4) {
        if (csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList)
          resource_id = csires->csi_ResourceConfigId;
      } else {
        if (csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList)
          resource_id = csires->csi_ResourceConfigId;
      }
    }
  }
  // if there are no associated resources, do not configure
  if (resource_id < 0)
    return;
  NR_CSI_ReportConfig_t *csirep = calloc(1, sizeof(*csirep));
  csirep->reportConfigId = rep_id;
  csirep->carrier = NULL;
  csirep->resourcesForChannelMeasurement = resource_id;
  csirep->csi_IM_ResourcesForInterference = NULL;
  csirep->nzp_CSI_RS_ResourcesForInterference = NULL;
  csirep->reportConfigType.present = NR_CSI_ReportConfig__reportConfigType_PR_periodic;
  csirep->reportConfigType.choice.periodic = calloc(1, sizeof(*csirep->reportConfigType.choice.periodic));
  set_csi_meas_periodicity(servingcellconfigcommon, csirep, uid, curr_bwp, true);
  asn1cSeqAdd(&csirep->reportConfigType.choice.periodic->pucch_CSI_ResourceList.list, pucchcsires);
  if (do_csi && num_antenna_ports < 4) {
    csirep->reportQuantity.present = NR_CSI_ReportConfig__reportQuantity_PR_cri_RSRP;
    csirep->reportQuantity.choice.cri_RSRP = (NULL_t)0;
  } else {
    csirep->reportQuantity.present = NR_CSI_ReportConfig__reportQuantity_PR_ssb_Index_RSRP;
    csirep->reportQuantity.choice.ssb_Index_RSRP = (NULL_t)0;
    if (do_sinr) {
      csirep->reportQuantity.present = NR_CSI_ReportConfig__reportQuantity_PR_none;
      csirep->reportQuantity.choice.none = (NULL_t)0;
      csirep->ext2 = calloc(1, sizeof(*csirep->ext2));
      csirep->ext2->reportQuantity_r16 = calloc(1, sizeof(*csirep->ext2->reportQuantity_r16));
      csirep->ext2->reportQuantity_r16->present = NR_CSI_ReportConfig__ext2__reportQuantity_r16_PR_ssb_Index_SINR_r16;
      csirep->ext2->reportQuantity_r16->choice.ssb_Index_SINR_r16 = (NULL_t)0;
    }
  }
  csirep->groupBasedBeamReporting.present = NR_CSI_ReportConfig__groupBasedBeamReporting_PR_disabled;
  csirep->groupBasedBeamReporting.choice.disabled = calloc(1, sizeof(*csirep->groupBasedBeamReporting.choice.disabled));
  csirep->groupBasedBeamReporting.choice.disabled->nrofReportedRS = calloc(1, sizeof(*csirep->groupBasedBeamReporting.choice.disabled->nrofReportedRS));
  *csirep->groupBasedBeamReporting.choice.disabled->nrofReportedRS = NR_CSI_ReportConfig__groupBasedBeamReporting__disabled__nrofReportedRS_n1;
  asn1cSeqAdd(&csi_MeasConfig->csi_ReportConfigToAddModList->list, csirep);
}

static void update_cqitables(struct NR_SetupRelease_PDSCH_Config *pdsch_Config, NR_CSI_MeasConfig_t *csi_MeasConfig)
{
  int nb_csi = csi_MeasConfig->csi_ReportConfigToAddModList->list.count;
  for (int i = 0; i < nb_csi; i++) {
    NR_CSI_ReportConfig_t *csirep = csi_MeasConfig->csi_ReportConfigToAddModList->list.array[i];
    if(csirep->cqi_Table) {
      if(pdsch_Config->choice.setup->mcs_Table!=NULL)
        *csirep->cqi_Table = NR_CSI_ReportConfig__cqi_Table_table2;
      else
        *csirep->cqi_Table = NR_CSI_ReportConfig__cqi_Table_table1;
    }
  }
}

NR_BCCH_BCH_Message_t *get_new_MIB_NR(const NR_ServingCellConfigCommon_t *scc)
{
  NR_BCCH_BCH_Message_t *mib = calloc(1, sizeof(*mib));
  if (mib == NULL)
    abort();
  mib->message.present = NR_BCCH_BCH_MessageType_PR_mib;
  mib->message.choice.mib = calloc(1, sizeof(struct NR_MIB));
  if (mib->message.choice.mib == NULL)
    abort();

  // 36.331 SFN BIT STRING (SIZE (8)  , 38.331 SFN BIT STRING (SIZE (6))
  uint8_t sfn_msb = 0; // encoding will update with the correct frame number
  mib->message.choice.mib->systemFrameNumber.buf = CALLOC(1, sizeof(uint8_t));
  mib->message.choice.mib->systemFrameNumber.buf[0] = sfn_msb << 2;
  mib->message.choice.mib->systemFrameNumber.size = 1;
  mib->message.choice.mib->systemFrameNumber.bits_unused = 2;

  // 38.331 spare BIT STRING (SIZE (1))
  uint16_t *spare = CALLOC(1, sizeof(uint16_t));
  if (spare == NULL)
    abort();
  mib->message.choice.mib->spare.buf = (uint8_t *)spare;
  mib->message.choice.mib->spare.size = 1;
  mib->message.choice.mib->spare.bits_unused = 7; // This makes a spare of 1 bits

  AssertFatal(scc->ssbSubcarrierSpacing != NULL, "scc->ssbSubcarrierSpacing is null\n");
  int ssb_subcarrier_offset = 31; // default value for NSA
  if (IS_SA_MODE(get_softmodem_params())) {
    ssb_subcarrier_offset = get_ssb_subcarrier_offset(*scc->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencySSB,
                                                      scc->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencyPointA,
                                                      *scc->ssbSubcarrierSpacing);
  }
  mib->message.choice.mib->ssb_SubcarrierOffset = ssb_subcarrier_offset & 15;

  /*
   * The SIB1 will be sent in this allocation (Type0-PDCCH) : 38.213, 13-4 Table and 38.213 13-11 to 13-14 tables
   * the reverse allocation is in nr_ue_decode_mib()
   */
  const NR_PDCCH_ConfigCommon_t *pdcch_cc = scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon->choice.setup;
  long cset0 = pdcch_cc->controlResourceSetZero ? *pdcch_cc->controlResourceSetZero : 0;
  mib->message.choice.mib->pdcch_ConfigSIB1.controlResourceSetZero = cset0;
  long ss0 = pdcch_cc->searchSpaceZero ? *pdcch_cc->searchSpaceZero : 0;
  mib->message.choice.mib->pdcch_ConfigSIB1.searchSpaceZero = ss0;

  switch (*scc->ssbSubcarrierSpacing) {
    case NR_SubcarrierSpacing_kHz15:
      mib->message.choice.mib->subCarrierSpacingCommon = NR_MIB__subCarrierSpacingCommon_scs15or60;
      break;

    case NR_SubcarrierSpacing_kHz30:
      mib->message.choice.mib->subCarrierSpacingCommon = NR_MIB__subCarrierSpacingCommon_scs30or120;
      break;

    case NR_SubcarrierSpacing_kHz60:
      mib->message.choice.mib->subCarrierSpacingCommon = NR_MIB__subCarrierSpacingCommon_scs15or60;
      break;

    case NR_SubcarrierSpacing_kHz120:
      mib->message.choice.mib->subCarrierSpacingCommon = NR_MIB__subCarrierSpacingCommon_scs30or120;
      break;

    case NR_SubcarrierSpacing_kHz240:
      AssertFatal(1 == 0, "Unknown subCarrierSpacingCommon %d\n", (int)*scc->ssbSubcarrierSpacing);
      break;

    default:
      AssertFatal(1 == 0, "Unknown subCarrierSpacingCommon %d\n", (int)*scc->ssbSubcarrierSpacing);
  }

  switch (scc->dmrs_TypeA_Position) {
    case NR_ServingCellConfigCommon__dmrs_TypeA_Position_pos2:
      mib->message.choice.mib->dmrs_TypeA_Position = NR_MIB__dmrs_TypeA_Position_pos2;
      break;

    case NR_ServingCellConfigCommon__dmrs_TypeA_Position_pos3:
      mib->message.choice.mib->dmrs_TypeA_Position = NR_MIB__dmrs_TypeA_Position_pos3;
      break;

    default:
      AssertFatal(1 == 0, "Unknown dmrs_TypeA_Position %d\n", (int)scc->dmrs_TypeA_Position);
  }

  mib->message.choice.mib->cellBarred = NR_MIB__cellBarred_notBarred;
  mib->message.choice.mib->intraFreqReselection = NR_MIB__intraFreqReselection_notAllowed;
  return mib;
}

void free_MIB_NR(NR_BCCH_BCH_Message_t *mib)
{
  ASN_STRUCT_FREE(asn_DEF_NR_BCCH_BCH_Message, mib);
}

int encode_MIB_NR(NR_BCCH_BCH_Message_t *mib, int frame, uint8_t *buf, int buf_size)
{
  DevAssert(mib != NULL && mib->message.choice.mib->systemFrameNumber.buf != NULL);
  uint8_t sfn_msb = (uint8_t)((frame >> 4) & 0x3f);
  *mib->message.choice.mib->systemFrameNumber.buf = sfn_msb << 2;

  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_BCCH_BCH_Message, NULL, mib, buf, buf_size);
  AssertFatal(enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %lu)!\n", enc_rval.failed_type->name, enc_rval.encoded);
  LOG_D(NR_RRC, "Encoded MIB for frame %d sfn_msb %d, bits %lu\n", frame, sfn_msb, enc_rval.encoded);
  return (enc_rval.encoded + 7) / 8;
}

int encode_MIB_NR_setup(NR_MIB_t *mib, int frame, uint8_t *buf, int buf_size)
{
  DevAssert(mib != NULL);
  uint8_t sfn_msb = (uint8_t)((frame >> 4) & 0x3f);
  *mib->systemFrameNumber.buf = sfn_msb << 2;

  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_MIB, NULL, mib, buf, buf_size);
  AssertFatal(enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %lu)!\n", enc_rval.failed_type->name, enc_rval.encoded);
  LOG_D(NR_RRC, "Encoded MIB for frame %d sfn_msb %d, bits %lu\n", frame, sfn_msb, enc_rval.encoded);
  return (enc_rval.encoded + 7) / 8;
}

static struct NR_SSB_MTC__periodicityAndOffset get_SSB_MTC_periodicityAndOffset(long ssb_periodicityServingCell)
{
  struct NR_SSB_MTC__periodicityAndOffset po = {0};
  switch (ssb_periodicityServingCell) {
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms5:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf5;
      po.choice.sf5 = 0;
      break;
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms10:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf10;
      po.choice.sf10 = 0;
      break;
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms20:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf20;
      po.choice.sf20 = 0;
      break;
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms40:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf40;
      po.choice.sf40 = 0;
      break;
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms80:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf80;
      po.choice.sf80 = 0;
      break;
    case NR_ServingCellConfigCommon__ssb_periodicityServingCell_ms160:
      po.present = NR_SSB_MTC__periodicityAndOffset_PR_sf160;
      po.choice.sf160 = 0;
      break;
    default:
      AssertFatal(false, "illegal ssb_periodicityServingCell %ld\n", ssb_periodicityServingCell);
      break;
  }
  return po;
}

NR_MeasurementTimingConfiguration_t *get_new_MeasurementTimingConfiguration(const NR_ServingCellConfigCommon_t *scc)
{
  NR_MeasurementTimingConfiguration_t *mtc = calloc(1, sizeof(*mtc));
  AssertFatal(mtc != NULL, "out of memory\n");
  mtc->criticalExtensions.present = NR_MeasurementTimingConfiguration__criticalExtensions_PR_c1;
  mtc->criticalExtensions.choice.c1 = calloc(1, sizeof(*mtc->criticalExtensions.choice.c1));
  AssertFatal(mtc->criticalExtensions.choice.c1 != NULL, "out of memory\n");
  mtc->criticalExtensions.choice.c1->present = NR_MeasurementTimingConfiguration__criticalExtensions__c1_PR_measTimingConf;
  NR_MeasurementTimingConfiguration_IEs_t *mtc_ie = calloc(1, sizeof(*mtc_ie));
  AssertFatal(mtc_ie != NULL, "out of memory\n");
  mtc->criticalExtensions.choice.c1->choice.measTimingConf = mtc_ie;
  mtc_ie->measTiming = calloc(1, sizeof(*mtc_ie->measTiming));
  AssertFatal(mtc_ie->measTiming != NULL, "out of memory\n");

  asn1cSequenceAdd(mtc_ie->measTiming->list, NR_MeasTiming_t, mt);
  AssertFatal(mt != NULL, "out of memory\n");
  mt->frequencyAndTiming = calloc(1, sizeof(*mt->frequencyAndTiming));
  AssertFatal(mt->frequencyAndTiming != NULL, "out of memory\n");
  mt->frequencyAndTiming->carrierFreq = *scc->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencySSB;
  mt->frequencyAndTiming->ssbSubcarrierSpacing = *scc->ssbSubcarrierSpacing;

  NR_SSB_MTC_t *ssb_mtc = &mt->frequencyAndTiming->ssb_MeasurementTimingConfiguration;
  ssb_mtc->duration = NR_SSB_MTC__duration_sf1;
  ssb_mtc->periodicityAndOffset = get_SSB_MTC_periodicityAndOffset(*scc->ssb_periodicityServingCell);

  return mtc;
}

int encode_MeasurementTimingConfiguration(const struct NR_MeasurementTimingConfiguration *mtc, uint8_t *buf, int buf_len)
{
  DevAssert(mtc != NULL);
  if (LOG_DEBUGFLAG(DEBUG_ASN1))
    xer_fprint(stdout, &asn_DEF_NR_MeasurementTimingConfiguration, mtc);
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_MeasurementTimingConfiguration, NULL, mtc, buf, buf_len);
  AssertFatal(enc_rval.encoded > 0, "ASN1 message encoding failed (%s, %lu)!\n", enc_rval.failed_type->name, enc_rval.encoded);
  return (enc_rval.encoded + 7) / 8;
}

void free_MeasurementTimingConfiguration(NR_MeasurementTimingConfiguration_t *mtc)
{
  ASN_STRUCT_FREE(asn_DEF_NR_MeasurementTimingConfiguration, mtc);
}

static long get_NR_UE_TimersAndConstants_t300(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->t300) {
    case 100:
      return NR_UE_TimersAndConstants__t300_ms100;
    case 200:
      return NR_UE_TimersAndConstants__t300_ms200;
    case 300:
      return NR_UE_TimersAndConstants__t300_ms300;
    case 400:
      return NR_UE_TimersAndConstants__t300_ms400;
    case 600:
      return NR_UE_TimersAndConstants__t300_ms600;
    case 1000:
      return NR_UE_TimersAndConstants__t300_ms1000;
    case 1500:
      return NR_UE_TimersAndConstants__t300_ms1500;
    case 2000:
      return NR_UE_TimersAndConstants__t300_ms2000;
    default:
      AssertFatal(1 == 0, "Invalid value configured for t300!\n");
  }
}

static long get_NR_UE_TimersAndConstants_t301(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->t301) {
    case 100:
      return NR_UE_TimersAndConstants__t301_ms100;
    case 200:
      return NR_UE_TimersAndConstants__t301_ms200;
    case 300:
      return NR_UE_TimersAndConstants__t301_ms300;
    case 400:
      return NR_UE_TimersAndConstants__t301_ms400;
    case 600:
      return NR_UE_TimersAndConstants__t301_ms600;
    case 1000:
      return NR_UE_TimersAndConstants__t301_ms1000;
    case 1500:
      return NR_UE_TimersAndConstants__t301_ms1500;
    case 2000:
      return NR_UE_TimersAndConstants__t301_ms2000;
    default:
      AssertFatal(1 == 0, "Invalid value configured for t301!\n");
  }
}

static long get_NR_UE_TimersAndConstants_t310(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->t310) {
    case 0:
      return NR_UE_TimersAndConstants__t310_ms0;
    case 50:
      return NR_UE_TimersAndConstants__t310_ms50;
    case 100:
      return NR_UE_TimersAndConstants__t310_ms100;
    case 200:
      return NR_UE_TimersAndConstants__t310_ms200;
    case 500:
      return NR_UE_TimersAndConstants__t310_ms500;
    case 1000:
      return NR_UE_TimersAndConstants__t310_ms1000;
    case 2000:
      return NR_UE_TimersAndConstants__t310_ms2000;
    default:
      AssertFatal(1 == 0, "Invalid value configured for t310!\n");
  }
}

static long get_NR_UE_TimersAndConstants_n310(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->n310) {
    case 1:
      return NR_UE_TimersAndConstants__n310_n1;
    case 2:
      return NR_UE_TimersAndConstants__n310_n2;
    case 3:
      return NR_UE_TimersAndConstants__n310_n3;
    case 4:
      return NR_UE_TimersAndConstants__n310_n4;
    case 6:
      return NR_UE_TimersAndConstants__n310_n6;
    case 8:
      return NR_UE_TimersAndConstants__n310_n8;
    case 10:
      return NR_UE_TimersAndConstants__n310_n10;
    case 20:
      return NR_UE_TimersAndConstants__n310_n20;
    default:
      AssertFatal(1 == 0, "Invalid value configured for n310!\n");
  }
}

static long get_NR_UE_TimersAndConstants_t311(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->t311) {
    case 1000:
      return NR_UE_TimersAndConstants__t311_ms1000;
    case 3000:
      return NR_UE_TimersAndConstants__t311_ms3000;
    case 5000:
      return NR_UE_TimersAndConstants__t311_ms5000;
    case 10000:
      return NR_UE_TimersAndConstants__t311_ms10000;
    case 15000:
      return NR_UE_TimersAndConstants__t311_ms15000;
    case 20000:
      return NR_UE_TimersAndConstants__t311_ms20000;
    case 30000:
      return NR_UE_TimersAndConstants__t311_ms30000;
    default:
      AssertFatal(1 == 0, "Invalid value configured for t311!\n");
  }
}

static long get_NR_UE_TimersAndConstants_n311(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->n311) {
    case 1:
      return NR_UE_TimersAndConstants__n311_n1;
    case 2:
      return NR_UE_TimersAndConstants__n311_n2;
    case 3:
      return NR_UE_TimersAndConstants__n311_n3;
    case 4:
      return NR_UE_TimersAndConstants__n311_n4;
    case 5:
      return NR_UE_TimersAndConstants__n311_n5;
    case 6:
      return NR_UE_TimersAndConstants__n311_n6;
    case 8:
      return NR_UE_TimersAndConstants__n311_n8;
    case 10:
      return NR_UE_TimersAndConstants__n311_n10;
    default:
      AssertFatal(1 == 0, "Invalid value configured for n311!\n");
  }
}

static long get_NR_UE_TimersAndConstants_t319(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->t319) {
    case 100:
      return NR_UE_TimersAndConstants__t319_ms100;
    case 200:
      return NR_UE_TimersAndConstants__t319_ms200;
    case 300:
      return NR_UE_TimersAndConstants__t319_ms300;
    case 400:
      return NR_UE_TimersAndConstants__t319_ms400;
    case 600:
      return NR_UE_TimersAndConstants__t319_ms600;
    case 1000:
      return NR_UE_TimersAndConstants__t319_ms1000;
    case 1500:
      return NR_UE_TimersAndConstants__t319_ms1500;
    case 2000:
      return NR_UE_TimersAndConstants__t319_ms2000;
    default:
      AssertFatal(1 == 0, "Invalid value configured for t319!\n");
  }
}

void add_sib_to_systeminformation(NR_SystemInformation_IEs_t *si, struct NR_SystemInformation_IEs__sib_TypeAndInfo__Member *type)
{
  asn1cSeqAdd(&si->sib_TypeAndInfo.list, type);
}

void update_SIB1_NR_SI(NR_BCCH_DL_SCH_Message_t *sib1_bcch, int num_sibs, int sibs[num_sibs])
{
  NR_SIB1_t *sib1 = sib1_bcch->message.choice.c1->choice.systemInformationBlockType1;
  //si-SchedulingInfo
  NR_SI_SchedulingInfo_t *info = calloc(1, sizeof(*info));
  // TODO need to compute optimal SI-windowlength automatically
  //      based on the number of SSBs, TDD configuration and SS configuration
  info->si_WindowLength = NR_SI_SchedulingInfo__si_WindowLength_s10;
  NR_SchedulingInfo_t *schedulingInfo = calloc(1, sizeof(*schedulingInfo));
  schedulingInfo->si_BroadcastStatus = NR_SchedulingInfo__si_BroadcastStatus_broadcasting;
  schedulingInfo->si_Periodicity = NR_SchedulingInfo__si_Periodicity_rf16;
  bool reg_sib = false;
  NR_SI_SchedulingInfo_v1700_t *si_schedulingInfo_v17 = NULL;
  NR_SchedulingInfo2_r17_t *schedulingInfo2_r17 = NULL;
  bool v17_sib = false;
  for (int i = 0; i < num_sibs; i++) {
    int sib_num = sibs[i];
    AssertFatal(sib_num > 1 && sib_num < 21, "other SIB invalid type\n");
    if (sib_num < 15) {
      reg_sib = true;
      NR_SIB_TypeInfo_t *mapping = calloc(1, sizeof(*mapping));
      // NR_SIB_TypeInfo__type_sibType2 = 0
      // NR_SIB_TypeInfo__type_sibType3 = 1
      // and so on
      mapping->type = sib_num - 2;
      // The field is mandatory present if the type is different from SIB6, SIB7 or SIB8. For SIB6, SIB7 and SIB8 it is absent.
      if (sib_num != 6 && sib_num != 7 && sib_num != 8) {
        mapping->valueTag = calloc(1, sizeof(*mapping->valueTag));
        *mapping->valueTag = 0;
      }
      asn1cSeqAdd(&schedulingInfo->sib_MappingInfo.list, mapping);
    } else {
      if (v17_sib == false) {
        si_schedulingInfo_v17 = calloc(1, sizeof(*si_schedulingInfo_v17));
        schedulingInfo2_r17 = calloc(1, sizeof(*schedulingInfo2_r17));
        schedulingInfo2_r17->si_BroadcastStatus_r17 = NR_SchedulingInfo2_r17__si_BroadcastStatus_r17_broadcasting;
        schedulingInfo2_r17->si_Periodicity_r17 = NR_SchedulingInfo2_r17__si_Periodicity_r17_rf16;
        schedulingInfo2_r17->si_WindowPosition_r17 = 2;
      }
      NR_SIB_TypeInfo_v1700_t *mapping17 = calloc(1, sizeof(*mapping17));
      mapping17->sibType_r17.present = NR_SIB_TypeInfo_v1700__sibType_r17_PR_type1_r17;
      // NR_SIB_TypeInfo_v1700__sibType_r17__type1_r17_sibType15 = 0
      // and so on
      mapping17->sibType_r17.choice.type1_r17 = sib_num - 15;
      mapping17->valueTag_r17 = calloc(1, sizeof(*mapping17->valueTag_r17));
      *mapping17->valueTag_r17 = 0;
      asn1cSeqAdd(&schedulingInfo2_r17->sib_MappingInfo_r17.list, mapping17);
    }
  }
  AssertFatal(reg_sib, "At least 1 SIB from SchedulingInfo (SIB2 to SIB14 included) needs to be present\n");
  asn1cSeqAdd(&info->schedulingInfoList.list, schedulingInfo);
  sib1->si_SchedulingInfo = info;
  if (si_schedulingInfo_v17) {
    asn1cSeqAdd(&si_schedulingInfo_v17->schedulingInfoList2_r17.list, schedulingInfo2_r17);
    NR_SIB1_v1610_IEs_t *sib1_v1610 = NULL;
    if (sib1->nonCriticalExtension)
      sib1_v1610 = sib1->nonCriticalExtension;
    else {
      sib1_v1610 = calloc_or_fail(1, sizeof(*sib1_v1610));
      sib1->nonCriticalExtension = sib1_v1610;
    }
    NR_SIB1_v1630_IEs_t *sib1_v1630 = NULL;
    if (sib1_v1610->nonCriticalExtension)
      sib1_v1630 = sib1_v1610->nonCriticalExtension;
    else {
      sib1_v1630 = calloc_or_fail(1, sizeof(*sib1_v1630));
      sib1_v1610->nonCriticalExtension = sib1_v1630;
    }
    NR_SIB1_v1700_IEs_t *sib1_v17 = NULL;
    if (sib1_v1630->nonCriticalExtension)
      sib1_v17 = sib1_v1630->nonCriticalExtension;
    else {
      sib1_v17 = calloc_or_fail(1, sizeof(*sib1_v17));
      sib1_v1630->nonCriticalExtension = sib1_v17;
    }
    sib1_v17->si_SchedulingInfo_v1700 = si_schedulingInfo_v17;
  }
  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_BCCH_DL_SCH_Message, sib1_bcch);
  }
}

int encode_sysinfo_ie(NR_SystemInformation_IEs_t *sysInfo, uint8_t *buf, int len)
{
  if (sysInfo->sib_TypeAndInfo.list.count == 0)
    return 0;
  NR_SystemInformation_t si = {.criticalExtensions.present = NR_SystemInformation__criticalExtensions_PR_systemInformation};
  si.criticalExtensions.choice.systemInformation = sysInfo;
  struct NR_BCCH_DL_SCH_MessageType__c1 c1 = {.present = NR_BCCH_DL_SCH_MessageType__c1_PR_systemInformation};
  c1.choice.systemInformation = &si;
  NR_BCCH_DL_SCH_MessageType_t message_type = {.present = NR_BCCH_DL_SCH_MessageType_PR_c1};
  message_type.choice.c1 = &c1;
  NR_BCCH_DL_SCH_Message_t sib_message = {.message = message_type};
  return encode_SIB_NR(&sib_message, buf, len);
}

static bool is_ntn_band(int band)
{
  // TS 3GPP 38.101-5 V1807 Section 5.2.2
  if (band >= 254 && band <= 256) // FR1 NTN
    return true;

  // TS 3GPP 38.101-5 V1807 Section 5.2.3
  if (band >= 510 && band <= 512) // FR2 NTN
    return true;

  return false;
}

static BIT_STRING_t bit_string_clone(const BIT_STRING_t *orig)
{
  BIT_STRING_t bs = {.size = orig->size, .bits_unused = orig->bits_unused};
  bs.buf = malloc_or_fail(bs.size * sizeof(*bs.buf));
  memcpy(bs.buf, orig->buf, bs.size);
  return bs;
}

NR_BCCH_DL_SCH_Message_t *get_SIB1_NR(const NR_ServingCellConfigCommon_t *scc,
                                      const plmn_id_t *plmn,
                                      uint64_t cellID,
                                      int tac,
                                      const nr_mac_config_t *mac_config)
{
  AssertFatal(cellID < (1l << 36), "cellID must fit within 36 bits, but is %lu\n", cellID);

  NR_BCCH_DL_SCH_Message_t *sib1_message = CALLOC(1,sizeof(NR_BCCH_DL_SCH_Message_t));
  AssertFatal(sib1_message != NULL, "out of memory\n");
  sib1_message->message.present = NR_BCCH_DL_SCH_MessageType_PR_c1;
  sib1_message->message.choice.c1 = CALLOC(1,sizeof(struct NR_BCCH_DL_SCH_MessageType__c1));
  AssertFatal(sib1_message->message.choice.c1 != NULL, "out of memory\n");
  sib1_message->message.choice.c1->present = NR_BCCH_DL_SCH_MessageType__c1_PR_systemInformationBlockType1;
  sib1_message->message.choice.c1->choice.systemInformationBlockType1 = CALLOC(1,sizeof(struct NR_SIB1));
  AssertFatal(sib1_message->message.choice.c1->choice.systemInformationBlockType1 != NULL, "out of memory\n");
  NR_SIB1_t *sib1 = sib1_message->message.choice.c1->choice.systemInformationBlockType1;

  // cellSelectionInfo
  sib1->cellSelectionInfo = CALLOC(1,sizeof(*sib1->cellSelectionInfo));
  AssertFatal(sib1->cellSelectionInfo != NULL, "out of memory\n");
  // Fixme: should be in config file
  //The IE Q-RxLevMin is used to indicate for cell selection/ re-selection the required minimum received RSRP level in the (NR) cell.
  //Corresponds to parameter Qrxlevmin in TS38.304.
  //Actual value Qrxlevmin = field value * 2 [dBm].
  sib1->cellSelectionInfo->q_RxLevMin = -65;

  // cellAccessRelatedInfo
  // TODO : Add support for more than one PLMN
  int num_plmn = 1; // int num_plmn = configuration->num_plmn;
  asn1cSequenceAdd(sib1->cellAccessRelatedInfo.plmn_IdentityInfoList.list, struct NR_PLMN_IdentityInfo, nr_plmn_info);
  for (int i = 0; i < num_plmn; ++i) {
    asn1cSequenceAdd(nr_plmn_info->plmn_IdentityList.list, struct NR_PLMN_Identity, nr_plmn);
    asn1cCalloc(nr_plmn->mcc, mcc);
    int confMcc = plmn->mcc;
    asn1cSequenceAdd(mcc->list, NR_MCC_MNC_Digit_t, mcc0);
    *mcc0 = (confMcc / 100) % 10;
    asn1cSequenceAdd(mcc->list, NR_MCC_MNC_Digit_t, mcc1);
    *mcc1 = (confMcc / 10) % 10;
    asn1cSequenceAdd(mcc->list, NR_MCC_MNC_Digit_t, mcc2);
    *mcc2 = confMcc % 10;
    int mnc = plmn->mnc;
    if (plmn->mnc_digit_length == 3) {
      asn1cSequenceAdd(nr_plmn->mnc.list, NR_MCC_MNC_Digit_t, mnc0);
      *mnc0 = (mnc / 100) % 10;
    }
    asn1cSequenceAdd(nr_plmn->mnc.list, NR_MCC_MNC_Digit_t, mnc1);
    *mnc1 = (mnc / 10) % 10;
    asn1cSequenceAdd(nr_plmn->mnc.list, NR_MCC_MNC_Digit_t, mnc2);
    *mnc2 = (mnc) % 10;
  }

  NR_CELL_ID_TO_BIT_STRING(cellID, &nr_plmn_info->cellIdentity);
  nr_plmn_info->cellReservedForOperatorUse = NR_PLMN_IdentityInfo__cellReservedForOperatorUse_notReserved;

  nr_plmn_info->trackingAreaCode = CALLOC(1, sizeof(NR_TrackingAreaCode_t));
  AssertFatal(nr_plmn_info->trackingAreaCode != NULL, "out of memory\n");
  uint32_t tmp2 = htobe32(tac);
  nr_plmn_info->trackingAreaCode->buf = CALLOC(1, 3);
  AssertFatal(nr_plmn_info->trackingAreaCode->buf != NULL, "out of memory\n");
  memcpy(nr_plmn_info->trackingAreaCode->buf, ((char *)&tmp2) + 1, 3);
  nr_plmn_info->trackingAreaCode->size = 3;
  nr_plmn_info->trackingAreaCode->bits_unused = 0;

  // connEstFailureControl
  // TODO: add connEstFailureControl

  // servingCellConfigCommon
  asn1cCalloc(sib1->servingCellConfigCommon, ServCellCom);
  NR_BWP_DownlinkCommon_t *initialDownlinkBWP = &ServCellCom->downlinkConfigCommon.initialDownlinkBWP;
  initialDownlinkBWP->genericParameters = clone_generic_parameters(&scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters);

  const NR_FrequencyInfoDL_t *frequencyInfoDL = scc->downlinkConfigCommon->frequencyInfoDL;
  for (int i = 0; i < frequencyInfoDL->frequencyBandList.list.count; i++) {
    asn1cSequenceAdd(ServCellCom->downlinkConfigCommon.frequencyInfoDL.frequencyBandList.list,
                     struct NR_NR_MultiBandInfo,
                     nrMultiBandInfo);
    asn1cCallocOne(nrMultiBandInfo->freqBandIndicatorNR, *frequencyInfoDL->frequencyBandList.list.array[i]);
  }

  const NR_FreqBandIndicatorNR_t band = *frequencyInfoDL->frequencyBandList.list.array[0];
  frequency_range_t frequency_range = get_freq_range_from_band(band);
  sib1->servingCellConfigCommon->downlinkConfigCommon.frequencyInfoDL.offsetToPointA = get_ssb_offset_to_pointA(*scc->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencySSB,
                               scc->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencyPointA,
                               scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.subcarrierSpacing,
                               frequency_range);

  LOG_I(NR_RRC,
	"SIB1 freq: offsetToPointA %d\n",
        (int)sib1->servingCellConfigCommon->downlinkConfigCommon.frequencyInfoDL.offsetToPointA);
  
  for (int i = 0; i < frequencyInfoDL->scs_SpecificCarrierList.list.count; i++) {
    const NR_SCS_SpecificCarrier_t *orig = frequencyInfoDL->scs_SpecificCarrierList.list.array[i];
    NR_SCS_SpecificCarrier_t *new = NULL;
    const int copy_result = asn_copy(&asn_DEF_NR_SCS_SpecificCarrier, (void **)&new, orig);
    AssertFatal(copy_result == 0, "unable to copy NR_SCS_SpecificCarrier from scc to SIB1 structure\n");
    asn1cSeqAdd(&ServCellCom->downlinkConfigCommon.frequencyInfoDL.scs_SpecificCarrierList.list, new);
  }

  initialDownlinkBWP->pdcch_ConfigCommon = clone_pdcch_configcommon(scc->downlinkConfigCommon->initialDownlinkBWP->pdcch_ConfigCommon);
  AssertFatal(initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->commonSearchSpaceList != NULL,
              "expected commonSearchSpaceList to be populated through SCC\n");
  AssertFatal(initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->searchSpaceSIB1 != NULL,
              "expected searchSpaceSIB1 to be populated through SCC\n");
  AssertFatal(initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->ra_SearchSpace != NULL,
              "expected ra_SearchSpace to be populated through SCC\n");
  AssertFatal(initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->pagingSearchSpace != NULL,
              "expected pagingSearchSpace to be populated through SCC\n");
  AssertFatal(initialDownlinkBWP->pdcch_ConfigCommon->choice.setup->searchSpaceOtherSystemInformation != NULL,
              "expected searchSpaceOtherSystemInformation to be populated through SCC\n");

  initialDownlinkBWP->pdsch_ConfigCommon = clone_pdsch_configcommon(scc->downlinkConfigCommon->initialDownlinkBWP->pdsch_ConfigCommon);
  ServCellCom->downlinkConfigCommon.bcch_Config.modificationPeriodCoeff = NR_BCCH_Config__modificationPeriodCoeff_n2;
  ServCellCom->downlinkConfigCommon.pcch_Config.defaultPagingCycle = NR_PagingCycle_rf256;
  ServCellCom->downlinkConfigCommon.pcch_Config.nAndPagingFrameOffset.present = NR_PCCH_Config__nAndPagingFrameOffset_PR_quarterT;
  ServCellCom->downlinkConfigCommon.pcch_Config.nAndPagingFrameOffset.choice.quarterT = 1;
  ServCellCom->downlinkConfigCommon.pcch_Config.ns = NR_PCCH_Config__ns_one;

  asn1cCalloc(ServCellCom->downlinkConfigCommon.pcch_Config.firstPDCCH_MonitoringOccasionOfPO, P0);
  P0->present = NR_PCCH_Config__firstPDCCH_MonitoringOccasionOfPO_PR_sCS120KHZoneT_SCS60KHZhalfT_SCS30KHZquarterT_SCS15KHZoneEighthT;

  asn1cCalloc(P0->choice.sCS120KHZoneT_SCS60KHZhalfT_SCS30KHZquarterT_SCS15KHZoneEighthT, Z8);
  asn1cSequenceAdd(Z8->list, long, ZoneEight);
  *ZoneEight = 0;

  asn1cCalloc(ServCellCom->uplinkConfigCommon, UL);
  asn_set_empty(&UL->frequencyInfoUL.scs_SpecificCarrierList.list);
  const NR_FrequencyInfoUL_t *frequencyInfoUL = scc->uplinkConfigCommon->frequencyInfoUL;
  for (int i = 0; i < frequencyInfoUL->scs_SpecificCarrierList.list.count; i++) {
    const NR_SCS_SpecificCarrier_t *orig = frequencyInfoUL->scs_SpecificCarrierList.list.array[i];
    NR_SCS_SpecificCarrier_t *new = NULL;
    const int copy_result = asn_copy(&asn_DEF_NR_SCS_SpecificCarrier, (void **)&new, orig);
    AssertFatal(copy_result == 0, "unable to copy NR_SCS_SpecificCarrier from scc to SIB1 structure\n");
    asn1cSeqAdd(&UL->frequencyInfoUL.scs_SpecificCarrierList.list, new);
  }

  asn1cCallocOne(UL->frequencyInfoUL.p_Max, *frequencyInfoUL->p_Max);

  frame_type_t frame_type =
      get_frame_type((int)*scc->downlinkConfigCommon->frequencyInfoDL->frequencyBandList.list.array[0],
                     *scc->ssbSubcarrierSpacing);

  if (frame_type == FDD) {
    UL->frequencyInfoUL.absoluteFrequencyPointA = malloc(sizeof(*UL->frequencyInfoUL.absoluteFrequencyPointA));
    AssertFatal(UL->frequencyInfoUL.absoluteFrequencyPointA != NULL, "out of memory\n");
    *UL->frequencyInfoUL.absoluteFrequencyPointA =
        *scc->uplinkConfigCommon->frequencyInfoUL->absoluteFrequencyPointA;
    UL->frequencyInfoUL.frequencyBandList = calloc(1, sizeof(*UL->frequencyInfoUL.frequencyBandList));
    AssertFatal(UL->frequencyInfoUL.frequencyBandList != NULL, "out of memory\n");
    for (int i = 0; i < frequencyInfoUL->frequencyBandList->list.count; i++) {
      asn1cSequenceAdd(UL->frequencyInfoUL.frequencyBandList->list, struct NR_NR_MultiBandInfo, nrMultiBandInfo);
      asn1cCallocOne(nrMultiBandInfo->freqBandIndicatorNR, *frequencyInfoUL->frequencyBandList->list.array[i]);
    }
  }

  UL->initialUplinkBWP.genericParameters = clone_generic_parameters(&scc->uplinkConfigCommon->initialUplinkBWP->genericParameters);
  UL->initialUplinkBWP.rach_ConfigCommon = clone_rach_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->rach_ConfigCommon);

  if (scc->uplinkConfigCommon->initialUplinkBWP->ext1) {
    NR_SetupRelease_MsgA_ConfigCommon_r16_t *msgA_configcommon =
        clone_msga_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->ext1->msgA_ConfigCommon_r16);
    if (msgA_configcommon) {
      // Add the struct ext1
      UL->initialUplinkBWP.ext1 = calloc(1, sizeof(*UL->initialUplinkBWP.ext1));
      UL->initialUplinkBWP.ext1->msgA_ConfigCommon_r16 = msgA_configcommon;
    }
  }

  UL->initialUplinkBWP.pusch_ConfigCommon = clone_pusch_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->pusch_ConfigCommon);
  free(UL->initialUplinkBWP.pusch_ConfigCommon->choice.setup->groupHoppingEnabledTransformPrecoding);
  UL->initialUplinkBWP.pusch_ConfigCommon->choice.setup->groupHoppingEnabledTransformPrecoding = NULL;

  UL->initialUplinkBWP.pucch_ConfigCommon = clone_pucch_configcommon(scc->uplinkConfigCommon->initialUplinkBWP->pucch_ConfigCommon);

  UL->timeAlignmentTimerCommon = NR_TimeAlignmentTimer_infinity;

  ServCellCom->n_TimingAdvanceOffset = scc->n_TimingAdvanceOffset;

  uint8_t bitmap8,temp_bitmap=0;
  switch (scc->ssb_PositionsInBurst->present) {
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_shortBitmap:
      ServCellCom->ssb_PositionsInBurst.inOneGroup = bit_string_clone(&scc->ssb_PositionsInBurst->choice.shortBitmap);
      break;
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_mediumBitmap:
      ServCellCom->ssb_PositionsInBurst.inOneGroup = bit_string_clone(&scc->ssb_PositionsInBurst->choice.mediumBitmap);
      break;
    /*
     * groupPresence: This field is present when maximum number of SS/PBCH blocks per half frame equals to 64 as defined in
     * TS 38.213 [13], clause 4.1. The first/leftmost bit corresponds to the SS/PBCH index 0-7, the second bit corresponds to
     * SS/PBCH block 8-15, and so on. Value 0 in the bitmap indicates that the SSBs according to inOneGroup are absent. Value 1
     * indicates that the SS/PBCH blocks are transmitted in accordance with inOneGroup. inOneGroup: When maximum number of SS/PBCH
     * blocks per half frame equals to 64 as defined in TS 38.213 [13], clause 4.1, all 8 bit are valid; The first/ leftmost bit
     * corresponds to the first SS/PBCH block index in the group (i.e., to SSB index 0, 8, and so on); the second bit corresponds to
     * the second SS/PBCH block index in the group (i.e., to SSB index 1, 9, and so on), and so on. Value 0 in the bitmap indicates
     * that the corresponding SS/PBCH block is not transmitted while value 1 indicates that the corresponding SS/PBCH block is
     * transmitted.
     */
    case NR_ServingCellConfigCommon__ssb_PositionsInBurst_PR_longBitmap:
      ServCellCom->ssb_PositionsInBurst.inOneGroup.buf = calloc_or_fail(1, sizeof(uint8_t));
      ServCellCom->ssb_PositionsInBurst.inOneGroup.size = 1;
      ServCellCom->ssb_PositionsInBurst.inOneGroup.bits_unused = 0;
      ServCellCom->ssb_PositionsInBurst.groupPresence = calloc(1, sizeof(BIT_STRING_t));
      AssertFatal(ServCellCom->ssb_PositionsInBurst.groupPresence != NULL, "out of memory\n");
      ServCellCom->ssb_PositionsInBurst.groupPresence->size = 1;
      ServCellCom->ssb_PositionsInBurst.groupPresence->bits_unused = 0;
      ServCellCom->ssb_PositionsInBurst.groupPresence->buf = calloc(1, sizeof(uint8_t));
      AssertFatal(ServCellCom->ssb_PositionsInBurst.groupPresence->buf != NULL, "out of memory\n");
      ServCellCom->ssb_PositionsInBurst.groupPresence->buf[0] = 0;
      for (int i = 0; i < 8; i++) {
        bitmap8 = scc->ssb_PositionsInBurst->choice.longBitmap.buf[i];
        if (bitmap8 != 0) {
          if (temp_bitmap == 0)
            temp_bitmap = bitmap8;
          else
            AssertFatal(temp_bitmap == bitmap8,
                        "For longBitmap the groups of 8 SSBs containing at least 1 transmitted SSB should be all the same\n");

          ServCellCom->ssb_PositionsInBurst.inOneGroup.buf[0] = bitmap8;
          ServCellCom->ssb_PositionsInBurst.groupPresence->buf[0] |= 1<<(7-i);
        }
      }
      break;
    default:
      AssertFatal(false, "ssb_PositionsInBurst not present\n");
      break;
  }

  ServCellCom->ssb_PeriodicityServingCell = *scc->ssb_periodicityServingCell;
  if (scc->tdd_UL_DL_ConfigurationCommon) {
    int copy_result = asn_copy(&asn_DEF_NR_TDD_UL_DL_ConfigCommon, (void **)&ServCellCom->tdd_UL_DL_ConfigurationCommon, scc->tdd_UL_DL_ConfigurationCommon);
    AssertFatal(copy_result == 0, "Was unable to copy tdd_UL_DL_ConfigurationCommon from scc to SIB19 structure\n");
  }
  ServCellCom->ss_PBCH_BlockPower = scc->ss_PBCH_BlockPower;

  // ims-EmergencySupport
  // TODO: add ims-EmergencySupport

  // eCallOverIMS-Support
  // TODO: add eCallOverIMS-Support

  // ue-TimersAndConstants
  sib1->ue_TimersAndConstants = CALLOC(1,sizeof(struct NR_UE_TimersAndConstants));
  AssertFatal(sib1->ue_TimersAndConstants != NULL, "out of memory\n");
  const nr_mac_timers_t *timer_config = &mac_config->timer_config;
  sib1->ue_TimersAndConstants->t300 = get_NR_UE_TimersAndConstants_t300(timer_config);
  sib1->ue_TimersAndConstants->t301 = get_NR_UE_TimersAndConstants_t301(timer_config);
  sib1->ue_TimersAndConstants->t310 = get_NR_UE_TimersAndConstants_t310(timer_config);
  sib1->ue_TimersAndConstants->n310 = get_NR_UE_TimersAndConstants_n310(timer_config);
  sib1->ue_TimersAndConstants->t311 = get_NR_UE_TimersAndConstants_t311(timer_config);
  sib1->ue_TimersAndConstants->n311 = get_NR_UE_TimersAndConstants_n311(timer_config);
  sib1->ue_TimersAndConstants->t319 = get_NR_UE_TimersAndConstants_t319(timer_config);

  // uac-BarringInfo
  /*sib1->uac_BarringInfo = CALLOC(1, sizeof(struct NR_SIB1__uac_BarringInfo));
  NR_UAC_BarringInfoSet_t *nr_uac_BarringInfoSet = CALLOC(1, sizeof(NR_UAC_BarringInfoSet_t));
  asn_set_empty(&sib1->uac_BarringInfo->uac_BarringInfoSetList);
  nr_uac_BarringInfoSet->uac_BarringFactor = NR_UAC_BarringInfoSet__uac_BarringFactor_p95;
  nr_uac_BarringInfoSet->uac_BarringTime = NR_UAC_BarringInfoSet__uac_BarringTime_s4;
  nr_uac_BarringInfoSet->uac_BarringForAccessIdentity.buf = CALLOC(1, 1);
  nr_uac_BarringInfoSet->uac_BarringForAccessIdentity.size = 1;
  nr_uac_BarringInfoSet->uac_BarringForAccessIdentity.bits_unused = 1;
  asn1cSeqAdd(&sib1->uac_BarringInfo->uac_BarringInfoSetList, nr_uac_BarringInfoSet);*/

  // useFullResumeID
  // TODO: add useFullResumeID

  // lateNonCriticalExtension
  // TODO: add lateNonCriticalExtension

  // nonCriticalExtension
  if (mac_config->redcap || (scc->ext2 && scc->ext2->ntn_Config_r17)) {
    sib1->nonCriticalExtension = calloc_or_fail(1, sizeof(*sib1->nonCriticalExtension));
    NR_SIB1_v1610_IEs_t *sib1_1610 = sib1->nonCriticalExtension;
    sib1_1610->nonCriticalExtension = calloc_or_fail(1, sizeof(*sib1_1610->nonCriticalExtension));
    NR_SIB1_v1630_IEs_t *sib1_1630 = sib1_1610->nonCriticalExtension;
    sib1_1630->nonCriticalExtension = calloc_or_fail(1, sizeof(*sib1_1630->nonCriticalExtension));
    NR_SIB1_v1700_IEs_t *sib1_1700 = sib1_1630->nonCriticalExtension;

    if (mac_config->redcap) {
      sib1_1700->redCap_ConfigCommon_r17 = calloc_or_fail(1, sizeof(*sib1_1700->redCap_ConfigCommon_r17));
      sib1_1700->redCap_ConfigCommon_r17->cellBarredRedCap_r17 =
          calloc_or_fail(1, sizeof(*sib1_1700->redCap_ConfigCommon_r17->cellBarredRedCap_r17));

      const nr_redcap_config_t *redcap_config = mac_config->redcap;
      struct NR_RedCap_ConfigCommonSIB_r17__cellBarredRedCap_r17 *CellBarredRedCap_r17 =
          sib1_1700->redCap_ConfigCommon_r17->cellBarredRedCap_r17;
      CellBarredRedCap_r17->cellBarredRedCap1Rx_r17 = redcap_config->cellBarredRedCap1Rx_r17;
      CellBarredRedCap_r17->cellBarredRedCap2Rx_r17 = redcap_config->cellBarredRedCap2Rx_r17;
      sib1_1700->intraFreqReselectionRedCap_r17 = calloc_or_fail(1, sizeof(*sib1_1700->intraFreqReselectionRedCap_r17));
      *sib1_1700->intraFreqReselectionRedCap_r17 = redcap_config->intraFreqReselectionRedCap_r17;
    }

    if (is_ntn_band(band)) {
      // If cell provides NTN access, set cellBarredNTN to notBarred.
      asn1cCallocOne(sib1_1700->cellBarredNTN_r17, NR_SIB1_v1700_IEs__cellBarredNTN_r17_notBarred);
    }
  }

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_BCCH_DL_SCH_Message, sib1_message);
  }

  return sib1_message;
}

void free_SIB1_NR(NR_BCCH_DL_SCH_Message_t *sib1)
{
  ASN_STRUCT_FREE(asn_DEF_NR_BCCH_DL_SCH_Message, sib1);
}

int encode_SIB_NR(NR_BCCH_DL_SCH_Message_t *sib, uint8_t *buffer, int max_buffer_size)
{
  AssertFatal(max_buffer_size <= NR_MAX_SIB_LENGTH / 8,
              "Maximum buffer size too large: 3GPP TS 38.331 section 5.2.1 - The physical layer imposes a limit to the "
              "maximum size a SIB can take. The maximum SIB1 or SI message size is 2976 bits.\n");
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_BCCH_DL_SCH_Message, NULL, sib, buffer, max_buffer_size);
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded <= max_buffer_size * 8, "ASN1 message encoding failed (%s, %lu)!\n", enc_rval.failed_type->name, enc_rval.encoded);
  return (enc_rval.encoded + 7) / 8;
}

NR_SIB19_r17_t *get_SIB19_NR(const NR_ServingCellConfigCommon_t *scc)
{
  NR_SIB19_r17_t *sib19 = calloc(1, sizeof(*sib19));
  // use ntn-config from NR_ServingCellConfigCommon_t
  const int copy_result = asn_copy(&asn_DEF_NR_NTN_Config_r17, (void **) &sib19->ntn_Config_r17, scc->ext2->ntn_Config_r17);
  AssertFatal(copy_result == 0, "Was unable to copy ntn_Config_r17 from scc to SIB19 structure\n");
  return sib19;
}

void free_SIB19_NR(NR_BCCH_DL_SCH_Message_t *sib19)
{
  ASN_STRUCT_FREE(asn_DEF_NR_BCCH_DL_SCH_Message, sib19);
}

static NR_PhysicalCellGroupConfig_t *configure_phy_cellgroup(void)
{
  NR_PhysicalCellGroupConfig_t *physicalCellGroupConfig = calloc(1, sizeof(*physicalCellGroupConfig));
  AssertFatal(physicalCellGroupConfig != NULL, "Couldn't allocate physicalCellGroupConfig. Out of memory!\n");
  physicalCellGroupConfig->pdsch_HARQ_ACK_Codebook = NR_PhysicalCellGroupConfig__pdsch_HARQ_ACK_Codebook_dynamic;
  return physicalCellGroupConfig;
}

static long *get_sr_ProhibitTimer(const nr_mac_timers_t *timer_config)
{
  if (timer_config->sr_ProhibitTimer == 0)
    return NULL;

  long *ret = calloc(1, sizeof(*ret));
  switch (timer_config->sr_ProhibitTimer) {
    case 1:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms1;
      break;
    case 2:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms2;
      break;
    case 4:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms4;
      break;
    case 8:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms8;
      break;
    case 16:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms16;
      break;
    case 32:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms32;
      break;
    case 64:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms64;
      break;
    case 128:
      *ret = NR_SchedulingRequestToAddMod__sr_ProhibitTimer_ms128;
      break;
    default:
      AssertFatal(1 == 0, "Invalid value configured for sr_ProhibitTimer!\n");
  }
  return ret;
}

static long get_sr_TransMax(const nr_mac_timers_t *timer_config)
{
  switch (timer_config->sr_TransMax) {
    case 4:
      return NR_SchedulingRequestToAddMod__sr_TransMax_n4;
    case 8:
      return NR_SchedulingRequestToAddMod__sr_TransMax_n8;
    case 16:
      return NR_SchedulingRequestToAddMod__sr_TransMax_n16;
    case 32:
      return NR_SchedulingRequestToAddMod__sr_TransMax_n32;
    case 64:
      return NR_SchedulingRequestToAddMod__sr_TransMax_n64;
    default:
      AssertFatal(1 == 0, "Invalid value configured for sr_TransMax!\n");
  }
}

static long *get_sr_ProhibitTimer_v1700(const nr_mac_timers_t *timer_config)
{
  if (timer_config->sr_ProhibitTimer_v1700 == 0)
    return NULL;

  long *ret = calloc(1, sizeof(*ret));
  switch (timer_config->sr_ProhibitTimer_v1700) {
    case 192:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms192;
      break;
    case 256:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms256;
      break;
    case 320:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms320;
      break;
    case 384:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms384;
      break;
    case 448:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms448;
      break;
    case 512:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms512;
      break;
    case 576:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms576;
      break;
    case 640:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms640;
      break;
    case 1082:
      *ret = NR_SchedulingRequestToAddModExt_v1700__sr_ProhibitTimer_v1700_ms1082;
      break;
    default:
      AssertFatal(1 == 0, "Invalid value configured for sr_ProhibitTimer_v1700!\n");
  }
  return ret;
}

static NR_MAC_CellGroupConfig_t *configure_mac_cellgroup(const nr_mac_timers_t *timer_config)
{
  NR_MAC_CellGroupConfig_t * mac_CellGroupConfig = calloc(1, sizeof(*mac_CellGroupConfig));
  AssertFatal(mac_CellGroupConfig != NULL, "Couldn't allocate mac-CellGroupConfig. Out of memory!\n");
  mac_CellGroupConfig->bsr_Config = calloc(1, sizeof(*mac_CellGroupConfig->bsr_Config));
  mac_CellGroupConfig->bsr_Config->periodicBSR_Timer = NR_BSR_Config__periodicBSR_Timer_sf10;
  mac_CellGroupConfig->bsr_Config->retxBSR_Timer = NR_BSR_Config__retxBSR_Timer_sf80;
  mac_CellGroupConfig->tag_Config = calloc(1, sizeof(*mac_CellGroupConfig->tag_Config));
  mac_CellGroupConfig->tag_Config->tag_ToReleaseList = NULL;
  mac_CellGroupConfig->tag_Config->tag_ToAddModList = calloc(1,sizeof(*mac_CellGroupConfig->tag_Config->tag_ToAddModList));
  struct NR_TAG *tag=calloc(1,sizeof(*tag));
  tag->tag_Id = 0;
  tag->timeAlignmentTimer = NR_TimeAlignmentTimer_infinity;
  asn1cSeqAdd(&mac_CellGroupConfig->tag_Config->tag_ToAddModList->list,tag);

  set_phr_config(mac_CellGroupConfig);

  mac_CellGroupConfig->schedulingRequestConfig = calloc(1, sizeof(*mac_CellGroupConfig->schedulingRequestConfig));
  mac_CellGroupConfig->schedulingRequestConfig->schedulingRequestToAddModList = CALLOC(1,sizeof(*mac_CellGroupConfig->schedulingRequestConfig->schedulingRequestToAddModList));
  struct NR_SchedulingRequestToAddMod *schedulingrequestlist = CALLOC(1,sizeof(*schedulingrequestlist));
  schedulingrequestlist->schedulingRequestId = 0;
  schedulingrequestlist->sr_ProhibitTimer = get_sr_ProhibitTimer(timer_config);
  schedulingrequestlist->sr_TransMax = get_sr_TransMax(timer_config);
  asn1cSeqAdd(&(mac_CellGroupConfig->schedulingRequestConfig->schedulingRequestToAddModList->list),schedulingrequestlist);

  if (timer_config->sr_ProhibitTimer_v1700 != 0) {
    mac_CellGroupConfig->ext4 = calloc(1, sizeof(*mac_CellGroupConfig->ext4));
    mac_CellGroupConfig->ext4->schedulingRequestConfig_v1700 =
        calloc(1, sizeof(*mac_CellGroupConfig->ext4->schedulingRequestConfig_v1700));
    mac_CellGroupConfig->ext4->schedulingRequestConfig_v1700->schedulingRequestToAddModListExt_v1700 =
        calloc(1, sizeof(*mac_CellGroupConfig->ext4->schedulingRequestConfig_v1700->schedulingRequestToAddModListExt_v1700));
    struct NR_SchedulingRequestToAddModExt_v1700 *schedulingrequestlist_v1700 = calloc(1, sizeof(*schedulingrequestlist_v1700));
    schedulingrequestlist_v1700->sr_ProhibitTimer_v1700 = get_sr_ProhibitTimer_v1700(timer_config);
    asn1cSeqAdd(&(mac_CellGroupConfig->ext4->schedulingRequestConfig_v1700->schedulingRequestToAddModListExt_v1700->list),
                schedulingrequestlist_v1700);
  }

  mac_CellGroupConfig->skipUplinkTxDynamic=false;
  mac_CellGroupConfig->ext1 = NULL;
  return mac_CellGroupConfig;
}

// Set HARQ related IEs according to the number of DL, UL harqprocesses configured
static void fill_harq_IEs(NR_ServingCellConfig_t *scc, int num_dlharq, int num_ulharq)
{

  AssertFatal(scc && scc->pdsch_ServingCellConfig &&
              scc->pdsch_ServingCellConfig->present == NR_SetupRelease_PDSCH_ServingCellConfig_PR_setup,
              "PDSCH_Servingcellconfig IEs NOT present\n");

  NR_PDSCH_ServingCellConfig_t *pdsch_scc = scc->pdsch_ServingCellConfig->choice.setup;

  switch (num_dlharq) {
    case 2:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n2;
      break;
    case 4:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n4;
      break;
    case 6:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n6;
      break;
    case 8:
      // 8 if IEs nrofHARQ_ProcessesForPDSCH and nrofHARQ_ProcessesForPDSCH_v1700 are not present
      free_and_zero(pdsch_scc->nrofHARQ_ProcessesForPDSCH);
      break;
    case 10:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n10;
      break;
    case 12:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n12;
      break;
    case 16:
      *pdsch_scc->nrofHARQ_ProcessesForPDSCH = NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n16;
      break;
    case 32:
      if (!pdsch_scc->ext3)
        pdsch_scc->ext3 = calloc(1, sizeof(*pdsch_scc->ext3));
      asn1cCallocOne(pdsch_scc->ext3->nrofHARQ_ProcessesForPDSCH_v1700,
                     NR_PDSCH_ServingCellConfig__ext3__nrofHARQ_ProcessesForPDSCH_v1700_n32);
      NR_BWP_DownlinkDedicated_t *dlbwp = scc->initialDownlinkBWP;
      if (dlbwp && dlbwp->pdsch_Config &&
          dlbwp->pdsch_Config->present == NR_SetupRelease_PDSCH_Config_PR_setup) {
        NR_PDSCH_Config_t *dlcfg = scc->initialDownlinkBWP->pdsch_Config->choice.setup;
        if (!dlcfg->ext3)
          dlcfg->ext3 = calloc(1, sizeof(*dlcfg->ext3));
        asn1cCallocOne(dlcfg->ext3->harq_ProcessNumberSizeDCI_1_1_r17, 5);
      }
      int num_dl_bwp = 0;
      if (scc->downlinkBWP_ToAddModList)
        num_dl_bwp = scc->downlinkBWP_ToAddModList->list.count;
      for (int i = 0;i < num_dl_bwp; i++) {
        dlbwp = scc->downlinkBWP_ToAddModList->list.array[i]->bwp_Dedicated;
        if (dlbwp && dlbwp->pdsch_Config &&
            dlbwp->pdsch_Config->present == NR_SetupRelease_PDSCH_Config_PR_setup) {
          NR_PDSCH_Config_t *dlcfg = dlbwp->pdsch_Config->choice.setup;
          if (!dlcfg->ext3)
            dlcfg->ext3 = calloc(1, sizeof(*dlcfg->ext3));
          asn1cCallocOne(dlcfg->ext3->harq_ProcessNumberSizeDCI_1_1_r17, 5);
        }
      }
      break;
    default: // Already IE should have been set to 16 harq processes
      break;
  }


  AssertFatal(scc->uplinkConfig,"uplinkConfig IE NOT present\n");

  if (num_ulharq == 32) {
    if (!scc->uplinkConfig->pusch_ServingCellConfig)
      scc->uplinkConfig->pusch_ServingCellConfig = calloc(1, sizeof(*scc->uplinkConfig->pusch_ServingCellConfig));
    scc->uplinkConfig->pusch_ServingCellConfig->present = NR_SetupRelease_PUSCH_ServingCellConfig_PR_setup;
    if (!scc->uplinkConfig->pusch_ServingCellConfig->choice.setup)
      scc->uplinkConfig->pusch_ServingCellConfig->choice.setup = calloc(1, sizeof(NR_PUSCH_ServingCellConfig_t));
    NR_PUSCH_ServingCellConfig_t *pusch_scc = scc->uplinkConfig->pusch_ServingCellConfig->choice.setup;
    if (!pusch_scc->ext3)
      pusch_scc->ext3 = calloc(1, sizeof(*pusch_scc->ext3));
    asn1cCallocOne(pusch_scc->ext3->nrofHARQ_ProcessesForPUSCH_r17,
                   NR_PUSCH_ServingCellConfig__ext3__nrofHARQ_ProcessesForPUSCH_r17_n32);
    NR_BWP_UplinkDedicated_t *ulbwp = scc->uplinkConfig->initialUplinkBWP;
    if (ulbwp && ulbwp->pusch_Config &&
        ulbwp->pusch_Config->present == NR_SetupRelease_PUSCH_Config_PR_setup) {
      NR_PUSCH_Config_t *ulcfg = ulbwp->pusch_Config->choice.setup;
      if (!ulcfg->ext2)
        ulcfg->ext2 = calloc(1, sizeof(*ulcfg->ext2));
      asn1cCallocOne(ulcfg->ext2->harq_ProcessNumberSizeDCI_0_1_r17, 5);
    }
    int num_ul_bwp = 0;
    if (scc->uplinkConfig->uplinkBWP_ToAddModList)
      num_ul_bwp = scc->uplinkConfig->uplinkBWP_ToAddModList->list.count;
    for (int i = 0;i < num_ul_bwp; i++) {
      ulbwp = scc->uplinkConfig->uplinkBWP_ToAddModList->list.array[i]->bwp_Dedicated;
      if (ulbwp && ulbwp->pusch_Config &&
          ulbwp->pusch_Config->present == NR_SetupRelease_PUSCH_Config_PR_setup) {
        NR_PUSCH_Config_t *ulcfg = ulbwp->pusch_Config->choice.setup;
        if (!ulcfg->ext2)
          ulcfg->ext2 = calloc(1, sizeof(*ulcfg->ext2));
        asn1cCallocOne(ulcfg->ext2->harq_ProcessNumberSizeDCI_0_1_r17, 5);
      }
    }
  }
}

static NR_SpCellConfig_t *get_initial_SpCellConfig(int uid,
                                                   const NR_ServingCellConfigCommon_t *scc,
                                                   const NR_ServingCellConfig_t *servingcellconfigdedicated,
                                                   const nr_mac_config_t *configuration)
{
  const int pdsch_AntennaPorts =
      configuration->pdsch_AntennaPorts.N1 * configuration->pdsch_AntennaPorts.N2 * configuration->pdsch_AntennaPorts.XP;
  int curr_bwp = NRRIV2BW(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  NR_SpCellConfig_t *SpCellConfig = calloc(1, sizeof(*SpCellConfig));
  SpCellConfig->servCellIndex = NULL;
  SpCellConfig->reconfigurationWithSync = NULL;
  SpCellConfig->rlmInSyncOutOfSyncThreshold = NULL;
  SpCellConfig->rlf_TimersAndConstants = NULL;

  SpCellConfig->spCellConfigDedicated = calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated));
  SpCellConfig->spCellConfigDedicated->uplinkConfig = calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated->uplinkConfig));
  NR_UplinkConfig_t *uplinkConfig = SpCellConfig->spCellConfigDedicated->uplinkConfig;

  NR_BWP_UplinkDedicated_t *initialUplinkBWP = calloc(1, sizeof(*initialUplinkBWP));
  uplinkConfig->initialUplinkBWP = initialUplinkBWP;
  initialUplinkBWP->pucch_Config = calloc(1, sizeof(*initialUplinkBWP->pucch_Config));
  initialUplinkBWP->pucch_Config->present = NR_SetupRelease_PUCCH_Config_PR_setup;
  NR_PUCCH_Config_t *pucch_Config = calloc(1, sizeof(*pucch_Config));
  initialUplinkBWP->pucch_Config->choice.setup = pucch_Config;
  pucch_Config->resourceSetToAddModList = calloc(1, sizeof(*pucch_Config->resourceSetToAddModList));
  pucch_Config->resourceSetToReleaseList = NULL;
  pucch_Config->resourceToAddModList = calloc(1, sizeof(*pucch_Config->resourceToAddModList));
  pucch_Config->resourceToReleaseList = NULL;
  int num_pucch2 = get_nb_pucch2_per_slot(scc, curr_bwp);
  config_pucch_resset0(pucch_Config, uid, curr_bwp, num_pucch2, NULL);
  config_pucch_resset1(pucch_Config, uid, num_pucch2, NULL);
  set_pucch_power_config(pucch_Config, configuration->do_CSIRS);

  initialUplinkBWP->pusch_Config = config_pusch(NULL, configuration->use_deltaMCS, scc, NULL);

  long maxMIMO_Layers = uplinkConfig && uplinkConfig->pusch_ServingCellConfig
                                && uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1
                                && uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
                            ? *uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
                            : 1;

  // We are using do_srs = 0 here because the periodic SRS will only be enabled in update_cellGroupConfig() if do_srs == 1
  initialUplinkBWP->srs_Config = get_config_srs(scc, NULL, curr_bwp, uid, 0, maxMIMO_Layers, configuration->minRXTXTIME, 0);

  scheduling_request_config(scc, pucch_Config, scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.subcarrierSpacing);

  set_dl_DataToUL_ACK(pucch_Config,
                      configuration->minRXTXTIME,
                      scc->uplinkConfigCommon->initialUplinkBWP->genericParameters.subcarrierSpacing);

  SpCellConfig->spCellConfigDedicated->initialDownlinkBWP =
      calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated->initialDownlinkBWP));
  NR_BWP_DownlinkDedicated_t *bwp_Dedicated = SpCellConfig->spCellConfigDedicated->initialDownlinkBWP;
  bwp_Dedicated->pdcch_Config = calloc(1, sizeof(*bwp_Dedicated->pdcch_Config));
  bwp_Dedicated->pdcch_Config->present = NR_SetupRelease_PDCCH_Config_PR_setup;
  bwp_Dedicated->pdcch_Config->choice.setup = calloc(1, sizeof(*bwp_Dedicated->pdcch_Config->choice.setup));

  bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList =
      calloc(1, sizeof(*bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList));

  bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList =
      calloc(1, sizeof(*bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList));

  uint64_t bitmap = get_ssb_bitmap(scc);
  NR_ControlResourceSet_t *coreset = get_coreset_config(0, curr_bwp, bitmap);

  asn1cSeqAdd(&bwp_Dedicated->pdcch_Config->choice.setup->controlResourceSetToAddModList->list, coreset);

  int css_num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS];
  css_num_agg_level_candidates[PDCCH_AGG_LEVEL1] = NR_SearchSpace__nrofCandidates__aggregationLevel1_n0;
  css_num_agg_level_candidates[PDCCH_AGG_LEVEL2] = NR_SearchSpace__nrofCandidates__aggregationLevel2_n0;
  css_num_agg_level_candidates[PDCCH_AGG_LEVEL4] = NR_SearchSpace__nrofCandidates__aggregationLevel4_n1;
  css_num_agg_level_candidates[PDCCH_AGG_LEVEL8] = NR_SearchSpace__nrofCandidates__aggregationLevel8_n0;
  css_num_agg_level_candidates[PDCCH_AGG_LEVEL16] = NR_SearchSpace__nrofCandidates__aggregationLevel16_n0;
  int searchspaceid = 4;
  NR_SearchSpace_t *ss = rrc_searchspace_config(true, searchspaceid, 0, css_num_agg_level_candidates);
  searchspaceid = 5;
  int rrc_num_agg_level_candidates[NUM_PDCCH_AGG_LEVELS];
  int num_cces = get_coreset_num_cces(coreset->frequencyDomainResources.buf, coreset->duration);
  verify_agg_levels(num_cces,
                    configuration->num_agg_level_candidates,
                    coreset->controlResourceSetId,
                    searchspaceid,
                    rrc_num_agg_level_candidates);
  NR_SearchSpace_t *ss2 = rrc_searchspace_config(false, searchspaceid, coreset->controlResourceSetId, rrc_num_agg_level_candidates);
  asn1cSeqAdd(&bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList->list, ss);
  asn1cSeqAdd(&bwp_Dedicated->pdcch_Config->choice.setup->searchSpacesToAddModList->list, ss2);

  bwp_Dedicated->pdsch_Config = config_pdsch(bitmap, 0, pdsch_AntennaPorts);

  SpCellConfig->spCellConfigDedicated->tag_Id = 0;
  SpCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig =
      calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig));
  NR_PDSCH_ServingCellConfig_t *pdsch_servingcellconfig = calloc(1, sizeof(*pdsch_servingcellconfig));
  SpCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig->present = NR_SetupRelease_PDSCH_ServingCellConfig_PR_setup;
  SpCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig->choice.setup = pdsch_servingcellconfig;

  pdsch_servingcellconfig->codeBlockGroupTransmission = NULL;
  pdsch_servingcellconfig->xOverhead = NULL;
  asn1cCallocOne(pdsch_servingcellconfig->nrofHARQ_ProcessesForPDSCH, NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n16);
  pdsch_servingcellconfig->pucch_Cell = NULL;
  set_dl_maxmimolayers(pdsch_servingcellconfig, scc, NULL, configuration->maxMIMO_layers);
  if (configuration->disable_harq) {
    if (!pdsch_servingcellconfig->ext3)
      pdsch_servingcellconfig->ext3 = calloc(1, sizeof(*pdsch_servingcellconfig->ext3));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17 = calloc(1, sizeof(*pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->present = NR_SetupRelease_DownlinkHARQ_FeedbackDisabled_r17_PR_setup;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf = calloc(4, sizeof(uint8_t));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.size = 4;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.bits_unused = 0;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[0] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[1] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[2] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[3] = 0xFF;
  }

  // Downlink BWPs
  int n_dl_bwp = 0;
  if (servingcellconfigdedicated && servingcellconfigdedicated->downlinkBWP_ToAddModList
      && servingcellconfigdedicated->downlinkBWP_ToAddModList->list.count > 0) {
    n_dl_bwp = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.count;
  }
  int default_dl_bwp = 0;
  int first_active_dl_bwp = 0;
  if (n_dl_bwp > 0) {
    SpCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList =
        calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList));
    for (int bwp_loop = 0; bwp_loop < n_dl_bwp; bwp_loop++) {
      NR_BWP_Downlink_t *bwp = calloc(1, sizeof(*bwp));
      config_downlinkBWP(bwp, scc, servingcellconfigdedicated, NULL, 0, false, bwp_loop, true, configuration->num_agg_level_candidates);
      asn1cSeqAdd(&SpCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList->list, bwp);
    }
    const NR_BWP_Id_t *firstActiveDownlinkBWP_Id = servingcellconfigdedicated->firstActiveDownlinkBWP_Id;
    first_active_dl_bwp = firstActiveDownlinkBWP_Id ? *firstActiveDownlinkBWP_Id : 1;
    const NR_BWP_Id_t	*defaultDownlinkBWP_Id = servingcellconfigdedicated->defaultDownlinkBWP_Id;
    default_dl_bwp = defaultDownlinkBWP_Id ? *defaultDownlinkBWP_Id : 1;
  }
  asn1cCallocOne(SpCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id, first_active_dl_bwp);
  asn1cCallocOne(SpCellConfig->spCellConfigDedicated->defaultDownlinkBWP_Id, default_dl_bwp);

  // Uplink BWPs
  int n_ul_bwp = 0;
  if (servingcellconfigdedicated && servingcellconfigdedicated->uplinkConfig
      && servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList
      && servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.count > 0) {
    n_ul_bwp = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.count;
  }
  int first_active_ul_bwp = 0;
  if (n_ul_bwp > 0) {
    uplinkConfig->uplinkBWP_ToAddModList = calloc(1, sizeof(*uplinkConfig->uplinkBWP_ToAddModList));
    for (int bwp_loop = 0; bwp_loop < n_ul_bwp; bwp_loop++) {
      NR_BWP_Uplink_t *ubwp = calloc(1, sizeof(*ubwp));
      config_uplinkBWP(ubwp, bwp_loop, true, uid, configuration, servingcellconfigdedicated, scc, NULL);
      asn1cSeqAdd(&uplinkConfig->uplinkBWP_ToAddModList->list, ubwp);
    }
    const NR_BWP_Id_t *firstActiveUplinkBWP_Id = servingcellconfigdedicated->uplinkConfig->firstActiveUplinkBWP_Id;
    first_active_ul_bwp = firstActiveUplinkBWP_Id ? *firstActiveUplinkBWP_Id : 1;
  }
  asn1cCallocOne(uplinkConfig->firstActiveUplinkBWP_Id, first_active_ul_bwp);

  SpCellConfig->spCellConfigDedicated->csi_MeasConfig = calloc(1, sizeof(*SpCellConfig->spCellConfigDedicated->csi_MeasConfig));
  SpCellConfig->spCellConfigDedicated->csi_MeasConfig->present = NR_SetupRelease_CSI_MeasConfig_PR_setup;

  NR_CSI_MeasConfig_t *csi_MeasConfig = calloc(1, sizeof(*csi_MeasConfig));
  SpCellConfig->spCellConfigDedicated->csi_MeasConfig->choice.setup = csi_MeasConfig;

  csi_MeasConfig->csi_SSB_ResourceSetToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_SSB_ResourceSetToAddModList));
  csi_MeasConfig->csi_SSB_ResourceSetToReleaseList = NULL;
  csi_MeasConfig->csi_ResourceConfigToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_ResourceConfigToAddModList));
  csi_MeasConfig->csi_ResourceConfigToReleaseList = NULL;
  csi_MeasConfig->csi_ReportConfigToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_ReportConfigToAddModList));
  csi_MeasConfig->csi_ReportConfigToReleaseList = NULL;

  NR_CSI_SSB_ResourceSet_t *ssbresset0 = calloc(1, sizeof(*ssbresset0));
  ssbresset0->csi_SSB_ResourceSetId = 0;

  for (int i = 0; i < 64; i++) {
    if ((bitmap >> (63 - i)) & 0x01) {
      NR_SSB_Index_t *ssbres = NULL;
      asn1cCallocOne(ssbres, i);
      asn1cSeqAdd(&ssbresset0->csi_SSB_ResourceList.list, ssbres);
    }
  }
  asn1cSeqAdd(&csi_MeasConfig->csi_SSB_ResourceSetToAddModList->list, ssbresset0);

  int bwp_loop_end = n_dl_bwp > 0 ? n_dl_bwp : 1;
  for (int bwp_loop = 0; bwp_loop < bwp_loop_end; bwp_loop++) {
    int curr_bwp, bwp_id;
    struct NR_SetupRelease_PDSCH_Config *pdsch_Config;
    if (n_dl_bwp == 0) {
      pdsch_Config = SpCellConfig->spCellConfigDedicated->initialDownlinkBWP->pdsch_Config;
      curr_bwp = NRRIV2BW(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
      bwp_id = 0;
    } else {
      NR_BWP_Downlink_t *bwp = SpCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList->list.array[bwp_loop];
      pdsch_Config = bwp->bwp_Dedicated->pdsch_Config;
      curr_bwp = NRRIV2BW(bwp->bwp_Common->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
      bwp_id = bwp->bwp_Id;
    }

    config_csirs(scc, csi_MeasConfig, pdsch_AntennaPorts, curr_bwp, configuration->do_CSIRS, bwp_loop);
    config_csiim(configuration->do_CSIRS, pdsch_AntennaPorts, curr_bwp, csi_MeasConfig, bwp_loop);

    NR_CSI_ResourceConfig_t *csires1 = calloc(1, sizeof(*csires1));
    csires1->csi_ResourceConfigId = bwp_id + 20;
    csires1->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB;
    csires1->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB =
        calloc(1, sizeof(*csires1->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB));
    csires1->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList =
        calloc(1, sizeof(*csires1->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList));
    NR_CSI_SSB_ResourceSetId_t *ssbres00 = calloc(1, sizeof(*ssbres00));
    *ssbres00 = 0;
    asn1cSeqAdd(&csires1->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList->list, ssbres00);
    csires1->bwp_Id = bwp_id;
    csires1->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
    asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, csires1);

    int pucch_Resource = 2;

    if (configuration->do_CSIRS) {
      NR_CSI_ResourceConfig_t *csires0 = calloc(1, sizeof(*csires0));
      csires0->csi_ResourceConfigId = bwp_id;
      csires0->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB;
      csires0->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB =
          calloc(1, sizeof(*csires0->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB));
      csires0->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList =
          calloc(1, sizeof(*csires0->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList));
      NR_NZP_CSI_RS_ResourceSetId_t *nzp0 = calloc(1, sizeof(*nzp0));
      *nzp0 = bwp_loop;
      asn1cSeqAdd(&csires0->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList->list, nzp0);
      csires0->bwp_Id = bwp_id;
      csires0->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
      asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, csires0);
    }

    if (configuration->do_CSIRS && pdsch_AntennaPorts > 1) {
      NR_CSI_ResourceConfig_t *csires2 = calloc(1, sizeof(*csires2));
      csires2->csi_ResourceConfigId = bwp_id + 10;
      csires2->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_csi_IM_ResourceSetList;
      csires2->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList =
          calloc(1, sizeof(*csires2->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList));
      NR_CSI_IM_ResourceSetId_t *csiim00 = calloc(1, sizeof(*csiim00));
      *csiim00 = bwp_loop;
      asn1cSeqAdd(&csires2->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList->list, csiim00);
      csires2->bwp_Id = bwp_id;
      csires2->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
      asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, csires2);

      NR_PUCCH_CSI_Resource_t *pucchcsi = calloc(1, sizeof(*pucchcsi));
      pucchcsi->uplinkBandwidthPartId = bwp_id;
      pucchcsi->pucch_Resource = pucch_Resource;
      config_csi_meas_report(csi_MeasConfig,
                             scc,
                             pucchcsi,
                             pdsch_Config,
                             &configuration->pdsch_AntennaPorts,
                             *pdsch_servingcellconfig->ext1->maxMIMO_Layers,
                             bwp_id,
                             uid,
                             curr_bwp);
    }
    NR_PUCCH_CSI_Resource_t *pucchrsrp = calloc(1, sizeof(*pucchrsrp));
    pucchrsrp->uplinkBandwidthPartId = bwp_id;
    pucchrsrp->pucch_Resource = pucch_Resource;
    config_rsrp_meas_report(csi_MeasConfig,
                            scc,
                            pucchrsrp,
                            configuration->do_CSIRS,
                            bwp_id + 10,
                            uid,
                            curr_bwp,
                            pdsch_AntennaPorts,
                            configuration->do_SINR);
  }

  fill_harq_IEs(SpCellConfig->spCellConfigDedicated, configuration->num_dlharq, configuration->num_ulharq);

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_SpCellConfig, SpCellConfig);
  }
  return SpCellConfig;
}

NR_RLC_BearerConfig_t *get_SRB_RLC_BearerConfig(long channelId,
                                                long priority,
                                                long bucketSizeDuration,
                                                const nr_rlc_configuration_t *default_rlc_config)
{
  NR_RLC_BearerConfig_t *rlc_BearerConfig = NULL;
  rlc_BearerConfig                                                 = calloc(1, sizeof(NR_RLC_BearerConfig_t));
  rlc_BearerConfig->logicalChannelIdentity                         = channelId;
  rlc_BearerConfig->servedRadioBearer                              = calloc(1, sizeof(*rlc_BearerConfig->servedRadioBearer));
  rlc_BearerConfig->servedRadioBearer->present                     = NR_RLC_BearerConfig__servedRadioBearer_PR_srb_Identity;
  rlc_BearerConfig->servedRadioBearer->choice.srb_Identity         = channelId;
  rlc_BearerConfig->reestablishRLC                                 = NULL;

  NR_RLC_Config_t *rlc_Config                                      = calloc(1, sizeof(NR_RLC_Config_t));
  rlc_Config->present                                              = NR_RLC_Config_PR_am;
  rlc_Config->choice.am                                            = calloc(1, sizeof(*rlc_Config->choice.am));
  rlc_Config->choice.am->dl_AM_RLC.sn_FieldLength                  = calloc(1, sizeof(NR_SN_FieldLengthAM_t));
  *(rlc_Config->choice.am->dl_AM_RLC.sn_FieldLength)               = encode_sn_field_length_am(default_rlc_config->srb.sn_field_length);
  rlc_Config->choice.am->dl_AM_RLC.t_Reassembly                    = encode_t_reassembly(default_rlc_config->srb.t_reassembly);
  rlc_Config->choice.am->dl_AM_RLC.t_StatusProhibit                = encode_t_status_prohibit(default_rlc_config->srb.t_status_prohibit);
  rlc_Config->choice.am->ul_AM_RLC.sn_FieldLength                  = calloc(1, sizeof(NR_SN_FieldLengthAM_t));
  *(rlc_Config->choice.am->ul_AM_RLC.sn_FieldLength)               = encode_sn_field_length_am(default_rlc_config->srb.sn_field_length);
  rlc_Config->choice.am->ul_AM_RLC.t_PollRetransmit                = encode_t_poll_retransmit(default_rlc_config->srb.t_poll_retransmit);
  rlc_Config->choice.am->ul_AM_RLC.pollPDU                         = encode_poll_pdu(default_rlc_config->srb.poll_pdu);
  rlc_Config->choice.am->ul_AM_RLC.pollByte                        = encode_poll_byte(default_rlc_config->srb.poll_byte);
  rlc_Config->choice.am->ul_AM_RLC.maxRetxThreshold                = encode_max_retx_threshold(default_rlc_config->srb.max_retx_threshold);
  rlc_BearerConfig->rlc_Config                                     = rlc_Config;

  NR_LogicalChannelConfig_t *logicalChannelConfig                  = calloc(1, sizeof(NR_LogicalChannelConfig_t));
  logicalChannelConfig->ul_SpecificParameters                      = calloc(1, sizeof(*logicalChannelConfig->ul_SpecificParameters));
  logicalChannelConfig->ul_SpecificParameters->priority            = priority;
  logicalChannelConfig->ul_SpecificParameters->prioritisedBitRate  = NR_LogicalChannelConfig__ul_SpecificParameters__prioritisedBitRate_infinity;
  logicalChannelConfig->ul_SpecificParameters->bucketSizeDuration  = bucketSizeDuration;

  long *logicalChannelGroup                                        = CALLOC(1, sizeof(long));
  *logicalChannelGroup                                             = 0;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelGroup = logicalChannelGroup;
  logicalChannelConfig->ul_SpecificParameters->schedulingRequestID = CALLOC(1, sizeof(*logicalChannelConfig->ul_SpecificParameters->schedulingRequestID));
  *logicalChannelConfig->ul_SpecificParameters->schedulingRequestID = 0;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelSR_Mask = 0;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelSR_DelayTimerApplied = 0;
  rlc_BearerConfig->mac_LogicalChannelConfig                       = logicalChannelConfig;

  return rlc_BearerConfig;
}

static void nr_drb_config(struct NR_RLC_Config *rlc_Config,
                          NR_RLC_Config_PR rlc_config_pr,
                          const nr_rlc_configuration_t *default_rlc_config)
{
  switch (rlc_config_pr) {
    case NR_RLC_Config_PR_um_Bi_Directional:
      // RLC UM Bi-directional Bearer configuration
      LOG_I(RLC, "RLC UM Bi-directional Bearer configuration selected \n");
      rlc_Config->choice.um_Bi_Directional = calloc(1, sizeof(*rlc_Config->choice.um_Bi_Directional));
      rlc_Config->choice.um_Bi_Directional->ul_UM_RLC.sn_FieldLength =
          calloc(1, sizeof(*rlc_Config->choice.um_Bi_Directional->ul_UM_RLC.sn_FieldLength));
      *rlc_Config->choice.um_Bi_Directional->ul_UM_RLC.sn_FieldLength = encode_sn_field_length_um(default_rlc_config->drb_um.sn_field_length);
      rlc_Config->choice.um_Bi_Directional->dl_UM_RLC.sn_FieldLength =
          calloc(1, sizeof(*rlc_Config->choice.um_Bi_Directional->dl_UM_RLC.sn_FieldLength));
      *rlc_Config->choice.um_Bi_Directional->dl_UM_RLC.sn_FieldLength = encode_sn_field_length_um(default_rlc_config->drb_um.sn_field_length);
      rlc_Config->choice.um_Bi_Directional->dl_UM_RLC.t_Reassembly = encode_t_reassembly(default_rlc_config->drb_um.t_reassembly);
      break;
    case NR_RLC_Config_PR_am:
      // RLC AM Bearer configuration
      rlc_Config->choice.am = calloc(1, sizeof(*rlc_Config->choice.am));
      rlc_Config->choice.am->ul_AM_RLC.sn_FieldLength = calloc(1, sizeof(*rlc_Config->choice.am->ul_AM_RLC.sn_FieldLength));
      *rlc_Config->choice.am->ul_AM_RLC.sn_FieldLength = encode_sn_field_length_am(default_rlc_config->drb_am.sn_field_length);
      rlc_Config->choice.am->ul_AM_RLC.t_PollRetransmit = encode_t_poll_retransmit(default_rlc_config->drb_am.t_poll_retransmit);
      rlc_Config->choice.am->ul_AM_RLC.pollPDU = encode_poll_pdu(default_rlc_config->drb_am.poll_pdu);
      rlc_Config->choice.am->ul_AM_RLC.pollByte = encode_poll_byte(default_rlc_config->drb_am.poll_byte);
      rlc_Config->choice.am->ul_AM_RLC.maxRetxThreshold = encode_max_retx_threshold(default_rlc_config->drb_am.max_retx_threshold);
      rlc_Config->choice.am->dl_AM_RLC.sn_FieldLength = calloc(1, sizeof(*rlc_Config->choice.am->dl_AM_RLC.sn_FieldLength));
      *rlc_Config->choice.am->dl_AM_RLC.sn_FieldLength = encode_sn_field_length_am(default_rlc_config->drb_am.sn_field_length);
      rlc_Config->choice.am->dl_AM_RLC.t_Reassembly = encode_t_reassembly(default_rlc_config->drb_am.t_reassembly);
      rlc_Config->choice.am->dl_AM_RLC.t_StatusProhibit = encode_t_status_prohibit(default_rlc_config->drb_am.t_status_prohibit);
      break;
    default:
      AssertFatal(false, "RLC config type %d not handled\n", rlc_config_pr);
      break;
  }
  rlc_Config->present = rlc_config_pr;
}

NR_RLC_BearerConfig_t *get_DRB_RLC_BearerConfig(long lcChannelId,
                                                long drbId,
                                                NR_RLC_Config_PR rlc_conf,
                                                long priority,
                                                const nr_rlc_configuration_t *default_rlc_config)
{
  NR_RLC_BearerConfig_t *rlc_BearerConfig                  = calloc(1, sizeof(NR_RLC_BearerConfig_t));
  rlc_BearerConfig->logicalChannelIdentity                 = lcChannelId;
  rlc_BearerConfig->servedRadioBearer                      = calloc(1, sizeof(*rlc_BearerConfig->servedRadioBearer));
  rlc_BearerConfig->servedRadioBearer->present             = NR_RLC_BearerConfig__servedRadioBearer_PR_drb_Identity;
  rlc_BearerConfig->servedRadioBearer->choice.drb_Identity = drbId;
  rlc_BearerConfig->reestablishRLC                         = NULL;

  NR_RLC_Config_t *rlc_Config  = calloc(1, sizeof(NR_RLC_Config_t));
  nr_drb_config(rlc_Config, rlc_conf, default_rlc_config);
  rlc_BearerConfig->rlc_Config = rlc_Config;

  NR_LogicalChannelConfig_t *logicalChannelConfig                 = calloc(1, sizeof(NR_LogicalChannelConfig_t));
  logicalChannelConfig->ul_SpecificParameters                     = calloc(1, sizeof(*logicalChannelConfig->ul_SpecificParameters));
  logicalChannelConfig->ul_SpecificParameters->priority           = priority;
  logicalChannelConfig->ul_SpecificParameters->prioritisedBitRate = NR_LogicalChannelConfig__ul_SpecificParameters__prioritisedBitRate_kBps8;
  logicalChannelConfig->ul_SpecificParameters->bucketSizeDuration = NR_LogicalChannelConfig__ul_SpecificParameters__bucketSizeDuration_ms100;

  long *logicalChannelGroup                                          = CALLOC(1, sizeof(long));
  *logicalChannelGroup                                               = 1;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelGroup   = logicalChannelGroup;
  logicalChannelConfig->ul_SpecificParameters->schedulingRequestID   = CALLOC(1, sizeof(*logicalChannelConfig->ul_SpecificParameters->schedulingRequestID));
  *logicalChannelConfig->ul_SpecificParameters->schedulingRequestID  = 0;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelSR_Mask = 0;
  logicalChannelConfig->ul_SpecificParameters->logicalChannelSR_DelayTimerApplied = 0;
  rlc_BearerConfig->mac_LogicalChannelConfig                         = logicalChannelConfig;

  return rlc_BearerConfig;
}

NR_CellGroupConfig_t *get_initial_cellGroupConfig(int uid,
                                                  const NR_ServingCellConfigCommon_t *scc,
                                                  const NR_ServingCellConfig_t *servingcellconfigdedicated,
                                                  const nr_mac_config_t *configuration,
                                                  const nr_rlc_configuration_t *default_rlc_config)
{
  NR_CellGroupConfig_t *cellGroupConfig = calloc(1, sizeof(*cellGroupConfig));
  cellGroupConfig->cellGroupId = 0;

  /* Rlc Bearer Config */
  /* TS38.331 9.2.1	Default SRB configurations */
  cellGroupConfig->rlc_BearerToAddModList = calloc(1, sizeof(*cellGroupConfig->rlc_BearerToAddModList));
  NR_RLC_BearerConfig_t *rlc_BearerConfig =
      get_SRB_RLC_BearerConfig(1, 1, NR_LogicalChannelConfig__ul_SpecificParameters__bucketSizeDuration_ms1000, default_rlc_config);
  asn1cSeqAdd(&cellGroupConfig->rlc_BearerToAddModList->list, rlc_BearerConfig);

  cellGroupConfig->rlc_BearerToReleaseList = NULL;

  /* mac CellGroup Config */
  cellGroupConfig->mac_CellGroupConfig = configure_mac_cellgroup(&configuration->timer_config);

  cellGroupConfig->physicalCellGroupConfig = configure_phy_cellgroup();

  cellGroupConfig->spCellConfig = get_initial_SpCellConfig(uid, scc, servingcellconfigdedicated, configuration);

  cellGroupConfig->sCellToAddModList = NULL;
  cellGroupConfig->sCellToReleaseList = NULL;
  return cellGroupConfig;
}

void update_cellGroupConfig(NR_CellGroupConfig_t *cellGroupConfig,
                            const int uid,
                            NR_UE_NR_Capability_t *uecap,
                            const nr_mac_config_t *configuration,
                            const NR_ServingCellConfigCommon_t *scc)
{
  DevAssert(cellGroupConfig != NULL);
  DevAssert(cellGroupConfig->spCellConfig != NULL);
  DevAssert(configuration != NULL);
  DevAssert(scc != NULL);

  NR_SpCellConfig_t *SpCellConfig = cellGroupConfig->spCellConfig;

  NR_PDSCH_ServingCellConfig_t *pdsch_servingcellconfig = SpCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig->choice.setup;
  set_dl_maxmimolayers(pdsch_servingcellconfig, scc, uecap, configuration->maxMIMO_layers);

  NR_CSI_MeasConfig_t *csi_MeasConfig = SpCellConfig->spCellConfigDedicated->csi_MeasConfig->choice.setup;
  for (int report = 0; report < csi_MeasConfig->csi_ReportConfigToAddModList->list.count; report++) {
    NR_CSI_ReportConfig_t *csirep = csi_MeasConfig->csi_ReportConfigToAddModList->list.array[report];
    if(csirep->codebookConfig)
      config_csi_codebook(&configuration->pdsch_AntennaPorts, *pdsch_servingcellconfig->ext1->maxMIMO_Layers, csirep->codebookConfig);
  }

  int curr_bwp = NRRIV2BW(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
  NR_UplinkConfig_t *uplinkConfig =
      SpCellConfig && SpCellConfig->spCellConfigDedicated ? SpCellConfig->spCellConfigDedicated->uplinkConfig : NULL;

  uint8_t ul_max_layers = 1;
  if (uecap && uecap->featureSets && uecap->featureSets->featureSetsUplinkPerCC
      && uecap->featureSets->featureSetsUplinkPerCC->list.count > 0) {
    NR_FeatureSetUplinkPerCC_t *ul_feature_setup_per_cc = uecap->featureSets->featureSetsUplinkPerCC->list.array[0];
    if (ul_feature_setup_per_cc->mimo_CB_PUSCH->maxNumberMIMO_LayersCB_PUSCH) {
      switch (*ul_feature_setup_per_cc->mimo_CB_PUSCH->maxNumberMIMO_LayersCB_PUSCH) {
        case NR_MIMO_LayersUL_twoLayers:
          ul_max_layers = 2;
          break;
        case NR_MIMO_LayersUL_fourLayers:
          ul_max_layers = 4;
          break;
        default:
          ul_max_layers = 1;
      }
    }
    ul_max_layers = min(ul_max_layers, configuration->pusch_AntennaPorts);
    if (uplinkConfig->initialUplinkBWP->pusch_Config) {
      NR_PUSCH_Config_t *pusch_Config = uplinkConfig->initialUplinkBWP->pusch_Config->choice.setup;
      if (pusch_Config->maxRank == NULL) {
        pusch_Config->maxRank = calloc(1, sizeof(*pusch_Config->maxRank));
      }
      *pusch_Config->maxRank = ul_max_layers;
    }
    if (!uplinkConfig->pusch_ServingCellConfig)
      uplinkConfig->pusch_ServingCellConfig = calloc(1, sizeof(*uplinkConfig->pusch_ServingCellConfig));
    uplinkConfig->pusch_ServingCellConfig->present = NR_SetupRelease_PUSCH_ServingCellConfig_PR_setup;
    if (!uplinkConfig->pusch_ServingCellConfig->choice.setup)
      uplinkConfig->pusch_ServingCellConfig->choice.setup = calloc(1, sizeof(*uplinkConfig->pusch_ServingCellConfig->choice.setup));
    if (!uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1)
      uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1 =
          calloc(1, sizeof(*uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1));
    asn1cCallocOne(uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers, ul_max_layers);
  }

  long maxMIMO_Layers = uplinkConfig && uplinkConfig->pusch_ServingCellConfig
                                && uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1
                                && uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
                            ? *uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
                            : 1;

  // UL and SRS configuration
  if (configuration->do_SRS && uplinkConfig && uplinkConfig->initialUplinkBWP) {
    ASN_STRUCT_FREE(asn_DEF_NR_SetupRelease_SRS_Config, uplinkConfig->initialUplinkBWP->srs_Config);
    uplinkConfig->initialUplinkBWP->srs_Config = get_config_srs(scc,
                                                                uecap,
                                                                curr_bwp,
                                                                uid,
                                                                0,
                                                                maxMIMO_Layers,
                                                                configuration->minRXTXTIME,
                                                                configuration->do_SRS);
  }

  // Set DL MCS table
  NR_BWP_DownlinkDedicated_t *bwp_Dedicated = SpCellConfig->spCellConfigDedicated->initialDownlinkBWP;
  set_dl_mcs_table(scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters.subcarrierSpacing,
                   configuration->force_256qam_off ? NULL : uecap,
                   bwp_Dedicated,
                   scc);

  NR_BWP_UplinkDedicated_t *ul_bwp_Dedicated = SpCellConfig->spCellConfigDedicated->uplinkConfig->initialUplinkBWP;
  set_ul_mcs_table(configuration->force_UL256qam_off ? NULL : uecap, scc, ul_bwp_Dedicated->pusch_Config->choice.setup);

  struct NR_ServingCellConfig__downlinkBWP_ToAddModList *DL_BWP_list =
      SpCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList;
  struct NR_UplinkConfig__uplinkBWP_ToAddModList *UL_BWP_list = uplinkConfig ? uplinkConfig->uplinkBWP_ToAddModList : NULL;
  if (DL_BWP_list) {
    for (int i = 0; i < DL_BWP_list->list.count; i++) {
      NR_BWP_Downlink_t *bwp = DL_BWP_list->list.array[i];
      int scs = bwp->bwp_Common->genericParameters.subcarrierSpacing;
      set_dl_mcs_table(scs, configuration->force_256qam_off ? NULL : uecap, bwp->bwp_Dedicated, scc);
    }
  }
  if (UL_BWP_list) {
    for (int i = 0; i < UL_BWP_list->list.count; i++) {
      NR_BWP_Uplink_t *ul_bwp = UL_BWP_list->list.array[i];
      int bwp_size = NRRIV2BW(ul_bwp->bwp_Common->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);
      if (ul_bwp->bwp_Dedicated->pusch_Config) {
        NR_PUSCH_Config_t *pusch_Config = ul_bwp->bwp_Dedicated->pusch_Config->choice.setup;
        set_ul_mcs_table(configuration->force_UL256qam_off ? NULL : uecap, scc, pusch_Config);
        if (pusch_Config->maxRank == NULL) {
          pusch_Config->maxRank = calloc(1, sizeof(*pusch_Config->maxRank));
        }
        *pusch_Config->maxRank = ul_max_layers;
      }

      ASN_STRUCT_FREE(asn_DEF_NR_SetupRelease_SRS_Config, ul_bwp->bwp_Dedicated->srs_Config);
      ul_bwp->bwp_Dedicated->srs_Config = get_config_srs(scc,
                                                         uecap,
                                                         bwp_size,
                                                         uid,
                                                         i + 1,
                                                         maxMIMO_Layers,
                                                         configuration->minRXTXTIME,
                                                         configuration->do_SRS);
    }
  }
  update_cqitables(bwp_Dedicated->pdsch_Config, csi_MeasConfig);
}

void free_cellGroupConfig(NR_CellGroupConfig_t *cellGroupConfig)
{
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cellGroupConfig);
}

int encode_cellGroupConfig(NR_CellGroupConfig_t *cellGroupConfig, uint8_t *buffer, int max_buffer_size)
{
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_CellGroupConfig, NULL, cellGroupConfig, buffer, max_buffer_size);
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded <= max_buffer_size * 8,
              "ASN1 message encoding failed (%s, %lu)!\n",
              enc_rval.failed_type->name,
              enc_rval.encoded);
  return (enc_rval.encoded + 7) / 8;
}

NR_CellGroupConfig_t *decode_cellGroupConfig(const uint8_t *buffer, int buffer_size)
{
  NR_CellGroupConfig_t *cellGroupConfig = NULL;
  asn_dec_rval_t rval = uper_decode(NULL, &asn_DEF_NR_CellGroupConfig, (void **)&cellGroupConfig, buffer, buffer_size, 0, 0);
  AssertFatal(rval.code == RC_OK, "could not decode cellGroupConfig\n");
  return cellGroupConfig;
}

static NR_ServingCellConfigCommon_t *clone_ServingCellConfigCommon(const NR_ServingCellConfigCommon_t *scc)
{
  if (scc == NULL)
    return NULL;

  uint8_t buf[16384];
  asn_enc_rval_t enc_rval = uper_encode_to_buffer(&asn_DEF_NR_ServingCellConfigCommon, NULL, scc, buf, sizeof(buf));
  AssertFatal(enc_rval.encoded > 0 && enc_rval.encoded < sizeof(buf), "could not clone NR_ServingCellConfigCommon: problem while encoding\n");
  NR_ServingCellConfigCommon_t *clone = NULL;
  asn_dec_rval_t dec_rval = uper_decode(NULL, &asn_DEF_NR_ServingCellConfigCommon, (void **)&clone, buf, enc_rval.encoded, 0, 0);
  AssertFatal(dec_rval.code == RC_OK && dec_rval.consumed == enc_rval.encoded, "could not clone NR_ServingCellConfigCommon: problem while decoding\n");
  return clone;
}

NR_CellGroupConfig_t *get_default_secondaryCellGroup(const NR_ServingCellConfigCommon_t *servingcellconfigcommon,
                                                     const NR_ServingCellConfig_t *servingcellconfigdedicated,
                                                     const NR_UE_NR_Capability_t *uecap,
                                                     int scg_id,
                                                     int servCellIndex,
                                                     const nr_mac_config_t *configuration,
                                                     int uid)
{
  const nr_pdsch_AntennaPorts_t *pdschap = &configuration->pdsch_AntennaPorts;
  const int dl_antenna_ports = pdschap->N1 * pdschap->N2 * pdschap->XP;
  const int do_csirs = configuration->do_CSIRS;

  AssertFatal(servingcellconfigcommon, "servingcellconfigcommon is null\n");
  AssertFatal(servingcellconfigdedicated, "servingcellconfigdedicated is null\n");

  if (uecap == NULL)
    LOG_E(RRC, "No UE Capabilities available when programming default CellGroup in NSA\n");

  uint64_t bitmap = get_ssb_bitmap(servingcellconfigcommon);

  NR_CellGroupConfig_t *secondaryCellGroup = calloc(1, sizeof(*secondaryCellGroup));
  secondaryCellGroup->cellGroupId = scg_id;

  /* rlc_BearerToAddModList is handled outside */

  secondaryCellGroup->mac_CellGroupConfig = configure_mac_cellgroup(&configuration->timer_config);
  secondaryCellGroup->physicalCellGroupConfig = configure_phy_cellgroup();
  secondaryCellGroup->spCellConfig = calloc(1, sizeof(*secondaryCellGroup->spCellConfig));
  secondaryCellGroup->spCellConfig->servCellIndex = calloc(1, sizeof(*secondaryCellGroup->spCellConfig->servCellIndex));
  *secondaryCellGroup->spCellConfig->servCellIndex = servCellIndex;

  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->rlf_TimersAndConstants));
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->present = NR_SetupRelease_RLF_TimersAndConstants_PR_setup;
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup));
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->t310 = NR_RLF_TimersAndConstants__t310_ms4000;
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->n310 = NR_RLF_TimersAndConstants__n310_n20;
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->n311 = NR_RLF_TimersAndConstants__n311_n1;
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->ext1 =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->ext1));
  secondaryCellGroup->spCellConfig->rlf_TimersAndConstants->choice.setup->ext1->t311 =
      NR_RLF_TimersAndConstants__ext1__t311_ms30000;
  secondaryCellGroup->spCellConfig->rlmInSyncOutOfSyncThreshold = NULL;

  secondaryCellGroup->spCellConfig->spCellConfigDedicated =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->tdd_UL_DL_ConfigurationDedicated = NULL;

  /// initialDownlinkBWP

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP->pdcch_Config = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP->pdsch_Config =
      config_pdsch(bitmap, 0, dl_antenna_ports);
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP->sps_Config =
      NULL; // calloc(1,sizeof(struct NR_SetupRelease_SPS_Config));

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->initialDownlinkBWP->radioLinkMonitoringConfig = NULL;

  /// initialUplinkBWP
  if (!secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig) {
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig));
  }

  NR_BWP_UplinkDedicated_t *initialUplinkBWP = calloc(1, sizeof(*initialUplinkBWP));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->initialUplinkBWP = initialUplinkBWP;
  initialUplinkBWP->pucch_Config = NULL;

  NR_PUSCH_Config_t *pusch_Config = NULL;
  if (servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList) {
    pusch_Config = clone_pusch_config(
        servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.array[0]->bwp_Dedicated->pusch_Config->choice.setup);
  }
  initialUplinkBWP->pusch_Config = config_pusch(pusch_Config, configuration->use_deltaMCS,servingcellconfigcommon, uecap);

  long maxMIMO_Layers =
      servingcellconfigdedicated->uplinkConfig && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig
              && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1
              && servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
          ? *servingcellconfigdedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup->ext1->maxMIMO_Layers
          : 1;

  int curr_bwp = NRRIV2BW(servingcellconfigcommon->downlinkConfigCommon->initialDownlinkBWP->genericParameters.locationAndBandwidth,
                          MAX_BWP_SIZE);
  initialUplinkBWP->srs_Config = get_config_srs(servingcellconfigcommon,
                                                NULL,
                                                curr_bwp,
                                                uid,
                                                0,
                                                maxMIMO_Layers,
                                                configuration->minRXTXTIME,
                                                configuration->do_SRS);

  // Downlink BWPs
  int n_dl_bwp = 1;
  if (servingcellconfigdedicated->downlinkBWP_ToAddModList) {
    n_dl_bwp = servingcellconfigdedicated->downlinkBWP_ToAddModList->list.count;
  }
  if (n_dl_bwp > 0) {
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList));
    for (int bwp_loop = 0; bwp_loop < n_dl_bwp; bwp_loop++) {
      NR_BWP_Downlink_t *bwp = calloc(1, sizeof(*bwp));
      config_downlinkBWP(bwp,
                         servingcellconfigcommon,
                         servingcellconfigdedicated,
                         uecap,
                         dl_antenna_ports,
                         configuration->force_256qam_off,
                         bwp_loop,
                         false,
                         configuration->num_agg_level_candidates);
      asn1cSeqAdd(&secondaryCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList->list, bwp);
    }
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id));
    *secondaryCellGroup->spCellConfig->spCellConfigDedicated->firstActiveDownlinkBWP_Id =
        servingcellconfigdedicated->firstActiveDownlinkBWP_Id ? *servingcellconfigdedicated->firstActiveDownlinkBWP_Id : 1;
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->defaultDownlinkBWP_Id =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->defaultDownlinkBWP_Id));
    *secondaryCellGroup->spCellConfig->spCellConfigDedicated->defaultDownlinkBWP_Id =
        servingcellconfigdedicated->defaultDownlinkBWP_Id ? *servingcellconfigdedicated->defaultDownlinkBWP_Id : 1;
  }

  // Uplink BWPs
  int n_ul_bwp = 1;
  if (servingcellconfigdedicated->uplinkConfig && servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList) {
    n_ul_bwp = servingcellconfigdedicated->uplinkConfig->uplinkBWP_ToAddModList->list.count;
  }
  if (n_ul_bwp > 0) {
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->uplinkBWP_ToAddModList =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->uplinkBWP_ToAddModList));
    for (int bwp_loop = 0; bwp_loop < n_ul_bwp; bwp_loop++) {
      NR_BWP_Uplink_t *ubwp = calloc(1, sizeof(*ubwp));
      config_uplinkBWP(ubwp, bwp_loop, false, uid, configuration, servingcellconfigdedicated, servingcellconfigcommon, uecap);
      asn1cSeqAdd(&secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->uplinkBWP_ToAddModList->list, ubwp);
    }
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id));
    *secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->firstActiveUplinkBWP_Id =
        servingcellconfigdedicated->uplinkConfig->firstActiveUplinkBWP_Id
            ? *servingcellconfigdedicated->uplinkConfig->firstActiveUplinkBWP_Id
            : 1;
  }

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->bwp_InactivityTimer = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToReleaseList = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->uplinkBWP_ToReleaseList = NULL;

  if (!secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig)
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig =
        calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig->present =
      NR_SetupRelease_PUSCH_ServingCellConfig_PR_setup;
  if (!secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup)
    secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup = calloc(
        1,
        sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup));
  NR_PUSCH_ServingCellConfig_t *pusch_scc =
      secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->pusch_ServingCellConfig->choice.setup;
  pusch_scc->codeBlockGroupTransmission = NULL;
  pusch_scc->rateMatching = NULL;
  pusch_scc->xOverhead = NULL;
  if (!pusch_scc->ext1)
    pusch_scc->ext1 = calloc(1, sizeof(*pusch_scc->ext1));
  asn1cCallocOne(pusch_scc->ext1->maxMIMO_Layers, 1);
  pusch_scc->ext1->processingType2Enabled = NULL;

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->uplinkConfig->carrierSwitching = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->supplementaryUplink = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->pdcch_ServingCellConfig = NULL;

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig));
  NR_PDSCH_ServingCellConfig_t *pdsch_servingcellconfig = calloc(1, sizeof(*pdsch_servingcellconfig));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig->present =
      NR_SetupRelease_PDSCH_ServingCellConfig_PR_setup;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->pdsch_ServingCellConfig->choice.setup = pdsch_servingcellconfig;
  pdsch_servingcellconfig->codeBlockGroupTransmission = NULL;
  pdsch_servingcellconfig->xOverhead = NULL;
  asn1cCallocOne(pdsch_servingcellconfig->nrofHARQ_ProcessesForPDSCH, NR_PDSCH_ServingCellConfig__nrofHARQ_ProcessesForPDSCH_n16);
  pdsch_servingcellconfig->pucch_Cell = NULL;
  set_dl_maxmimolayers(pdsch_servingcellconfig, servingcellconfigcommon, uecap, configuration->maxMIMO_layers);
  pdsch_servingcellconfig->ext1->processingType2Enabled = NULL;
  if (configuration->disable_harq) {
    if (!pdsch_servingcellconfig->ext3)
      pdsch_servingcellconfig->ext3 = calloc(1, sizeof(*pdsch_servingcellconfig->ext3));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17 = calloc(1, sizeof(*pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->present = NR_SetupRelease_DownlinkHARQ_FeedbackDisabled_r17_PR_setup;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf = calloc(4, sizeof(uint8_t));
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.size = 4;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.bits_unused = 0;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[0] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[1] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[2] = 0xFF;
    pdsch_servingcellconfig->ext3->downlinkHARQ_FeedbackDisabled_r17->choice.setup.buf[3] = 0xFF;
  }

  secondaryCellGroup->spCellConfig->spCellConfigDedicated->csi_MeasConfig = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->csi_MeasConfig =
      calloc(1, sizeof(*secondaryCellGroup->spCellConfig->spCellConfigDedicated->csi_MeasConfig));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->csi_MeasConfig->present = NR_SetupRelease_CSI_MeasConfig_PR_setup;

  NR_CSI_MeasConfig_t *csi_MeasConfig = calloc(1, sizeof(*csi_MeasConfig));
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->csi_MeasConfig->choice.setup = csi_MeasConfig;

  csi_MeasConfig->csi_ResourceConfigToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_ResourceConfigToAddModList));
  csi_MeasConfig->csi_ResourceConfigToReleaseList = NULL;

  csi_MeasConfig->csi_SSB_ResourceSetToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_SSB_ResourceSetToAddModList));
  csi_MeasConfig->csi_SSB_ResourceSetToReleaseList = NULL;

  csi_MeasConfig->csi_ReportConfigToAddModList = calloc(1, sizeof(*csi_MeasConfig->csi_ReportConfigToAddModList));
  csi_MeasConfig->csi_ReportConfigToReleaseList = NULL;

  NR_CSI_SSB_ResourceSet_t *ssbresset0 = calloc(1, sizeof(*ssbresset0));
  ssbresset0->csi_SSB_ResourceSetId = 0;

  for (int i = 0; i < 64; i++) {
    if ((bitmap >> (63 - i)) & 0x01) {
      NR_SSB_Index_t *ssbres = NULL;
      asn1cCallocOne(ssbres, i);
      asn1cSeqAdd(&ssbresset0->csi_SSB_ResourceList.list, ssbres);
    }
  }
  asn1cSeqAdd(&csi_MeasConfig->csi_SSB_ResourceSetToAddModList->list, ssbresset0);

  for (int bwp_loop = 0; bwp_loop < n_dl_bwp; bwp_loop++) {
    NR_BWP_Downlink_t *bwp =
        secondaryCellGroup->spCellConfig->spCellConfigDedicated->downlinkBWP_ToAddModList->list.array[bwp_loop];
    int curr_bwp = NRRIV2BW(bwp->bwp_Common->genericParameters.locationAndBandwidth, MAX_BWP_SIZE);

    config_csirs(servingcellconfigcommon, csi_MeasConfig, dl_antenna_ports, curr_bwp, do_csirs, bwp_loop);
    config_csiim(do_csirs, dl_antenna_ports, curr_bwp, csi_MeasConfig, bwp_loop);

    if (do_csirs) {
      NR_CSI_ResourceConfig_t *csires = calloc(1, sizeof(*csires));
      csires->csi_ResourceConfigId = bwp->bwp_Id;
      csires->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB;
      csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB =
          calloc(1, sizeof(*csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB));
      csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList =
          calloc(1, sizeof(*csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList));
      NR_NZP_CSI_RS_ResourceSetId_t *nzp0 = calloc(1, sizeof(*nzp0));
      *nzp0 = bwp_loop;
      asn1cSeqAdd(&csires->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->nzp_CSI_RS_ResourceSetList->list, nzp0);
      csires->bwp_Id = bwp->bwp_Id;
      csires->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
      asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, csires);
    }
    if (do_csirs && dl_antenna_ports > 1) {
      NR_CSI_ResourceConfig_t *csiresim = calloc(1, sizeof(*csiresim));
      csiresim->csi_ResourceConfigId = bwp->bwp_Id + 10;
      csiresim->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_csi_IM_ResourceSetList;
      csiresim->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList =
          calloc(1, sizeof(*csiresim->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList));
      NR_CSI_IM_ResourceSetId_t *csiim00 = calloc(1, sizeof(*csiim00));
      *csiim00 = bwp_loop;
      asn1cSeqAdd(&csiresim->csi_RS_ResourceSetList.choice.csi_IM_ResourceSetList->list, csiim00);
      csiresim->bwp_Id = bwp->bwp_Id;
      csiresim->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
      asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, csiresim);
    }
    NR_CSI_ResourceConfig_t *ssbres = calloc(1, sizeof(*ssbres));
    ssbres->csi_ResourceConfigId = bwp->bwp_Id + 20;
    ssbres->csi_RS_ResourceSetList.present = NR_CSI_ResourceConfig__csi_RS_ResourceSetList_PR_nzp_CSI_RS_SSB;
    ssbres->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB = calloc(1, sizeof(*ssbres->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB));
    ssbres->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList =
        calloc(1, sizeof(*ssbres->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList));
    NR_CSI_SSB_ResourceSetId_t *ssbres00 = calloc(1, sizeof(*ssbres00));
    *ssbres00 = 0;
    asn1cSeqAdd(&ssbres->csi_RS_ResourceSetList.choice.nzp_CSI_RS_SSB->csi_SSB_ResourceSetList->list, ssbres00);
    ssbres->bwp_Id = bwp->bwp_Id;
    ssbres->resourceType = NR_CSI_ResourceConfig__resourceType_periodic;
    asn1cSeqAdd(&csi_MeasConfig->csi_ResourceConfigToAddModList->list, ssbres);

    NR_PUCCH_CSI_Resource_t *pucchcsires1 = calloc(1, sizeof(*pucchcsires1));
    pucchcsires1->uplinkBandwidthPartId = bwp->bwp_Id;
    pucchcsires1->pucch_Resource = 2;

    config_csi_meas_report(csi_MeasConfig,
                           servingcellconfigcommon,
                           pucchcsires1,
                           bwp->bwp_Dedicated->pdsch_Config,
                           pdschap,
                           *pdsch_servingcellconfig->ext1->maxMIMO_Layers,
                           bwp->bwp_Id,
                           uid,
                           curr_bwp);
    config_rsrp_meas_report(csi_MeasConfig,
                            servingcellconfigcommon,
                            pucchcsires1,
                            do_csirs,
                            bwp->bwp_Id + 10,
                            uid,
                            curr_bwp,
                            dl_antenna_ports,
                            configuration->do_SINR);
  }
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->sCellDeactivationTimer = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->crossCarrierSchedulingConfig = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->tag_Id = 0;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->dummy1 = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->pathlossReferenceLinking = NULL;
  secondaryCellGroup->spCellConfig->spCellConfigDedicated->servingCellMO = NULL;

  fill_harq_IEs(secondaryCellGroup->spCellConfig->spCellConfigDedicated, configuration->num_dlharq, configuration->num_ulharq);

  if (LOG_DEBUGFLAG(DEBUG_ASN1)) {
    xer_fprint(stdout, &asn_DEF_NR_SpCellConfig, (void *)secondaryCellGroup->spCellConfig);
  }
  return secondaryCellGroup;
}

NR_ReconfigurationWithSync_t *get_reconfiguration_with_sync(rnti_t rnti, uid_t uid, const NR_ServingCellConfigCommon_t *scc)
{
  NR_ReconfigurationWithSync_t *reconfigurationWithSync = calloc(1, sizeof(*reconfigurationWithSync));
  reconfigurationWithSync->newUE_Identity = rnti;
  reconfigurationWithSync->t304 = NR_ReconfigurationWithSync__t304_ms2000;
  reconfigurationWithSync->rach_ConfigDedicated = NULL;
  reconfigurationWithSync->ext1 = NULL;

  reconfigurationWithSync->spCellConfigCommon = clone_ServingCellConfigCommon(scc);

  reconfigurationWithSync->rach_ConfigDedicated = calloc(1, sizeof(*reconfigurationWithSync->rach_ConfigDedicated));
  reconfigurationWithSync->rach_ConfigDedicated->present = NR_ReconfigurationWithSync__rach_ConfigDedicated_PR_uplink;
  NR_RACH_ConfigDedicated_t *uplink = calloc(1, sizeof(*uplink));
  reconfigurationWithSync->rach_ConfigDedicated->choice.uplink = uplink;
  uplink->ra_Prioritization = NULL;
  uplink->cfra = calloc(1, sizeof(struct NR_CFRA));
  uplink->cfra->ext1 = NULL;
  uplink->cfra->occasions = NULL;
  uplink->cfra->resources.present = NR_CFRA__resources_PR_ssb;
  uplink->cfra->resources.choice.ssb = calloc(1, sizeof(struct NR_CFRA__resources__ssb));
  uplink->cfra->resources.choice.ssb->ra_ssb_OccasionMaskIndex = 0;

  uint64_t bitmap = get_ssb_bitmap(scc);
  for (int i = 0; i < 64; i++) {
    if (((bitmap >> (63 - i)) & 0x01) == 0)
      continue;

    NR_CFRA_SSB_Resource_t *ssbElem = calloc(1, sizeof(*ssbElem));
    ssbElem->ssb = i;
    ssbElem->ra_PreambleIndex = 63 - (uid % 64);
    asn1cSeqAdd(&uplink->cfra->resources.choice.ssb->ssb_ResourceList.list, ssbElem);
  }

  return reconfigurationWithSync;
}
