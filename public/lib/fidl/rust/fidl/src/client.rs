// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! An implementation of a client for a fidl interface.

use {ClientEnd, ServerEnd, EncodeBuf, DecodeBuf, MsgType, Error};

use tokio_core::reactor::Handle;
use tokio_fuchsia;
use futures::{Async, Future, Poll, future};
use futures::task::{self, Task};
use slab::Slab;
use std::{io, mem};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicUsize, Ordering};

/// A FIDL service.
///
/// Implementations of this trait can be used to manufacture instances of a FIDL service
/// and get metadata about a particular service.
pub trait FidlService: Sized {
    /// The type of the structure against which FIDL requests are made.
    /// Queries made against the proxy are sent to the paired `ServerEnd`.
    type Proxy;

    /// Create a new proxy from a `ClientEnd`.
    fn new_proxy(client_end: ClientEnd<Self>, handle: &Handle) -> Result<Self::Proxy, Error>;

    /// Create a new `Proxy`/`ServerEnd` pair.
    fn new_pair(handle: &Handle) -> Result<(Self::Proxy, ServerEnd<Self>), Error>;

    /// The name of the service.
    const NAME: &'static str;

    /// The version of the service.
    const VERSION: u32;
}

/// An enum reprenting either a resolved message interest or a task on which to alert
/// that a response message has arrived.
#[derive(Debug)]
enum MessageInterest {
    WillPoll,
    Waiting(Task),
    Received(DecodeBuf),
}

impl MessageInterest {
    /// Check if a message has been received.
    fn is_received(&self) -> bool {
        if let MessageInterest::Received(_) = *self {
            true
        } else {
            false
        }
    }

    fn unwrap_received(self) -> DecodeBuf {
        if let MessageInterest::Received(buf) = self {
            buf
        } else {
            panic!("expected received message")
        }
    }
}

/// A shared client channel which tracks expected and received responses
#[derive(Debug)]
struct ClientInner {
    channel: tokio_fuchsia::Channel,

    /// The number of `Some` entries in `message_interests`.
    /// This is used to prevent unnecessary locking.
    received_messages_count: AtomicUsize,

    /// A map of message interests to either `None` (no message received yet)
    /// or `Some(DecodeBuf)` when a message has been received.
    /// An interest is registered with `register_msg_interest` and deregistered
    /// by either receiving a message via a call to `poll_recv` or manually
    /// deregistering with `deregister_msg_interest`
    message_interests: Mutex<Slab<MessageInterest>>,
}

impl ClientInner {
    /// Registers interest in a response message.
    ///
    /// This function returns a `usize` ID which should be used to send a message
    /// via the channel. Responses are then received using `poll_recv`.
    fn register_msg_interest(&self) -> usize {
        self.message_interests.lock().unwrap().insert(
            MessageInterest::WillPoll) as usize
    }

    /// Check for receipt of a message with a given ID.
    fn poll_recv(&self, id: usize, task_to_register_opt: Option<&Task>) -> Poll<DecodeBuf, Error> {
        // TODO(cramertj) return errors if one has occured _ever_ in poll_recv, not just if
        // one happens on this call.

        // Look to see if there are messages available
        if self.received_messages_count.load(Ordering::SeqCst) > 0 {
            let mut message_interests = self.message_interests.lock().unwrap();

            // If a message was received for the ID in question,
            // remove the message interest entry and return the response.
            if message_interests.get(id).expect("Polled unregistered interest").is_received() {
                let buf = message_interests.remove(id).unwrap_received();
                self.received_messages_count.fetch_sub(1, Ordering::SeqCst);
                return Ok(Async::Ready(buf));
            }
        }

        // Receive messages from the channel until a message with the appropriate ID
        // is found, or the channel is exhausted.
        loop {
            let mut buf = DecodeBuf::new();
            match self.channel.recv_from(buf.get_mut_buf()) {
                Ok(()) => {},
                Err(e) => {
                    if e.kind() != io::ErrorKind::WouldBlock {
                        return Err(Error::ClientRead(e));
                    }

                    let mut message_interests = self.message_interests.lock().unwrap();

                    if message_interests.get(id)
                        .expect("Polled unregistered interst")
                        .is_received()
                    {
                        // If, by happy accident, we just raced to getting the result,
                        // then yay! Return success.
                        let buf = message_interests.remove(id).unwrap_received();
                        self.received_messages_count.fetch_sub(1, Ordering::SeqCst);
                        return Ok(Async::Ready(buf));
                    } else {
                        // Set the current task to be notified when a response arrives.
                        if let Some(task_to_register) = task_to_register_opt {
                            *message_interests.get_mut(id)
                                .expect("Polled unregistered interest") =
                                    MessageInterest::Waiting(task_to_register.clone());
                        }
                        return Ok(Async::NotReady);
                    }
                }
            }

            if let MsgType::Response = buf.decode_message_header().ok_or(Error::InvalidHeader)? {
                // TODO(cramertj) use TryFrom here after stabilization
                let recvd_id = buf.get_message_id() as usize;

                // If a message was received for the ID in question,
                // remove the message interest entry and return the response.
                if recvd_id == id {
                    self.message_interests.lock().unwrap().remove(id);
                    return Ok(Async::Ready(buf));
                }

                // Look for a message interest with the given ID.
                // If one is found, store the message so that it can be picked up later.
                let mut message_interests = self.message_interests.lock().unwrap();
                if let Some(entry) = message_interests.get_mut(recvd_id) {
                    let old_entry = mem::replace(entry, MessageInterest::Received(buf));
                    self.received_messages_count.fetch_add(1, Ordering::SeqCst);
                    if let MessageInterest::Waiting(task) = old_entry {
                        // Wake up the task to let them know a message has arrived.
                        task.notify();
                    }
                }
            }
        }
    }

    fn deregister_msg_interest(&self, id: usize) {
        if self.message_interests
               .lock().unwrap().remove(id)
               .is_received()
        {
            self.received_messages_count.fetch_sub(1, Ordering::SeqCst);
        }
    }
}

/// A FIDL client which can be used to send buffers and receive responses via a channel.
#[derive(Debug, Clone)]
pub struct Client {
    inner: Arc<ClientInner>,
}

/// A future representing the response to a FIDL message.
pub type SendMessageExpectResponseFuture = future::Either<
        future::FutureResult<DecodeBuf, Error>,
        MessageResponse>;

impl Client {
    /// Create a new client.
    pub fn new(channel: tokio_fuchsia::Channel, handle: &Handle) -> Client {
        // Unused for now-- left in public API to allow for future backwards
        // compatibility and to allow for future changes to spawn futures.
        let _ = handle;
        Client {
            inner: Arc::new(ClientInner {
                channel: channel,
                received_messages_count: AtomicUsize::new(0),
                message_interests: Mutex::new(Slab::new()),
            })
        }
    }

    /// Send a message without expecting a response.
    pub fn send_msg(&self, buf: &mut EncodeBuf) -> Result<(), Error> {
        let (out_buf, handles) = buf.get_mut_content();
        Ok(self.inner.channel.write(out_buf, handles).map_err(Error::ClientWrite)?)
    }

    /// Send a message and receive a response future.
    pub fn send_msg_expect_response(&self, buf: &mut EncodeBuf)
            -> SendMessageExpectResponseFuture
    {
        let id = self.inner.register_msg_interest();
        buf.set_message_id(id as u64);
        let (out_buf, handles) = buf.get_mut_content();
        if let Err(e) = self.inner.channel.write(out_buf, handles) {
            return future::Either::A(future::err(Error::ClientWrite(e)));
        }

        future::Either::B(MessageResponse {
            id: id,
            client: Some(self.inner.clone()),
            last_registered_task: None,
        })
    }
}

#[must_use]
/// A future which polls for the response to a client message.
#[derive(Debug)]
pub struct MessageResponse {
    id: usize,
    // `None` if the message response has been recieved
    client: Option<Arc<ClientInner>>,
    last_registered_task: Option<Task>,
}

impl Future for MessageResponse {
    type Item = DecodeBuf;
    type Error = Error;
    fn poll(&mut self) -> Poll<Self::Item, Self::Error> {
        let res;
        {
            let client = self.client.as_ref().ok_or(Error::PollAfterCompletion)?;

            let current_task_is_registered =
                self.last_registered_task
                    .as_ref()
                    .map_or(false, |task| task.will_notify_current());

            if !current_task_is_registered {
                let current = task::current();
                res = client.poll_recv(self.id, Some(&current));
                self.last_registered_task = Some(current);
            } else {
                res = client.poll_recv(self.id, None);
            }
        }

        // Drop the client reference if the response has been received
        if let Ok(Async::Ready(_)) = res {
            self.client.take();
        }

        res
    }
}

impl Drop for MessageResponse {
    fn drop(&mut self) {
        if let Some(ref client) = self.client {
            client.deregister_msg_interest(self.id)
        }
    }
}

#[cfg(test)]
mod tests {
    use zircon::{self, MessageBuf};
    use std::time::Duration;
    use tokio_fuchsia::Channel;
    use tokio_core::reactor::{Core, Timeout};
    use byteorder::{ByteOrder, LittleEndian};
    use super::*;

    #[test]
    fn client() {
        let mut core = Core::new().unwrap();
        let handle = core.handle();

        let (client_end, server_end) = zircon::Channel::create().unwrap();
        let client_end = Channel::from_channel(client_end, &handle).unwrap();
        let client = Client::new(client_end, &handle);

        let server = Channel::from_channel(server_end, &handle).unwrap();
        let mut buffer = MessageBuf::new();
        let receiver = server.recv_msg(&mut buffer).map(|(_chan, buf)| {
            let bytes = &[16, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0];
            println!("{:?}", buf.bytes());
            assert_eq!(bytes, buf.bytes());
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let rcv_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive message in time!");
        });
        let receiver = receiver.select(rcv_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let sender = Timeout::new(Duration::from_millis(100), &handle).unwrap().map(|()|{
            let mut req = EncodeBuf::new_request(42);
            client.send_msg(&mut req).unwrap();
        });

        let done = receiver.join(sender);
        core.run(done).unwrap();
    }

    #[test]
    fn client_with_response() {
        let mut core = Core::new().unwrap();
        let handle = core.handle();

        let (client_end, server_end) = zircon::Channel::create().unwrap();
        let client_end = Channel::from_channel(client_end, &handle).unwrap();
        let client = Client::new(client_end, &handle);

        let server = Channel::from_channel(server_end, &handle).unwrap();
        let mut buffer = MessageBuf::new();
        let receiver = server.recv_msg(&mut buffer).map(|(chan, buf)| {
            let bytes = &[24, 0, 0, 0, 1, 0, 0, 0, 42, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0];
            println!("{:?}", buf.bytes());
            assert_eq!(bytes, buf.bytes());
            let id = LittleEndian::read_u64(&buf.bytes()[16..24]);

            let mut response = EncodeBuf::new_response(42);
            response.set_message_id(id);
            let (out_buf, handles) = response.get_mut_content();
            let _ = chan.write(out_buf, handles);
        });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let rcv_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive message in time!");
        });
        let receiver = receiver.select(rcv_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let mut req = EncodeBuf::new_request_expecting_response(42);
        let sender = client.send_msg_expect_response(&mut req)
            .map_err(|e| {
                println!("error {:?}", e);
                io::Error::new(io::ErrorKind::Other, "fidl error")
            });

        // add a timeout to receiver so if test is broken it doesn't take forever
        let send_timeout = Timeout::new(Duration::from_millis(300), &handle).unwrap().map(|()| {
            panic!("did not receive response in time!");
        });
        let sender = sender.select(send_timeout).map(|(_,_)| ()).map_err(|(err,_)| err);

        let done = receiver.join(sender);
        core.run(done).unwrap();
    }
}
