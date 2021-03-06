// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/android/forwarder2/host_controller.h"

#include <string>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "tools/android/forwarder2/command.h"
#include "tools/android/forwarder2/forwarder.h"
#include "tools/android/forwarder2/socket.h"

namespace forwarder2 {

// static
scoped_ptr<HostController> HostController::Create(
    int device_port,
    int host_port,
    int adb_port,
    int exit_notifier_fd,
    const DeletionCallback& deletion_callback) {
  scoped_ptr<HostController> host_controller;
  scoped_ptr<PipeNotifier> delete_controller_notifier(new PipeNotifier());
  scoped_ptr<Socket> adb_control_socket(new Socket());
  adb_control_socket->AddEventFd(exit_notifier_fd);
  adb_control_socket->AddEventFd(delete_controller_notifier->receiver_fd());
  if (!adb_control_socket->ConnectTcp(std::string(), adb_port)) {
    LOG(ERROR) << "Could not connect HostController socket on port: "
               << adb_port;
    return host_controller.Pass();
  }
  // Send the command to the device start listening to the "device_forward_port"
  bool send_command_success = SendCommand(
      command::LISTEN, device_port, adb_control_socket.get());
  CHECK(send_command_success);
  int device_port_allocated;
  command::Type command;
  if (!ReadCommand(
          adb_control_socket.get(), &device_port_allocated, &command) ||
      command != command::BIND_SUCCESS) {
    LOG(ERROR) << "Device binding error using port " << device_port;
    return host_controller.Pass();
  }
  host_controller.reset(
      new HostController(
          device_port_allocated, host_port, adb_port, exit_notifier_fd,
          deletion_callback, adb_control_socket.Pass(),
          delete_controller_notifier.Pass()));
  return host_controller.Pass();
}

HostController::~HostController() {
  DCHECK(deletion_task_runner_->RunsTasksOnCurrentThread());
  delete_controller_notifier_->Notify();
  // Note that the Forwarder instance (that also received a delete notification)
  // might still be running on its own thread at this point. This is not a
  // problem since it will self-delete once the socket that it is operating on
  // is closed.
}

void HostController::Start() {
  thread_.Start();
  ReadNextCommandSoon();
}

HostController::HostController(
    int device_port,
    int host_port,
    int adb_port,
    int exit_notifier_fd,
    const DeletionCallback& deletion_callback,
    scoped_ptr<Socket> adb_control_socket,
    scoped_ptr<PipeNotifier> delete_controller_notifier)
    : device_port_(device_port),
      host_port_(host_port),
      adb_port_(adb_port),
      global_exit_notifier_fd_(exit_notifier_fd),
      deletion_callback_(deletion_callback),
      adb_control_socket_(adb_control_socket.Pass()),
      delete_controller_notifier_(delete_controller_notifier.Pass()),
      deletion_task_runner_(base::MessageLoopProxy::current()),
      thread_("HostControllerThread") {
}

void HostController::ReadNextCommandSoon() {
  thread_.message_loop_proxy()->PostTask(
      FROM_HERE,
      base::Bind(&HostController::ReadCommandOnInternalThread,
                 base::Unretained(this)));
}

void HostController::ReadCommandOnInternalThread() {
  if (!ReceivedCommand(command::ACCEPT_SUCCESS, adb_control_socket_.get())) {
    SelfDelete();
    return;
  }
  // Try to connect to host server.
  scoped_ptr<Socket> host_server_data_socket(CreateSocket());
  if (!host_server_data_socket->ConnectTcp(std::string(), host_port_)) {
    LOG(ERROR) << "Could not Connect HostServerData socket on port: "
               << host_port_;
    SendCommand(
        command::HOST_SERVER_ERROR, device_port_, adb_control_socket_.get());
    if (ReceivedCommand(command::ACK, adb_control_socket_.get())) {
      // It can continue if the host forwarder could not connect to the host
      // server but the device acknowledged that, so that the device could
      // re-try later.
      ReadNextCommandSoon();
      return;
    }
    SelfDelete();
    return;
  }
  SendCommand(
      command::HOST_SERVER_SUCCESS, device_port_, adb_control_socket_.get());
  StartForwarder(host_server_data_socket.Pass());
  ReadNextCommandSoon();
}

void HostController::StartForwarder(
    scoped_ptr<Socket> host_server_data_socket) {
  scoped_ptr<Socket> adb_data_socket(CreateSocket());
  if (!adb_data_socket->ConnectTcp("", adb_port_)) {
    LOG(ERROR) << "Could not connect AdbDataSocket on port: " << adb_port_;
    SelfDelete();
    return;
  }
  // Open the Adb data connection, and send a command with the
  // |device_forward_port| as a way for the device to identify the connection.
  SendCommand(command::DATA_CONNECTION, device_port_, adb_data_socket.get());

  // Check that the device received the new Adb Data Connection. Note that this
  // check is done through the |adb_control_socket_| that is handled in the
  // DeviceListener thread just after the call to WaitForAdbDataSocket().
  if (!ReceivedCommand(command::ADB_DATA_SOCKET_SUCCESS,
                       adb_control_socket_.get())) {
    LOG(ERROR) << "Device could not handle the new Adb Data Connection.";
    SelfDelete();
    return;
  }
  forwarder2::StartForwarder(
      host_server_data_socket.Pass(), adb_data_socket.Pass());
}

scoped_ptr<Socket> HostController::CreateSocket() {
  scoped_ptr<Socket> socket(new Socket());
  socket->AddEventFd(global_exit_notifier_fd_);
  socket->AddEventFd(delete_controller_notifier_->receiver_fd());
  return socket.Pass();
}

void HostController::SelfDelete() {
  scoped_ptr<HostController> self_deleter(this);
  deletion_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&HostController::SelfDeleteOnDeletionTaskRunner,
                 deletion_callback_, base::Passed(&self_deleter)));
  // Tell the device to delete its corresponding controller instance before we
  // self-delete.
  Socket socket;
  if (!socket.ConnectTcp("", adb_port_)) {
    LOG(ERROR) << "Could not connect to device on port " << adb_port_;
    return;
  }
  if (!SendCommand(command::UNLISTEN, device_port_, &socket)) {
    LOG(ERROR) << "Could not send unmap command for port " << device_port_;
    return;
  }
  if (!ReceivedCommand(command::UNLISTEN_SUCCESS, &socket)) {
    LOG(ERROR) << "Unamp command failed for port " << device_port_;
    return;
  }
}

// static
void HostController::SelfDeleteOnDeletionTaskRunner(
    const DeletionCallback& deletion_callback,
    scoped_ptr<HostController> controller) {
  deletion_callback.Run(controller.Pass());
}

}  // namespace forwarder2
