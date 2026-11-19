#pragma once

namespace gtav::workshop {
struct DesktopView;
enum class PhoneAction { None, Close, Session, Build };
PhoneAction DrawPhone(const DesktopView&, unsigned fps, float scale);
void ResetPhone();  // render thread, upon opening a new phone session
}  // namespace gtav::workshop
