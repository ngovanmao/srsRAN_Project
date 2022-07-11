/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#ifndef SRSGNB_RAN_RESOURCE_ALLOCATION_RB_INTERVAL_H
#define SRSGNB_RAN_RESOURCE_ALLOCATION_RB_INTERVAL_H

#include "srsgnb/adt/interval.h"

namespace srsgnb {

/// Struct to express a {min,...,max} range of CRBs within a carrier.
struct crb_interval : public interval<unsigned> {
  using interval::interval;
};

/// Struct to express a {min,...,max} range of PRBs within a BWP.
struct prb_interval : public interval<unsigned> {
  using interval::interval;
};

} // namespace srsgnb

namespace fmt {

/// FMT formatter for prb_intervals.
template <>
struct formatter<srsgnb::prb_interval> : public formatter<srsgnb::interval<uint32_t>> {
};

/// FMT formatter for crb_intervals.
template <>
struct formatter<srsgnb::crb_interval> : public formatter<srsgnb::interval<uint32_t>> {
};

} // namespace fmt

#endif // SRSGNB_RAN_RESOURCE_ALLOCATION_RB_INTERVAL_H
