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

/*! \file PHY/NR_TRANSPORT/nr_dci.c
* \brief Implements DCI encoding and PDCCH TX procedures (38.212/38.213/38.214). V15.4.0 2019-01.
* \author Guy De Souza
* \date 2018
* \version 0.1
* \company Eurecom
* \email: desouza@eurecom.fr
* \note
* \warning
*/


#include "nr_dci.h"
#include "nr_dlsch.h"
#include "nr_sch_dmrs.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "common/utils/nr/nr_common.h"
#include "SCHED_NR/sched_nr.h"

//#define DEBUG_PDCCH_DMRS
//#define DEBUG_DCI
//#define DEBUG_CHANNEL_CODING

static void nr_pdcch_scrambling(uint32_t *in, uint32_t size, uint32_t Nid, uint32_t scrambling_RNTI, uint32_t *out)
{
  int roundedSz = ((size + 31) / 32);
  uint32_t *seq = gold_cache((scrambling_RNTI << 16) + Nid, roundedSz);
  LOG_D(NR_PHY_DCI, "PDCCH scrambling_RNTI %x \n", scrambling_RNTI);
  for (int i = 0; i < roundedSz; i++)
    out[i] = in[i] ^ seq[i];
}

static void nr_generate_dci(PHY_VARS_gNB *gNB,
                            nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu_rel15,
                            int txdataF_offset,
                            NR_DL_FRAME_PARMS *frame_parms,
                            int slot)
{
  // fill reg list per symbol
  int reg_list[MAX_DCI_CORESET][NR_MAX_PDCCH_AGG_LEVEL * NR_NB_REG_PER_CCE];
  nr_fill_reg_list(reg_list, pdcch_pdu_rel15);
  // compute rb_offset and n_prb based on frequency allocation
  int rb_offset;
  int n_rb;
  get_coreset_rballoc(pdcch_pdu_rel15->FreqDomainResource,&n_rb,&rb_offset);
  uint16_t cset_start_sc = frame_parms->first_carrier_offset + (pdcch_pdu_rel15->BWPStart + rb_offset) * NR_NB_SC_PER_RB;
  int idx1 = pdcch_pdu_rel15->StartSymbolIndex+pdcch_pdu_rel15->DurationSymbols;
  int idx2 = (((n_rb + rb_offset + pdcch_pdu_rel15->BWPStart) * 3) + 15) & ~15;
  c16_t mod_dmrs[idx1][idx2] __attribute__((aligned(16)));

  for (int d = 0; d < pdcch_pdu_rel15->numDlDci; d++) {
    /*The coreset is initialised
     * in frequency: the first subcarrier is obtained by adding the first CRB overlapping the SSB and the rb_offset for coreset 0
     * or the rb_offset for other coresets
     * in time: by its first slot and its first symbol*/
    const nfapi_nr_dl_dci_pdu_t *dci_pdu = &pdcch_pdu_rel15->dci_pdu[d];

    uint32_t cset_start_symb = pdcch_pdu_rel15->StartSymbolIndex;
    uint32_t cset_nsymb = pdcch_pdu_rel15->DurationSymbols;
    int dci_idx = 0;
    // multi-beam number (for concurrent beams)
    int bitmap = SL_to_bitmap(cset_start_symb, pdcch_pdu_rel15->DurationSymbols);
    int beam_nb = beam_index_allocation(gNB->enable_analog_das,
                                        dci_pdu->precodingAndBeamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx,
                                        &gNB->gNB_config.analog_beamforming_ve,
                                        &gNB->common_vars,
                                        slot,
                                        frame_parms->symbols_per_slot,
                                        bitmap);

    LOG_D(NR_PHY_DCI, "pdcch: Coreset rb_offset %d, nb_rb %d BWP Start %d\n", rb_offset, n_rb, pdcch_pdu_rel15->BWPStart);
    LOG_D(NR_PHY_DCI,
          "pdcch: Coreset starting subcarrier %d on symbol %d (%d symbols)\n",
          cset_start_sc,
          cset_start_symb,
          cset_nsymb);
    // DMRS length is per OFDM symbol
    uint32_t dmrs_length = (n_rb+pdcch_pdu_rel15->BWPStart)*6; //2(QPSK)*3(per RB)*6(REG per CCE)
    uint32_t encoded_length = dci_pdu->AggregationLevel*108; //2(QPSK)*9(per RB)*6(REG per CCE)
    if (dci_pdu->RNTI != 0xFFFF)
      LOG_D(NR_PHY_DCI,
            "DL_DCI : rb_offset %d, nb_rb %d, DMRS length per symbol %d\t DCI encoded length %d (precoder_granularity %d, "
            "reg_mapping %d), Scrambling_Id %d, ScramblingRNTI %x, PayloadSizeBits %d AggregationLevel %d\n",
            rb_offset,
            n_rb,
            dmrs_length,
            encoded_length,
            pdcch_pdu_rel15->precoderGranularity,
            pdcch_pdu_rel15->CceRegMappingType,
            dci_pdu->ScramblingId,
            dci_pdu->ScramblingRNTI,
            dci_pdu->PayloadSizeBits,
            dci_pdu->AggregationLevel);
    dmrs_length += rb_offset*6; // To accommodate more DMRS symbols in case of rb offset
      
    /// DMRS QPSK modulation
    for (int symb = cset_start_symb; symb < cset_start_symb + pdcch_pdu_rel15->DurationSymbols; symb++) {
      const uint32_t *gold = nr_gold_pdcch(frame_parms->N_RB_DL, frame_parms->symbols_per_slot, dci_pdu->ScramblingId, slot, symb);
      nr_modulation(gold, dmrs_length, DMRS_MOD_ORDER, (int16_t *)mod_dmrs[symb]); // Qm = 2 as DMRS is QPSK modulated

#ifdef DEBUG_PDCCH_DMRS
      if(dci_pdu->RNTI!=0xFFFF) {
        for (int i=0; i<dmrs_length>>1; i++)
          printf("symb %d i %d %p gold seq 0x%08x mod_dmrs %d %d\n",
                 symb,
                 i,
                 &gold_pdcch_dmrs[symb][i >> 5],
                 gold_pdcch_dmrs[symb][i >> 5],
                 mod_dmrs[symb][i].r,
                 mod_dmrs[symb][i].i);
      }
#endif
    }
    
    /// DCI payload processing
    // CRC attachment + Scrambling + Channel coding + Rate matching
    uint32_t encoder_output[NR_MAX_DCI_SIZE_DWORD];

    uint16_t n_RNTI = dci_pdu->RNTI;
    uint16_t Nid    = dci_pdu->ScramblingId;
    uint16_t scrambling_RNTI = dci_pdu->ScramblingRNTI;

    polar_encoder_fast((uint64_t*)dci_pdu->Payload, (void*)encoder_output, n_RNTI, 1, 
                       NR_POLAR_DCI_MESSAGE_TYPE, dci_pdu->PayloadSizeBits, dci_pdu->AggregationLevel);
#ifdef DEBUG_CHANNEL_CODING
//debug dump dci
    printf("polar rnti %x,length %d, L %d\n",n_RNTI, dci_pdu->PayloadSizeBits,pdcch_pdu_rel15->dci_pdu->AggregationLevel);
    printf("DCI PDU: [0]->0x%lx \t [1]->0x%lx\n",
	   ((uint64_t*)dci_pdu->Payload)[0], ((uint64_t*)dci_pdu->Payload)[1]);
    printf("Encoded Payload (length:%u dwords):\n", encoded_length>>5);
    
    for (int i=0; i<encoded_length>>5; i++)
      printf("[%d]->0x%08x \t", i,encoder_output[i]);    

    printf("\n");
#endif
    /// Scrambling
    uint32_t scrambled_output[NR_MAX_DCI_SIZE_DWORD]= {0};
    nr_pdcch_scrambling(encoder_output, encoded_length, Nid, scrambling_RNTI, scrambled_output);
#ifdef DEBUG_CHANNEL_CODING
    printf("scrambled output: [0]->0x%08x \t [1]->0x%08x \t [2]->0x%08x \t [3]->0x%08x\t [4]->0x%08x\t [5]->0x%08x\t \
[6]->0x%08x \t [7]->0x%08x \t [8]->0x%08x \t [9]->0x%08x\t [10]->0x%08x\t [11]->0x%08x\n",
	   scrambled_output[0], scrambled_output[1], scrambled_output[2], scrambled_output[3], scrambled_output[4],scrambled_output[5],
	   scrambled_output[6], scrambled_output[7], scrambled_output[8], scrambled_output[9], scrambled_output[10],scrambled_output[11] );
#endif
    /// QPSK modulation
    c16_t mod_dci[NR_MAX_DCI_SIZE / 2] __attribute__((aligned(16)));
    nr_modulation(scrambled_output, encoded_length, DMRS_MOD_ORDER, (int16_t *)mod_dci); // Qm = 2 as DMRS is QPSK modulated
#ifdef DEBUG_DCI
    
    for (int i=0; i<encoded_length>>1; i++)
      printf("i %d mod_dci %d %d\n", i, mod_dci[i].r, mod_dci[i].i);

#endif

    /// Resource mapping
    uint16_t amp = gNB->TX_AMP;
    c16_t *txdataF = gNB->common_vars.txdataF[beam_nb][0] + txdataF_offset;
    if (cset_start_sc >= frame_parms->ofdm_symbol_size)
      cset_start_sc -= frame_parms->ofdm_symbol_size;

    int num_regs = dci_pdu->AggregationLevel * NR_NB_REG_PER_CCE / pdcch_pdu_rel15->DurationSymbols;
    /*Mapping the encoded DCI along with the DMRS */
    for(int symbol_idx = 0; symbol_idx < pdcch_pdu_rel15->DurationSymbols; symbol_idx++) {
      // allocating rbs per symbol
      for (int reg_count = 0; reg_count < num_regs; reg_count++) {
        int k = cset_start_sc + reg_list[d][reg_count] * NR_NB_SC_PER_RB;
        LOG_D(NR_PHY_DCI, "REG %d k %d\n", reg_list[d][reg_count], k);
        if (k >= frame_parms->ofdm_symbol_size)
          k -= frame_parms->ofdm_symbol_size;

        int l = cset_start_symb + symbol_idx;

        // dmrs index depends on reference point for k according to 38.211 7.4.1.3.2
        int dmrs_idx;
        if (pdcch_pdu_rel15->CoreSetType == NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG)
          dmrs_idx = (reg_list[d][reg_count] + pdcch_pdu_rel15->BWPStart) * 3;
        else
          dmrs_idx = (reg_list[d][reg_count] + rb_offset) * 3;

        int k_prime = 0;

        for (int m = 0; m < NR_NB_SC_PER_RB; m++) {
          if (m == (k_prime << 2) + 1) { // DMRS if not already mapped
            txdataF[l * frame_parms->ofdm_symbol_size + k] = c16mulRealShift(mod_dmrs[l][dmrs_idx], amp, 15);

#ifdef DEBUG_PDCCH_DMRS
            LOG_I(NR_PHY_DCI,
                  "PDCCH DMRS %d: l %d position %d => (%d,%d)\n",
                  dmrs_idx,
                  l,
                  k,
                  txdataF[l * frame_parms->ofdm_symbol_size + k].r,
                  txdataF[l * frame_parms->ofdm_symbol_size + k].i);
#endif

            dmrs_idx++;
            k_prime++;

          } else { // DCI payload
            txdataF[l * frame_parms->ofdm_symbol_size + k] = c16mulRealShift(mod_dci[dci_idx], amp, 15);
#ifdef DEBUG_DCI
            LOG_I(NR_PHY_DCI,
                  "PDCCH: l %d position %d => (%d,%d)\n",
                  l,
                  k,
		  		  txdataF[l * frame_parms->ofdm_symbol_size + k].r,
		  txdataF[l * frame_parms->ofdm_symbol_size + k].i;
#endif

            dci_idx++;
          }

          k++;

          if (k >= frame_parms->ofdm_symbol_size)
            k -= frame_parms->ofdm_symbol_size;
        } // m
      } // reg_count
    } // symbol_idx

    LOG_D(NR_PHY_DCI,
          "DCI: payloadSize = %d | payload = %llx\n",
          dci_pdu->PayloadSizeBits,
          *(unsigned long long *)dci_pdu->Payload);
  } // for (int d=0;d<pdcch_pdu_rel15->numDlDci;d++)
}

void nr_generate_dci_top(processingData_L1tx_t *msgTx, int slot, int txdataF_offset)
{
  PHY_VARS_gNB *gNB = msgTx->gNB;
  NR_DL_FRAME_PARMS *frame_parms = &gNB->frame_parms;
  start_meas(&gNB->dci_generation_stats);
  for (int i = 0; i < msgTx->num_ul_pdcch; i++)
    nr_generate_dci(msgTx->gNB, &msgTx->ul_pdcch_pdu[i].pdcch_pdu.pdcch_pdu_rel15, txdataF_offset, frame_parms, slot);
  for (int i = 0; i < msgTx->num_dl_pdcch; i++)
    nr_generate_dci(msgTx->gNB, &msgTx->pdcch_pdu[i].pdcch_pdu_rel15, txdataF_offset, frame_parms, slot);
  stop_meas(&gNB->dci_generation_stats);
}

