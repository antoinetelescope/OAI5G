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

#include <math.h>
#include "sim.h"


/* linear phase noise model */
void phase_noise(double ts, int16_t *InRe, int16_t *InIm)
{
  static uint64_t i=0;
  double fd = 300;//0.01*30000
  double phase = (double)(i * fd * ts);
  c16_t rot = get_sin_cos(phase);
  c16_t val = {.r = *InRe, .i = *InIm};
  val = c16mulShift(val, rot, 14);
  *InRe = val.r;
  *InIm = val.i;
  i++;
}

void add_noise(c16_t **rxdata,
               const double **r_re,
               const double **r_im,
               const double sigma,
               const int length,
               const int slot_offset,
               const double ts,
               const int delay,
               const uint16_t pdu_bit_map,
               const uint16_t ptrs_bit_map,
               const uint8_t nb_antennas_rx)
{
  for (int i = 0; i < length; i++) {
    for (int ap = 0; ap < nb_antennas_rx; ap++) {
      c16_t *rxd = &rxdata[ap][slot_offset + i + delay];
      rxd->r = r_re[ap][i] + sqrt(sigma / 2) * gaussZiggurat(0.0, 1.0); // convert to fixed point
      rxd->i = r_im[ap][i] + sqrt(sigma / 2) * gaussZiggurat(0.0, 1.0);
      /* Add phase noise if enabled */
      if (pdu_bit_map & ptrs_bit_map) {
        phase_noise(ts, &rxdata[ap][slot_offset + i + delay].r, &rxdata[ap][slot_offset + i + delay].i);
      }
    }
  }
}
