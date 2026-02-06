#pragma once

#include "byte_stream.hh"
#include "tcp_receiver_message.hh"
#include "tcp_sender_message.hh"

#include <algorithm>
#include <deque>
#include <functional>

class TCPSender
{
public:
  /* Construct TCP sender with given default Retransmission Timeout and possible ISN */
  TCPSender( ByteStream&& input, Wrap32 isn, uint64_t initial_RTO_ms )
    : input_( std::move( input ) ), isn_( isn ), initial_RTO_ms_( initial_RTO_ms )
  {}

  /* Generate an empty TCPSenderMessage */
  TCPSenderMessage make_empty_message() const;

  /* Receive and process a TCPReceiverMessage from the peer's receiver */
  void receive( const TCPReceiverMessage& msg );

  /* Type of the `transmit` function that the push and tick methods can use to send messages */
  using TransmitFunction = std::function<void( const TCPSenderMessage& )>;

  /* Push bytes from the outbound stream */
  void push( const TransmitFunction& transmit );

  /* Time has passed by the given # of milliseconds since the last time the tick() method was called */
  void tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit );

  // Accessors
  uint64_t sequence_numbers_in_flight() const;  // How many sequence numbers are outstanding?
  uint64_t consecutive_retransmissions() const; // How many consecutive retransmissions have happened?
  const Writer& writer() const { return input_.writer(); }
  const Reader& reader() const { return input_.reader(); }
  Writer& writer() { return input_.writer(); }

private:
  Reader& reader() { return input_.reader(); }

  ByteStream input_;
  Wrap32 isn_;
  uint64_t initial_RTO_ms_;

  struct Seg
  {
    uint64_t abs_seqno {}; // 这个 segment 第一个序号的绝对序号（SYN 为 0）
    std::string payload {};
    bool SYN {};
    bool FIN {};
    bool is_probe{}; // 是零窗口探测（当时 remote advert window==0 且不是初始 SYN）
    size_t seq_len() const { return SYN + payload.size() + FIN; }
  };

  std::deque<Seg> outq_ {};     // outstanding segments（已发送未被确认）
  uint64_t next_seqno_abs_ {};  // 下一个可分配的绝对序号（0 对应 SYN）
  uint64_t bytes_in_flight_ {}; // 仍在飞行中的序列号数量（包括 SYN/FIN）

  // 重传计时器状态
  uint64_t rto_ms_ {};           // 当前 RTO
  uint64_t timer_ms_ {};         // 已过去的计时（自 timer 启动或重置）
  bool timer_running_ {};        // 计时器是否在运行
  unsigned consecutive_retx_ {}; // 连续重传次数

  // 最近报告的接收端窗口与最后 ack（绝对序号）
  uint16_t window_size_ {};
  uint64_t last_ack_abs_ {};
  bool fin_sent_ {};
};
