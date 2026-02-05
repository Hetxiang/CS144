#include "reassembler.hh"
#include "debug.hh"

#include <algorithm>
#include <cstdint>

using namespace std;

void Reassembler::insert( uint64_t first_index, string data, bool is_last_substring )
{
  uint64_t end_index = first_index + data.size();

  // 标记 EOF
  if ( is_last_substring ) {
    eof_ = true;
    eof_index_ = end_index;
  }

  // 插入位于或超出 EOF 且已写到 EOF 时，直接关闭并返回
  if ( eof_ && first_index >= eof_index_ ) {
    if ( next_index_ >= eof_index_ ) {
      output_.writer().close();
    }
    return;
  }

  // 裁剪到可写窗口 [next_index_, next_index_ + capacity)
  const uint64_t window_end = next_index_ + output_.writer().available_capacity();
  if ( end_index <= next_index_ || first_index >= window_end ) {
    return;
  }

  // 裁剪已写前缀与窗口尾部及 EOF
  if ( first_index < next_index_ ) {
    data = data.substr( next_index_ - first_index );
    first_index = next_index_;
  }
  end_index = first_index + data.size();

  if ( end_index > window_end ) {
    data = data.substr( 0, window_end - first_index );
    end_index = window_end;
  }

  if ( eof_ && end_index > eof_index_ ) {
    data = data.substr( 0, eof_index_ - first_index );
    end_index = eof_index_;
  }

  if ( data.empty() ) {
    return;
  }

  // 合并与当前片段重叠或相邻的缓存区间
  uint64_t merge_start = first_index;
  uint64_t merge_end = end_index;

  // 找第一个可能重叠的段
  auto it_low = pending_.lower_bound( merge_start );
  if ( it_low != pending_.begin() ) {
    auto prev = std::prev( it_low );
    if ( prev->first + prev->second.size() >= merge_start ) {
      it_low = prev;
      merge_start = std::min( merge_start, it_low->first );
    }
  }

  // 扩展到所有重叠段
  auto it_high = it_low;
  while ( it_high != pending_.end() && it_high->first <= merge_end ) {
    merge_end = std::max( merge_end, it_high->first + it_high->second.size() );
    ++it_high;
  }

  // 构造合并数据
  std::string merged( merge_end - merge_start, '\0' );
  std::copy( data.begin(), data.end(), merged.begin() + ( first_index - merge_start ) );
  for ( auto it = it_low; it != it_high; ++it ) {
    const uint64_t off = it->first - merge_start;
    std::copy( it->second.begin(), it->second.end(), merged.begin() + off );
  }

  if ( it_low != it_high ) {
    pending_.erase( it_low, it_high );
  }
  pending_[merge_start] = std::move( merged );

  // 推进连续段
  while ( !pending_.empty() && pending_.begin()->first == next_index_ ) {
    auto& seg = pending_.begin()->second;
    output_.writer().push( seg );
    next_index_ += seg.size();
    pending_.erase( pending_.begin() );
  }

  if ( eof_ && next_index_ >= eof_index_ ) {
    output_.writer().close();
  }
}

// How many bytes are stored in the Reassembler itself?
// This function is for testing only; don't add extra state to support it.
uint64_t Reassembler::count_bytes_pending() const
{
  uint64_t total = 0;
  for ( const auto& p : pending_ ) {
    total += p.second.size();
  }
  return total;
}
