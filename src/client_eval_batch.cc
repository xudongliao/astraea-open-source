#include <getopt.h>
#include <signal.h>
#include <stdio.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "address.hh"
#include "child_process.hh"
#include "common.hh"
#include "current_time.hh"
#include "deepcc_socket.hh"
#include "exception.hh"
#include "filesystem.hh"
#include "ipc_socket.hh"
#include "json.hpp"
#include "logging.hh"
#include "pid.hh"
#include "poller.hh"
#include "serialization.hh"
#include "socket.hh"
#include "system_runner.hh"
#include "tcp_info.hh"

using namespace std;
using namespace std::literals;
using clock_type = std::chrono::high_resolution_clock;
using namespace PollerShortNames;
typedef DeepCCSocket::TCPInfoRequestType RequestType;

// short name
using json = nlohmann::json;

// send_traffic should be atomic
std::atomic<bool> send_traffic(true);
int global_flow_id = 0;
std::unique_ptr<IPCSocket> inference_server = nullptr;

Address inference_server_addr;
std::chrono::_V2::system_clock::time_point ts_now = clock_type::now();
std::unique_ptr<std::ofstream> perf_log;

/* define message type */
enum class MessageType { INIT = 0, START = 1, END = 2, ALIVE = 3, OBSERVE = 4 };

template <typename E>
constexpr typename std::underlying_type<E>::type to_underlying(E e) noexcept {
  return static_cast<typename std::underlying_type<E>::type>(e);
}

/* algorithm name */
const char* ALG = "Astraea";

void unix_send_message(std::unique_ptr<IPCSocket>& ipc_sock,
                       const MessageType& type, const json& state,
                       const int observer_id = -1, const int step = -1) {
  json message;
  if (!state.empty()) {
    message["state"] = state;
  }
  // message["state"] = state;
  message["flow_id"] = global_flow_id;
  if (type == MessageType::OBSERVE) {
    message["type"] = to_underlying(MessageType::OBSERVE);
    message["observer"] = observer_id;
    message["step"] = step;
  } else {
    // we just need to copy the type
    message["type"] = to_underlying(type);
  }

  uint16_t len = message.dump().length();
  if (ipc_sock) {
    ipc_sock->write(put_field(len) + message.dump());
  }
}

std::string unix_recv_message(std::unique_ptr<IPCSocket>& ipc) {
  auto header = ipc->read_exactly(2);
  auto data_len = get_uint16(header.data());
  auto data = ipc->read_exactly(data_len);
  return data;
}

void signal_handler(int sig) {
  if (sig == SIGINT or sig == SIGKILL or sig == SIGTERM) {
    LOG(INFO) << "Caught signal, Client " << global_flow_id << " exiting...";
    // first disable read from fd
    // disable write to IPC
    send_traffic = false;
    // terminate pyhelper
    // close iperf
    if (perf_log) {
      perf_log->close();
    }
    if (inference_server) {
      unix_send_message(inference_server, MessageType::END, json());
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    exit(1);
  }
}

void do_congestion_control(DeepCCSocket& sock,
                           std::unique_ptr<IPCSocket>& ipc_sock) {
  auto state = sock.get_tcp_deepcc_info_json(RequestType::REQUEST_ACTION);
  LOG(TRACE) << "Client " << global_flow_id << " send state: " << state.dump();
  unix_send_message(ipc_sock, MessageType::ALIVE, state);
  // set timestamp
  ts_now = clock_type::now();
  // wait for action
  auto data = unix_recv_message(ipc_sock);
  int cwnd = 0;
  try {
    cwnd = json::parse(data).at("cwnd");
  } catch (const std::exception& e) {
    LOG(WARNING) << "Client " << global_flow_id
                 << " failed to parse action: " << data;
    return;
  }
  sock.set_tcp_cwnd(cwnd);
  auto elapsed = clock_type::now() - ts_now;
  LOG(DEBUG)
      << "Client " << global_flow_id << " GET cwnd: " << cwnd
      << ", elapsed time is "
      << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
      << "us";
  if (perf_log) {
    unsigned int srtt = state["srtt_us"];
    // change srtt to us
    srtt = srtt >> 3;
    *perf_log << state["min_rtt"] << "\t" << state["avg_urtt"] << "\t"
              << state["cnt"] << "\t" << srtt << "\t" << state["avg_thr"]
              << "\t" << state["thr_cnt"] << "\t" << state["pacing_rate"]
              << "\t" << state["loss_bytes"] << "\t" << state["packets_out"]
              << "\t" << state["retrans_out"] << "\t"
              << state["max_packets_out"] << "\t" << state["cwnd"] << "\t"
              << cwnd << endl;
  }
}

void control_thread(DeepCCSocket& sock, std::unique_ptr<IPCSocket>& ipc,
                    const std::chrono::milliseconds interval) {
  // start regular congestion control parttern
  auto when_started = clock_type::now();
  auto target_time = when_started + interval;
  while (send_traffic.load()) {
    do_congestion_control(sock, ipc);
    std::this_thread::sleep_until(target_time);
    target_time += interval;
  }
}

void data_thread(TCPSocket& sock) {
  string data(BUFSIZ, 'a');
  while (send_traffic.load()) {
    sock.write(data, true);
  }
  LOG(INFO) << "Data thread exits";
}

void usage_error(const string& program_name) {
  cerr << "Usage: " << program_name << " [OPTION]... [COMMAND]" << endl;
  cerr << endl;
  cerr << "Options = --ip=IP_ADDR --port=PORT --cong=ALGORITHM"
          "--interval=INTERVAL (Milliseconds) --id=None --perf-log=None"
       << endl;
  cerr << endl;
  cerr << "Default congestion control algorithms for incoming TCP is CUBIC; "
       << endl
       << "Default control interval is 10ms; " << endl
       << "Default flow id is None; " << endl;

  throw runtime_error("invalid arguments");
}

int main(int argc, char** argv) {
  /* register signal handler */
  signal(SIGTERM, signal_handler);
  signal(SIGKILL, signal_handler);
  signal(SIGINT, signal_handler);
  /* ignore SIGPIPE generated by Socket write */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    throw runtime_error("signal: failed to ignore SIGPIPE");
  }

  if (argc < 1) {
    usage_error(argv[0]);
  }
  const option command_line_options[] = {
      {"ip", required_argument, nullptr, 'a'},
      {"port", required_argument, nullptr, 'p'},
      {"cong", optional_argument, nullptr, 'c'},
      {"interval", optional_argument, nullptr, 't'},
      {"id", optional_argument, nullptr, 'f'},
      {"perf-log", optional_argument, nullptr, 'l'},
      {0, 0, nullptr, 0}};

  /* use RL inference or not */
  bool use_RL = false;
  string ip, service, pyhelper, model, cong_ctl, interval, id, perf_log_path;
  while (true) {
    const int opt = getopt_long(argc, argv, "", command_line_options, nullptr);
    if (opt == -1) { /* end of options */
      break;
    }
    switch (opt) {
    case 'a':
      ip = optarg;
      break;
    case 'c':
      cong_ctl = optarg;
      break;
    case 'f':
      id = optarg;
      break;
    case 'l':
      perf_log_path = optarg;
      break;
    case 'p':
      service = optarg;
      break;
    case 't':
      interval = optarg;
      break;
    case '?':
      usage_error(argv[0]);
      break;
    default:
      throw runtime_error("getopt_long: unexpected return value " +
                          to_string(opt));
    }
  }

  if (optind > argc) {
    usage_error(argv[0]);
  }

  /* assign flow_id */
  if (not id.empty()) {
    global_flow_id = stoi(id);
    LOG(INFO) << "Flow id: " << global_flow_id;
  }

  std::chrono::milliseconds control_interval(20ms);
  if (cong_ctl == "astraea") {
    /* IPC and control interval */
    IPCSocket ipcsock;
    ipcsock.set_reuseaddr();

    if (not interval.empty()) {
      control_interval = std::move(std::chrono::milliseconds(stoi(interval)));
    }
    inference_server = make_unique<IPCSocket>(std::move(ipcsock));
    inference_server->connect("/tmp/astraea.sock");
    // send initial message
    json init_message;
    unix_send_message(inference_server, MessageType::START, init_message);
    LOG(INFO) << "Sent init message to inference server ...";
    auto data = unix_recv_message(inference_server);
    json reply = json::parse(data);
    global_flow_id = reply["flow_id"];
    LOG(INFO) << "Client " << global_flow_id
              << " IPC with env has been established, control interval is "
              << control_interval.count() << "ms";
    /* has checked all things, we can use RL */
    use_RL = true;
  }

  /* default CC is cubic */
  if (cong_ctl.empty()) {
    cong_ctl = "cubic";
  }

  /* start TCP flow */
  int port = stoi(service);
  // init server addr
  Address address(ip, port);
  /* set reuse_addr */
  DeepCCSocket client;
  client.set_reuseaddr();
  client.connect(address);

  client.set_congestion_control(cong_ctl);
  client.set_nodelay();
  LOG(DEBUG) << "Client " << global_flow_id << " set congestion control to "
             << cong_ctl;
  /* !! should be set after socket connected */
  int enable_deepcc = 2;
  client.enable_deepcc(enable_deepcc);
  LOG(DEBUG) << "Client " << global_flow_id << " "
             << "enables deepCC plugin: " << enable_deepcc;

  /* setup performance log */
  if (not perf_log_path.empty()) {
    perf_log.reset(new std::ofstream(perf_log_path));
    if (not perf_log->good()) {
      throw runtime_error(perf_log_path + ": error opening for writing");
    }
    // write header
    *perf_log << "min_rtt\t"
              << "avg_urtt\t"
              << "cnt\t"
              << "srtt_us\t"
              << "avg_thr\t"
              << "thr_cnt\t"
              << "pacing_rate\t"
              << "loss_bytes\t"
              << "packets_out\t"
              << "retrans_out\t"
              << "max_packets_out\t"
              << "CWND in Kernel\t"
              << "CWND to Assign" << endl;
  }
  /* start data thread and control thread */
  thread ct;
  if (use_RL and inference_server != nullptr) {
    ct = thread(control_thread, std::ref(client), std::ref(inference_server),
                control_interval);
    LOG(DEBUG) << "Client " << global_flow_id << " Started control thread ... ";
  }
  thread dt(data_thread, std::ref(client));
  LOG(INFO) << "Client " << global_flow_id << " is sending data ... ";

  /* wait for finish */
  dt.join();
  ct.join();
  // LOG(INFO) << "Joined data thread, to exiting ... sleep for a while";
}
