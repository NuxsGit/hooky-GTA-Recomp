#pragma once

#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace gtav::tu27 {
inline bool Readable(rex::memory::Memory& memory, uint32_t address, uint32_t size, bool write = false) {
  if (address < 0x10000 || address % 4 || !size || size > UINT32_MAX - address) return false;
  auto* heap = memory.LookupHeap(address);
  if (!heap || heap != memory.LookupHeap(address + size - 1)) return false;
  using Access = rex::memory::PageAccess;
  const auto access = heap->QueryRangeAccess(address, address + size - 1);
  return access == Access::kReadWrite || access == Access::kExecuteReadWrite ||
         (!write && (access == Access::kReadOnly || access == Access::kExecuteReadOnly));
}

inline uint32_t Load(rex::memory::Memory& memory, uint32_t addr) {
  return rex::memory::load_and_swap<uint32_t>(memory.TranslateVirtual(addr));
}

struct Vec {
  float x, y, z;
  bool Valid(float bound) const {
    return std::abs(x) <= bound && std::abs(y) <= bound && std::abs(z) <= bound;
  }
};

// A separate PPCContext and bounded stack scratch keep the real VM call's arguments, return
// values and registers intact. Run on the bound guest script thread, not the overlay thread.
class NativeCall {
 public:
  NativeCall(PPCContext& context, uint8_t* base, rex::memory::Memory& memory,
             rex::runtime::FunctionDispatcher& dispatcher)
      : context_(context), base_(base), memory_(memory), dispatcher_(dispatcher), sp_(context.r1.u32) {}

  bool SafeStack() const { return sp_ > 0x800 && Readable(memory_, sp_ - 0x800, 0x800, true); }
  bool Supports(std::initializer_list<uint32_t> natives) const {
    return std::all_of(natives.begin(), natives.end(), [this](uint32_t n) { return dispatcher_.GetFunction(n) != nullptr; });
  }

  bool Invoke(uint32_t native, std::initializer_list<uint32_t> args = {}, PPCFunc* original = nullptr,
              std::optional<uint32_t> reference = std::nullopt) {
    return Call(native, args, original, reference, -1, {});
  }
  std::optional<uint32_t> ReadOutput(uint32_t native, std::initializer_list<uint32_t> args, unsigned index) {
    if (index >= args.size() || !Call(native, args, nullptr, {}, int(index), {}) || Result() != 1) return {};
    const uint32_t word = Load(memory_, sp_ - 0x80);
    return word == kUnset ? std::nullopt : std::optional(word);
  }
  bool InvokeString(uint32_t native, std::string_view text) {
    return !text.empty() && text.size() <= 31 && Call(native, {0}, nullptr, {}, 0, text);
  }

 private:
  bool Call(uint32_t native, std::initializer_list<uint32_t> args, PPCFunc* original,
            std::optional<uint32_t> reference, int output_index, std::string_view text) {
    if (!ok_) return false;
    auto* fn = original ? original : dispatcher_.GetFunction(native);
    if (!fn || args.size() > 16 || (reference && args.size())) { ok_ = false; return false; }
    const uint32_t call = sp_ - 0x100;
    result_ = call + 0x10;
    const uint32_t arguments = call + 0x30;
    memory_.Zero(call, 0x100);
    Store(call, result_);
    Store(call + 4, static_cast<uint32_t>(args.size()));
    Store(call + 8, arguments);
    for (uint32_t i = 0; i < 4; ++i) Store(result_ + i * 4, kUnset);
    uint32_t offset = arguments;
    for (const auto value : args) { Store(offset, value); offset += 4; }
    if (output_index >= 0) {
      const uint32_t data = call + 0x80;  // separate bounded scratch; zero-filled, guard after it
      Store(arguments + 4 * uint32_t(output_index), data);
      Store(data + 32, kUnset);
      if (text.empty()) Store(data, kUnset);
      else std::memcpy(memory_.TranslateVirtual(data), text.data(), text.size());
    }
    if (reference) {
      // Pointer natives such as REMOVE_BLIP and DELETE_VEHICLE may zero the caller's handle.
      // Give them a writable guest word in scratch, never a host pointer or real VM slot.
      Store(call + 0x20, *reference);
      Store(arguments, call + 0x20);
      Store(call + 4, 1);
    }
    PPCContext side = context_;
    side.r1.u32 -= 0x200;  // linkage/argument-save area below the native-call scratch
    side.r3.u64 = call;
    fn(side, base_);
    if (Load(memory_, result_ + 12) != kUnset || (output_index >= 0 && Load(memory_, call + 0xa0) != kUnset)) ok_ = false;
    return ok_;
  }

 public:
  uint32_t Read(uint32_t native, std::initializer_list<uint32_t> args = {}) {
    if (!Invoke(native, args)) return 0;
    const auto value = Result();
    if (value == kUnset) ok_ = false;
    return value;
  }
  bool Flag(uint32_t native, std::initializer_list<uint32_t> args = {}) {
    const uint32_t value = Read(native, args);
    if (value > 1) ok_ = false;
    return value != 0;
  }
  float Float(uint32_t native, std::initializer_list<uint32_t> args = {}) { return std::bit_cast<float>(Read(native, args)); }
  Vec Vector(uint32_t native, std::initializer_list<uint32_t> args = {}) {
    if (!Invoke(native, args)) return {};
    return {std::bit_cast<float>(Result(0)), std::bit_cast<float>(Result(1)), std::bit_cast<float>(Result(2))};
  }
  uint32_t Result(unsigned i = 0) const { return Load(memory_, result_ + 4 * i); }
  bool Okay() const { return ok_; }
  NativeCall CleanupCall() const {
    return NativeCall(context_, base_, memory_, dispatcher_);
  }

 private:
  static constexpr uint32_t kUnset = 0x7fc00000;  // unmodified vector slots are not finite
  void Store(uint32_t addr, uint32_t value) {
    rex::memory::store_and_swap<uint32_t>(memory_.TranslateVirtual(addr), value);
  }
  PPCContext& context_;
  uint8_t* base_;
  rex::memory::Memory& memory_;
  rex::runtime::FunctionDispatcher& dispatcher_;
  uint32_t sp_, result_ = 0;
  bool ok_ = true;
};

}  // namespace gtav::tu27
