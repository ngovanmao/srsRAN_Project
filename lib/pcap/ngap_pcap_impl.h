/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "pcap_file_base.h"
#include "srsgnb/adt/byte_buffer.h"
#include "srsgnb/pcap/pcap.h"
#include "srsgnb/support/executors/task_worker.h"

namespace srsgnb {

class ngap_pcap_impl : public ngap_pcap
{
public:
  ngap_pcap_impl();
  ~ngap_pcap_impl() override;
  ngap_pcap_impl(const ngap_pcap_impl& other)            = delete;
  ngap_pcap_impl& operator=(const ngap_pcap_impl& other) = delete;
  ngap_pcap_impl(ngap_pcap_impl&& other)                 = delete;
  ngap_pcap_impl& operator=(ngap_pcap_impl&& other)      = delete;

  void open(const char* filename_) override;
  void close() override;
  bool is_write_enabled() override;
  void push_pdu(srsgnb::byte_buffer pdu) override;
  void push_pdu(srsgnb::const_span<uint8_t> pdu) override;

private:
  void                 write_pdu(srsgnb::byte_buffer buf);
  task_worker          worker;
  std::vector<uint8_t> tmp_mem;
  pcap_file_base       writter;
};

} // namespace srsgnb
