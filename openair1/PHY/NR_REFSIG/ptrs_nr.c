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

/**********************************************************************
*
* FILENAME    :  ptrs_nr.c
*
* MODULE      :  phase tracking reference signals
*
* DESCRIPTION :  resource element mapping of ptrs sequences
*                3GPP TS 38.211 and 3GPP TS 38.214
*
************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include "dmrs_nr.h"
#include "PHY/NR_REFSIG/ptrs_nr.h"
#include "PHY/NR_REFSIG/nr_refsig.h"

/*******************************************************************
*
* NAME :         set_ptrs_symb_idx
*
* PARAMETERS :   ptrs_symbols           PTRS OFDM symbol indicies bit mask
*                duration_in_symbols    number of scheduled ofdm symbols
*                start_symbol           first ofdm symbol within slot
*                L_ptrs                 the parameter L_ptrs
*                dmrs_symb_pos          bitmap of the time domain positions of the DMRS symbols
*
* RETURN :       sets the bit map of PTRS ofdm symbol indicies
*
* DESCRIPTION :  3GPP TS 38.211 6.4.1.2.2.1
*
*********************************************************************/

void set_ptrs_symb_idx(uint16_t *ptrs_symbols,
                       uint8_t duration_in_symbols,
                       uint8_t start_symbol,
                       uint8_t L_ptrs,
                       uint16_t dmrs_symb_pos)
{
  int i = 0;
  int l_ref = start_symbol;
  const int last_symbol = start_symbol + duration_in_symbols - 1;
  if (L_ptrs == 0) {
    LOG_E(PHY,"bug: impossible L_ptrs\n");
    *ptrs_symbols = 0;
    return;
  }

  while ((l_ref + i * L_ptrs) <= last_symbol) {
    int is_dmrs_symbol = 0, l_counter;
    for(l_counter = l_ref + i * L_ptrs; l_counter >= max(l_ref + (i - 1) * L_ptrs + 1, l_ref); l_counter--) {
      if((dmrs_symb_pos >> l_counter) & 0x01) {
        is_dmrs_symbol = 1;
        break;
      }
    }
    if (is_dmrs_symbol) {
      l_ref = l_counter;
      i = 1;
      continue;
    }
    *ptrs_symbols = *ptrs_symbols | (1 << (l_ref + i * L_ptrs));
    i++;
  }
}

unsigned int get_first_ptrs_re(const rnti_t rnti, const uint8_t K_ptrs, const uint16_t nRB, const uint8_t k_RE_ref)
{
  const uint16_t nRB_Kptrs = nRB % K_ptrs;
  const uint16_t k_RB_ref = nRB_Kptrs ? (rnti % nRB_Kptrs) : (rnti % K_ptrs);
  return (k_RE_ref + k_RB_ref * NR_NB_SC_PER_RB);
}

/*******************************************************************
*
* NAME :         is_ptrs_subcarrier
*
* PARAMETERS : k                      subcarrier index
*              n_rnti                 UE CRNTI
*              K_ptrs                 the parameter K_ptrs
*              N_RB                   number of RBs scheduled
*              k_RE_ref               the parameter k_RE_ref
*              start_sc               first subcarrier index
*              ofdm_symbol_size       number of samples in an OFDM symbol
*
* RETURN :       1 if subcarrier k is PTRS, or 0 otherwise
*
* DESCRIPTION :  3GPP TS 38.211 6.4.1.2 Phase-tracking reference signal
*
*********************************************************************/

uint8_t is_ptrs_subcarrier(uint16_t k,
                           uint16_t n_rnti,
                           uint8_t K_ptrs,
                           uint16_t N_RB,
                           uint8_t k_RE_ref,
                           uint16_t start_sc,
                           uint16_t ofdm_symbol_size)
{
  uint16_t k_RB_ref;

  if (N_RB % K_ptrs == 0)
    k_RB_ref = n_rnti % K_ptrs;
  else
    k_RB_ref = n_rnti % (N_RB % K_ptrs);

  if (k < start_sc)
    k += ofdm_symbol_size;

  if ((k - k_RE_ref - k_RB_ref * NR_NB_SC_PER_RB - start_sc) % (K_ptrs * NR_NB_SC_PER_RB) == 0)
    return 1;

  return 0;
}

/* return the total number of ptrs symbol in a slot */
uint8_t get_ptrs_symbols_in_slot(uint16_t l_prime_mask, uint16_t start_symb, uint16_t nb_symb)
{
  uint8_t tmp = 0;
  for (int i = start_symb; i <= nb_symb; i++)
    tmp += (l_prime_mask >> i) & 0x01;
  return tmp;
}

/* return the position of next ptrs symbol in a slot */
int8_t get_next_ptrs_symbol_in_slot(uint16_t  ptrs_symb_pos, uint8_t counter, uint8_t nb_symb)
{
  for(int8_t symbol = counter; symbol < nb_symb; symbol++) {
    if((ptrs_symb_pos >> symbol) & 0x01) {
      return symbol;
    }
  }
  return -1;
}

/* get the next nearest estimate from DMRS or PTRS */
int8_t get_next_estimate_in_slot(uint16_t  ptrsSymbPos,uint16_t  dmrsSymbPos, uint8_t counter,uint8_t nb_symb)
{
  int8_t nextPtrs = get_next_ptrs_symbol_in_slot(ptrsSymbPos, counter,nb_symb);
  int8_t nextDmrs = get_next_dmrs_symbol_in_slot(dmrsSymbPos, counter,nb_symb);
  if(nextDmrs == -1) {
    return nextPtrs;
  }
  /* Special case when DMRS is next valid estimation */
  if(nextPtrs == -1) {
    return nextDmrs;
  }
  return (nextPtrs > nextDmrs)?nextDmrs:nextPtrs;
}

/*******************************************************************
 *
 * NAME :         nr_ptrs_cpe_estimation
 *
 * PARAMETERS :   K_ptrs        : K value for PTRS
 *                ptrsReOffset  : RE offset for PTRS
 *                nb_rb         : No. of resource blocks
 *                rnti          : RNTI
 *                Ns            :
 *                symbol        : OFDM symbol
 *              ofdm_symbol_size: OFDM Symbol Size
 *                rxF_comp      : pointer to channel compensated signal
 *                gold_seq      : Gold sequence pointer
 *                error_est     : Estimated error output vector [Re Im]
 *                ptrs_sc       : Total PTRS RE in a symbol
 * RETURN :       nothing
 *
 * DESCRIPTION :
 *  perform phase estimation from regenerated PTRS SC and channel compensated
 *  signal
 *********************************************************************/
void nr_ptrs_cpe_estimation(uint8_t K_ptrs,
                            uint8_t ptrsReOffset,
                            uint16_t nb_rb,
                            uint16_t rnti,
                            unsigned char Ns,
                            unsigned char symbol,
                            uint16_t ofdm_symbol_size,
                            int16_t *rxF_comp,
                            const uint32_t *gold_seq,
                            int16_t *error_est,
                            int32_t *ptrs_sc)
{
//#define DEBUG_PTRS 1
  if (K_ptrs == 0) {
    LOG_E(PHY,"K_ptrs == 0\n");
    return;
  }

  uint16_t sc_per_symbol = (nb_rb + K_ptrs - 1) / K_ptrs;
  c16_t ptrs_p[(1 + sc_per_symbol / 4) * 4];
  c16_t dmrs_comp_p[(1 + sc_per_symbol / 4) * 4];

  /* generate PTRS RE for the symbol */
  nr_gen_ref_conj_symbols(gold_seq, sc_per_symbol * 2, (int16_t *)ptrs_p, 2); // 2 for QPSK
  uint32_t re_cnt = 0, cnt = 0;
  /* loop over all sub carriers to get compensated RE on ptrs symbols*/
  for (int re = 0; re < NR_NB_SC_PER_RB * nb_rb; re++) {
    uint8_t is_ptrs_re = is_ptrs_subcarrier(re,
                                            rnti,
                                            K_ptrs,
                                            nb_rb,
                                            ptrsReOffset,
                                            0,// start_re is 0 here
                                            ofdm_symbol_size);
    if(is_ptrs_re) {
      dmrs_comp_p[re_cnt].r = rxF_comp[re *2];
      dmrs_comp_p[re_cnt].i = rxF_comp[(re *2)+1];
      re_cnt++;
    }
    else {
      /* Skip PTRS symbols and keep data in a continuous vector */
      rxF_comp[cnt *2]= rxF_comp[re *2];
      rxF_comp[(cnt *2)+1]= rxF_comp[(re *2)+1];
      cnt++;
    }
  }/* RE loop */
  /* update the total ptrs RE in a symbol */
  *ptrs_sc = re_cnt;

  c16_t ptrs_ch_p[(1 + sc_per_symbol / 4) * 4];
  /*Multiple compensated data with conj of PTRS */
  mult_cpx_vector((c16_t *)dmrs_comp_p, (c16_t *)ptrs_p, (c16_t *)ptrs_ch_p, (1 + sc_per_symbol / 4) * 4, 15); // 2^15 shifted

  /* loop over all ptrs sub carriers in a symbol */
  /* sum the error vector */
  double real = 0.0;
  double imag = 0.0;
  for(int i = 0;i < sc_per_symbol; i++) {
    real += ptrs_ch_p[i].r;
    imag += ptrs_ch_p[i].i;
  }
#ifdef DEBUG_PTRS
  double alpha = atan(imag/real);
  printf("[PHY][PTRS]: Symbol  %d atan(Im,real):= %f \n",symbol, alpha );
#endif
  /* mean */
  real /= sc_per_symbol;
  imag /= sc_per_symbol;
  /* absolute calculation */
  double abs = sqrt(((real * real) + (imag *  imag)));
  /* normalized error estimation */
  error_est[0]= (real / abs)*(1<<15);
  /* compensation in given by conjugate of estimated phase (e^-j*2*pi*fd*t)*/
  error_est[1]= (-1)*(imag / abs)*(1<<15);
#ifdef DEBUG_PTRS
  printf("[PHY][PTRS]: Estimated Symbol  %d -> %d + j* %d \n",symbol, error_est[0], error_est[1] );
#endif
}


/*******************************************************************
 *
 * NAME :     nr_ptrs_process_slot
 *
 * PARAMETERS : dmrsSymbPos            DMRS symbol index mask per slot
 *              ptrsSymbpos            PTRS symbol index mask per slot
 *              estPerSymb             Estimated CPE pointer
 *              startSymbIdx           First symbol index in a slot
 *              noSymb                 total number of OFDM symbols in a slot
 * RETURN :     True if slot is process correctly
 * DESCRIPTION :
 *  Process whole slot and interpolate or extrapolate estimation for the symbols
 *  where there is neither PTRS nor DMRS configured
 *
 *********************************************************************/
int8_t nr_ptrs_process_slot(uint16_t dmrsSymbPos,
                            uint16_t ptrsSymbPos,
                            int16_t *estPerSymb,
                            uint16_t startSymbIdx,
                            uint16_t noSymb)
{
  double         slope[2]    = {0,0};
  double         *slope_p    = &slope[0]; 
  uint8_t         symbInSlot = startSymbIdx + noSymb;
  int8_t         rightRef   = 0;
  int8_t         leftRef    = 0;
  int8_t         tmp        = 0;
  for(uint8_t symb = startSymbIdx; symb <symbInSlot;  symb ++) {
    /* Update left and right reference from an estimated symbol */
    if((is_ptrs_symbol(symb, ptrsSymbPos)) || (is_dmrs_symbol(symb,dmrsSymbPos))) {
      leftRef  =  symb;
      rightRef =  get_next_estimate_in_slot(ptrsSymbPos,dmrsSymbPos,symb+1,symbInSlot);
    }
    else {
      /* The very first symbol must be a PTRS or DMRS */
      if((symb == startSymbIdx) && (leftRef == -1) && (rightRef == -1)) {
        printf("Wrong PTRS Setup, PTRS compensation will be skipped !");
        return -1;
      }
      /* check for left side first */
      /*  right side a DMRS symbol then we need to left extrapolate */
      if (rightRef != -1 && is_dmrs_symbol(rightRef, dmrsSymbPos)) {
        /* calculate slope from next valid estimates*/
        tmp =  get_next_estimate_in_slot(ptrsSymbPos,dmrsSymbPos,rightRef+1,symbInSlot);
        /* Special case when DMRS is not followed by PTRS symbol then reuse old slope */
        if(tmp!=-1) {
          get_slope_from_estimates(rightRef, tmp, estPerSymb, slope_p);
        }
        ptrs_estimate_from_slope(estPerSymb,slope_p,leftRef, rightRef);
        symb = rightRef -1;
      } else if (rightRef != -1 && is_ptrs_symbol(rightRef, ptrsSymbPos)) {
        /* calculate slope from next valid estimates */
        get_slope_from_estimates(leftRef,rightRef,estPerSymb, slope_p);
        ptrs_estimate_from_slope(estPerSymb,slope_p,leftRef, rightRef);
        symb = rightRef -1;
      } else if ((rightRef == -1) && (symb < symbInSlot)) {
        // in right extrapolation use the last slope
#ifdef DEBUG_PTRS
        printf("[PHY][PTRS]: Last Slop Reused :(%4f %4f)\n", slope_p[0],slope_p[1]);
#endif
        ptrs_estimate_from_slope(estPerSymb,slope_p,symb-1,symbInSlot);
        symb = symbInSlot;
      } else {
        printf("Wrong PTRS Setup, PTRS compensation will be skipped !");
        return -1;
      }
    }
  }
  return 0;
}

/* Calculate slope from 2 reference points */
void get_slope_from_estimates(uint8_t start, uint8_t end, int16_t *est_p, double *slope_p)
{
  uint8_t distance = end - start;
  slope_p[0] = (double)(est_p[end*2] - est_p[start*2]) /distance;
  slope_p[1] = (double)(est_p[(end*2)+1] - est_p[(start*2)+1]) /distance;
#ifdef DEBUG_PTRS
  printf("[PHY][PTRS]: Slop is :(%4f %4f) between Symbol %2d & Symbol %2d\n", slope_p[0],slope_p[1], start, end);
  //printf("%d %d - %d %d\n",est_p[end*2],est_p[(end*2)+1],est_p[start*2],est_p[(start*2)+1]);
#endif
}

/* estimate from slope */
void ptrs_estimate_from_slope(int16_t *error_est, double *slope_p, uint8_t start, uint8_t end)
{
  c16_t *error=(struct complex16 *) error_est;
  for(uint8_t i = 1; i< (end -start);i++) {
    error[start+i].r = error[start].r + (int16_t)(i * slope_p[0]);// real
    error[start+i].i = error[start].i + (int16_t)(i * slope_p[1]); //imag
#ifdef DEBUG_PTRS
    printf("[PHY][PTRS]: Estimated Symbol %2d -> %4d %4d from Slope (%4f %4f)\n", start+i,error_est[(start+i)*2],error_est[((start +i)*2)+1],
           slope_p[0],slope_p[1]);
#endif
  }
}
