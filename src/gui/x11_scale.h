// ES40 emulator.
#ifndef ES40_X11_SCALE_H
#define ES40_X11_SCALE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace x11_scale_detail
{
struct rectangle
{
  unsigned x, y, width, height;
};

inline rectangle fit(unsigned sw, unsigned sh, unsigned tw, unsigned th)
{
  unsigned width = tw, height = th;
  if(std::uint64_t(sw) * th > std::uint64_t(sh) * tw)
    height = (std::max)(1u, unsigned(std::uint64_t(sh) * tw / sw));
  else
    width = (std::max)(1u, unsigned(std::uint64_t(sw) * th / sh));
  return { (tw - width) / 2, (th - height) / 2, width, height };
}

struct sample
{
  unsigned first, second, fraction;
};

inline sample linear_axis(unsigned position, unsigned source_size, unsigned target_size)
{
  const double source = (std::max)(0.0, (std::min)(double(source_size) - 1,
      (position + 0.5) * source_size / target_size - 0.5));
  const unsigned first = unsigned(source);
  return { first, (std::min)(first + 1, source_size - 1),
           unsigned((source - first) * 256 + 0.5) };
}

inline std::uint32_t mix(std::uint32_t first, std::uint32_t second, unsigned fraction)
{
  const unsigned inverse = 256 - fraction;
  const std::uint32_t rb = (((first & 0x00ff00ff) * inverse +
      (second & 0x00ff00ff) * fraction + 0x00800080) >> 8) & 0x00ff00ff;
  const std::uint32_t g = (((first & 0x0000ff00) * inverse +
      (second & 0x0000ff00) * fraction + 0x00008000) >> 8) & 0x0000ff00;
  return rb | g;
}

inline unsigned nearest_axis(unsigned position, unsigned source_size, unsigned target_size)
{
  return unsigned((std::uint64_t(position) * source_size + source_size / 2) / target_size);
}
}

template<class StorePixel>
bool x11_compose_frame(const std::uint32_t* pixels, unsigned source_width,
    unsigned source_height, unsigned target_width, unsigned target_height,
    bool linear, StorePixel store)
{
  if(!pixels || !source_width || !source_height || !target_width || !target_height ||
     source_width > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint32_t) / source_height)
    return false;
  const auto area = x11_scale_detail::fit(source_width, source_height,
                                         target_width, target_height);
  for(unsigned y = 0; y < target_height; ++y)
  {
    const bool inside_y = y >= area.y && y - area.y < area.height;
    x11_scale_detail::sample vertical = {};
    unsigned nearest_y = 0;
    if(inside_y)
    {
      if(linear) vertical = x11_scale_detail::linear_axis(y - area.y, source_height, area.height);
      else nearest_y = x11_scale_detail::nearest_axis(y - area.y, source_height, area.height);
    }
    for(unsigned x = 0; x < target_width; ++x)
    {
      std::uint32_t pixel = 0;
      if(inside_y && x >= area.x && x - area.x < area.width)
      {
        if(linear)
        {
          const auto horizontal = x11_scale_detail::linear_axis(x - area.x, source_width, area.width);
          const auto* top = pixels + std::size_t(vertical.first) * source_width;
          const auto* bottom = pixels + std::size_t(vertical.second) * source_width;
          const auto a = x11_scale_detail::mix(top[horizontal.first], top[horizontal.second], horizontal.fraction);
          const auto b = x11_scale_detail::mix(bottom[horizontal.first], bottom[horizontal.second], horizontal.fraction);
          pixel = x11_scale_detail::mix(a, b, vertical.fraction);
        }
        else
        {
          const unsigned source_x = x11_scale_detail::nearest_axis(x - area.x, source_width, area.width);
          pixel = pixels[std::size_t(nearest_y) * source_width + source_x] & 0x00ffffff;
        }
      }
      store(x, y, pixel);
    }
  }
  return true;
}

#endif
