#include "tcp_sender.hh"
#include "debug.hh"
#include "tcp_config.hh"

using namespace std;

// How many sequence numbers are outstanding?
uint64_t TCPSender::sequence_numbers_in_flight() const
{
  return bytes_in_flight_;
}

// How many consecutive retransmissions have happened?
uint64_t TCPSender::consecutive_retransmissions() const
{
  return consecutive_retx_;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  // 有效窗口：当对端通告窗为 0 时，push 方法应当“假装”窗口为 1
  uint64_t effective_win = ( window_size_ == 0 ) ? 1 : window_size_;

  // 一直尝试填充窗口
  while ( effective_win > bytes_in_flight_ ) {
    uint64_t avail = effective_win - bytes_in_flight_;

    bool syn = ( next_seqno_abs_ == 0 );

    // 计算 payload 在本地 buffer 中的起始偏移
    uint64_t data_index = 0;
    if ( next_seqno_abs_ == 0 ) {
      data_index = 0; // SYN + payload 的 payload 从 buffer 开始
    } else {
      // 数据字节的第一个绝对序号是 1
      data_index = ( next_seqno_abs_ - 1 ) - reader().bytes_popped();
    }

    auto buf = reader().peek();
    size_t buf_avail = 0;
    if ( data_index < buf.size() ) {
      buf_avail = buf.size() - static_cast<size_t>( data_index );
    }

    // 可用于 payload 的数量（先不考虑 FIN）
    size_t max_payload = static_cast<size_t>( std::min<uint64_t>( buf_avail, std::min<uint64_t>( TCPConfig::MAX_PAYLOAD_SIZE, avail - ( syn ? 1 : 0 ) ) ) );

    bool fin = false;
    // 如果之前已经发送过 FIN，后续不要再次发送 FIN
    const bool fin_already_sent = fin_sent_ || std::any_of( outq_.begin(), outq_.end(), []( const Seg& s ) { return s.FIN; } );

    // 决定是否附带 FIN：需要流已关闭并且此次 segment 包含了所有剩余数据，且之前未发送过 FIN
    if ( writer().is_closed() && not fin_already_sent ) {
      // 如果没有数据且能放下 FIN，或者发送的 payload 刚好覆盖了所有 buffer 的剩余数据，则可附带 FIN
      if ( max_payload == 0 ) {
        if ( avail >= ( syn ? 1 : 0 ) + 1 ) {
          fin = true;
        }
      } else {
        if ( static_cast<size_t>( data_index ) + max_payload == buf.size() && avail >= ( syn ? 1 : 0 ) + static_cast<uint64_t>( max_payload ) + 1 ) {
          fin = true;
        }
      }
    }

    // 如果既没有 SYN、也没有 payload、也没有 FIN，则什么都不发了
    if ( !syn && max_payload == 0 && !fin ) {
      break;
    }

    // 限制消息中的 payload 长度
    size_t payload_len = max_payload;

    TCPSenderMessage msg{};
    msg.seqno = Wrap32::wrap( next_seqno_abs_, isn_ );
    msg.SYN = syn;
    msg.FIN = fin;
    msg.RST = writer().has_error();
    if ( payload_len ) {
      msg.payload = std::string( buf.substr( static_cast<size_t>( data_index ), payload_len ) );
    }

    // 发送出去
    transmit( msg );

    // 如果这段占据序列号（SYN/payload/FIN），就把它加入 outstanding 队列
    if ( msg.sequence_length() ) {
      Seg s;
      s.abs_seqno = next_seqno_abs_;
      s.SYN = msg.SYN;
      s.FIN = msg.FIN;
      s.payload = msg.payload;
      // 如果发送时对端窗口为 0 且不是初始 SYN，则这是零窗口探测
      s.is_probe = ( window_size_ == 0 && s.abs_seqno != 0 );

      outq_.push_back( std::move( s ) );
      bytes_in_flight_ += msg.sequence_length();
      next_seqno_abs_ += msg.sequence_length();

      // 如果发送了 FIN，记住此事实，避免以后重复发送 FIN
      if ( msg.FIN ) {
        fin_sent_ = true;
      }

      // 如果计时器未运行，启动它
      if ( not timer_running_ ) {
        timer_running_ = true;
        timer_ms_ = 0;
        rto_ms_ = initial_RTO_ms_;
      }
    }

    // 在窗口中已发满则停止
    if ( bytes_in_flight_ >= effective_win ) {
      break;
    }
  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  TCPSenderMessage msg{};
  msg.seqno = Wrap32::wrap( next_seqno_abs_, isn_ );
  msg.RST = writer().has_error();
  return msg;
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  // RST -> 标记流为 error
  if ( msg.RST ) {
    writer().set_error();
    return;
  }

  // 更新窗口大小（无论 ack 是否有值）
  window_size_ = msg.window_size;

  if ( not msg.ackno.has_value() ) {
    return;
  }

  // unwrap ack 到绝对序号
  const uint64_t ack_abs = msg.ackno.value().unwrap( isn_, next_seqno_abs_ );

  // 如果 ack 超出了我们知道的下一个序号，则忽略（不修改计时器）
  if ( ack_abs > next_seqno_abs_ ) {
    return;
  }

  // 如果 ack 没有进展（重复 ack 或旧 ack），忽略
  if ( ack_abs <= last_ack_abs_ ) {
    return;
  }

  // ack 确认了新的数据
  last_ack_abs_ = ack_abs;

  // 移除所有被完全确认的 outstanding segments
  while ( not outq_.empty() ) {
    const Seg& s = outq_.front();
    if ( s.abs_seqno + s.seq_len() <= ack_abs ) {
      bytes_in_flight_ -= s.seq_len();
      outq_.pop_front();
    } else {
      break;
    }
  }

  // 将被 ack 的数据从输入流中移除（注意 ack_abs 中 0 表示 SYN）
  uint64_t acked_data = ( ack_abs > 0 ? ack_abs - 1 : 0 );
  uint64_t already_popped = reader().bytes_popped();
  if ( acked_data > already_popped ) {
    uint64_t to_pop = acked_data - already_popped;
    uint64_t buffered = reader().bytes_buffered();
    if ( to_pop > buffered ) {
      to_pop = buffered; // 保守处理
    }
    if ( to_pop ) {
      reader().pop( to_pop );
    }
  }

  // 重置 RTO 并重启/停止计时器
  rto_ms_ = initial_RTO_ms_;
  consecutive_retx_ = 0;
  if ( outq_.empty() ) {
    timer_running_ = false;
    timer_ms_ = 0;
  } else {
    timer_running_ = true;
    timer_ms_ = 0;
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  if ( not timer_running_ ) {
    return;
  }

  timer_ms_ += ms_since_last_tick;

  if ( timer_ms_ < rto_ms_ ) {
    return;
  }

  // 计时器到期：重传最早的 outstanding segment
  if ( outq_.empty() ) {
    timer_running_ = false;
    timer_ms_ = 0;
    return;
  }

  const Seg& s = outq_.front();
  TCPSenderMessage msg{};
  msg.seqno = Wrap32::wrap( s.abs_seqno, isn_ );
  msg.SYN = s.SYN;
  msg.FIN = s.FIN;
  msg.payload = s.payload;

  transmit( msg );

  // 如果重传的是占用序列号的 segment（SYN/payload/FIN），并且不是零窗口探测（零窗口探测不加倍 RTO），更新连续重传次数并倍增 RTO
  if ( s.seq_len() > 0 && !s.is_probe ) {
    ++consecutive_retx_;
    rto_ms_ = rto_ms_ * 2;
  }

  // 重新开始计时
  timer_ms_ = 0;
  timer_running_ = true;
}
