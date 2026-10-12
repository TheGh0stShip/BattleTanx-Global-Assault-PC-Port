#include "btga/world.hpp"

#include <array>
#include <stdexcept>

namespace btga {
namespace {
std::uint16_t u16(std::span<const std::uint8_t> b,std::size_t o){return (std::uint16_t(b[o])<<8)|b[o+1];}
std::int16_t s16(std::span<const std::uint8_t> b,std::size_t o){return static_cast<std::int16_t>(u16(b,o));}
std::uint32_t u32(std::span<const std::uint8_t> b,std::size_t o){return (std::uint32_t(b[o])<<24)|(std::uint32_t(b[o+1])<<16)|(std::uint32_t(b[o+2])<<8)|b[o+3];}
}
World parse_world(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<32) throw std::runtime_error("world is shorter than its offset header");
    std::array<std::uint32_t,8> offsets{};
    for(std::size_t i=0;i<offsets.size();++i) offsets[i]=u32(bytes,i*4);
    if(offsets.front()!=32||offsets.back()!=bytes.size()) throw std::runtime_error("world offsets do not cover payload");
    for(std::size_t i=1;i<offsets.size();++i) if(offsets[i]<offsets[i-1]) throw std::runtime_error("world offsets are not monotonic");
    if(offsets[1]-offsets[0]!=4||(offsets[2]-offsets[1])%16||(offsets[3]-offsets[2])%12)
        throw std::runtime_error("world fixed-record sections are malformed");
    const auto group_count=u32(bytes,offsets[0]);
    if(group_count!=(offsets[2]-offsets[1])/16) throw std::runtime_error("world group count disagrees with section");
    World world;
    for(std::size_t o=offsets[1];o<offsets[2];o+=16) world.groups.push_back({u16(bytes,o),u16(bytes,o+2),
        s16(bytes,o+4),s16(bytes,o+6),s16(bytes,o+8),s16(bytes,o+10),s16(bytes,o+12),s16(bytes,o+14)});
    for(std::size_t o=offsets[2];o<offsets[3];o+=12) world.placements.push_back({s16(bytes,o),s16(bytes,o+2),s16(bytes,o+4),u16(bytes,o+6),u32(bytes,o+8)});
    for(const auto& group:world.groups) if(std::size_t(group.first_placement)+group.placement_count>world.placements.size())
        throw std::runtime_error("world group placement range exceeds section");
    const auto definition_size=offsets[4]-offsets[3];
    for(const auto& placement:world.placements) if(placement.definition_offset>=definition_size)
        throw std::runtime_error("world placement definition offset exceeds section");
    return world;
}
} // namespace btga
