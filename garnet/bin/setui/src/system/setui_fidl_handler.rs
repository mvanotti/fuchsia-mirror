// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
  crate::switchboard::base::{
    SettingRequest, SettingResponseResult, SettingType, Switchboard, SystemLoginOverrideMode,
  },
  crate::switchboard::hanging_get_handler::{HangingGetHandler, Sender},
  crate::switchboard::switchboard_impl::SwitchboardImpl,
  fidl_fuchsia_settings::*,
  fidl_fuchsia_setui::*,
  fuchsia_async as fasync,
  futures::lock::Mutex,
  futures::prelude::*,
  parking_lot::RwLock,
  std::sync::Arc,
};

impl From<fidl_fuchsia_setui::LoginOverride> for SystemLoginOverrideMode {
  fn from(login_override: fidl_fuchsia_setui::LoginOverride) -> Self {
    match login_override {
      fidl_fuchsia_setui::LoginOverride::None => SystemLoginOverrideMode::None,
      fidl_fuchsia_setui::LoginOverride::AutologinGuest => SystemLoginOverrideMode::AutologinGuest,
      fidl_fuchsia_setui::LoginOverride::AuthProvider => SystemLoginOverrideMode::AuthProvider,
    }
  }
}

fn convert_login_override(
  login_override: fidl_fuchsia_settings::LoginOverride,
) -> fidl_fuchsia_setui::LoginOverride {
  match login_override {
    fidl_fuchsia_settings::LoginOverride::None => fidl_fuchsia_setui::LoginOverride::None,
    fidl_fuchsia_settings::LoginOverride::AutologinGuest => {
      fidl_fuchsia_setui::LoginOverride::AutologinGuest
    }
    fidl_fuchsia_settings::LoginOverride::AuthProvider => {
      fidl_fuchsia_setui::LoginOverride::AuthProvider
    }
  }
}

impl Sender<SystemSettings> for SetUiServiceWatchResponder {
  fn send_response(self, data: SystemSettings) {
    let mut mode = None;

    if let Some(login_override) = data.mode {
      mode = Some(convert_login_override(login_override));
    }

    self
      .send(&mut SettingsObject {
        setting_type: fidl_fuchsia_setui::SettingType::Account,
        data: SettingData::Account(fidl_fuchsia_setui::AccountSettings { mode: mode }),
      })
      .ok();
  }
}

pub fn spawn_setui_fidl_handler(
  switchboard_handle: Arc<RwLock<SwitchboardImpl>>,
  mut stream: SetUiServiceRequestStream,
) {
  let hanging_get_handler: Arc<
    Mutex<HangingGetHandler<SystemSettings, SetUiServiceWatchResponder>>,
  > = HangingGetHandler::create(switchboard_handle.clone(), SettingType::System);

  fasync::spawn(async move {
    while let Ok(Some(req)) = stream.try_next().await {
      #[allow(unreachable_patterns)]
      match req {
        SetUiServiceRequest::Mutate { setting_type: _, mutation, responder } => {
          if let Mutation::AccountMutationValue(mutation_info) = mutation {
            if let Some(operation) = mutation_info.operation {
              if operation == AccountOperation::SetLoginOverride {
                if let Some(login_override) = mutation_info.login_override {
                  set_login_override(
                    switchboard_handle.clone(),
                    SystemLoginOverrideMode::from(login_override),
                    responder,
                  );
                  continue;
                }
              }
            }
          }

          responder.send(&mut MutationResponse { return_code: ReturnCode::Failed }).ok();
        }
        SetUiServiceRequest::Watch { setting_type, responder } => {
          if setting_type != fidl_fuchsia_setui::SettingType::Account {
            continue;
          }
          let mut hanging_get_lock = hanging_get_handler.lock().await;
          hanging_get_lock.watch(responder).await;
        }
        _ => {}
      }
    }
  });
}

fn set_login_override(
  switchboard: Arc<RwLock<dyn Switchboard + Send + Sync>>,
  mode: SystemLoginOverrideMode,
  responder: SetUiServiceMutateResponder,
) {
  let (response_tx, response_rx) = futures::channel::oneshot::channel::<SettingResponseResult>();
  if switchboard
    .write()
    .request(SettingType::System, SettingRequest::SetLoginOverrideMode(mode), response_tx)
    .is_ok()
  {
    fasync::spawn(async move {
      // Return success if we get a Ok result from the
      // switchboard.
      if let Ok(Ok(_optional_response)) = response_rx.await {
        responder.send(&mut MutationResponse { return_code: ReturnCode::Ok }).ok();
      } else {
        responder.send(&mut MutationResponse { return_code: ReturnCode::Failed }).ok();
      }
    });
  } else {
    responder.send(&mut MutationResponse { return_code: ReturnCode::Failed }).ok();
  }
}
