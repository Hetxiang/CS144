#include "wrapping_integers.hh"
#include "debug.hh"

using namespace std;

Wrap32 Wrap32::wrap( uint64_t n, Wrap32 zero_point )
{
  // 将绝对序号 n 映射为相对于 zero_point 的 32 位序号（取低 32 位）
  uint32_t raw = static_cast<uint32_t>( zero_point.raw_value_ + static_cast<uint32_t>( n ) );
  return Wrap32 { raw };
}

uint64_t Wrap32::unwrap( Wrap32 zero_point, uint64_t checkpoint ) const
{
  // 在所有与当前 Wrap32 等价的绝对序号中，选择最接近 checkpoint 的那个
  const uint64_t wrap = 1ULL << 32;
  uint64_t base = static_cast<uint32_t>( raw_value_ - zero_point.raw_value_ ); // 差值模 2^32

  uint64_t k = checkpoint / wrap;
  const uint64_t max_k = UINT64_MAX / wrap; // 防止 k+1 时乘法溢出

  auto dist = []( uint64_t a, uint64_t b ) -> uint64_t { return a > b ? a - b : b - a; };

  uint64_t best = base + k * wrap; // 初始选择 k 值对应的绝对序号

  // 尝试 k-1 和 k+1，看是否更接近 checkpoint
  if ( k > 0 ) {
    uint64_t c = base + ( k - 1 ) * wrap;
    if ( dist( c, checkpoint ) < dist( best, checkpoint ) ) {
      best = c;
    }
  }
  if ( k < max_k ) {
    uint64_t c = base + ( k + 1 ) * wrap;
    if ( dist( c, checkpoint ) < dist( best, checkpoint ) ) {
      best = c;
    }
  }

  return best;
}
