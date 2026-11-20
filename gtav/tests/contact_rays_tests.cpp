#include "graphics_fixture.h"
#include "native/contact_rays.h"

using namespace gtav::native;

int main() {
  const auto low = ContactOptions(1, 50), medium = ContactOptions(2, 100), high = ContactOptions(3, 25);
  CHECK(low.quality == 1 && low.strength == 0.5f && low.Enabled());
  CHECK(medium.quality == 2 && medium.strength == 1.0f && high.quality == 3 && high.strength == 0.25f);
  CHECK(!ContactOptions(0, 100).Enabled() && !ContactOptions(3, 0).Enabled());
  CHECK(!ContactOptions(-1, 50).Enabled() && !ContactOptions(4, 50).Enabled());
  CHECK(!ContactOptions(1, -1).Enabled() && !ContactOptions(1, 101).Enabled());
  CHECK(BeginContactStatus(ContactOptions(0, 50), true) == ContactRayStatus::Off);
  CHECK(BeginContactStatus(low, false) == ContactRayStatus::Unavailable);
  CHECK(BeginContactStatus(medium, true) == ContactRayStatus::NoCompatiblePass);
  CHECK(ContactStatusName(ContactRayStatus::NoAlignedDepth) == "no_aligned_depth");

  constexpr uint64_t copy = 0x487A191F12345678ull, shadow = 0x5416149F12345678ull;
  constexpr uint32_t copy_alpha = 1u << 16, multiply_alpha = 6u << 24;
  CHECK(ClassifyContactMask(copy, 8, 3, 15, copy_alpha) == ContactMaskDraw::Initialize);
  CHECK(ClassifyContactMask(shadow, 6, 4, 8, multiply_alpha) == ContactMaskDraw::Multiply);
  CHECK(ClassifyContactMask(shadow, 4, 6, 8, multiply_alpha) == ContactMaskDraw::Multiply);
  CHECK(ClassifyContactMask(shadow, 4, 6, 15, multiply_alpha) == ContactMaskDraw::None);
  CHECK(ClassifyContactMask(shadow, 4, 6, 0x88, multiply_alpha) == ContactMaskDraw::None);
  CHECK(ClassifyContactMask(shadow, 4, 6, 8, copy_alpha) == ContactMaskDraw::None);
  CHECK(ClassifyContactMask(shadow, 4, 9, 8, multiply_alpha) == ContactMaskDraw::None);
  CHECK(ClassifyContactMask(shadow >> 32, 4, 6, 8, multiply_alpha) == ContactMaskDraw::None);
  CHECK(ContactViewport(0, 0, 1280, 720) == kContactScene);
  CHECK(ContactViewport(0, 0, 640, 360) == kContactHalfScene);
  CHECK(ContactViewport(5, 0, 1280, 720).Empty());
  CHECK(ContactViewport(0, 384, 1280, 336).Empty());
  CHECK(ContactViewport(0, 0, 1280, 721).Empty());

  const ContactMaskStamp mask{1, 0x5555, kContactScene};
  CHECK(mask.Matches(1, 0x5555, kContactScene));
  CHECK(!mask.Matches(9, 0x5555, kContactScene));
  CHECK(!mask.Matches(1, 0x5556, kContactScene));  // same host EDRAM alias, different guest surface
  CHECK(!mask.Matches(1, 0x5555, kContactHalfScene));

  using Z = ContactDepthOrder;
  ContactDepthWriters writes;
  constexpr uint32_t forward = (3 << 4) | 6, reversed = (6 << 4) | 6, equal = (2 << 4) | 6;
  writes.Write(1, equal, true, 0x1111);
  CHECK(writes.For(1) == Z::Unknown);
  writes.Write(1, forward, true, 0x1111);
  writes.Write(1, equal, true, 0x1111);
  CHECK(writes.For(1) == Z::Forward && writes.For(2) == Z::Unknown);
  writes.Write(1, reversed, true, 0x1111);
  writes.Write(1, forward, true, 0x1111);
  CHECK(writes.For(1) == Z::Unknown);
  writes.Write(2, reversed, true, 0x1111);
  writes.Write(2, forward & ~4u, true, 0x1111);  // read-only
  CHECK(writes.For(2) == Z::Reversed);
  writes.Write(2, reversed, false, 0x1111);  // sub-view or special depth range
  CHECK(writes.For(2) == Z::Unknown);
  writes.Write(3, reversed, true, 0x1111);
  writes.Write(3, reversed, true, 0x2222);
  CHECK(writes.For(3) == Z::Unknown);  // depth alias
  const auto rev = writes.revision;
  writes.Clear();
  writes.Write(3, forward, true, 0x2222);
  CHECK(writes.For(3) == Z::Forward && writes.revision > rev);

  const int origin = 1, other = 2;
  for (float scale : {0.5f, 0.666667f, 1.0f, 1.5f}) {
    const Extent size{ScaledTexels(1280, scale), ScaledTexels(720, scale)};
    const auto top = ScaleResolveRegion({0, 0, 1280, 384, 0, 0}, scale, size.width, size.height);
    const auto bottom = ScaleResolveRegion({0, 384, 1280, 720, 0, 384}, scale, size.width, size.height);
    const ResolveRegion full{0, 0, size.width, size.height, 0, 0};
    ContactDepthCoverage depth;
    depth.Copy(7, &origin, size, Z::Forward, 52, top);
    CHECK(!depth.Ready(7));
    depth.Copy(7, &origin, size, Z::Forward, 52, bottom);
    CHECK(depth.Ready(7) && !depth.Ready(8) && depth.order == Z::Forward);
    depth.Copy(7, &origin, size, Z::Reversed, 52, full);
    CHECK(depth.Ready(7) && depth.order == Z::Reversed);
    depth.Copy(8, &origin, size, Z::Forward, 52, top);
    depth.Copy(8, &other, size, Z::Forward, 52, bottom);
    CHECK(!depth.Ready(8));  // cannot assemble two different targets
    depth.Copy(8, &origin, size, Z::Forward, 52, top);
    depth.Copy(8, &origin, size, Z::Forward, 53, bottom);
    CHECK(!depth.Ready(8));  // no combining different writes even to the same host depth
    depth.Copy(8, &origin, size, Z::Forward, 52, top);
    depth.Copy(9, &origin, size, Z::Forward, 52, bottom);
    CHECK(!depth.Ready(9));  // cannot reuse last frame's top tile
    depth.Copy(9, &origin, size, Z::Forward, 52, top);
    auto shifted = bottom;
    --shifted.dest_y;
    depth.Copy(9, &origin, size, Z::Forward, 52, shifted);
    CHECK(!depth.Ready(9));
    depth.Copy(9, &origin, size, Z::Forward, 52, top);
    auto gap = bottom;
    ++gap.dest_y;
    ++gap.top;
    depth.Copy(9, &origin, size, Z::Forward, 52, gap);
    CHECK(!depth.Ready(9));
    depth.Copy(9, &origin, size, Z::Unknown, 52, full);
    CHECK(!depth.Ready(9));
  }
  std::puts("Contact rays: opt-in, shadow fingerprint, depth direction, scaled tile coverage and fallbacks passed");
}
