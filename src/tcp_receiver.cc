#include "tcp_receiver.hh"
#include "debug.hh"

using namespace std;

void TCPReceiver::receive( TCPSenderMessage message )
{
  // 收到 RST -> 标记流为 error
  if ( message.RST ) {
    // 标记整个流为 error
    reassembler_.reader().set_error();
    return;
  }

  // 如果还没有 ISN 且收到 SYN，则设置 ISN
  if ( not isn_.has_value() ) {
    if ( message.SYN ) {
      isn_ = message.seqno;
    } else {
      return; // 在未收到 SYN 之前忽略任何非 SYN 报文
    }
  }

  // 计算 payload 在流中的索引（以 0 为起点）
  uint64_t first_index = 0;
  if ( message.SYN ) { // SYN 占用一个序号，payload（如果有）从索引 0 开始
    first_index = 0;
  } else {
    // message.seqno 对应 payload 的第一个字节的序号
    // unwrap 返回相对于 ISN 的绝对序号 n（其中 n==1 表示第一个数据字节）
    const uint64_t checkpoint = 1 + reassembler_.writer().bytes_pushed();
    const uint64_t abs_seqno = message.seqno.unwrap( *isn_, checkpoint );
    // payload 的流索引为 abs_seqno - 1
    if ( abs_seqno > 0 ) {
      first_index = abs_seqno - 1;
    } else {
      return; // 如果出现异常情况（理论上不应发生），则忽略
    }
  }

  // 只有在有数据或 FIN 时才插入 Reassembler
  if ( not message.payload.empty() || message.FIN ) {
    reassembler_.insert( first_index, std::move( message.payload ), message.FIN );
  }
}

TCPReceiverMessage TCPReceiver::send() const
{
  TCPReceiverMessage out {};

  // RST 如果流有错误
  out.RST = reassembler_.reader().has_error();

  // 窗口大小是 writer 的可用容量，但不能超过 UINT16_MAX
  const uint64_t avail = reassembler_.writer().available_capacity();
  out.window_size = static_cast<uint16_t>( std::min<uint64_t>( avail, UINT16_MAX ) );

  // 如果没有 ISN，则 ackno 为空
  if ( not isn_.has_value() ) {
    out.ackno = std::nullopt;
    return out;
  }

  // 计算 ack（下一个期望的序号）：1（SYN） + 已推送字节数，如果 FIN 已被完全接收并写入则再 +1
  uint64_t ack_abs = 1 + reassembler_.writer().bytes_pushed() + ( reassembler_.writer().is_closed() ? 1 : 0 );

  out.ackno = Wrap32::wrap( ack_abs, *isn_ );
  return out;
}
