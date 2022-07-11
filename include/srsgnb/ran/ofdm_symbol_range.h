/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#ifndef SRSGNB_RAN_OFDM_SYMBOL_RANGE_H
#define SRSGNB_RAN_OFDM_SYMBOL_RANGE_H

#include "srsgnb/adt/interval.h"
#include "srsgnb/ran/frame_types.h"
#include "srsgnb/ran/sliv.h"

namespace srsgnb {

/// Range [start,stop) of OFDM symbols.
struct ofdm_symbol_range : public interval<uint8_t> {
  using interval<uint8_t>::interval;
};

/// \brief Converts SLIV to OFDM symbol start S and length L.
/// \param[in] sliv An index giving a combination (jointly encoded) of start symbols and length indicator (SLIV).
/// \param[out] symbols Symbol interval as [S, S+L).
inline ofdm_symbol_range sliv_to_ofdm_symbols(uint32_t sliv)
{
  uint8_t symbol_S, symbol_L;
  sliv_to_s_and_l(NOF_OFDM_SYM_PER_SLOT_NORMAL_CP, sliv, symbol_S, symbol_L);
  return {symbol_S, static_cast<uint8_t>(symbol_S + symbol_L)};
}

} // namespace srsgnb

#endif // SRSGNB_RAN_OFDM_SYMBOL_RANGE_H
