#pragma once

// Works with both popular LiquidCrystal_I2C forks:
//   johnrickman / marcoschwartz (Arduino Library Manager) -> init()
//   fdebrabander (GitHub)                                -> begin()
// The int/long overload trick picks init() when it exists, begin() otherwise.
template <class T>
auto lcdStart(T& l, int) -> decltype(l.init(), void()) { l.init(); }

template <class T>
void lcdStart(T& l, long) { l.begin(); }
