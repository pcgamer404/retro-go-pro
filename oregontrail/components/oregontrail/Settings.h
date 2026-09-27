#pragma once
namespace prefs {
constexpr int kBrightMin=10,kBrightMax=100,kBrightStep=10; constexpr int kVolMax=10;
extern int brightness; extern int volume; void load(); void save();
}
