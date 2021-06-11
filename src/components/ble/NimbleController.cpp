#include "NimbleController.h"
#include <hal/nrf_rtc.h>
#define min // workaround: nimble's min/max macros conflict with libstdc++
#define max
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_id.h>
#include <host/util/util.h>
#undef max
#undef min
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include "components/ble/BleController.h"
#include "components/ble/NotificationManager.h"
#include "components/datetime/DateTimeController.h"
#include "systemtask/SystemTask.h"

using namespace Pinetime::Controllers;

#define RECEIVER_ID 123

NimbleController::NimbleController(Pinetime::System::SystemTask& systemTask,
                                   Pinetime::Controllers::Ble& bleController,
        DateTime& dateTimeController,
        Pinetime::Controllers::NotificationManager& notificationManager,
        Controllers::Battery& batteryController,
        Pinetime::Drivers::SpiNorFlash& spiNorFlash,
        Controllers::HeartRateController& heartRateController) :
        systemTask{systemTask},
        bleController{bleController},
        dateTimeController{dateTimeController},
        notificationManager{notificationManager},
        spiNorFlash{spiNorFlash},
        dfuService{systemTask, bleController, spiNorFlash},
        currentTimeClient{dateTimeController},
        anService{systemTask, notificationManager},
        alertNotificationClient{systemTask, notificationManager},
        currentTimeService{dateTimeController},
        musicService{systemTask},
        navService{systemTask},
        batteryInformationService{batteryController},
        immediateAlertService{systemTask, notificationManager},
        heartRateService{systemTask, heartRateController},
        serviceDiscovery({&currentTimeClient, &alertNotificationClient}) {
}

int GAPEventCallback(struct ble_gap_event *event, void *arg) {
  auto nimbleController = static_cast<NimbleController*>(arg);
  return nimbleController->OnGAPEvent(event);
}

void handleNotification(NotificationManager *notificationManager, Pinetime::System::SystemTask *systemTask, uint16_t room) {
  //std::string msg = "qwert";

  NotificationManager::Notification notif;
  notif.message = {
    't', 'e', 's', 'q', 't', 'i', '\0'
  };

  //std::copy(msg.begin(), msg.end(), notif.message.data());

  notif.category = Pinetime::Controllers::NotificationManager::Categories::HighProriotyAlert;
  notificationManager->Push(std::move(notif));

  systemTask->PushMessage(Pinetime::System::SystemTask::Messages::OnNewNotification);
}

void handleAcknowledgementAck() {

}

void handleDelete() {

}

int HandleDiscoveryEvent(struct ble_gap_event *event, NotificationManager *notificationManager, Pinetime::System::SystemTask *systemTask) {
  uint8_t len = event->disc.length_data;
  const uint8_t *data = event->disc.data;
  int pos = 0;
  uint8_t size;
  uint8_t type;
  bool found = false;
  handleNotification(notificationManager, systemTask, 1);
  if (len < 7) {
    return 0;
  }
  while (pos < len) {
    size = data[pos];
    if (len < pos + size) {
      // data seems too short
      return 0;
    }
    type = data[pos + 1];
    if (type != 0xFF) {
      pos += size + 1;
      continue;
    }
    if (size < 5) {
      return 0; // message seems too small
    }
    if (data[pos + 2] != 0x59 || data[pos + 3] != 0x00) {
      return 0; // seems unrelated
    }
    found = true;
  }
  if (!found) return 0;

  uint32_t notifId = (data[pos + 2] << 24) | (data[pos + 3] << 16) | (data[pos + 4] << 8) | data[pos + 5];
  uint8_t notifType = data[pos + 6];
  uint16_t receiver = (data[pos + 7] << 8) | data[pos + 8];
  uint16_t room  = (data[pos + 9] << 8) | data[pos + 9];

  /*if (receiver != RECEIVER_ID) {
    return 0;
  }*/

  /*switch (notifType) {
    case 0x00:
      handleNotification(notificationManager, systemTask, room);
      break;
    case 0x03:
      handleAcknowledgementAck();
      break;
    case 0x04:
      handleDelete();
      break;
    default:
      break;
  }*/
  return 0;
}

void NimbleController::Init() {
  while (!ble_hs_synced()) {}

  ble_svc_gap_init();
  ble_svc_gatt_init();

  deviceInformationService.Init();
  currentTimeClient.Init();
  currentTimeService.Init();
  musicService.Init();
  navService.Init();
  anService.Init();
  dfuService.Init();
  batteryInformationService.Init();
  immediateAlertService.Init();
  heartRateService.Init();
  int res;
  res = ble_hs_util_ensure_addr(0);
  ASSERT(res == 0);
  res = ble_hs_id_infer_auto(0, &addrType);
  ASSERT(res == 0);
  res = ble_svc_gap_device_name_set(deviceName);
  ASSERT(res == 0);
  Pinetime::Controllers::Ble::BleAddress address;
  res = ble_hs_id_copy_addr(addrType, address.data(), nullptr);
  ASSERT(res == 0);
  bleController.AddressType((addrType == 0) ? Ble::AddressTypes::Public : Ble::AddressTypes::Random);
  bleController.Address(std::move(address));

  res = ble_gatts_start();
  ASSERT(res == 0);

  StartScan();
}

void NimbleController::StartScan() {
  /* set scan parameters */
  struct ble_gap_disc_params scan_params;
  scan_params.itvl = 500;
  scan_params.window = 250;
  scan_params.filter_policy = 0;
  scan_params.limited = 0;
  scan_params.passive = 1;
  scan_params.filter_duplicates = 1;
  /* performs discovery procedure; value of own_addr_type is hard-coded,
     because NRPA is used */
  ble_gap_disc(BLE_OWN_ADDR_RANDOM, 1000, &scan_params, GAPEventCallback, this);
}

void NimbleController::StartAdvertising() {
  if(bleController.IsConnected() || ble_gap_conn_active() || ble_gap_adv_active()) return;

  ble_svc_gap_device_name_set(deviceName);

  /* set adv parameters */
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;
  /* advertising payload is split into advertising data and advertising
     response, because all data cannot fit into single packet; name of device
     is sent as response to scan request */
  struct ble_hs_adv_fields rsp_fields;

  /* fill all fields and parameters with zeros */
  memset(&adv_params, 0, sizeof(adv_params));
  memset(&fields, 0, sizeof(fields));
  memset(&rsp_fields, 0, sizeof(rsp_fields));

  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  fields.flags = BLE_HS_ADV_F_DISC_GEN |
                 BLE_HS_ADV_F_BREDR_UNSUP;
//  fields.uuids128 = BLE_UUID128(BLE_UUID128_DECLARE(
//          0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
//          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff));
  fields.uuids128 = &dfuServiceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

  rsp_fields.name = (uint8_t *)deviceName;
  rsp_fields.name_len = strlen(deviceName);
  rsp_fields.name_is_complete = 1;

  ble_gap_adv_set_fields(&fields);
//  ASSERT(res == 0); // TODO this one sometimes fails with error 22 (notsync)

  ble_gap_adv_rsp_set_fields(&rsp_fields);
//  ASSERT(res == 0);

  ble_gap_adv_start(addrType, NULL, 180000,
                          &adv_params, GAPEventCallback, this);
//  ASSERT(res == 0);// TODO I've disabled these ASSERT as they sometime asserts and reset the mcu.
  // For now, the advertising is restarted as soon as it ends. There may be a race condition
  // that prevent the advertising from restarting reliably.
  // I remove the assert to prevent this uncesseray crash, but in the long term, the management of
  // the advertising should be improve (better error handling, and advertise for 3 minutes after
  // the application has been woken up, for example.
}

int NimbleController::OnGAPEvent(ble_gap_event *event) {
  switch (event->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
      NRF_LOG_INFO("Advertising event : BLE_GAP_EVENT_ADV_COMPLETE");
      NRF_LOG_INFO("advertise complete; reason=%dn status=%d", event->adv_complete.reason, event->connect.status);
      break;
    case BLE_GAP_EVENT_CONNECT: {
      NRF_LOG_INFO("Advertising event : BLE_GAP_EVENT_CONNECT");

      /* A new connection was established or a connection attempt failed. */
      NRF_LOG_INFO("connection %s; status=%d ", event->connect.status == 0 ? "established" : "failed",
                   event->connect.status);

      if (event->connect.status != 0) {
        /* Connection failed; resume advertising. */
        StartAdvertising();
        bleController.Disconnect();
      } else {
        bleController.Connect();
        systemTask.PushMessage(Pinetime::System::SystemTask::Messages::BleConnected);
        connectionHandle = event->connect.conn_handle;
        // Service discovery is deffered via systemtask
      }
    }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      NRF_LOG_INFO("Advertising event : BLE_GAP_EVENT_DISCONNECT");
      NRF_LOG_INFO("disconnect; reason=%d", event->disconnect.reason);

      /* Connection terminated; resume advertising. */
      currentTimeClient.Reset();
      alertNotificationClient.Reset();
      connectionHandle = BLE_HS_CONN_HANDLE_NONE;
      bleController.Disconnect();
      StartAdvertising();
      break;
    case BLE_GAP_EVENT_CONN_UPDATE:
      NRF_LOG_INFO("Advertising event : BLE_GAP_EVENT_CONN_UPDATE");
      /* The central has updated the connection parameters. */
      NRF_LOG_INFO("connection updated; status=%d ", event->conn_update.status);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      /* Encryption has been enabled or disabled for this connection. */
      NRF_LOG_INFO("encryption change event; status=%d ", event->enc_change.status);
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
      NRF_LOG_INFO("subscribe event; conn_handle=%d attr_handle=%d "
                        "reason=%d prevn=%d curn=%d previ=%d curi=???\n",
                  event->subscribe.conn_handle,
                  event->subscribe.attr_handle,
                  event->subscribe.reason,
                  event->subscribe.prev_notify,
                  event->subscribe.cur_notify,
                  event->subscribe.prev_indicate);
      return 0;
    case BLE_GAP_EVENT_MTU:
      NRF_LOG_INFO("mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                  event->mtu.conn_handle,
                  event->mtu.channel_id,
                  event->mtu.value);
      return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      /* We already have a bond with the peer, but it is attempting to
       * establish a new secure link.  This app sacrifices security for
       * convenience: just throw away the old bond and accept the new link.
       */

      /* Delete the old bond. */
      struct ble_gap_conn_desc desc;
      ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
      ble_store_util_delete_peer(&desc.peer_id_addr);

      /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
       * continue with the pairing operation.
       */
    }
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      /* Peer sent us a notification or indication. */
      size_t notifSize = OS_MBUF_PKTLEN(event->notify_rx.om);

      NRF_LOG_INFO("received %s; conn_handle=%d attr_handle=%d "
                   "attr_len=%d",
                   event->notify_rx.indication ?
                   "indication" :
                   "notification",
                   event->notify_rx.conn_handle,
                   event->notify_rx.attr_handle,
                   notifSize);

      alertNotificationClient.OnNotification(event);
      return 0;
    }
      /* Attribute data is contained in event->notify_rx.attr_data. */

    case BLE_GAP_EVENT_DISC: {
      NRF_LOG_INFO("advertisement discovered");
      return HandleDiscoveryEvent(event, &notificationManager, &systemTask);
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
      NRF_LOG_INFO("ble discovery complete, start again");
      StartScan();
      return 0;
    }
    default:
//      NRF_LOG_INFO("Advertising event : %d", event->type);
      break;
  }
  return 0;
}

void NimbleController::StartDiscovery() {
  serviceDiscovery.StartDiscovery(connectionHandle);
}


uint16_t NimbleController::connHandle() {
    return connectionHandle;
}

