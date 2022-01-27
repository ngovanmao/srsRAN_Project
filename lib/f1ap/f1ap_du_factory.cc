#include "srsgnb/f1ap/f1ap_du_factory.h"
#include "f1ap_du_impl.h"
#include "f1ap_packet_procedures_impl.h"
#include "srsgnb/du_manager/du_manager.h"

/// Notice this would be the only place were we include concrete class implementation files.

using namespace srsgnb;

std::unique_ptr<f1ap_du_interface> srsgnb::create_f1ap_du(f1ap_du_pdu_notifier&      pdu_listener,
                                                          du_manager_interface_f1ap& du_manager)
{
  auto f1ap_du = std::make_unique<f1ap_du_impl>(pdu_listener, du_manager);
  return f1ap_du;
}
