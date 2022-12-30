/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "pdcp_entity_tx.h"
#include "srsgnb/security/ciphering.h"
#include "srsgnb/security/integrity.h"
#include "srsgnb/support/bit_encoding.h"
#include "srsgnb/support/srsgnb_assert.h"

using namespace srsgnb;

/// \brief Receive an SDU from the upper layers, apply encription
/// and integrity protection and pass the resulting PDU
/// to the lower layers.
///
/// \param sdu Buffer that hold the SDU from higher layers.
/// \ref TS 38.323 section 5.2.1: Transmit operation
void pdcp_entity_tx::handle_sdu(byte_buffer sdu)
{
  metrics_add_sdus(1, sdu.length());

  // The PDCP is not allowed to use the same COUNT value more than once for a given security key,
  // see TS 38.331, section 5.3.1.2. To avoid this, we notify the RRC once we exceed a "maximum"
  // COUNT. It is then the RRC's responsibility to refresh the keys. We continue transmitting until
  // we reached a maximum hard COUNT, after which we simply refuse to TX any further.
  if (st.tx_next >= cfg.max_count.hard) {
    if (!max_count_overflow) {
      logger.log_error("Reached maximum COUNT, refusing to transmit further. COUNT={}", st.tx_next);
      upper_cn.on_protocol_failure();
      max_count_overflow = true;
    }
    return;
  }
  if (st.tx_next >= cfg.max_count.notify) {
    if (!max_count_notified) {
      logger.log_warning("Approaching COUNT wrap-around, notifying RRC. COUNT={}", st.tx_next);
      upper_cn.on_max_count_reached();
      max_count_notified = true;
    }
  }

  // Perform header compression
  // TODO

  // Prepare header
  pdcp_data_pdu_header hdr = {};
  hdr.sn                   = SN(st.tx_next);

  // Pack header
  byte_buffer header_buf = {};
  write_data_pdu_header(header_buf, hdr);

  // Apply ciphering and integrity protection
  byte_buffer protected_buf =
      apply_ciphering_and_integrity_protection(std::move(header_buf), std::move(sdu), st.tx_next);

  // Set meta-data for RLC (TODO)
  // sdu->md.pdcp_sn = tx_next;

  // Start discard timer. If using RLC AM, we store
  // the PDU to use later in the data recovery procedure.
  if (cfg.discard_timer != pdcp_discard_timer::infinity && cfg.discard_timer != pdcp_discard_timer::not_configured) {
    unique_timer discard_timer = timers.create_unique_timer();
    discard_timer.set(static_cast<uint32_t>(cfg.discard_timer), discard_callback{this, st.tx_next});
    discard_timer.run();
    discard_info info;
    if (cfg.rlc_mode == pdcp_rlc_mode::um) {
      info = {{}, std::move(discard_timer)};
    } else {
      info = {protected_buf.copy(), std::move(discard_timer)};
    }
    discard_timers_map.insert(std::make_pair(st.tx_next, std::move(info)));
    logger.log_debug(
        "Discard timer set for COUNT {}. Timeout: {}ms", st.tx_next, static_cast<uint32_t>(cfg.discard_timer));
  }

  // Write to lower layers
  write_data_pdu_to_lower_layers(st.tx_next, std::move(protected_buf));

  // Increment TX_NEXT
  st.tx_next++;
}

void pdcp_entity_tx::write_data_pdu_to_lower_layers(uint32_t count, byte_buffer buf)
{
  logger.log_info(buf.begin(),
                  buf.end(),
                  "TX Data PDU ({}B), COUNT={}, HFN={}, SN={}, integrity={}, encryption={}",
                  buf.length(),
                  count,
                  HFN(count),
                  SN(count),
                  integrity_enabled,
                  ciphering_enabled);
  metrics_add_pdus(1, buf.length());
  pdcp_tx_pdu tx_pdu = {};
  tx_pdu.buf         = std::move(buf);
  if (is_drb()) {
    tx_pdu.pdcp_count = count; // Set only for data PDUs on DRBs.
  }
  lower_dn.on_new_pdu(std::move(tx_pdu));
}

void pdcp_entity_tx::write_control_pdu_to_lower_layers(byte_buffer buf)
{
  logger.log_info(buf.begin(), buf.end(), "TX Control PDU ({}B)", buf.length());
  metrics_add_pdus(1, buf.length());
  pdcp_tx_pdu tx_pdu = {};
  tx_pdu.buf         = std::move(buf);
  // tx_pdu.pdcp_count is not set for control PDUs
  lower_dn.on_new_pdu(std::move(tx_pdu));
}

void pdcp_entity_tx::handle_status_report(byte_buffer_slice_chain status)
{
  byte_buffer buf = {status.begin(), status.end()};
  bit_decoder dec(buf);

  // Unpack and check PDU header
  uint32_t dc;
  dec.unpack(dc, 1);
  if (dc != to_number(pdcp_dc_field::control)) {
    logger.log_warning(buf.begin(),
                       buf.end(),
                       "Cannot handle status report due to invalid D/C field: Expected {}, Got {}",
                       to_number(pdcp_dc_field::control),
                       dc);
    return;
  }
  uint32_t cpt;
  dec.unpack(cpt, 3);
  if (cpt != to_number(pdcp_control_pdu_type::status_report)) {
    logger.log_warning(buf.begin(),
                       buf.end(),
                       "Cannot handle status report due to invalid control PDU type: Expected {}, Got {}",
                       to_number(pdcp_control_pdu_type::status_report),
                       cpt);
    return;
  }
  uint32_t reserved;
  dec.unpack(reserved, 4);
  if (reserved != 0) {
    logger.log_warning(buf.begin(), buf.end(), "Ignoring status report because reserved bits are set: {}", reserved);
    return;
  }

  // Unpack FMC field
  uint32_t fmc;
  dec.unpack(fmc, 32);
  logger.log_info("Received PDCP status report with FMC={}", fmc);

  // Discard any SDU with COUNT < FMC
  for (auto it = discard_timers_map.begin(); it != discard_timers_map.end();) {
    if (it->first < fmc) {
      logger.log_debug("Discarding SDU with COUNT={}", it->first);
      lower_dn.on_discard_pdu(it->first);
      it = discard_timers_map.erase(it);
    } else {
      ++it;
    }
  }

  // Evaluate bitmap: discard any SDU with the bit in the bitmap set to 1
  unsigned bit;
  while (dec.unpack(bit, 1)) {
    fmc++;
    // Bit == 0: PDCP SDU with COUNT = (FMC + bit position) modulo 2^32 is missing.
    // Bit == 1: PDCP SDU with COUNT = (FMC + bit position) modulo 2^32 is correctly received.
    if (bit == 1) {
      logger.log_debug("Discarding SDU with COUNT={}", fmc);
      lower_dn.on_discard_pdu(fmc);
      discard_timers_map.erase(fmc);
    }
  }
}

/*
 * Ciphering and Integrity Protection Helpers
 */
byte_buffer pdcp_entity_tx::apply_ciphering_and_integrity_protection(byte_buffer hdr, byte_buffer sdu, uint32_t count)
{
  // TS 38.323, section 5.9: Integrity protection
  // The data unit that is integrity protected is the PDU header
  // and the data part of the PDU before ciphering.
  security::sec_mac mac = {};
  if (integrity_enabled == security::integrity_enabled::enabled) {
    byte_buffer buf = {};
    buf.append(hdr);
    buf.append(sdu);
    integrity_generate(mac, buf, count);
  }

  // TS 38.323, section 5.8: Ciphering
  // The data unit that is ciphered is the MAC-I and the
  // data part of the PDCP Data PDU except the
  // SDAP header and the SDAP Control PDU if included in the PDCP SDU.
  byte_buffer ct;
  if (ciphering_enabled == security::ciphering_enabled::enabled) {
    byte_buffer buf = {};
    buf.append(sdu);
    // Append MAC-I
    if (is_srb() || (is_drb() && (integrity_enabled == security::integrity_enabled::enabled))) {
      buf.append(mac);
    }
    ct = cipher_encrypt(buf, count);
  } else {
    ct.append(sdu);
    // Append MAC-I
    if (is_srb() || (is_drb() && (integrity_enabled == security::integrity_enabled::enabled))) {
      ct.append(mac);
    }
  }

  // Construct the protected buffer
  byte_buffer protected_buf;
  protected_buf.append(hdr);
  protected_buf.append(ct);

  return protected_buf;
}

void pdcp_entity_tx::integrity_generate(security::sec_mac& mac, byte_buffer_view buf, uint32_t count)
{
  // If control plane use RRC integrity key. If data use user plane key
  const security::sec_128_as_key& k_int = is_srb() ? sec_cfg.k_128_rrc_int : sec_cfg.k_128_up_int;
  switch (sec_cfg.integ_algo) {
    case security::integrity_algorithm::nia0:
      break;
    case security::integrity_algorithm::nia1:
      security_nia1(mac, k_int, count, lcid - 1, direction, buf.begin(), buf.end());
      break;
    case security::integrity_algorithm::nia2:
      security_nia2(mac, k_int, count, lcid - 1, direction, buf.begin(), buf.end());
      break;
    case security::integrity_algorithm::nia3:
      security_nia3(mac, k_int, count, lcid - 1, direction, buf.begin(), buf.end());
      break;
    default:
      break;
  }

  logger.log_debug("Integrity gen input: COUNT {}, Bearer ID {}, Direction {}", count, lcid, direction);
  logger.log_debug((uint8_t*)k_int.data(), k_int.size(), "Integrity gen key:");
  logger.log_debug(buf.begin(), buf.end(), "Integrity gen input message:");
  logger.log_debug((uint8_t*)mac.data(), mac.size(), "MAC (generated)");
}

byte_buffer pdcp_entity_tx::cipher_encrypt(byte_buffer_view msg, uint32_t count)
{
  // If control plane use RRC integrity key. If data use user plane key
  const security::sec_128_as_key& k_enc = is_srb() ? sec_cfg.k_128_rrc_enc : sec_cfg.k_128_up_enc;

  logger.log_debug("Cipher encrypt input: COUNT: {}, Bearer ID: {}, Direction {}", count, lcid, direction);
  logger.log_debug((uint8_t*)k_enc.data(), k_enc.size(), "Cipher encrypt key:");
  logger.log_debug(msg.begin(), msg.end(), "Cipher encrypt input msg");

  byte_buffer ct;

  switch (sec_cfg.cipher_algo) {
    case security::ciphering_algorithm::nea0:
      ct.append(msg);
      break;
    case security::ciphering_algorithm::nea1:
      ct = security_nea1(k_enc, count, lcid - 1, direction, msg.begin(), msg.end());
      break;
    case security::ciphering_algorithm::nea2:
      ct = security_nea2(k_enc, count, lcid - 1, direction, msg.begin(), msg.end());
      break;
    case security::ciphering_algorithm::nea3:
      ct = security_nea3(k_enc, count, lcid - 1, direction, msg.begin(), msg.end());
      break;
    default:
      break;
  }
  logger.log_debug(ct.begin(), ct.end(), "Cipher encrypt output msg");
  return ct;
}

/*
 * Status report and data recovery
 */
void pdcp_entity_tx::send_status_report()
{
  if (cfg.status_report_required) {
    logger.log_info("Status report triggered");
    byte_buffer status_report = status_provider->compile_status_report();
    write_control_pdu_to_lower_layers(std::move(status_report));
  } else {
    logger.log_warning("Status report triggered but not configured");
  }
}

void pdcp_entity_tx::data_recovery()
{
  srsgnb_assert(is_drb() && cfg.rlc_mode == pdcp_rlc_mode::am, "Invalid bearer type for data recovery.");
  logger.log_info("Data recovery requested");

  /*
   * TS 38.323 Sec. 5.4.1:
   * [...] the receiving PDCP entity shall trigger a PDCP status report when:
   * [...] -upper layer requests a PDCP data recovery; [...]
   */
  if (cfg.status_report_required) {
    send_status_report();
  }
  for (const auto& info : discard_timers_map) {
    write_data_pdu_to_lower_layers(info.first, info.second.buf.copy());
  }
}
/*
 * PDU Helpers
 */
void pdcp_entity_tx::write_data_pdu_header(byte_buffer& buf, const pdcp_data_pdu_header& hdr) const
{
  // Sanity check: 18bit SRB not allowed
  srsgnb_assert(!(is_srb() && cfg.sn_size == pdcp_sn_size::size18bits), "Invalid 18 bit SRB PDU");

  byte_buffer_writer hdr_writer = buf;

  // Set D/C if required
  if (is_drb()) {
    hdr_writer.append(0x80); // D/C bit field (1).
  } else {
    hdr_writer.append(0x00); // No D/C bit field.
  }

  // Add SN
  switch (cfg.sn_size) {
    case pdcp_sn_size::size12bits:
      hdr_writer.back() |= (hdr.sn & 0x00000f00U) >> 8U;
      hdr_writer.append((hdr.sn & 0x000000ffU));
      break;
    case pdcp_sn_size::size18bits:
      hdr_writer.back() |= (hdr.sn & 0x00030000U) >> 16U;
      hdr_writer.append((hdr.sn & 0x0000ff00U) >> 8U);
      hdr_writer.append((hdr.sn & 0x000000ffU));
      break;
    default:
      logger.log_error("Invalid SN length configuration: {} bits", cfg.sn_size);
  }
}

/*
 * Timers
 */
// Discard Timer Callback (discardTimer)
void pdcp_entity_tx::discard_callback::operator()(uint32_t timer_id)
{
  parent->logger.log_debug("Discard timer expired for PDU with COUNT={}", discard_count);

  // Notify the RLC of the discard. It's the RLC to actually discard, if no segment was transmitted yet.
  parent->lower_dn.on_discard_pdu(discard_count);

  // Add discard to metrics
  parent->metrics_add_discard_timouts(1);

  // Remove timer from map
  // NOTE: this will delete the callback. It *must* be the last instruction.
  parent->discard_timers_map.erase(discard_count);
}
