// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::terminal::Terminal,
    anyhow::{Context as _, Error},
    fidl::endpoints::{RequestStream, ServerEnd},
    fidl_fuchsia_hardware_pty::DeviceMarker,
    fidl_fuchsia_virtualconsole::{SessionManagerRequest, SessionManagerRequestStream},
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::{io::AsyncReadExt, prelude::*},
    pty::Pty,
    std::{cell::RefCell, fs::File, rc::Rc},
    term_model::{ansi::Processor, event::EventListener},
};

const BYTE_BUFFER_MAX_SIZE: usize = 128;

pub trait SessionManagerClient: 'static + Clone {
    type Listener;

    fn create_terminal(
        &self,
        id: u32,
        title: String,
        make_active: bool,
        pty_fd: File,
    ) -> Result<Terminal<Self::Listener>, Error>;
    fn request_update(&self, id: u32);
}

pub struct SessionManager {
    keep_log_visible: bool,
    first_session_id: u32,
    next_session_id: Rc<RefCell<u32>>,
}

impl SessionManager {
    pub fn new(keep_log_visible: bool, first_session_id: u32) -> Self {
        let next_session_id = Rc::new(RefCell::new(first_session_id));

        Self { keep_log_visible, first_session_id, next_session_id }
    }

    pub fn bind<T: SessionManagerClient>(&mut self, client: &T, channel: fasync::Channel)
    where
        <T as SessionManagerClient>::Listener: EventListener,
    {
        let keep_log_visible = self.keep_log_visible;
        let first_session_id = self.first_session_id;
        let next_session_id = Rc::clone(&self.next_session_id);
        let client = client.clone();

        fasync::Task::local(
            async move {
                let mut stream = SessionManagerRequestStream::from_channel(channel);
                while let Some(request) = stream.try_next().await? {
                    match request {
                        SessionManagerRequest::CreateSession { session, responder } => {
                            let id = {
                                let mut next_session_id = next_session_id.borrow_mut();
                                let id = *next_session_id;
                                *next_session_id += 1;
                                id
                            };
                            let make_active = !keep_log_visible && id == first_session_id;
                            match Self::create_session(session, &client, id, make_active).await {
                                Ok(_) => {
                                    responder
                                        .send(zx::Status::OK.into_raw())
                                        .context("error sending response")?;
                                }
                                _ => {
                                    responder
                                        .send(zx::Status::INTERNAL.into_raw())
                                        .context("error sending response")?;
                                }
                            }
                        }
                        SessionManagerRequest::HasPrimaryConnected { responder } => {
                            responder
                                .send(Self::has_primary_connected())
                                .context("error sending response")?;
                        }
                    }
                }
                Ok(())
            }
            .unwrap_or_else(|e: anyhow::Error| eprintln!("{:?}", e)),
        )
        .detach();
    }

    async fn create_session<T: SessionManagerClient>(
        session: ServerEnd<DeviceMarker>,
        client: &T,
        id: u32,
        make_active: bool,
    ) -> Result<(), Error>
    where
        <T as SessionManagerClient>::Listener: EventListener,
    {
        let client = client.clone();
        let pty = Pty::with_server_end(session).await.expect("failed to create PTY");
        let fd = pty.try_clone_fd().expect("unable to clone PTY fd");
        let terminal = client
            .create_terminal(id, String::new(), make_active, fd)
            .expect("failed to create terminal");
        let term = terminal.clone_term();
        let read_fd = pty.try_clone_fd().expect("unable to clone PTY write fd");

        fasync::Task::local(async move {
            let mut evented_fd = unsafe {
                // EventedFd::new() is unsafe because it can't guarantee the lifetime of
                // the file descriptor passed to it exceeds the lifetime of the EventedFd.
                // Since we're cloning the file when passing it in, the EventedFd
                // effectively owns that file descriptor and thus controls it's lifetime.
                fasync::net::EventedFd::new(read_fd).expect("failed to create evented_fd")
            };

            let mut write_fd = pty.try_clone_fd().expect("unable to clone PTY write fd");
            let mut parser = Processor::new();

            let mut read_buf = [0u8; BYTE_BUFFER_MAX_SIZE];
            loop {
                let result = evented_fd.read(&mut read_buf).await;
                let read_count = result.unwrap_or_else(|e: std::io::Error| {
                    println!("vc: failed to read bytes, dropping current message: {:?}", e);
                    0
                });
                let mut term = term.borrow_mut();
                if read_count > 0 {
                    for byte in &read_buf[0..read_count] {
                        parser.advance(&mut *term, *byte, &mut write_fd);
                    }
                    client.request_update(id);
                }
            }
        })
        .detach();

        Ok(())
    }

    fn has_primary_connected() -> bool {
        // TODO(fxb/73628): Return correct state when exposed by Carnelian.
        false
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::colors::ColorScheme,
        fuchsia_async as fasync,
        term_model::event::{Event, EventListener},
    };

    #[derive(Default)]
    struct TestListener;

    impl EventListener for TestListener {
        fn send_event(&self, _event: Event) {}
    }

    #[derive(Default, Clone)]
    struct TestSessionManagerClient;

    impl SessionManagerClient for TestSessionManagerClient {
        type Listener = TestListener;

        fn create_terminal(
            &self,
            _id: u32,
            title: String,
            _make_active: bool,
            pty_fd: File,
        ) -> Result<Terminal<Self::Listener>, Error> {
            Ok(Terminal::new(
                TestListener::default(),
                title,
                ColorScheme::default(),
                1024,
                Some(pty_fd),
            ))
        }
        fn request_update(&self, _id: u32) {}
    }

    #[fasync::run_singlethreaded(test)]
    async fn can_create_session() -> Result<(), Error> {
        let client = TestSessionManagerClient::default();
        let (_, server_end) = fidl::endpoints::create_endpoints()?;
        let _ = SessionManager::create_session(server_end, &client, 0, false).await?;
        Ok(())
    }
}
