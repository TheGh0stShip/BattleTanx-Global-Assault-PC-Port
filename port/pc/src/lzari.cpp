#include "btga/lzari.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace btga {
namespace {
constexpr int n=4096, f=60, threshold=2, n_char=314;
constexpr std::uint32_t q1=1U<<15, q2=2*q1, q3=3*q1, q4=4*q1, max_cum=q1-1;

class Decoder {
  public:
    explicit Decoder(std::span<const std::uint8_t> source) : source_(source) {
        for (int sym=n_char;sym>0;--sym) { sym_to_char_[sym]=sym-1; sym_freq_[sym]=1; sym_cum_[sym-1]=sym_cum_[sym]+1; }
        for (int i=n;i>0;--i) position_cum_[i-1]=position_cum_[i]+10000/(i+200);
    }
    void start() { for(int i=0;i<17;++i) value_=value_*2+bit(); }
    int character() {
        const auto span=high_-low_;
        const auto scaled=((value_-low_+1)*sym_cum_[0]-1)/span;
        const int sym=search(sym_cum_,scaled,n_char);
        high_=low_+span*sym_cum_[sym-1]/sym_cum_[0]; low_+=span*sym_cum_[sym]/sym_cum_[0];
        renormalize(); const int value=sym_to_char_[sym]; update(sym); return value;
    }
    int position() {
        const auto span=high_-low_;
        const auto scaled=((value_-low_+1)*position_cum_[0]-1)/span;
        const int pos=search(position_cum_,scaled,n)-1;
        high_=low_+span*position_cum_[pos]/position_cum_[0]; low_+=span*position_cum_[pos+1]/position_cum_[0];
        renormalize(); return pos;
    }
  private:
    int bit() {
        mask_ >>= 1;
        if (!mask_) {
            if (offset_ >= source_.size()) { padding_ += 8; if (padding_ > 32) throw std::runtime_error("truncated LZARI stream"); byte_=0xFF; }
            else byte_=source_[offset_++];
            mask_=0x80;
        }
        return (byte_&mask_)!=0;
    }
    template<std::size_t Size> static int search(const std::array<std::uint32_t,Size>& cumulative,
                                                  std::uint32_t value,int high) {
        int low=1; while(low<high){const int middle=(low+high)/2;if(cumulative[middle]>value)low=middle+1;else high=middle;} return low;
    }
    void update(int sym) {
        if(sym_cum_[0]>=max_cum){std::uint32_t total=0;for(int i=n_char;i>0;--i){sym_cum_[i]=total;sym_freq_[i]=(sym_freq_[i]+1)>>1;total+=sym_freq_[i];}sym_cum_[0]=total;}
        int i=sym; while(sym_freq_[i]==sym_freq_[i-1])--i;
        if(i<sym) std::swap(sym_to_char_[i],sym_to_char_[sym]);
        ++sym_freq_[i]; --i; while(i>=0) ++sym_cum_[i--];
    }
    void renormalize() {
        for(;;){if(low_>=q2){value_-=q2;low_-=q2;high_-=q2;}else if(low_>=q1&&high_<=q3){value_-=q1;low_-=q1;high_-=q1;}else if(high_>q2)return;low_*=2;high_*=2;value_=value_*2+bit();}
    }
    std::span<const std::uint8_t> source_; std::size_t offset_{}; int mask_{},byte_{},padding_{};
    std::uint32_t low_{},high_{q4},value_{};
    std::array<int,n_char+1> sym_to_char_{};
    std::array<std::uint32_t,n_char+1> sym_freq_{},sym_cum_{};
    std::array<std::uint32_t,n+1> position_cum_{};
};

std::uint32_t be32(std::span<const std::uint8_t> data) {
    return (std::uint32_t(data[0])<<24)|(std::uint32_t(data[1])<<16)|(std::uint32_t(data[2])<<8)|data[3];
}
}

std::vector<std::uint8_t> lzari_decompress(std::span<const std::uint8_t> stream,std::size_t max_output) {
    if(stream.size()<4) throw std::runtime_error("LZARI stream is too short");
    const std::size_t output_size=be32(stream);
    if(output_size>max_output) throw std::runtime_error("LZARI output exceeds configured limit");
    if(output_size==0) return {};
    if(stream.size()<7) throw std::runtime_error("LZARI stream is too short");
    Decoder decoder(stream.subspan(4)); decoder.start();
    std::array<std::uint8_t,n> ring{}; std::fill_n(ring.begin(),n-f,std::uint8_t{' '});
    int ring_offset=n-f; std::vector<std::uint8_t> output; output.reserve(output_size);
    while(output.size()<output_size){
        int character=decoder.character();
        if(character<256){output.push_back(static_cast<std::uint8_t>(character));ring[ring_offset]=static_cast<std::uint8_t>(character);ring_offset=(ring_offset+1)&(n-1);continue;}
        const int source_offset=(ring_offset-decoder.position()-1)&(n-1); const int length=character-255+threshold;
        if(output.size()+static_cast<std::size_t>(length)>output_size) throw std::runtime_error("LZARI match exceeds output");
        for(int i=0;i<length;++i){const auto value=ring[(source_offset+i)&(n-1)];output.push_back(value);ring[ring_offset]=value;ring_offset=(ring_offset+1)&(n-1);}
    }
    return output;
}
} // namespace btga
